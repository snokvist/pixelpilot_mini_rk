# Picture-in-picture (PiP) on a second DRM plane (YUV420_8BIT)

This note captures what is required to add a second, independent video stream (PiP) that:

- listens on a second UDP port,
- reuses the same MPP-based decoder path as the main stream,
- targets a configurable DRM plane (for example plane `96`), and
- prefers `YUV420_8BIT` where possible.

## Key finding from current implementation

The current decoder path (`src/video_decoder.c`) allocates external DRM dumb buffers and registers framebuffers as `DRM_FORMAT_NV12` only. Plane selection is also hard-coded to look for linear `NV12` support.

That means there is no generic "convert to YUV420_8BIT" stage today. The path is zero-copy NV12-oriented.

## Why plane 96 is special on your dump

On the provided `drm_info`, plane `96` advertises `YUV420_8BIT`, but via `IN_FORMATS` entries tied to `ARM_AFBC` modifiers (compressed surfaces), not plain linear buffers.

By contrast, the current code uses `drmModeAddFB2(..., flags = 0)` with linear dumb buffers. So simply switching fourcc to `YUV420_8BIT` is not sufficient:

1. framebuffer creation must carry AFBC modifiers (`drmModeAddFB2WithModifiers`),
2. backing buffers must be allocated in a way compatible with AFBC layout,
3. decoder output format/modifier must match what the plane accepts.

## Recommended implementation strategy

1. **Add a second pipeline instance (`PipelineState`)**
   - `main`: existing stream (`udp.port`)
   - `pip`: second stream (`pip.udp-port`)

2. **Add PiP configuration keys** (INI + CLI)
   - `pip.enable` (bool)
   - `pip.udp-port` (int)
   - `pip.video-plane-id` (int, default `96`)
   - `pip.x`, `pip.y`, `pip.width`, `pip.height`
   - optional `pip.format` (`auto|nv12|yuv420_8bit`)

3. **Refactor video decoder plane selection to be format-aware**
   - replace NV12-only probes with reusable helpers:
     - `plane_accepts_format_with_modifiers(...)`
     - `video_decoder_select_plane_for_format(...)`

4. **Keep decoder path shared, instantiated twice**
   - no architectural fork: same `VideoDecoder` API, two instances, different config/UDP source.
   - each instance has independent `IdrRequester`, restart state, and stats labels.

5. **Initial safe mode: PiP on NV12-capable plane**
   - quickest route: enable PiP first with `NV12` on any plane that supports linear NV12.
   - this gives feature value immediately and avoids AFBC complexities.

6. **Optional advanced mode: YUV420_8BIT + AFBC**
   - add modifier-aware FB import and atomic commit path.
   - only enable when both decoder buffers and selected plane expose a matching AFBC modifier set.

## Do we need conversion to YUV420_8BIT?

Usually **not** as a software conversion step in this architecture.

- If PiP runs in linear NV12 mode, continue using current zero-copy decode → plane flow.
- If PiP must use plane 96 in `YUV420_8BIT`, this is primarily a **buffer layout/modifier compatibility** problem (AFBC), not a CPU colorspace-conversion problem.

If hardware/driver constraints force format conversion, then an explicit conversion stage (RGA/GStreamer) may be needed, but that should be a fallback due to extra latency/CPU/GPU cost.


## Current implementation status

- PiP now supports strict requested-plane selection so it cannot silently steal the main NV12 plane.
- PiP format configuration plumbing is in place (`auto`/`nv12`/`yuv420_8bit`).
- `yuv420_8bit` path is partially wired (format/plane/modifier selection), while AFBC/modifier-backed framebuffer correctness remains under implementation; `auto` currently falls back to `nv12` when needed.

## Practical rollout plan

- **Phase 1:** dual UDP + dual decoder + configurable PiP rectangle using NV12-compatible plane.
- **Phase 2:** add modifier negotiation and AFBC-capable path for plane 96/YUV420_8BIT.
- **Phase 3:** auto-fallback logic (`requested format/plane -> best supported pair`) with clear logs.
