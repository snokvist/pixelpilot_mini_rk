// SPDX-License-Identifier: MIT

#ifndef RTP_JITTERBUFFER_H
#define RTP_JITTERBUFFER_H

#include <glib-object.h>

G_BEGIN_DECLS

GType sstar_rtp_jitterbuffer_get_type(void);

#define SSTAR_TYPE_RTP_JITTERBUFFER (sstar_rtp_jitterbuffer_get_type())
#define SSTAR_RTP_JITTERBUFFER(obj)                                                                    \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), SSTAR_TYPE_RTP_JITTERBUFFER, SstarRtpJitterBuffer))
#define SSTAR_IS_RTP_JITTERBUFFER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), SSTAR_TYPE_RTP_JITTERBUFFER))

typedef struct _SstarRtpJitterBuffer SstarRtpJitterBuffer;
typedef struct _SstarRtpJitterBufferClass SstarRtpJitterBufferClass;

gboolean sstar_rtp_jitterbuffer_register(void);

G_END_DECLS

#endif // RTP_JITTERBUFFER_H
