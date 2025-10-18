# Video Stabilizer Review and Roadmap

## Current implementation snapshot

The decoder allocates paired DRM dumb buffers for each frame slot so that raw
MPP output can be copied into a stabilised surface via the RGA wrapper before it
is queued to the KMS plane.【F:src/video_decoder.c†L393-L666】  The stabiliser
module itself crops the decoded NV12 frame according to configurable guard
bands, applies optional manual/demo translations, and blits the cropped region
back into the processed buffer while scaling it to the original output size.【F:src/video_stabilizer.c†L282-L520】  Runtime configuration exposes knobs for
manual offsets, demo mode, guard bands, and diagnostics but no automatic motion
estimation pipeline feeds per-frame transforms into the decoder.

The `video_decoder_set_stabilizer_params()` entrypoint simply stores the most
recent parameters provided by external code, leaving the stabiliser to operate
purely on manual or demo offsets when no caller injects motion data.【F:src/video_decoder.c†L1041-L1056】  As a consequence, the stabiliser only
produces static crops unless another subsystem actively pushes per-frame
translations.

## Gaps preventing real stabilisation

1. **Missing motion source.** Nothing inside the decoder or pipeline estimates
   frame-to-frame motion, so `StabilizerParams` remains unset in normal runs and
   the RGA stage merely copies or applies static/manual crops.
2. **Limited guard-band management.** Guard bands default to zero and must be
   tuned manually, yet meaningful stabilisation requires symmetric margins that
   accommodate the expected translation range without exhausting the decoder’s
   stride slack.
3. **No smoothing or history.** Even if external code supplied translations, the
   stabiliser would apply them directly without filtering or dampening, risking
   jitter.
4. **Translation-only path.** Rotation fields are accepted but ignored; only
   integer translations end up in the crop window, so the pipeline cannot handle
   angular motion or sub-pixel adjustments.【F:src/video_stabilizer.c†L392-L433】

## Goal

Deliver true image stabilisation by deriving per-frame transforms from the
incoming video stream (or auxiliary sensors) and feeding those into the existing
RGA copy path so that the displayed output compensates for camera shake in real
time.

## Plan of record

1. **Introduce a motion-estimation module.**
   - Map the luminance plane of decoded frames (using `drmModeMapDumb` on the
     raw buffer handles) and downsample to a manageable resolution for analysis.
   - Implement a coarse-to-fine search (e.g., block matching or phase
     correlation) to estimate integer/sub-pixel translations against the
     previous frame, tracking confidence/quality metrics.
   - Maintain a low-pass filtered translation history to smooth abrupt changes
     before emitting `StabilizerParams`.

2. **Integrate with the decoder.**
   - Instantiate the estimator alongside the stabiliser and update it from the
     frame thread prior to invoking `video_stabilizer_process()` so every frame
     has a fresh transform.【F:src/video_decoder.c†L600-L666】
   - When the estimator yields a confident motion vector, call
     `video_decoder_set_stabilizer_params()` with the filtered translation; fall
     back to manual/demo options when estimation fails.

3. **Automate guard-band selection.**
   - Derive default guard bands from the configured maximum translation (e.g.,
     `guard = ceil(max_translation)`), clamping to stride headroom to avoid RGA
     errors.
   - Surface diagnostics that report both the chosen guard band and any clipping
     imposed by stride margins so operators know when the requested motion
     exceeds the safe window.

4. **Enhance configurability and observability.**
   - Add INI/CLI toggles for estimator behaviour (enable/disable, search radius,
     filter strength) and expose runtime counters (e.g., estimation success
     rate, average translation) through existing diagnostics logging.

5. **Testing strategy.**
   - Extend `tests/video_stabilizer_test` with synthetic jitter sequences to
     validate that estimator-generated parameters move the crop window as
     expected.
   - Provide a scripted integration test that replays a shaky clip, captures the
     stabilised output, and verifies the residual motion falls within the target
     tolerance.

Executing this plan will bridge the current gap between the static RGA cropper
and a genuine stabilisation pipeline, enabling real-time correction without
relying on demo waveforms or manual offsets.
