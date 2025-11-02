# External data feed for OSD widgets

This note sketches an approach for letting third-party processes inject text and
numeric values into the on-screen display without having to recompile the
receiver. The goal is to expose up to eight text rows and eight floating-point
metrics that downstream OSD widgets can render or plot through the existing
`{token}` placeholder system.

## Transport and lifecycle

* Introduce a small helper module (for example `src/osd_bridge.c`) that owns a
  UNIX domain datagram socket. The socket path is supplied via an INI/CLI option
  such as `[osd.external].socket = /run/pixelpilot/osd.sock` so the feature can
  be disabled by default.
* The helper runs on a lightweight thread that blocks in `recvmsg()` and parses
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

Enable the listener either by passing `--osd-external-socket /run/pixelpilot/osd.sock`
on the command line or by adding the following to the INI file:

```ini
[osd.external]
enable = true
socket = /run/pixelpilot/osd.sock
```

Leaving the path blank keeps the helper disabled so existing deployments are not
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
linger:

```json
{"value":[42.0], "ttl_ms": 5000}
```

When the helper thread observes that `clock_gettime(CLOCK_MONOTONIC)` exceeds
`last_update_ns + ttl_ms`, it zeroes the snapshot before the next OSD refresh.

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
   socat - UNIX-SENDTO:/run/pixelpilot/osd.sock <<<'{"text":["BATTERY 12.6V"]}'
   ```
   Confirm the OSD text widget renders the injected string and that a line plot
   bound to `ext.value1` responds to subsequent updates.
3. Verify that unplugging publishers does not leak descriptors or wake the OSD
   thread unnecessarily. Use `strace -p` to ensure only the helper is blocked on
   `recvmsg()`.
