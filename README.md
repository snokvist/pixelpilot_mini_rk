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
those metrics, switch the sink with `--custom-sink udpsrc` (or `pipeline.custom-sink = udpsrc` in the INI file). The pipeline
will then create a bare `udpsrc` element and UEP/receiver statistics are disabled entirely.

Use this mode when integrating with external tooling or experimenting with alternative buffering strategies where the
application-level receiver is unnecessary. Revert with `--custom-sink receiver` (or remove the INI override) to restore the
default behaviour and regain access to the telemetry counters.

## OSD rendering with pixman

The on-screen display now renders through libpixman so drawing code can lean on the library's optimized compositing routines
instead of per-pixel loops. Each frame buffer is wrapped in a cached `pixman_image_t`, rectangles are filled through
`pixman_image_fill_rectangles`, and glyph alpha masks are cached per scale so repeated text draws only composite the stored
mask back into the frame buffer. This approach provides a few benefits even if it costs a small amount of CPU time compared
with the previous handcrafted routines:

* **Maintenance & flexibility** – Using pixman consolidates color conversion, clipping, and blending logic into a proven
  library. That unlocks easier future enhancements such as anti-aliased fonts or rotated widgets without rewriting the whole
  renderer.
* **Correct alpha handling** – The compositor helpers apply premultiplied-alpha rules for every draw call, eliminating the
  rounding bugs and translucent artifacts that were difficult to avoid in the manual loops.
* **Glyph reuse** – The glyph cache means we only rasterize each character once per scale, so dynamic overlays avoid the worst
  case cost of re-building glyph masks every frame.

If CPU usage increases slightly, the most likely causes are the general-purpose blending steps that pixman performs for every
glyph composite and the creation of a temporary solid fill image for each text draw. When profiling shows these costs matter,
consider caching solid-color images per palette entry or pre-compositing frequently reused text lines into their own pixman
surfaces so updates reuse the prepared image instead of re-issuing individual glyph composites every frame.

## Image widgets (static or animated)

The pixman-backed renderer supports `type = image` widgets so overlays can mix sprites with text and plots. Each widget accepts a
single file path, a comma-separated list of frame paths, or a printf-style pattern combined with `frame-count`. Every frame must
be provided as a binary PPM (P6) with 8-bit color channels; the loader converts the pixels into premultiplied A8R8G8B8 surfaces
before compositing them onto the framebuffer.

Image widgets reuse the standard placement keys (`anchor`, `offset`) plus visuals such as `padding`, `background`, and `border`.
Animations add `frame-duration-ms` and `loop` controls, while single images simply omit those knobs (or set `loop = false`). The
sample INI demonstrates both styles:

```ini
[osd.element.pilot_logo]
type = image
anchor = top-right
offset = -12,12
source = assets/osd/pixelpilot_logo.ppm
padding = 4
background = transparent-grey
border = transparent-white
loop = false

[osd.element.pilot_anim]
type = image
anchor = mid-right
offset = -160,-64
source = assets/osd/pixelpilot_%02d.ppm
frame-count = 12
frame-duration-ms = 120
padding = 6
background = transparent-grey
border = transparent-white
loop = true
```

Provide the sample logo sprite and animation frames yourself. For the static overlay, add a 128×128 binary PPM such as
`assets/osd/pixelpilot_logo.ppm`. For the animation, drop numbered PPM files (for example,
`assets/osd/pixelpilot_00.ppm` through `assets/osd/pixelpilot_11.ppm`) into `assets/osd`. Adjust the paths, `frame-count`, and
durations as needed for your own artwork.
