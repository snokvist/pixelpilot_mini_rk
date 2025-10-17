#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "drm_modeset.h"
#include "idr_requester.h"
#include "video_stabilizer.h"

typedef struct VideoDecoder VideoDecoder;

VideoDecoder *video_decoder_new(void);
void video_decoder_free(VideoDecoder *vd);

int video_decoder_init(VideoDecoder *vd, const AppCfg *cfg, const ModesetResult *ms, int drm_fd);
void video_decoder_deinit(VideoDecoder *vd);

int video_decoder_start(VideoDecoder *vd);
void video_decoder_stop(VideoDecoder *vd);

int video_decoder_feed(VideoDecoder *vd, const guint8 *data, size_t size);
void video_decoder_send_eos(VideoDecoder *vd);

void video_decoder_set_idr_requester(VideoDecoder *vd, IdrRequester *requester);
void video_decoder_set_stabilizer_params(VideoDecoder *vd, const StabilizerParams *params);

size_t video_decoder_max_packet_size(const VideoDecoder *vd);

#endif // VIDEO_DECODER_H
