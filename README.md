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

## Display plane selection & QoS defaults

At start-up the receiver enables universal planes, picks the highest refresh mode advertised by the connected connector, and
auto-selects a DRM plane that can present NV12 video on the active CRTC. The detection logic prefers overlay planes, falls back
to primaries if necessary, and only accepts cursor planes when an explicit `--plane-id` override is provided. The selected plane
is logged and stored in the hotplug state so subsequent pipeline restarts reuse the hardware slot without guesswork.

To keep the display smooth the video branch always drives `kmssink` with `sync=false`, `qos=true`, downstream-leaky queues, and
the existing 20 ms lateness budget. These values are intentionally hard-coded to match the anti-judder behaviour of the upstream
PixelPilot project. If you need to experiment with alternative buffering strategies, rebuild with
`make CFLAGS+=-DENABLE_PIPELINE_TUNING` to re-enable the legacy INI/CLI knobs for queue sizes and `kmssink` flags. Remember to
restore the defaults before flight use to avoid reintroducing jitter.

## Bypassing the custom UDP receiver

The default pipeline feeds RTP packets into an `appsrc` element backed by the project-specific UDP receiver. This provides
extended telemetry (bitrate, jitter, packet counters, etc.) that powers the OSD widgets and log output. When you do not need
those metrics, enable GStreamer's native source with `--gst-udpsrc` (or `pipeline.use-gst-udpsrc = true` in the INI file). The
pipeline will then create a bare `udpsrc` element and UEP/receiver statistics are disabled entirely.

Use this mode when integrating with external tooling or experimenting with alternative buffering strategies where the
application-level receiver is unnecessary. Revert with `--no-gst-udpsrc` or by clearing the INI key to restore the default
behaviour and regain access to the telemetry counters.
