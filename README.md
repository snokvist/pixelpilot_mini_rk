# PixelPilot Mini RK

PixelPilot Mini RK is a lightweight receiver that ingests RTP video/audio over UDP and drives a KMS plane on Rockchip-based devices. The application wraps a GStreamer pipeline with DRM/KMS, OSD, and udev helpers.

## CPU affinity control

Use `--cpu-list` to provide a comma-separated list of CPU IDs that the process and its busy worker threads should run on. The main process mask is restricted to the specified CPUs and the UDP receiver and GStreamer bus threads are pinned in a round-robin fashion to spread the work across cores.

Example:

```sh
./pixelpilot_mini_rk --udp-port 5600 --cpu-list 2,3
```

This constrains the process to CPUs 2 and 3 while placing the UDP receiver thread on CPU 2 and the GStreamer bus thread on CPU 3.
