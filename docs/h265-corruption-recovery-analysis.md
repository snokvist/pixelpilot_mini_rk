# H.265 corruption/recovery behavior analysis (Windows/Linux/Rockchip)

## Executive findings (repo-specific)

1. **In this repository, H.265 decode error behavior is primarily governed by the Rockchip MPP decoder path, not by GStreamer software/hardware decode elements** (`avdec_h265`, `d3d11h265dec`, `nvh265dec`, `v4l2*dec`, etc.). The pipeline terminates at `appsink`, and compressed H.265 AU payloads are fed into the project’s own decoder (`video_decoder_feed` -> MPP).【F:src/pipeline.c†L317-L326】【F:src/pipeline.c†L669-L688】【F:src/video_decoder.c†L1689-L1716】
2. **Depayloading policy already biases toward "drop corrupted AU"**: `sstarh265depay` marks corruption on sequence gaps/FU issues and drops the AU unless `emit-partial-au=true`. Default is `false`.【F:src/h265_depay.c†L399-L408】【F:src/h265_depay.c†L226-L257】【F:src/h265_depay.c†L573-L582】【F:src/h265_depay.c†L618-L620】
3. **`sstarh265parse` is effectively passthrough and does not implement recovery/concealment logic**; it mostly normalizes caps negotiation (`byte-stream`, alignment pass-through).【F:src/h265_parse.c†L44-L73】【F:src/h265_parse.c†L91-L96】【F:src/h265_parse.c†L136-L151】
4. **Current receiver pipeline has no RTP jitterbuffer stage**, so loss/reorder handling is mostly from custom depay/statistics/decoder behavior rather than `rtpjitterbuffer` reordering/latency controls.【F:src/pipeline.c†L318-L326】【F:src/pipeline.c†L545-L548】

### Implemented enhancement plan (this branch)

- Added pipeline config control to explicitly choose damaged-AU forwarding: `[pipeline].depay-emit-partial-au`.
- Added decoder config control to choose drop-vs-keep behavior for frames flagged by MPP: `[pipeline].decoder-drop-error-frames`.
- Wired IDR warning triggers for `GST_BUFFER_FLAG_CORRUPTED` samples in the appsink pump so partial-frame mode still actively requests recovery.
- Added a custom RTP reorder stage (`sstarrtpjitterbuffer`) that can be enabled in both receiver and `udpsrc` paths using `[pipeline].jitterbuffer-*` settings.
- Kept low-latency guidance centered on one-frame buffering (`appsink-max-buffers=1`) and immediate IDR reaction.

---

## 1) Element(s) that govern corruption/recovery behavior

### Primary control (in this repo)
- **`video_decoder.c` (Rockchip MPP path)**:
  - Frames flagged by MPP with `errinfo`/`discard` are explicitly dropped.
  - Each drop triggers IDR requester warning flow.
  - Decoder is configured with split-parse/immediate-out/fast-play controls that materially affect recovery timing and what is emitted under damage.
  - Therefore this is the dominant determinant of green/garbled vs dropped/repeated output behavior on embedded RK builds.【F:src/video_decoder.c†L1306-L1329】【F:src/video_decoder.c†L1000-L1023】【F:src/video_decoder.c†L1540-L1560】

### Secondary controls (in this repo)
- **`sstarh265depay`**:
  - Detects RTP gaps / malformed FU/AP / timestamp rollover conditions and marks AU corrupted.
  - Default policy drops corrupted AU; optional policy emits partial AU with `CORRUPTED|DISCONT`.
  - This influences whether decoder receives broken access units at all.【F:src/h265_depay.c†L399-L408】【F:src/h265_depay.c†L152-L199】【F:src/h265_depay.c†L226-L285】
- **Queueing/appsink policy**:
  - Video queue is leaky downstream (`leaky=2`) with `max-size-buffers=16`, so backlog drops are possible under pressure.
  - `appsink` has `drop=true`, bounded buffers, `sync=false`; this minimizes latency but can increase sample loss under load.
  - These affect what reaches decoder in time but do not implement concealment themselves.【F:src/pipeline.c†L292-L316】
- **Custom UDP receiver + IDR requester**:
  - Tracks loss/jitter and can request IDR bursts on loss/jitter and decoder warnings.
  - This shortens corruption duration if transmitter honors IDR requests; otherwise corruption persists until natural keyframe.
  - In `udpsrc` mode, receiver stats/learning are bypassed, reducing this adaptive behavior.【F:src/udp_receiver.c†L438-L490】【F:src/udp_receiver.c†L504-L511】【F:src/pipeline.c†L911-L926】【F:README.md†L177-L189】

---

## 2) Decoder-specific vs parser/depay/jitter/queue/sink influence

## Decoder-specific (strongest)
- Cross-platform differences (stale frame repeat vs green/garbled) are generally decoder implementation details:
  - DPB/reference error handling
  - Concealment strategy for missing references
  - Whether damaged frames are output, dropped, or substituted
  - Resync aggressiveness after IDR/CRA
- In this repo’s runtime path, that behavior maps directly to **Rockchip MPP** decisions plus explicit drop-on-errinfo logic in `frame_thread_func`.【F:src/video_decoder.c†L1306-L1329】

## Parser/depay and transport shaping (secondary)
- `sstarh265depay` decides how much corruption reaches decoder (drop vs forward-partial).
- `sstarh265parse` currently does not add robust recovery semantics.
- No in-pipeline jitterbuffer currently means less controlled reorder/lateness handling.
- Queue/appsink parameters are latency/backpressure knobs, not codec concealment controls.【F:src/h265_depay.c†L573-L582】【F:src/h265_parse.c†L91-L96】【F:src/pipeline.c†L292-L316】

---

## 3) Relevant properties/flags in the actual pipeline

### Damaged frame output vs drop
- `sstarh265depay emit-partial-au` (default `false`):
  - `false` => drop corrupted AU before decoder.
  - `true` => forward damaged AU with `GST_BUFFER_FLAG_CORRUPTED|DISCONT`.
  - Current pipeline does not override it, so default drop behavior is active.【F:src/h265_depay.c†L573-L582】【F:src/h265_depay.c†L618-L620】【F:src/pipeline.c†L281-L326】
- MPP output filtering in frame thread:
  - `errinfo || discard` => frame dropped and IDR warning emitted.【F:src/video_decoder.c†L1318-L1329】

### Concealment/recovery strategy
- MPP controls set at init:
  - `MPP_DEC_SET_DISABLE_ERROR`
  - `MPP_DEC_SET_IMMEDIATE_OUT`
  - `MPP_DEC_SET_ENABLE_FAST_PLAY`
  - plus parser split mode.
- IDR requester triggers on decode warnings and UDP stats thresholds (loss/jitter), reducing time-to-recovery when source supports on-demand IDR.【F:src/video_decoder.c†L1000-L1023】【F:src/video_decoder.c†L1540-L1560】【F:src/udp_receiver.c†L438-L490】

### AU alignment / parameter-set handling
- Source caps force H.265 byte-stream AU alignment at depay output and appsink caps:
  - `video/x-h265, stream-format=byte-stream, alignment=au` in receiver mode.
  - `udpsrc` mode also enforces byte-stream and appsink alignment AU via caps.
- Parser preserves alignment field from incoming caps if present; defaults alignment to `au` when missing.【F:src/pipeline.c†L306-L313】【F:src/pipeline.c†L545-L548】【F:src/pipeline.c†L584-L587】【F:src/h265_parse.c†L59-L68】

### Missing-reference behavior
- Missing packets are detected at depay (sequence gap -> corrupted AU) and receiver stats.
- Missing-reference decode failures are surfaced by MPP as `errinfo` and dropped in frame thread.
- Recovery relies on keyframe arrival or successful IDR request path.【F:src/h265_depay.c†L399-L408】【F:src/video_decoder.c†L1318-L1329】【F:src/udp_receiver.c†L595-L601】【F:src/udp_receiver.c†L453-L458】

---

## 4) Platform matrix (actionable)

> Note: only embedded RK path is implemented in this repo. Windows/Linux desktop decoder rows below are guidance for comparison test benches.

| Platform | Decoder used | Typical loss/corruption behavior | Main knobs | Recommended “lowest visual corruption” | Recommended “lowest latency” |
|---|---|---|---|---|---|
| Embedded Linux (this repo) | **Rockchip MPP via `video_decoder.c`** | Corrupted references often dropped (`errinfo/discard`) and display can appear frozen/stale until IDR; garbled/green depends on MPP firmware/stream damage pattern | `sstarh265depay emit-partial-au`, MPP controls, IDR thresholds, queue/appsink depth | Keep `emit-partial-au=false`; keep IDR triggers enabled and aggressive (`loss-threshold=1`), ensure camera honors IDR endpoint | Keep leaky queue + small appsink buffers; optionally reduce buffering further but accept more drops |
| Windows (comparison bench) | Usually `d3d11h265dec` / vendor HW / fallback `avdec_h265` | Vendor decoders may repeat prior frame or output damaged surfaces until IDR/CRA | Decoder-specific error handling, `h265parse` config-interval/alignment, jitterbuffer latency/drop-on-late | Prefer dropping damaged AUs before decode, enforce AU alignment, periodic VPS/SPS/PPS insertion, moderate jitterbuffer | Minimal jitterbuffer latency, leaky queues, `sync=false`; accept higher visual artifacts |
| Linux desktop (comparison bench) | `va*`, `v4l2*`, `nvh265dec`, or `avdec_h265` | Behavior varies by backend; software decode often deterministic but slower; HW may conceal differently | Same as Windows + backend-specific properties | Use AU-aligned bytestream, drop corrupted AU upstream, periodic parameter-set insertion, request keyframe quickly | Short queues + low jitterbuffer latency + drop-on-late |

---

## 5) Proposed pipeline changes/snippets

## A. Harden current repo pipeline for lowest corruption duration

### Receiver mode (current architecture)
```c
/* after creating depay/parser/capsfilter/appsink */
g_object_set(depay,
             "payload-type", cfg->vid_pt,
             "emit-partial-au", FALSE,   // explicit: drop damaged AU
             NULL);

/* keep AU alignment enforced */
raw_caps = gst_caps_new_simple("video/x-h265",
                               "stream-format", G_TYPE_STRING, "byte-stream",
                               "alignment", G_TYPE_STRING, "au",
                               NULL);
```
(Explicitly documents current preferred behavior and avoids accidental partial-AU forwarding.)

## B. Optional latency/corruption tuning path (when using pure GStreamer decode benches)

### “Lowest visual corruption” bench pattern
```bash
gst-launch-1.0 -v udpsrc ... ! rtpjitterbuffer latency=80 drop-on-late=true ! \
  rtph265depay ! h265parse alignment=au config-interval=1 ! \
  avdec_h265 output-corrupt=false ! videoconvert ! autovideosink sync=false qos=false
```

### “Lowest latency” bench pattern
```bash
gst-launch-1.0 -v udpsrc ... ! rtpjitterbuffer latency=10 drop-on-late=true ! \
  rtph265depay ! h265parse alignment=au config-interval=-1 ! \
  <platform_hw_decoder> ! queue leaky=downstream max-size-buffers=4 ! \
  autovideosink sync=false qos=false
```

(Use platform-appropriate decoder substitution for matrix comparisons.)

---

## 6) Reproducible cross-platform test method

## Controlled impairment injection
1. **Network loss/reorder (Linux host):**
   - `tc qdisc add dev <iface> root netem loss 2% 25% delay 20ms 5ms reorder 10% 50%`
2. **Burst loss:**
   - `tc qdisc change dev <iface> root netem loss gemodel 1% 50% 90% 1%`
3. **Byte corruption/fuzz RTP payload:**
   - Insert a small UDP proxy mutating random bytes in H.265 NAL payload region at configured rate.

## Run matrix
- Keep the same sender stream (GOP, bitrate, packetization mode) across all receivers.
- Test at least:
  - clean baseline
  - 1% random loss
  - burst loss
  - corruption without loss
- Capture:
  - time from first corruption to first clean post-IDR frame
  - count of stale-frame intervals
  - count/duration of green/garbled output periods

## GStreamer/debug logging to collect
- `GST_DEBUG=2,*h265*:6,*rtp*:6,*jitterbuffer*:6,*basesink*:5`
- Repo-specific categories:
  - `sstarh265depay:6`
  - `sstarh265parse:6`
- App logs for:
  - `MPP: dropping frame errinfo=...`
  - `UDP receiver: ... requesting IDR`

## Pass/fail criteria
- **Pass (lowest corruption profile):**
  - no partial damaged AU rendered
  - recovery to clean output within target window (e.g. <= 1 GOP after loss)
  - no sustained (>500 ms) green/garbled output under 1% random loss
- **Pass (lowest latency profile):**
  - end-to-end latency target met
  - artifact bursts allowed but must self-recover within defined IDR window
- **Fail:**
  - repeated stale frame replay > target window
  - persistent garble until manual restart
  - IDR requester active but no keyframe recovery due to endpoint mismatch

---

## Root-cause conclusion

For this codebase, the dominant root cause of differing corruption display is **decoder implementation behavior** (Rockchip MPP in-repo, different decoder backends in Windows/Linux benches), while depay/parser/queues mostly decide whether damaged access units are forwarded, delayed, or dropped before decode. The most practical corruption-minimizing strategy is:

1. keep AU-aligned byte-stream,
2. drop corrupted AUs before decode (`emit-partial-au=false`),
3. trigger/validate fast IDR recovery path,
4. add/retune jitter buffering in comparison pipelines where reorder/lateness dominates.

This combination minimizes long green/garbled intervals and stale-reference artifacts without giving up full low-latency operation.
