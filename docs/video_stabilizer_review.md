# Video Stabilizer Review and Roadmap

## Current implementation snapshot

The decoder allocates paired DRM dumb buffers for each frame slot so that raw
MPP output can be copied into a stabilised surface via the RGA wrapper before it
is queued to the KMS plane.【F:src/video_decoder.c†L393-L666】  Each raw buffer is
memory-mapped once at setup, letting the CPU-backed motion estimator downsample
the luminance plane and compare the current frame against the previous history
without additional copies.【F:src/video_motion_estimator.c†L1-L236】  The estimator’s filtered translations are fed directly into the
stabiliser, which crops the decoded NV12 frame according to configurable guard
bands, applies the estimated/manual/demo translations, and blits the cropped
region back into the processed buffer while scaling it to the original output
size.【F:src/video_stabilizer.c†L282-L520】

Runtime configuration now covers estimator toggles (enable/disable, search
radius, downsample factor, smoothing, diagnostics) alongside the existing demo
and manual controls.【F:src/config.c†L212-L540】【F:src/config_ini.c†L1080-L1185】  Guard bands default to the translation clamp and are
clamped against stride headroom, emitting one-off diagnostics when manual or
estimated offsets are limited.【F:src/video_stabilizer.c†L300-L449】  The smoke test exercises the estimator with a synthetic jitter
sequence before running the RGA copy path, ensuring both components build and
execute on supported systems.【F:tests/video_stabilizer_test.c†L1-L289】

## Gaps preventing real stabilisation

1. **Rotation and sub-pixel support remain absent.** Rotation fields are still
   logged but ignored; the cropper applies integer translations only, so angular
   motion cannot be compensated.【F:src/video_stabilizer.c†L392-L433】
2. **Confidence gating and fallback heuristics.** The estimator emits a
   confidence metric but the decoder currently treats any valid estimate as
   authoritative. We should gate low-confidence samples, blend with manual
   offsets, or request IDRs when motion cannot be tracked reliably.【F:src/video_motion_estimator.c†L170-L236】
3. **Sensor fusion and calibration.** The block matcher handles luminance-only
   motion; integrating IMU or optical-flow inputs would improve responsiveness
   and resilience when the scene lacks texture.
4. **Performance profiling.** Downsampling at factor 1–2 increases CPU load.
   Width/height clamps now keep the estimator grid within ~256×144 pixels by
   default, but we should still profile typical ARM targets, tune the defaults,
   and consider NEON intrinsics or adaptive resolution.

## Goal

Keep improving image stabilisation by extending beyond pure translations,
hardening the estimator’s fallback story, and ensuring the implementation meets
real-time requirements on the target SoCs.

## Plan of record

1. **Rotation-aware stabilisation.** Explore extending the RGA path to handle
   small rotation matrices or multi-pass crops so the estimator (or an IMU feed)
   can compensate for angular motion.
2. **Confidence-driven fallbacks.** Use the estimator’s confidence metric to
   blend towards manual offsets, freeze the crop, or request an IDR when motion
   cannot be tracked reliably instead of always applying the filtered result.
3. **Sensor fusion.** Plumb optional IMU/gyro inputs through
   `video_decoder_set_stabilizer_params()` and merge them with the block-matcher
   output for improved robustness in low-texture scenes.
4. **Performance validation.** Benchmark the estimator on the RK3588 and RK3568
   targets, evaluate alternative downsample factors, and identify hotspots that
   would benefit from NEON acceleration.
5. **Automation.** Augment the smoke test with scripted comparisons against a
   recorded shaky clip, measuring residual motion to guard against regressions.
