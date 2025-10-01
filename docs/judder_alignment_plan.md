# DRM Atomic + kmssink Alignment

## Implemented adjustments
* Primary scanout blanking is now the default (with a `--keep-primary` escape hatch and plane guardrails), and the atomic commit detaches the legacy primary plane whenever blanking remains enabled.【F:src/config.c†L17-L29】【F:src/config.c†L72-L90】【F:src/config.c†L205-L260】【F:src/drm_modeset.c†L440-L447】
* Video buffering budgets were trimmed to 16/4/4 frames and are clamped at runtime to prevent operators from inflating latency beyond the low-latency envelope.【F:src/config.c†L27-L90】【F:src/config.c†L46-L70】
* kmssink is forced back to `sync=false`/`qos=true` before playback even if configuration overrides request otherwise, ensuring tardy frames are dropped instead of blocking atomic flips.【F:src/pipeline.c†L300-L316】
* The modeset routine now auto-detects a compatible overlay plane (preferring the highest `ZPOS`), caches it in `ModesetResult`, runs a TEST_ONLY dry run, and reuses the resolved plane for the real commit.【F:src/drm_modeset.c†L160-L483】【F:include/drm_modeset.h†L8-L12】
* Pipeline startup, OSD setup, and telemetry now consume the resolved plane ID so kmssink and overlays follow the same hardware selection on restarts.【F:src/pipeline.c†L507-L533】【F:src/main.c†L78-L94】【F:src/osd.c†L318-L325】

## Follow-up watch items
* Monitor hardware that exposes only a single plane shared between video and OSD; if necessary, relax the auto-detect skip for user-assigned OSD planes on such systems.
* Re-evaluate queue ceilings if future codecs or higher frame rates require deeper buffering without reintroducing judder.
