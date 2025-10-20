#ifndef STABILIZER_H
#define STABILIZER_H

#include <glib.h>
#include <stdint.h>

typedef struct Stabilizer Stabilizer;
struct VideoDecoder;

typedef struct {
    gboolean enable;
    guint queue_depth;
    guint downscale_width;
    guint downscale_height;
    guint search_radius;
    guint inset_percent;
    gdouble smoothing_factor;
} StabilizerConfig;

typedef struct {
    const guint8 *y_plane;
    guint32 stride;
    guint32 width;
    guint32 height;
    guint64 pts;
    guint64 capture_ns;
} StabilizerFrameDescriptor;

Stabilizer *stabilizer_new(void);
void stabilizer_free(Stabilizer *st);

int stabilizer_configure(Stabilizer *st, const StabilizerConfig *cfg);
int stabilizer_start(Stabilizer *st, struct VideoDecoder *decoder);
void stabilizer_stop(Stabilizer *st);

gboolean stabilizer_submit_frame(Stabilizer *st, const StabilizerFrameDescriptor *frame);

#endif // STABILIZER_H
