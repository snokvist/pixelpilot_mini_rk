# Video Stabilizer Pipeline

The video stabilizer module (`src/video_stabilizer.c`) provides an optional
post-processing stage that copies decoded NV12 frames into a secondary pool of
DMA-BUF backed buffers using Rockchip's RGA hardware. The module exposes a
simple lifecycle:

1. Allocate an instance with `video_stabilizer_new()` and initialise it with a
   `StabilizerConfig` derived from the runtime configuration file or CLI
   flags.
2. Whenever the decoder's geometry changes, call
   `video_stabilizer_configure()` to update the input/output dimensions and
   DMA strides.
3. For each decoded frame, invoke `video_stabilizer_process()` with the source
   PRIME FD, destination PRIME FD, and the per-frame `StabilizerParams`. The
   decoder can update the parameters via
   `video_decoder_set_stabilizer_params()` to apply motion-vector or gyro
   driven offsets. When librga is unavailable, the module automatically
   bypasses processing.

The decoder owns two frame pools: the raw MPP buffers and a matching set of
DRM dumb buffers that hold the stabilised frames. Synchronisation is handled by
propagating the stabiliser's release fence to the KMS plane via the
`IN_FENCE_FD`/`OUT_FENCE_PTR` properties when the driver supports them.

## Configuration

`config.ini` and CLI flags gain a `[stabilizer]` section and corresponding
`--stabilizer-*` options. These settings control whether the stabiliser is
active and the translation/rotation clamps used when interpreting per-frame
parameters. Refer to `src/config.c` and `src/config_ini.c` for the available
keys. Of note:

* `--stabilizer-diagnostics` (or `diagnostics = true` in the INI) prints an
  info log on the first processed frame and every 60th frame afterwards,
  confirming whether the module is cropping based on external parameters or
  the demo wave. A bypass reason is logged once if the module cannot run.
* `--stabilizer-demo-enable`, `--stabilizer-demo-amplitude`, and
  `--stabilizer-demo-frequency` enable a built-in sine/cosine waveform that
  nudges the crop window even when no motion vectors are supplied. This makes
  it obvious on-screen that the stabiliser path is active and also exercises
  the diagnostic logging.
* `--stabilizer-manual-enable`, `--stabilizer-manual-offset-x`, and
  `--stabilizer-manual-offset-y` apply a fixed translation whenever no
  per-frame parameters are provided. This is useful on hardware without motion
  metadata because it keeps the RGA copy path active even with the demo wave
  disabled.

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
off the waveform, and requests a static 12 pixel horizontal crop so the output
frame visibly differs from the raw decoder buffer. Adjust the offsets to match
your test scenario.

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
