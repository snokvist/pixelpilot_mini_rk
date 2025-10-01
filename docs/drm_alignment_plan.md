# DRM pipeline alignment plan

## Current pipeline behavior
- **Atomic display bootstrap** – `atomic_modeset_maxhz()` enables universal planes and atomic mode-setting, scans all connected
  outputs, chooses the highest refresh mode, auto-selects an NV12/YUV-capable overlay (falling back to a validated override when
  requested), and performs a one-shot atomic commit with a temporary ARGB dumb FB that is destroyed right after the commit.【F:src/drm_modeset.c†L96-L343】【F:src/drm_modeset.c†L409-L540】
- **Video plane ownership** – After the bootstrap the temporary FB is freed, the detected plane ID is propagated to the running
  configuration, and only GStreamer’s `kmssink` drives that plane; the main loop never reuses dumb framebuffers or reprograms the
  video plane outside of a new hotplug-triggered modeset.【F:src/drm_modeset.c†L490-L540】【F:src/main.c†L74-L175】
- **GStreamer QoS posture** – The video branch feeds `kmssink` behind leaky queues, keeps `sync` disabled, forces `qos` on, and
  constrains `max-lateness`, mirroring the jitter-friendly configuration from the reference project while allowing opt-in tuning
  via a debug build flag.【F:src/pipeline.c†L270-L332】
- **OSD isolation** – When enabled, the OSD code hunts for a secondary plane, creates its own dumb FB, refuses to touch the video
  plane, and drives updates through separate atomic commits so the video plane stays untouched during steady-state playback.【F:src/osd.c†L2254-L2378】

## Gaps versus the reference implementation
- Automated validation is still manual: the new regression checklist documents the expected mode/plane/QoS settings, but turning
  it into a scripted smoke test would make regressions even harder to miss.

## Plan to align with the reference behavior
1. **Auto-select a compatible video plane** — ✅ Completed.
2. **Enforce exclusive ownership of the video plane** — ✅ Completed.
3. **Lock in the proven QoS defaults** — ✅ Completed.
4. **Regression validation** — ✅ Checklist captured in `docs/drm_regression_checklist.md`; future work could automate it.
