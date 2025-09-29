# PixelPilot Mini RK

PixelPilot Mini RK is a lightweight receiver that ingests RTP video/audio over UDP and drives a KMS plane on Rockchip-based devices. The application wraps a GStreamer pipeline with DRM/KMS, OSD, and udev helpers.

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
* `--video-drop-on-latency` / `--no-video-drop-on-latency` toggles the RTP jitter buffer's `drop-on-latency` behaviour. Disabling
  the drops is useful when chasing decoder warnings so every frame is delivered, even if late.

Keep the defaults for normal flying where latency is paramount. Switch to non-leaky queues and higher buffer counts only for
short-term debugging sessions, as doing so can quickly introduce additional end-to-end delay.
