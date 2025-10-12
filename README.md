# PixelPilot Mini RK

PixelPilot Mini RK is a lightweight receiver that ingests RTP video/audio over UDP and drives a KMS plane on Rockchip-based devices. The application wraps a GStreamer pipeline with DRM/KMS, OSD, and udev helpers.

## Configuration via INI

All command-line options can be provided in an INI file and loaded with `--config /path/to/file.ini`. The parser merges the INI
defaults first and then applies explicit CLI flags so ad-hoc overrides still work.

The `config/osd-sample.ini` file documents every supported key, including the available OSD text tokens, line-plot metrics, and a quick
reference for anchors, offsets, color syntax, and the built-in named palette.
Copy it next to the binary and launch the receiver as:

```sh
./pixelpilot_mini_rk --config config/osd-sample.ini --osd
```

Any `line =` entry inside a `[osd.element.*]` section can reference `{token}` placeholders from the sample file's token table.
Line plots accept metrics such as `udp.bitrate.latest_mbps`, `udp.jitter.avg_ms`, or counter-style values like
`udp.lost_packets` and automatically handle scaling and rendering based on the INI-provided geometry.

### INI key reference

The table below summarises every INI option that the loader understands. Values map 1:1 to command-line switches and fall back
to the defaults listed in `src/config.c` when omitted.

| Section / key | Description |
| --- | --- |
| `[drm].card` | DRM card node to open (default `/dev/dri/card0`). |
| `[drm].connector` | Preferred connector name (e.g. `HDMI-A-1`). Leave blank to auto-pick the first connected head. |
| `[drm].video-plane-id` | Numeric plane ID used for the decoded video plane. |
| `[drm].use-udev` | `true` to enable the hotplug listener that reapplies modes when connectors change. |
| `[drm].osd-plane-id` | Optional explicit plane for the OSD overlay (0 keeps the auto-selection). |
| `[udp].port` | UDP port that the RTP stream arrives on. |
| `[udp].video-pt` / `[udp].audio-pt` | Payload types for the video (default 97/H.265) and audio (default 98/Opus) streams. |
| `[pipeline].appsink-max-buffers` | Maximum number of buffers queued on the appsink before older frames are dropped. Exposed via the OSD token `{pipeline.appsink_max_buffers}`. |
| `[pipeline].custom-sink` | `receiver` to use the custom UDP receiver, or `udpsrc` for the bare GStreamer `udpsrc` pipeline. |
| `[pipeline].pt97-filter` | `true` (default) keeps the RTP payload-type filter on `udpsrc`; set `false` to accept all payload types when CPU headroom is limited. |
| `[idr].enable` | `true` enables the automatic IDR requester that fires HTTP recovery bursts when decode warnings appear. |
| `[idr].port` | HTTP port that exposes the IDR trigger endpoint (default `80`). |
| `[idr].path` | HTTP path used when issuing the inline request (default `/request/idr`). |
| `[idr].timeout-ms` | TCP timeout applied to each IDR request in milliseconds (default `200`). |
| `[splash].enable` | `true` enables the idle fallback player that loops local H.265 sequences when the UDP stream is idle. |
| `[splash].input` | Path to an Annex-B H.265 elementary stream with intra-only frames that the splash player should loop. |
| `[splash].fps` | Frame rate used when timestamping splash buffers. |
| `[splash].idle-timeout-ms` | Milliseconds of inactivity on the video stream before the splash source takes over. |
| `[splash].default-sequence` | Optional name of the `[splash.sequence.*]` block that should start looping once idle. |
| `[splash.sequence.NAME].start` / `end` | Inclusive frame range describing a named splash sequence that can be queued. |
| `[audio].device` | ALSA device string handed to the sink (e.g. `plughw:CARD=rockchiphdmi0,DEV=0`). |
| `[audio].disable` | `true` drops the audio branch entirely (equivalent to `--no-audio`). |
| `[audio].optional` | `true` allows auto-fallback to a fakesink when the audio path fails; `false` keeps retrying the real sink. |
| `[record].enable` | `true` to persist the H.265 video elementary stream to MP4 via minimp4. |
| `[record].path` | Optional output path or directory for the MP4 file. If omitted, files land in `/media` with a timestamped name (video only, no audio). |
| `[record].mode` | Selects the minimp4 writer mode: `standard` (seekable, updates MP4 metadata at the end), `sequential` (append-only, avoids seeks), or `fragmented` (stream-friendly MP4 fragments). |
| `[restarts].limit` | Maximum automatic restarts allowed within the configured window. |
| `[restarts].window-ms` | Rolling window (milliseconds) for counting automatic restarts. |
| `[gst].log` | `true` forces `GST_DEBUG=3` unless already set in the environment. |
| `[cpu].affinity` | Comma-separated CPU IDs used to pin the main process and helper threads. |
| `[osd].enable` | Enable the OSD overlay plane. |
| `[osd].refresh-ms` | Interval between OSD refreshes. |
| `[osd].plane-id` | Optional override for the OSD plane (mirrors `[drm].osd-plane-id`). |
| `[osd].elements` | Comma-separated list describing the render order of `[osd.element.NAME]` blocks. |
| `[osd.element.NAME].type` | Widget style (`text`, `line`, or `bar`). Each type unlocks additional keys listed in the sample file. |
| `[osd.element.NAME].anchor` / `offset` / `size` / color keys | Control placement and styling for OSD widgets. See inline comments in the sample file for full semantics. |
| `[osd.element.NAME].line` | For text widgets, each `line =` entry appends a formatted row supporting `{token}` placeholders. |
| `[osd.element.NAME].metric` | For line/bar widgets, selects the metric token (e.g. `udp.bitrate.latest_mbps`) sampled each refresh. |

## CPU affinity control

Use `--cpu-list` to provide a comma-separated list of CPU IDs that the process and its busy worker threads should run on. The main process mask is restricted to the specified CPUs and the UDP receiver and GStreamer bus threads are pinned in a round-robin fashion to spread the work across cores.

Example:

```sh
./pixelpilot_mini_rk --udp-port 5600 --cpu-list 2,3
```

This constrains the process to CPUs 2 and 3 while placing the UDP receiver thread on CPU 2 and the GStreamer bus thread on CPU 3.

## Bypassing the custom UDP receiver

The default pipeline feeds RTP packets into an `appsrc` element backed by the project-specific UDP receiver. This provides
extended telemetry (bitrate, jitter, packet counters, etc.) that powers the OSD widgets and log output. When you do not need
those metrics, switch the sink with `--custom-sink udpsrc` (or `pipeline.custom-sink = udpsrc` in the INI file). The pipeline
will then create a bare `udpsrc` element and UEP/receiver statistics are disabled entirely.

Use this mode when integrating with external tooling or experimenting with alternative buffering strategies where the
application-level receiver is unnecessary. Revert with `--custom-sink receiver` (or remove the INI override) to restore the
default behaviour and regain access to the telemetry counters. Keep in mind that the automatic IDR requester also depends on
the custom receiver: it learns the sender's IP/port from incoming UDP packets and reuses that information when issuing HTTP
burst requests. The bare `udpsrc` pipeline never surfaces that metadata, so even if `[idr].enable = true` is set in a minimal
configuration the recovery logic stays inactive.

## UDP receiver statistics

The custom receiver keeps a rolling telemetry snapshot that is exposed through the OSD token table and the `udp.*` metric
namespace. Statistics are computed only while the feature is enabled (for example when the OSD requests them); toggling stats on
resets the counters and history buffer so each session starts with a clean slate.

Once the RTP payload types have been separated, only the video stream (payload type matching `[udp].video-pt`) feeds the
aggregate counters. Audio packets still increment `udp.audio_packets` so you can confirm the sender is active, but they no longer
impact bitrate, jitter, history samples, or frame statistics. Audio from the same SSRC nudges the internal sequence tracker forward
when it is only a small step ahead of the video stream so that interleaved payload type 98 traffic does not look like loss; audio
arriving on a different SSRC (or with a large sequence gap) is ignored entirely for the sequence heuristic.

| Counter | Description |
| --- | --- |
| `udp.total_packets` | Video RTP packets forwarded to the decoder (payload type matching `[udp].video-pt`). |
| `udp.video_packets` / `udp.audio_packets` | Video packets mirror the total; audio counts tick when `[udp].audio-pt` is observed, even if the audio branch is disabled. |
| `udp.ignored_packets` | Packets whose payload type did not match either configured stream. |
| `udp.duplicate_packets` | Packets that re-used the most recent sequence number. |
| `udp.lost_packets` | The cumulative count of missing sequence numbers detected while walking the video stream. |
| `udp.reordered_packets` | Packets that arrived with a sequence number lower than expected (but not a duplicate). |
| `udp.total_bytes` / `udp.video_bytes` / `udp.audio_bytes` | Video byte counters mirror the packet stats; audio bytes reflect `[udp].audio-pt` traffic only. |
| `udp.bitrate.latest_mbps` | Instantaneous video bitrate computed over a sliding 200 ms window. |
| `udp.bitrate.avg_mbps` | Exponentially weighted moving average of the instantaneous video bitrate. |
| `udp.jitter.latest_ms` | RFC 3550 style inter-arrival jitter derived from the video timestamps. |
| `udp.jitter.avg_ms` | EWMA of the video jitter metric to smooth short-term spikes. |
| `udp.frame.count` | Number of completed video frames detected via RTP marker bits. |
| `udp.frame.incomplete` | Frames that ended with missing video packets. |
| `udp.frame.last_kib` | Size of the most recent completed video frame (KiB). |
| `udp.frame.avg_kib` | EWMA of recent video frame sizes (KiB). |
| `udp.sequence.expected` | The next video sequence number the receiver is waiting for. |
| `udp.timestamp.last_video` | RTP timestamp from the most recent video packet. |
| `udp.idr_requests` | Total HTTP IDR requests issued while attempting to recover corrupted streams. |

The history buffer exposed through `udp.history.*` tokens retains the 512 most recent packet samples, including packet size,
payload type, arrival timestamp, and flags for loss, reordering, duplication, and frame boundaries. This makes it possible to
build custom diagnostics or render per-packet overlays directly from the INI configuration.

### Streaming stats over Server-Sent Events

Enable the lightweight SSE endpoint with `--sse-enable` (or `[sse].enable = true` in the INI file) to expose the cumulative UDP
receiver counters without running the on-screen display. The streamer listens on `127.0.0.1:8080` by default and can be tuned
via:

```
--sse-bind 0.0.0.0          # address to bind
--sse-port 9090             # TCP port for HTTP clients
--sse-interval-ms 1000      # emission interval in milliseconds
```

Each HTTP client that performs `GET /stats` receives a `text/event-stream` response. Payloads are emitted at the configured
interval and include all counters listed above together with recording telemetry:

```
event: stats
data: {"have_stats":true,"total_packets":1234,"video_packets":1234,
       "audio_packets":0,"ignored_packets":0,"duplicate_packets":0,
       "lost_packets":2,"reordered_packets":1,"total_mbytes":9.42,
       "video_mbytes":9.42,"audio_mbytes":0.00,"frame_count":45,
       "incomplete_frames":0,"last_frame_kib":112.5,"avg_frame_kib":108.3,
       "bitrate_mbps":12.340,"bitrate_avg_mbps":10.876,"jitter_ms":3.25,
       "jitter_avg_ms":2.97,"expected_sequence":54321,
       "idr_requests":7,"recording_enabled":true,
       "recording_active":false,"recording_duration_s":12.5,
       "recording_media_s":10.0,"recording_mbytes":30.00,
       "recording_path":"/media/pixelpilot-20240101-120000.mp4"}
```

When the receiver has not yet produced statistics, the streamer reports `{"have_stats":false}` so clients can ignore placeholder
updates. Byte-oriented counters in the SSE payload are pre-scaled to truncated megabytes for quick inspection.

## Automatic IDR recovery

Packet loss or corruption around an IDR frame leaves the decoder without valid reference data until a fresh I-frame arrives. When enabled, PixelPilot watches the video decoder warnings and immediately issues an HTTP GET burst (defaulting to `http://SOURCE:80/request/idr`) to request a new IDR. The requester fires one request instantly, three more spaced 50 ms apart, then falls back to an exponential interval capped at 500 ms while warnings persist. Each attempt uses a 200 ms TCP timeout so misbehaving endpoints do not clog the worker queue.

Tune the behaviour through `[idr]` in the INI (or the matching `--idr-*` CLI flags): disable it entirely with `idr.enable = false`, override the port or path to match the camera firmware, or extend the timeout when proxies or long RTT links sit between the devices. Every trigger is logged with the cumulative total, and the `udp.idr_requests` counter exposes the same total in OSD templates, SSE payloads, and other telemetry sinks.

When 64 consecutive HTTP bursts fail to clear the decoder warnings, the requester now gives up on further IDR spam and tells the main loop to rebuild the entire pipeline. This mirrors a manual restart: the pipeline tears down the UDP receiver, decoder, and sinks before bringing them back up with the existing configuration. The strategy avoids endless HTTP loops when the camera ignores triggers or the stream never delivers a usable key frame.

Manual restarts follow the same path. Send the process a `SIGHUP` (for example `kill -HUP $(cat /tmp/pixel_pilot_rk.pid)`) to force an immediate teardown/restart cycle without dropping other runtime toggles such as audio fallbacks or active OSD overlays.

## Splash fallback playback

When the primary UDP stream drops out it is often desirable to display a "waiting" slate rather than leaving the screen idle.
Set `[splash].enable = true` to arm the bundled splash player. The player loops H.265 frame ranges from a local Annex-B
elementary stream and feeds them through the existing decode path whenever the configured `[splash].idle-timeout-ms`
threshold elapses without new video packets. As soon as RTP traffic resumes the selector switches back to the live feed.

Splash sequences are defined in `[splash.sequence.NAME]` blocks and reference inclusive start/end frame numbers inside the
input file. Multiple sequences can be chained to create a simple playlist. The example below loops two segments while the
receiver is idle:

```ini
[splash]
enable = true
input = /opt/pixelpilot/splash/standby.h265
fps = 30.0
idle-timeout-ms = 2000
default-sequence = intro

[splash.sequence.intro]
start = 0
end = 179

[splash.sequence.loop]
start = 180
end = 359
```

Provide an H.265 stream with repeated key frames (all-I-frame content works best). Each sequence should align with GOP
boundaries to avoid decoding artifacts. Disable the feature by omitting the `[splash]` section or setting `enable = false`.
