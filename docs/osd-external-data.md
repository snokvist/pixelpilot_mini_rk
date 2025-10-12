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
