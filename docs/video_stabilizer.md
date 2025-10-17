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
keys.

Typical command line enablement looks like:

```sh
./pixelpilot_mini_rk --stabilizer-enable --stabilizer-strength 1.15 \
    --stabilizer-max-translation 24 --stabilizer-max-rotation 3
```

For INI-driven deployments copy `config/stabilizer-demo.ini` and tweak the
translation strength/clamps to suit the expected motion profile before passing
it to `--config`.

## Testing

Run `make test` to build and execute `tests/video_stabilizer_test`. The smoke
test validates basic API coverage on all platforms and attempts a real DMA-BUF
round-trip when both librga and a DRM device are present.
