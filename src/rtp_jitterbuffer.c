// SPDX-License-Identifier: MIT

#include "rtp_jitterbuffer.h"

#include <gst/gst.h>

GST_DEBUG_CATEGORY_STATIC(sstar_rtp_jitterbuffer_debug);
#define GST_CAT_DEFAULT sstar_rtp_jitterbuffer_debug

#define RTP_MIN_HEADER 12

typedef struct {
    guint16 sequence;
    GstBuffer *buffer;
} RtpQueuedPacket;

struct _SstarRtpJitterBuffer {
    GstElement parent;

    GstPad *sinkpad;
    GstPad *srcpad;

    gboolean enabled;
    guint latency_ms;
    guint max_misorder;

    gboolean have_expected_seq;
    guint16 expected_seq;
    gboolean gap_active;
    guint64 gap_started_ns;

    GQueue *queued;
};

struct _SstarRtpJitterBufferClass {
    GstElementClass parent_class;
};

G_DEFINE_TYPE(SstarRtpJitterBuffer, sstar_rtp_jitterbuffer, GST_TYPE_ELEMENT)

enum {
    PROP_0,
    PROP_ENABLED,
    PROP_LATENCY_MS,
    PROP_MAX_MISORDER,
};

static inline guint64 monotonic_ns(void) {
    return (guint64)g_get_monotonic_time() * 1000ull;
}

static inline gboolean parse_rtp_sequence(const GstMapInfo *map, guint16 *sequence_out) {
    if (map == NULL || sequence_out == NULL || map->size < RTP_MIN_HEADER) {
        return FALSE;
    }

    const guint8 *data = map->data;
    guint8 version = data[0] >> 6;
    if (version != 2) {
        return FALSE;
    }

    *sequence_out = ((guint16)data[2] << 8) | (guint16)data[3];
    return TRUE;
}

static inline gint16 seq_delta(guint16 a, guint16 b) {
    return (gint16)(a - b);
}

static void queued_packet_free(RtpQueuedPacket *packet) {
    if (packet == NULL) {
        return;
    }
    if (packet->buffer != NULL) {
        gst_buffer_unref(packet->buffer);
    }
    g_free(packet);
}

static void clear_queue(SstarRtpJitterBuffer *self) {
    if (self == NULL || self->queued == NULL) {
        return;
    }

    while (!g_queue_is_empty(self->queued)) {
        RtpQueuedPacket *packet = g_queue_pop_head(self->queued);
        queued_packet_free(packet);
    }
}

static void reset_state(SstarRtpJitterBuffer *self) {
    if (self == NULL) {
        return;
    }

    clear_queue(self);
    self->have_expected_seq = FALSE;
    self->expected_seq = 0;
    self->gap_active = FALSE;
    self->gap_started_ns = 0;
}

static void queue_insert_sorted(SstarRtpJitterBuffer *self, RtpQueuedPacket *packet) {
    if (self == NULL || self->queued == NULL || packet == NULL) {
        return;
    }

    GList *iter = self->queued->head;
    while (iter != NULL) {
        RtpQueuedPacket *current = (RtpQueuedPacket *)iter->data;
        if (current != NULL) {
            gint16 delta = seq_delta(packet->sequence, current->sequence);
            if (delta < 0) {
                g_queue_insert_before(self->queued, iter, packet);
                return;
            }
            if (delta == 0) {
                queued_packet_free(packet);
                return;
            }
        }
        iter = iter->next;
    }

    g_queue_push_tail(self->queued, packet);
}

static RtpQueuedPacket *queue_find_by_sequence(SstarRtpJitterBuffer *self, guint16 sequence) {
    if (self == NULL || self->queued == NULL) {
        return NULL;
    }

    GList *iter = self->queued->head;
    while (iter != NULL) {
        RtpQueuedPacket *packet = (RtpQueuedPacket *)iter->data;
        if (packet != NULL && packet->sequence == sequence) {
            g_queue_delete_link(self->queued, iter);
            return packet;
        }
        iter = iter->next;
    }

    return NULL;
}

static GstFlowReturn push_and_advance(SstarRtpJitterBuffer *self, GstBuffer *buffer) {
    if (self == NULL || buffer == NULL) {
        return GST_FLOW_ERROR;
    }

    GstFlowReturn ret = gst_pad_push(self->srcpad, buffer);
    if (ret != GST_FLOW_OK) {
        return ret;
    }

    self->expected_seq = (guint16)(self->expected_seq + 1u);
    self->have_expected_seq = TRUE;
    return GST_FLOW_OK;
}

static GstFlowReturn flush_ready_packets(SstarRtpJitterBuffer *self) {
    if (self == NULL) {
        return GST_FLOW_ERROR;
    }

    while (TRUE) {
        RtpQueuedPacket *queued = queue_find_by_sequence(self, self->expected_seq);
        if (queued == NULL) {
            self->gap_active = !g_queue_is_empty(self->queued);
            if (self->gap_active && self->gap_started_ns == 0) {
                self->gap_started_ns = monotonic_ns();
            }
            break;
        }

        self->gap_active = FALSE;
        self->gap_started_ns = 0;
        GstFlowReturn ret = push_and_advance(self, queued->buffer);
        queued->buffer = NULL;
        queued_packet_free(queued);
        if (ret != GST_FLOW_OK) {
            return ret;
        }
    }

    return GST_FLOW_OK;
}

static GstFlowReturn maybe_force_skip_gap(SstarRtpJitterBuffer *self, guint64 now_ns) {
    if (self == NULL || !self->gap_active || self->latency_ms == 0 || g_queue_is_empty(self->queued)) {
        return GST_FLOW_OK;
    }

    guint64 wait_ns = (guint64)self->latency_ms * 1000000ull;
    if (self->gap_started_ns == 0 || now_ns < self->gap_started_ns || now_ns - self->gap_started_ns < wait_ns) {
        return GST_FLOW_OK;
    }

    RtpQueuedPacket *first = (RtpQueuedPacket *)g_queue_peek_head(self->queued);
    if (first == NULL) {
        self->gap_active = FALSE;
        self->gap_started_ns = 0;
        return GST_FLOW_OK;
    }

    GST_DEBUG_OBJECT(self,
                     "Skipping missing RTP sequence range (%u..%u) after %u ms wait",
                     (guint)self->expected_seq,
                     (guint)((guint16)(first->sequence - 1u)),
                     self->latency_ms);

    self->expected_seq = first->sequence;
    self->gap_active = FALSE;
    self->gap_started_ns = 0;
    return flush_ready_packets(self);
}

static GstFlowReturn sstar_rtp_jitterbuffer_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer) {
    SstarRtpJitterBuffer *self = SSTAR_RTP_JITTERBUFFER(parent);
    (void)pad;

    if (buffer == NULL) {
        return GST_FLOW_OK;
    }

    if (!self->enabled) {
        return gst_pad_push(self->srcpad, buffer);
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }

    guint16 sequence = 0;
    gboolean have_sequence = parse_rtp_sequence(&map, &sequence);
    gst_buffer_unmap(buffer, &map);

    if (!have_sequence) {
        return gst_pad_push(self->srcpad, buffer);
    }

    guint64 now_ns = monotonic_ns();

    if (!self->have_expected_seq) {
        self->have_expected_seq = TRUE;
        self->expected_seq = sequence;
        self->gap_active = FALSE;
        self->gap_started_ns = 0;
    }

    gint16 delta = seq_delta(sequence, self->expected_seq);
    if (delta < 0) {
        GST_LOG_OBJECT(self, "Dropping late/duplicate RTP sequence %u expected=%u", (guint)sequence,
                       (guint)self->expected_seq);
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
    }

    if (delta == 0) {
        GstFlowReturn ret = push_and_advance(self, buffer);
        if (ret != GST_FLOW_OK) {
            return ret;
        }
        return flush_ready_packets(self);
    }

    guint max_misorder = self->max_misorder;
    if ((guint)delta > max_misorder) {
        GST_DEBUG_OBJECT(self,
                         "Large RTP jump seq=%u expected=%u delta=%d exceeds max-misorder=%u; forcing resync",
                         (guint)sequence,
                         (guint)self->expected_seq,
                         delta,
                         max_misorder);
        self->expected_seq = sequence;
        self->gap_active = FALSE;
        self->gap_started_ns = 0;

        GstFlowReturn ret = push_and_advance(self, buffer);
        if (ret != GST_FLOW_OK) {
            return ret;
        }
        return flush_ready_packets(self);
    }

    RtpQueuedPacket *queued = g_new0(RtpQueuedPacket, 1);
    if (queued == NULL) {
        return gst_pad_push(self->srcpad, buffer);
    }

    queued->sequence = sequence;
    queued->buffer = buffer;
    queue_insert_sorted(self, queued);

    if (!self->gap_active) {
        self->gap_active = TRUE;
        self->gap_started_ns = now_ns;
    }

    return maybe_force_skip_gap(self, now_ns);
}

static gboolean sstar_rtp_jitterbuffer_sink_event(GstPad *pad, GstObject *parent, GstEvent *event) {
    SstarRtpJitterBuffer *self = SSTAR_RTP_JITTERBUFFER(parent);
    (void)pad;

    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_STOP:
        reset_state(self);
        break;
    case GST_EVENT_EOS: {
        GstFlowReturn ret = flush_ready_packets(self);
        if (ret != GST_FLOW_OK) {
            GST_WARNING_OBJECT(self, "Failed to flush queued RTP packets on EOS: %s", gst_flow_get_name(ret));
        }
        break;
    }
    default:
        break;
    }

    return gst_pad_push_event(self->srcpad, event);
}

static void sstar_rtp_jitterbuffer_set_property(GObject *object, guint prop_id, const GValue *value,
                                                GParamSpec *pspec) {
    SstarRtpJitterBuffer *self = SSTAR_RTP_JITTERBUFFER(object);

    switch (prop_id) {
    case PROP_ENABLED:
        self->enabled = g_value_get_boolean(value);
        if (!self->enabled) {
            reset_state(self);
        }
        break;
    case PROP_LATENCY_MS:
        self->latency_ms = g_value_get_uint(value);
        break;
    case PROP_MAX_MISORDER:
        self->max_misorder = g_value_get_uint(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void sstar_rtp_jitterbuffer_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    SstarRtpJitterBuffer *self = SSTAR_RTP_JITTERBUFFER(object);

    switch (prop_id) {
    case PROP_ENABLED:
        g_value_set_boolean(value, self->enabled);
        break;
    case PROP_LATENCY_MS:
        g_value_set_uint(value, self->latency_ms);
        break;
    case PROP_MAX_MISORDER:
        g_value_set_uint(value, self->max_misorder);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void sstar_rtp_jitterbuffer_finalize(GObject *object) {
    SstarRtpJitterBuffer *self = SSTAR_RTP_JITTERBUFFER(object);

    reset_state(self);
    if (self->queued != NULL) {
        g_queue_free(self->queued);
        self->queued = NULL;
    }

    G_OBJECT_CLASS(sstar_rtp_jitterbuffer_parent_class)->finalize(object);
}

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE("sink",
                            GST_PAD_SINK,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("application/x-rtp"));

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE("src",
                            GST_PAD_SRC,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("application/x-rtp"));

static void sstar_rtp_jitterbuffer_class_init(SstarRtpJitterBufferClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = sstar_rtp_jitterbuffer_set_property;
    gobject_class->get_property = sstar_rtp_jitterbuffer_get_property;
    gobject_class->finalize = sstar_rtp_jitterbuffer_finalize;

    g_object_class_install_property(gobject_class,
                                    PROP_ENABLED,
                                    g_param_spec_boolean("enabled",
                                                         "Enabled",
                                                         "Enable RTP sequence reordering buffer",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(gobject_class,
                                    PROP_LATENCY_MS,
                                    g_param_spec_uint("latency-ms",
                                                      "Latency (ms)",
                                                      "Maximum wait for missing RTP packets before skipping gap",
                                                      0,
                                                      2000,
                                                      8,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(gobject_class,
                                    PROP_MAX_MISORDER,
                                    g_param_spec_uint("max-misorder",
                                                      "Max Misorder",
                                                      "Maximum sequence-distance to hold for reorder",
                                                      0,
                                                      4096,
                                                      64,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_set_static_metadata(element_class,
                                          "SStar RTP jitterbuffer",
                                          "Network/RTP",
                                          "Lightweight RTP sequence reorder buffer",
                                          "PixelPilot Project");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);
}

static void sstar_rtp_jitterbuffer_init(SstarRtpJitterBuffer *self) {
    self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
    gst_pad_set_chain_function(self->sinkpad, GST_DEBUG_FUNCPTR(sstar_rtp_jitterbuffer_chain));
    gst_pad_set_event_function(self->sinkpad, GST_DEBUG_FUNCPTR(sstar_rtp_jitterbuffer_sink_event));
    gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

    self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
    gst_pad_use_fixed_caps(self->srcpad);
    gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

    self->enabled = FALSE;
    self->latency_ms = 8;
    self->max_misorder = 64;
    self->have_expected_seq = FALSE;
    self->expected_seq = 0;
    self->gap_active = FALSE;
    self->gap_started_ns = 0;
    self->queued = g_queue_new();
}

gboolean sstar_rtp_jitterbuffer_register(void) {
    static gsize once_init = 0;
    static gboolean registered = FALSE;

    if (g_once_init_enter(&once_init)) {
        GST_DEBUG_CATEGORY_INIT(sstar_rtp_jitterbuffer_debug, "sstarrtpjitterbuffer", 0,
                                "SStar RTP jitterbuffer");
        registered = gst_element_register(NULL, "sstarrtpjitterbuffer", GST_RANK_PRIMARY + 10,
                                          SSTAR_TYPE_RTP_JITTERBUFFER);
        g_once_init_leave(&once_init, 1);
    }

    return registered;
}
