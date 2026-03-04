# Custom MPP Decoder Deep Analysis (pixelpilot_mini_rk)

## Scope
This analysis focuses on the custom Rockchip MPP video path in this repository (`src/video_decoder.c` + pipeline feed path), compares it with OpenIPC PixelPilot_rk, and proposes concrete latency/reliability improvements.

## 1) Current decoder architecture in this repo

### Data path and thread model
- Compressed access units arrive through GStreamer (`appsink`) and are read from `appsink_thread_func`.
- Each sample is sent to `video_decoder_feed`, which writes into a reusable 1 MiB packet buffer and calls `decode_put_packet`.
- Decoded frames are polled in `frame_thread_func` via `decode_get_frame`.
- Display commits are decoupled in `display_thread_func`, where only the latest pending framebuffer ID is committed atomically to DRM.

### External DRM buffer group strategy
- On decoder info-change events, the code allocates an external DRM-backed MPP frame group (`MPP_BUFFER_TYPE_DRM`) and binds each committed MPP buffer to a DRM framebuffer (`drmModeAddFB2` NV12).
- This is a zero-copy display path (MPP -> DRM plane with PRIME FDs).

### Decoder configuration choices
The decoder enables settings tuned for low-latency recovery behavior:
- `base:split_parse = 1`
- `MPP_DEC_SET_DISABLE_ERROR = 0xffff`
- `MPP_DEC_SET_IMMEDIATE_OUT = 0xffff`
- `MPP_DEC_SET_ENABLE_FAST_PLAY = 0xffff`
- output wait is short (`MPP_SET_OUTPUT_TIMEOUT` around 5 ms, or short block timeout fallback)

### Error handling behavior today
- In `frame_thread_func`, any frame with `errinfo` OR `discard` is dropped early.
- On drop, IDR requester warning hook is triggered to recover stream state.
- Net effect: conservative visual integrity (no obviously corrupted frames), but can create visible stutter/black gaps under burst loss.

### Upstream queue behavior and end-to-end latency
- Appsink is configured with `drop=true`, small max-buffers (default 4).
- UDP queue uses leaky mode (`leaky=2`), limiting backlog buildup.
- These are good low-latency defaults, but can increase packet/sample loss during transient stalls.

## 2) Comparison with OpenIPC/PixelPilot_rk decoder

## Core similarities
- Same fundamental MPP external-buffer architecture (`READ_BUF_SIZE=1 MiB`, `MAX_FRAMES=24`, external DRM group, info-change realloc).
- Similar MPP tuning (`split_parse`, disable_error, immediate_out, fast_play).
- Similar dual-thread decode/display model.

## Key behavior difference: handling decoder-error-marked frames
In OpenIPC `src/main.cpp` frame loop:
- It inspects `errinfo`/`discard` and may trigger IDR requests.
- But it does **not** unconditionally skip posting those frames to display.
- It still maps the MPP buffer and commits the framebuffer, which allows “partial decode” presentation (possible block corruption/green artifacts) instead of immediate frame drop.

In this repo (`src/video_decoder.c`):
- `errinfo || discard` causes immediate drop and `continue`.
- That avoids artifacts but increases freeze risk when reference chain is damaged and keyframes are sparse.

## Reliability mechanism differences
OpenIPC additionally leans on:
- RTP gap-driven IDR triggers in `gstrtpreceiver.cpp`.
- Decoder-feed stall detection with repeated retry timeout and conditional IDR request.

This repo already has strong IDR infrastructure (`IdrRequester`, jitter/loss trigger options in config), but frame-level policy remains stricter (drop-on-any-error).

## 3) Partial decode evaluation

## Why partial decode can feel better in FPV
For FPV UX, a temporally continuous but locally corrupted frame is often preferable to a frozen frame because:
- pilot perception favors motion continuity,
- control loop confidence is better with “imperfect now” than “perfect old frame,”
- short corruption windows can self-heal after the next reference refresh.

## How OpenIPC effectively does it
Not via an explicit “partial decode mode” API, but by policy:
- decoder outputs frames,
- app logs error flags and requests IDR,
- frame is still displayed when a buffer exists.

This turns decoder error flags into a recovery signal instead of a hard display veto.

## How to implement safely in this repo
Introduce a configurable policy rather than replacing current behavior globally.

### Proposed policy modes
1. `strict`: drop any `errinfo || discard` frame.
2. `partial` (recommended default): present decoder-flagged frames (possible artifacts) while still triggering IDR recovery.

### Guardrails for reliability
- Keep IDR trigger on any decoder issue.
- Add rate limit to warning logs and IDR requests to avoid storms.
- Track `last_good_commit_ms`, `consecutive_decoder_issues`, and `time_since_last_present`.
- If partial mode enabled, prefer only one of:
  - display every error frame (max continuity), or
  - display at limited cadence (e.g., 15–30 fps cap) to reduce visual noise.

### Suggested implementation points
- `include/config.h` / `src/config.c`: add decoder error policy config enum and thresholds.
- `src/video_decoder.c` (`frame_thread_func`): replace unconditional drop branch with policy gate.
- Existing `IdrRequester` hook remains unchanged; call it in both dropped and displayed-error paths.
- Extend stats exposure for OSD/debug (`decoder.err_frames`, `decoder.dropped_frames`, `decoder.partial_displayed_frames`).

## 4) Latency improvement opportunities (prioritized)

1. Reduce `video_decoder_feed` retry sleep from fixed 2 ms to bounded backoff + immediate retry first.
   - Current fixed sleep can add avoidable jitter under short buffer-full bursts.
2. Consider reducing appsink max buffers from 4 -> 2 for ultra-low-latency presets.
   - Keep 4 as default if users report instability.
3. Add “latest-only” semantics between frame thread and display thread.
   - Already close to this (`pending_fb` overwrite); ensure old pending frame is always replaced atomically before commit.
4. Optional: thread priority tuning for decode and appsink feeder threads.
   - Keep bounded to avoid starving networking/OSD.

## 5) Reliability improvement opportunities (prioritized)

1. Add strict/partial decoder policy as a config option and make `partial` the default.
2. Add decode starvation detector in this repo mirroring OpenIPC’s feed-stall logic.
   - If no successful present for X ms while data continues, trigger IDR.
3. Add per-minute counters and moving-window metrics:
   - decode_put_packet busy retries,
   - errinfo/discard counts,
   - time-to-recover after IDR request.
4. Introduce optional periodic IDR keepalive in very lossy RF conditions (low rate, e.g., every 1–2 s, configurable/off by default).

## 6) Recommended rollout plan

Phase A (low risk):
- Add metrics + config plumbing + logging (no behavior change).

Phase B:
- Add strict/partial mode behind config + CLI flags.

Phase C:
- Add latency knobs (feed retry, idle sleep, output timeout) with conservative defaults.

Phase D:
- Tune defaults based on measured:
  - median glass-to-glass latency,
  - freeze duration percentiles,
  - IDR recovery time.

## 7) Bottom line
- This repo’s decoder is architecturally strong and already tuned for low-latency zero-copy display.
- The biggest difference from OpenIPC is **policy**, not core decode plumbing.
- To get OpenIPC-like resilience under loss, implement configurable strict/partial decode display of decoder-flagged frames while preserving aggressive IDR recovery.
