# Video Stabilizer Pipeline

The video stabilizer module (`src/video_stabilizer.c`) provides an optional
post-processing stage that copies decoded NV12 frames into a secondary pool of
DMA-BUF backed buffers using Rockchip's RGA hardware. A companion motion
estimator (`src/video_motion_estimator.c`) downsamples the luma plane, performs
block matching against the previous frame, and emits filtered translations that
drive the stabiliser automatically when no external motion metadata is
available. The combined lifecycle looks like:

1. Allocate an instance with `video_stabilizer_new()` and initialise it with a
   `StabilizerConfig` derived from the runtime configuration file or CLI
   flags.
2. Whenever the decoder's geometry changes, call
   `video_stabilizer_configure()` to update the input/output dimensions and
   DMA strides.
3. For each decoded frame, invoke `video_stabilizer_process()` with the source
   PRIME FD, destination PRIME FD, and the per-frame `StabilizerParams`. The
   decoder seeds these parameters with the motion estimator's filtered
   translations; callers can still override the values by calling
   `video_decoder_set_stabilizer_params()` when gyro or optical-flow data is
   available. When librga is unavailable, the module automatically bypasses
   processing.

The decoder owns two frame pools: the raw MPP buffers and a matching set of
DRM dumb buffers that hold the stabilised frames. Synchronisation is handled by
propagating the stabiliser's release fence to the KMS plane via the
`IN_FENCE_FD`/`OUT_FENCE_PTR` properties when the driver supports them.

## Configuration

`config.ini` and CLI flags gain a `[stabilizer]` section and corresponding
`--stabilizer-*` options. These settings control whether the stabiliser is
active, how the built-in estimator behaves, and the translation/rotation clamps
used when interpreting per-frame parameters. Refer to `src/config.c` and
`src/config_ini.c` for the available keys. Of note:

* `--stabilizer-diagnostics` (or `diagnostics = true` in the INI) prints an
  info log on the first processed frame and every 60th frame afterwards,
  confirming whether the module is cropping based on external parameters or
  the demo wave. A bypass reason is logged once if the module cannot run.
* `--stabilizer-demo-enable`, `--stabilizer-demo-amplitude`, and
  `--stabilizer-demo-frequency` enable a built-in sine/cosine waveform that
  nudges the crop window even when no motion vectors are supplied. This makes
  it obvious on-screen that the stabiliser path is active and also exercises
  the diagnostic logging.
* `--stabilizer-estimator-enable`, `--stabilizer-estimator-search`,
  `--stabilizer-estimator-downsample`, `--stabilizer-estimator-smoothing`, and
  `--stabilizer-estimator-diagnostics` control the internal block-matching
  engine. The search radius defaults to the translation clamp, the downsample
  factor trades accuracy for CPU time, and the smoothing factor (0.0–0.98)
  damps the raw estimates before they are handed to the stabiliser.
* `--stabilizer-manual-enable`, `--stabilizer-manual-offset-x`, and
  `--stabilizer-manual-offset-y` still apply a fixed translation whenever no
  per-frame parameters are provided. They remain useful for smoke tests or for
  biasing the crop when the estimator is disabled.
* `--stabilizer-guard-band-x` / `--stabilizer-guard-band-y` (or
  `guard-band-x` / `guard-band-y` in the INI) reserve a symmetric crop margin
  around the decoded frame. The guard value is interpreted as the number of
  pixels trimmed from each edge before any translation is applied. The RGA blit
  scales the cropped region back up to the full output size so the stabilised
  frame still fills the display. Larger guard bands provide more room for
  per-frame translations (up to `guard + stride_extra` in each direction). When
  the options are omitted no base crop is applied.

Typical command line enablement looks like:

```sh
./pixelpilot_mini_rk --stabilizer-enable --stabilizer-strength 1.15 \
    --stabilizer-max-translation 24 --stabilizer-max-rotation 3
```

For INI-driven deployments copy `config/stabilizer-demo.ini` and tweak the
translation strength/clamps to suit the expected motion profile before passing
it to `--config`. The demo file ships with diagnostics and the internal demo
waveform enabled, so you can observe the stabiliser at work immediately.

If you prefer to keep the source frame aligned while still exercising the
stabiliser, use `config/stabilizer-manual.ini`. It enables diagnostics, turns
off the waveform, reserves a symmetric guard band, and requests a static
translation so the output frame visibly differs from the raw decoder buffer.
Manual offsets are clamped by both `max-translation` and the available
guard-band range (plus any decoder stride padding). When an offset is reduced
you will see a one-off diagnostic similar to:

```
Video stabilizer manual offsets (200,200) constrained by guard 96 x 96 (stride extra 32 x 0); crop=(128,96) src=(1248,888)
```

Increase `max-translation` or expand the guard band until the requested motion
fits inside the crop window.

Manual mode keeps the crop static; it is intended purely as a smoke test for
the RGA path. When the estimator is enabled you will see live diagnostics of
the measured translations alongside any clamp messages. External subsystems can
still provide explicit offsets by calling `video_decoder_set_stabilizer_params`
—those values override the estimator for the next frame.

With diagnostics enabled the log will periodically emit entries such as:

```
Video stabilizer applied crop=(6,2) demo=yes manual=no params=no frame=60
```

If the module cannot run you will see a one-off bypass reason (for example when
RGA support is unavailable or when per-frame metadata disables the transform).

## Testing

Run `make test` to build and execute `tests/video_stabilizer_test`. The smoke
test validates basic API coverage on all platforms and attempts a real DMA-BUF
round-trip when both librga and a DRM device are present.
