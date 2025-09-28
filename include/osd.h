#ifndef OSD_H
#define OSD_H

#include <stdint.h>

#include "config.h"
#include "drm_modeset.h"
#include "pipeline.h"

typedef struct OSD OSD;

void osd_init(OSD *osd);
int osd_setup(int fd, const AppCfg *cfg, const ModesetResult *ms, int video_plane_id, OSD *osd);
void osd_update_stats(int fd, const AppCfg *cfg, const ModesetResult *ms, const PipelineState *ps,
                      int audio_disabled, int restart_count, OSD *osd);
int osd_is_enabled(const OSD *osd);
int osd_is_active(const OSD *osd);
void osd_disable(int fd, OSD *osd);
void osd_teardown(int fd, OSD *osd);

#endif // OSD_H
