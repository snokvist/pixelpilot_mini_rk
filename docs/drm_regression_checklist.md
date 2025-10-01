# DRM regression checklist

Use this list after touching the display, pipeline, or hotplug code to confirm the
receiver still adheres to the anti-judder contract established with the upstream
PixelPilot project.

## 1. Modeset verification

* Boot the receiver with a monitor attached and capture the startup logs.
  * Confirm the selected connector line reports the highest advertised refresh
    rate for that panel (for example `60 Hz` on most HDMI displays).
  * Verify the log mentions the auto-selected plane and whether an override was
    honoured. Unexpected fallback to a primary plane or a missing `NV12`-capable
    overlay is a red flag that needs investigation.
* Run `modetest -p -M rockchip` (or the matching DRM driver name) and check that
  the chosen plane shows as `ACTIVE` with the expected `CRTC` and format.

## 2. Plane ownership audit

* While the pipeline is running, capture a `modetest -p` snapshot.
  * Only the auto-selected video plane should have a framebuffer attached for
    the main stream; the OSD plane may show a separate ARGB buffer when enabled.
  * The primary plane should stay detached if `blank-primary` is enabled during
    modeset.
* Toggle the OSD on/off and confirm its atomic commits never touch the video
  plane. Any attempt should now log an explicit error.

## 3. QoS & buffering sanity

* Inspect the `kmssink` debug logs (`GST_DEBUG=kmssink:4`) to ensure the element
  is still running with `sync=false`, `qos=true`, leaky queues, and the 20 ms
  lateness budget.
* If you temporarily build with `ENABLE_PIPELINE_TUNING` for experiments, make
  sure the release binary is rebuilt without the flag before flight testing.

Document the results (mode, plane, queue settings) alongside the commit touching
the pipeline so future contributors can cross-check their changes against a
known-good baseline.
