# External data feed for OSD widgets

This note sketches an approach for letting third-party processes inject text and
numeric values into the on-screen display without having to recompile the
receiver. The goal is to expose up to eight text rows and eight floating-point
metrics that downstream OSD widgets can render or plot through the existing
`{token}` placeholder system.

## Transport and lifecycle

* Introduce a small helper module (for example `src/osd_bridge.c`) that owns a
  UDP socket. The bind address and port are supplied via INI/CLI options such as
  `[osd.external].port = 5005` so the feature can be disabled by default.
* The helper runs on a lightweight thread that blocks in `recvfrom()` and parses
  newline-delimited JSON payloads (any JSON library already linked in the
  project can be reused; otherwise a tiny hand-written parser that only handles
  the expected schema is enough).
* Each message simply replaces the current snapshot. Store the decoded payload
  in a struct containing:
  ```c
  typedef struct {
      char text[8][64];
      double value[8];
      uint64_t last_update_ns;
  } OsdExternalFeed;
  ```
* Guard the snapshot with a `pthread_mutex_t`; the existing OSD refresh loop can
  lock, copy the data into its render context, and unlock. Because refreshes are
  already periodic (see `osd_refresh_ms` in the config), this adds no extra
  wakeups when nobody is publishing.

### Configuration

Enable the listener either by passing `--osd-external --osd-external-udp-port 5005`
on the command line or by adding the following to the INI file:

```ini
[osd.external]
enable = true
port = 5005
```

Leaving the port blank keeps the helper disabled so existing deployments are not
affected until they opt in.

## Message format

External producers only need to send fields they care about. A minimal update
looks like:

```json
{"text":["HELLO","WORLD"], "value":[12.5, 0.75]}
```

If fewer than eight entries are present, the remaining slots keep their prior
contents. Including an empty array clears previously published data. Optional
`ttl_ms` lets publishers request automatic expiry so stale telemetry does not
linger. The receiver tracks TTL per text/value slot so independent publishers
can multiplex the feed without clobbering each other:

```json
{"value":[42.0], "ttl_ms": 5000}
```

When the helper thread observes that `clock_gettime(CLOCK_MONOTONIC)` exceeds
the stored expiry for a slot, it clears only that entry before the next OSD
refresh. While a slot has an active TTL, updates that omit `ttl_ms` leave the
existing value untouched so time-limited overlays stay visible for their full
duration.

### Zoom/crop control channel

* Text slot 8 (`ext.text8`) is reserved for live zoom instructions that affect the
  primary video plane. A well-formed command uses the prefix `zoom=` followed by
  either the literal `off` or four comma-separated integers that describe the
  crop **as percentages** of the current decoded frame:

  ```text
  zoom=SCALE_X,SCALE_Y,CENTER_X,CENTER_Y
  ```

  * `SCALE_X` / `SCALE_Y` request the window size as a percentage of the decoded
    frame. `100,100` shows the full frame; `50,50` crops to half the width and
    height. Values below 1 are rejected. Values above 100 keep sampling the full
    frame but shrink the plane on screen, centring it with black borders. For
    example `zoom=150,150,50,50` renders the full frame at roughly two thirds of
    the output size. When the axes differ, the larger percentage (smaller
    shrink factor) wins so the picture stays proportional.
  * `CENTER_X` / `CENTER_Y` position the window by expressing its centre as a
    percentage of the frame. `50,50` keeps the crop centred, while `0,100` anchors
    it to the bottom-left. Positions above 100 are clamped toward the edges.

* The decoder clamps both the requested scale and the resulting window so the
  plane never samples outside the decoded frame. If the sender places the crop
  partially off-screen (for example `50,50,100,100`), the window is nudged back
  inside the frame before programming the plane. Any rejection or clamp is logged
  once when the command is applied.
* The DRM plane can only upscale by a factor of four in either direction, so the
  receiver enforces a minimum crop size to stay within that limit. Requests that
  would require more magnification are automatically widened or heightened to the
  smallest allowed size and a log entry records the adjustment. After that clamp,
  the crop is rounded to the chroma-aligned sizes that the Rockchip plane
  accepts: widths and heights snap to four-pixel increments and the origin lands
  on an even pixel. Expect the applied percentages in the log to differ slightly
  from what was requested. On a 1080p output fed with a 1280×720 stream, this
  means percentages much below `25,38` cannot be honoured.
* Commands are debounced: the receiver only reprograms the plane when the text
  changes. Publish the same string again only when refreshing its TTL.
* Include `ttl_ms` with every zoom update so the request naturally expires when
  the publisher stops sending data. A typical one-second zoom command that crops
  to half size around the centre looks like:

  ```json
  {"text":["","","","","","","","zoom=50,50,50,50"], "ttl_ms": 1000}
  ```

* Clearing the slot (empty string) or publishing `zoom=off` restores the full
  frame.

## Token exposure

Extend `osd_token_format()` so that strings become available through
`{ext.text1}` … `{ext.text8}` and numbers through `{ext.value1}` …
`{ext.value8}`. The existing renderer already uppercases ASCII characters, so we
match that convention. Numbers also double as plot metrics by teaching
`osd_metric_sample()` to recognize keys like `ext.value3` and returning the
cached `double`.

Because the renderer falls back to printing the literal token when a key is
unknown, configuration files created on older builds keep working: users only
see `{ext.text1}` until they update the binary to a release containing the
bridge.

### Animated outline widget

The OSD includes an `outline` element type that renders an alpha-blended frame
around the screen. The widget listens to any metric (for example
`ext.value1`) and animates whenever the sampled value crosses a configured
threshold. A representative INI snippet looks like:

```ini
[osd.element.signal_outline]
type = outline
metric = ext.value1
threshold = 30
trigger = below
color = 0x90FF4500
base-thickness = 8
pulse-period = 48
pulse-amplitude = 4
pulse-step = 2
```

With the example above the outline begins pulsing when `ext.value1 < 30`. The
color accepts any ARGB value, so using a partially transparent colour ensures
the animation blends with the video underneath. `base-thickness` establishes
the baseline border width, `pulse-period` controls the full in/out pulse cycle
(higher numbers slow the animation), and `pulse-amplitude` defines how far the
border expands beyond the baseline. `pulse-step` advances the animation phase on
each refresh, so larger steps make the pulse travel faster. Setting
`inactive-color` keeps a static border visible even when the trigger is not met.

## Error handling and observability

* Socket creation failures should log a warning and keep the rest of the
  application alive, mirroring how optional features are treated elsewhere in
  the tree.
* Malformed JSON (missing arrays, wrong types, etc.) should trigger a single
  rate-limited log entry but leave the previous snapshot untouched.
* Publish the helper’s status via a new token (for example `{ext.status}` with
  values `disabled`, `listening`, or `error`) so users can surface diagnostics in
  the OSD itself.

## Testing ideas

1. Unit-test the parser with representative payloads, including boundary cases
   (arrays longer than eight entries, missing fields, TTL expiry).
2. Run the binary with the feature enabled and pipe updates manually:
   ```sh
   printf '%s' '{"text":["BATTERY 12.6V"]}' | socat - UDP-DATAGRAM:127.0.0.1:5005
   ```
   Confirm the OSD text widget renders the injected string and that a line plot
   bound to `ext.value1` responds to subsequent updates.
3. Verify that unplugging publishers does not leak descriptors or wake the OSD
   thread unnecessarily. Use `strace -p` to ensure only the helper is blocked on
   `recvmsg()`.
