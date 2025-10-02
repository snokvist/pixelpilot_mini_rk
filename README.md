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

## Video buffering controls

When debugging decoder underruns or validating new transmitters it can be useful to temporarily increase buffering or disable
latency-based drops in the video path. The following options expose the queue sizing and jitter buffer policy that are normally
hard-coded for low-latency flight use:

* `--video-queue-leaky MODE` sets the `leaky` mode on the pre-, post-, and sink-side video queues. The default `2` (downstream)
  favors minimal latency by discarding the oldest buffers when the downstream stage stalls. Set `0` to disable dropping so the
  pipeline accumulates backlog for analysis.
* `--video-queue-pre-buffers N`, `--video-queue-post-buffers N`, and `--video-queue-sink-buffers N` adjust the queue depths for
  each stage (defaults: 96/8/8). Raising these values increases tolerance for jitter at the cost of additional latency and
  memory use.
Keep the defaults for normal flying where latency is paramount. Switch to non-leaky queues and higher buffer counts only for
short-term debugging sessions, as doing so can quickly introduce additional end-to-end delay.

## Bypassing the custom UDP receiver

The default pipeline feeds RTP packets into an `appsrc` element backed by the project-specific UDP receiver. This provides
extended telemetry (bitrate, jitter, packet counters, etc.) that powers the OSD widgets and log output. When you do not need
those metrics, enable GStreamer's native source with `--gst-udpsrc` (or `pipeline.use-gst-udpsrc = true` in the INI file). The
pipeline will then create a bare `udpsrc` element and UEP/receiver statistics are disabled entirely.

Use this mode when integrating with external tooling or experimenting with alternative buffering strategies where the
application-level receiver is unnecessary. Revert with `--no-gst-udpsrc` or by clearing the INI key to restore the default
behaviour and regain access to the telemetry counters.

## OSD rendering acceleration

The on-screen display paths rely on libpixman for essentially all runtime blits so that the hot loops stay out of C. During
`osd_setup` the firmware builds a glyph cache and a framebuffer wrapper so that text drawing reduces to
`pixman_image_composite32` calls, while clears, filled rectangles, and the thickened segments used by the line renderer are
emitted through `pixman_fill`. The CPU still decides the geometry (e.g. Bresenham stepping for diagonals), but the actual
pixel writes are handled by pixman. These APIs automatically pick the best available implementation at runtime, which includes
the hand-written ARM NEON fast paths shipped with upstream pixman when the library is built with NEON support. No additional
compiler flags are required in this repository; simply linking against pixman allows the OSD to take advantage of NEON, SSE2,
or other SIMD back ends that pixman exposes on the target CPU.
