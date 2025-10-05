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
default behaviour and regain access to the telemetry counters.

## UDP receiver statistics

The custom receiver keeps a rolling telemetry snapshot that is exposed through the OSD token table and the `udp.*` metric
namespace. Statistics are computed only while the feature is enabled (for example when the OSD requests them); toggling stats on
resets the counters and history buffer so each session starts with a clean slate.

| Counter | Description |
| --- | --- |
| `udp.total_packets` | All RTP packets observed, regardless of payload type. |
| `udp.video_packets` / `udp.audio_packets` | Packets that matched the configured video or audio payload types. Audio packets are still counted even when the audio pipeline is disabled so you can confirm that the sender is transmitting them. |
| `udp.ignored_packets` | Packets whose payload type did not match either configured stream. |
| `udp.duplicate_packets` | Packets that re-used the most recent sequence number. |
| `udp.lost_packets` | The cumulative count of missing sequence numbers detected while walking the video stream. |
| `udp.reordered_packets` | Packets that arrived with a sequence number lower than expected (but not a duplicate). |
| `udp.total_bytes` / `udp.video_bytes` / `udp.audio_bytes` | Byte counters that mirror the packet counters above. |
| `udp.bitrate.latest_mbps` | Instantaneous bitrate computed over a sliding 200 ms window. |
| `udp.bitrate.avg_mbps` | Exponentially weighted moving average of the instantaneous bitrate. |
| `udp.jitter.latest_ms` | RFC 3550 style inter-arrival jitter, reported in milliseconds. |
| `udp.jitter.avg_ms` | EWMA of the jitter metric to smooth short-term spikes. |
| `udp.frame.count` | Number of completed video frames detected via RTP marker bits. |
| `udp.frame.incomplete` | Frames that ended with missing packets. |
| `udp.frame.last_kib` | Size of the most recent completed frame (KiB). |
| `udp.frame.avg_kib` | EWMA of recent frame sizes (KiB). |
| `udp.sequence.expected` | The next video sequence number the receiver is waiting for. |
| `udp.timestamp.last_video` | RTP timestamp from the most recent video packet. |

The history buffer exposed through `udp.history.*` tokens retains the 512 most recent packet samples, including packet size,
payload type, arrival timestamp, and flags for loss, reordering, duplication, and frame boundaries. This makes it possible to
build custom diagnostics or render per-packet overlays directly from the INI configuration.
