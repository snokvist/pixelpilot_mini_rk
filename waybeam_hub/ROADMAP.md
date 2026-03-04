# Waybeam Hub Sync Roadmap

## Context: Where This Fits in the Waybeam Ecosystem

The Waybeam system has two sides connected by a shared WiFi medium:

```
GROUND SIDE                          AIR SIDE
─────────────────────────────        ─────────────────────────────────
Rockchip RK3566                      SigmaStar Infinity6E
  joystick2crsf (RC out)               ip2uart (UART↔UDP bridge)
  pixelpilot_mini_rk (decode)          infinity6e-pwm (CRSF→servo)
  waybeam_hub.py (menu/OSD)            waybeam_osd (LVGL overlay)
                                       waybeam_hub.c (menu/OSD/WebUI)
— OR —
Android phone
  Waybeam-android (decode+RC)

ESP32-C3 (primarily ground side, optionally air side)
  head-tracker PPM→CRSF bridge (ground: HDZero BoxPro+ input)
  camera UART debugger (air: SigmaStar serial access)
  can also serve as air-side PWM/servo controller
```

### What Crosses the WiFi Link Today

| Flow | Transport | Size | Rate | Budget |
|---|---|---|---|---|
| H.265 video | RTP UDP:5600 | Mbps | Continuous | ~90% of link |
| CRSF RC commands | UDP:14550 | 26 B/frame | ~50 Hz | ~1.2 KB/s |
| Telemetry (FC→ground) | UDP:14550 | Variable | Event-driven | ~1-5 KB/s |
| OSD menu commands | UDP:7777 | ≤1280 B | Event-driven | <1 KB/s |

**WiFi medium constraint — airtime, not bytes.** The cost of a WiFi packet
is dominated by **per-packet overhead**, not payload size. Every UDP datagram
— no matter how small — consumes a full transmit slot:

```
DIFS wait → Backoff → Preamble → PHY header → MAC header → Payload → FCS → SIFS → ACK
```

At low MCS rates (MCS0, or legacy 802.11b/g used for long-range FPV links),
a single 26-byte CRSF frame costs roughly the same airtime as a ~200-byte
packet because the fixed overhead dominates:

| Component | Duration (MCS0, 20 MHz) |
|---|---|
| DIFS + avg backoff | ~100-200 µs |
| Preamble + PHY header | ~40 µs (HT) or ~192 µs (legacy) |
| MAC header (36 B) + IP/UDP (28 B) + payload | ~75 µs at 6.5 Mbps |
| SIFS + ACK | ~50 µs |
| **Total per packet** | **~265-520 µs** |

At 50 Hz, CRSF RC alone burns **~13-26 ms/s of airtime** — that's 1.3-2.6%
of the medium gone just for 1.2 KB/s of actual data. Each additional small
UDP packet (sync hello, OSD command) costs the same slot overhead.

**CSMA/CA collision amplification.** The cost is worse than linear. WiFi uses
CSMA/CA (Carrier Sense Multiple Access with Collision Avoidance) — all
stations contend for the same medium. As airtime utilization increases:

- **Contention window grows exponentially.** After a failed transmit (no ACK
  received), the sender doubles its backoff window (CWmin → CWmax, typically
  15 → 1023 slot times). A single collision can cost 5-10 ms of idle backoff
  before the next attempt.
- **Collisions cascade.** Two stations transmitting simultaneously both back
  off and retry, increasing the probability of further collisions with other
  traffic. At >60-70% medium utilization, collision rates rise sharply.
- **Retransmissions consume extra airtime.** Each retry is a full slot
  (preamble + headers + payload + ACK wait). The MAC layer retries up to
  7 times (short retry limit) before dropping the frame. A single collision
  on a 26-byte RC packet can burn >2 ms of airtime across retries.
- **Latency tail grows.** Video and RC packets share the contention window.
  Added sync traffic doesn't just cost its own airtime — it increases the
  probability that video or RC packets collide and retry, adding jitter
  to the latency-critical streams.

On a saturated long-range FPV link (MCS0/1, single spatial stream), even
a few extra packets per second can measurably degrade video smoothness and
RC latency. This is why **packet count is the primary budget**, not throughput.

**Design implication:** Minimize **packet count**, not just byte count.
Piggyback sync data onto existing packets where possible. Avoid adding new
periodic senders. Target **<500 bytes/s steady state** and critically
**<1 extra packet/s** when idle. Never add periodic traffic that scales
with time rather than user interaction.

### What Does NOT Cross the WiFi Link (Except WebUI)

All SSE endpoints are **127.0.0.1 bound** (localhost only):
- `joystick2crsf` SSE:8070 (ground) — consumed by waybeam_hub.py locally
- `infinity6e-pwm` SSE:8070 (vehicle) — consumed by waybeam_hub.c locally
- `pixelpilot` SSE:8080 (ground) — stats for local WebUI only

**WebUI HTTP servers (port 8060) are intentionally network-accessible**
(`0.0.0.0` bind) so that any device on the subnet — including the Android
app's WebView — can access `/state` and `/command`. Traffic is minimal:
small JSON responses on user-initiated requests only.

### Device Capabilities Relevant to Sync

| Device | Has waybeam_hub | Has WebUI | Has Action Backend | Has OSD Out | Has SSE |
|---|---|---|---|---|---|
| Rockchip (py) | Yes | Yes (full) | Yes (shell actions) | Yes (UDP:5005+7777) | Consumer |
| SigmaStar (c) | Yes | Yes (light) | Yes (shell actions) | Yes (UDP:7777) | Consumer |
| Android | No | WebView client | No | No | No |
| ESP32 | No | Own WiFi AP | No | No | No |

**Android participates via the WebUI HTTP API.** The Android app already:
1. **Auto-detects the VTX host IP** from RTP source packets (every 5 seconds
   in `RtpReceiver.kt`, field `detectedSourceIp`)
2. **Has a WebView browser overlay** that loads `http://<vtx_host>:<port>`
   with JavaScript enabled, DOM storage, and SSL tolerance
3. **Resolves the VTX host** via `getResolvedVtxHost()` — auto-detected IP
   with fallback to manual config (`vtx_host_manual`, default `10.6.0.60`)
4. **Configurable port** via `browser_port` preference (default 80)

Since waybeam_hub.c binds its WebUI on `0.0.0.0:8060` with full CORS headers,
Android can load `http://<detected_vtx_ip>:8060` in its existing WebView to
get full access to vehicle menu control, CRSF channel display, and remote
actions — **with zero new protocol code in the Android app**. The only
change needed is pointing the browser at port 8060 instead of (or in addition
to) port 80.

Similarly, if the ground station Rockchip IP is known, Android can access
`http://<ground_ip>:8060` for the Python hub's WebUI.

This makes the **WebUI HTTP API (`/state` + `/command`) the universal
access layer** — any device with a browser or HTTP client can participate.

**ESP32 runs its own WiFi AP** (10.100.0.1) on a separate network. Not a sync
participant.

**Practical scope for the UDP sync protocol:** Sync is between
**waybeam_hub.py (ground)** and **waybeam_hub.c (vehicle)** — two instances
that both manage menus, assets, and actions but currently operate independently.
Android joins as a **WebUI client** of either hub, not as a sync peer.

---

## What Actually Needs Synchronizing

### Ground → Vehicle

| Data | Why | Current Path | Desired |
|---|---|---|---|
| Menu OSD text | Display menu on vehicle OSD | UDP:7777 (already works) | Keep as-is |
| Asset enable/disable | Toggle OSD elements | UDP:7777 asset_updates | Keep as-is |
| Remote action trigger | Run shell cmd on vehicle | **None** | New: command channel |
| Config push | Change vehicle settings | **None** | New: config sync |
| Zoom/gamma | Video processing params | UDP:5005 (ground-local) | Not needed cross-link |

### Vehicle → Ground

| Data | Why | Current Path | Desired |
|---|---|---|---|
| Vehicle menu state | Show in ground WebUI | **None** | New: state push |
| Action result/status | Show command output | **None** | New: status push |
| Link/failsafe state | Ground knows vehicle health | **None** | New: health push |
| CRSF channel echo | Verify RC reaching vehicle | SSE:8070 (localhost) | Could expose |

### Android → Vehicle / Ground (via WebUI)

Android doesn't need a sync protocol. It accesses hub state through HTTP:

| Data | How | Path |
|---|---|---|
| Vehicle menu/status | WebView loads vehicle hub | `http://<vtx_ip>:8060/state` |
| Vehicle actions | POST from WebView | `http://<vtx_ip>:8060/command` |
| Ground menu/status | WebView loads ground hub | `http://<ground_ip>:8060/state` |
| Ground actions | POST from WebView | `http://<ground_ip>:8060/command` |
| CRSF channels | Included in `/state` JSON | `crsf.channels[]` in response |

The VTX IP is already auto-detected from RTP. The ground station IP could be
discovered the same way (it's on the same subnet) or manually configured.

**Android changes needed:** Allow the user to set `browser_port` to 8060
(already a user preference), or add a "Waybeam Hub" browser target alongside
the existing VTX browser. No Kotlin code changes required for basic access.

---

## Design Principles

1. **Minimize WiFi traffic.** Video owns the medium. Sync must be event-driven,
   not polled. No periodic heartbeats faster than 0.2 Hz.

2. **Extend existing protocols, don't invent new ones.** The OSD JSON protocol
   (UDP, ≤1280 bytes, connectionless) already crosses the link. Add sync
   messages as a new message type within it rather than opening new ports.

3. **Leverage the action backend.** Both hubs already have shell command
   execution with timeout, output capture, and status reporting. Remote
   action triggers should route through this existing pipeline.

4. **Unicast only.** Multicast support on embedded Linux (OpenIPC/SigmaStar)
   is unreliable and adds IGMP complexity. Use direct unicast to known peer.

5. **Tolerate packet loss.** UDP is lossy. State pushes must be idempotent
   and self-describing. A missed delta is corrected by the next full state
   push, not by retransmission.

6. **No persistent connection.** No TCP, no SSE cross-link, no WebSocket.
   Pure UDP datagrams like the existing OSD protocol.

7. **Single peer model (v1).** One ground hub, one vehicle hub. No mesh,
   no leader election, no lease coordination. Keep it simple.

---

## Phase 1: Compatibility Baseline (current)

Normalize the shared vocabulary between py and c implementations:

1. **Source names:** `serial`, `joystick` (already aligned)
2. **Menu state shape:** `current_section`, `selected`, `menu_visible`, `status`
3. **Command verbs:** `menu`, `up`, `down`, `select`
4. **WebUI endpoints:** `GET /state`, `POST /command` (already aligned)
5. **Config keys:** Document which config.json keys both runtimes support
6. **CRSF state in /state JSON:** `crsf.link_up`, `crsf.channels[16]`,
   `crsf.nav_direction`, `crsf.select_pressed`, `crsf.combo_active` (done)

Parity checklist on every release:
- Config key coverage
- HTTP endpoint coverage
- Radio-rule trigger semantics
- OSD payload format (texts[], asset_updates[])

---

## Phase 2: Sync Protocol

### Transport

- **UDP unicast** on the existing WiFi link
- **Port:** Reuse OSD port (5005 ground, 7777 vehicle) — sync messages are
  distinguished by a `"sync"` top-level key in the JSON
- **Max datagram:** 1280 bytes (same as OSD protocol)
- **Encoding:** UTF-8 JSON (matches OSD protocol)
- **Peer address:** Configured in config.json as `sync_peer` (e.g., `"10.6.0.1"`)
  or auto-detected from incoming OSD packets' source IP

### Message Envelope

Every sync message has this shape:

```json
{
  "sync": {
    "type": "hello|state|command|result",
    "seq": 42,
    "from": "vehicle",
    "ts": 1234567890
  },
  ...type-specific fields...
}
```

The `"sync"` key distinguishes sync messages from regular OSD payloads
(which have `"texts"`, `"values"`, `"asset_updates"`, etc.). OSD consumers
that don't understand sync simply ignore the unknown key.

### Message Types

#### 1. `hello` — Presence Announcement

Sent once on startup and then every 5 seconds (0.2 Hz) as a low-rate
heartbeat. This is the only periodic message.

```json
{
  "sync": {"type": "hello", "seq": 1, "from": "vehicle", "ts": 1234567890},
  "runtime": "c",
  "role": "vehicle",
  "version": 1,
  "capabilities": ["actions", "menu", "osd", "webui"],
  "actions_available": ["reboot", "record", "restart_wfb"]
}
```

~200 bytes every 5 seconds = **40 bytes/s average**.

The `actions_available` list tells the peer which action names can be
triggered remotely, derived from the loaded menu.ini sections.

#### 2. `state` — Full or Delta State Push

Sent **on change only** (menu navigation, asset toggle, action completion,
link status change). Not periodic.

```json
{
  "sync": {"type": "state", "seq": 12, "from": "vehicle", "ts": 1234567890},
  "menu_visible": true,
  "current_section": "SYSTEM",
  "selected": 2,
  "selected_label": "reboot",
  "status": "OK [SYSTEM] record: Recording started",
  "link_up": true,
  "active_source": "serial",
  "asset_enabled": [true, true, null, null, null, null, null, false]
}
```

~300 bytes, only on user interaction = **negligible steady state**.

Fields are optional — only include what changed. Receiver merges into
its cached peer state.

#### 3. `command` — Remote Action Trigger

Ground hub tells vehicle hub to execute a named action from its menu.ini.

```json
{
  "sync": {"type": "command", "seq": 5, "from": "ground", "ts": 1234567890},
  "action": "record",
  "section": "PIXELPILOT"
}
```

~150 bytes, only on user click. The receiving hub looks up the action by
`(section, name)` in its loaded actions and routes it through the existing
`action_launch()` pipeline — same path as a local menu selection or radio
rule trigger. This means:

- Same timeout enforcement (action_timeout_ms)
- Same output capture (stdout first line)
- Same one-at-a-time serialization (reject if action already running)
- Same status reporting (feeds into the next `state` push)

**Security:** Only named actions from the loaded menu.ini are executable.
No raw shell commands over the sync channel. The action allowlist is
the menu.ini file itself.

#### 4. `result` — Action Completion Report

Sent after a remotely-triggered action completes (or times out).

```json
{
  "sync": {"type": "result", "seq": 13, "from": "vehicle", "ts": 1234567890},
  "action": "record",
  "section": "PIXELPILOT",
  "exit_code": 0,
  "output": "Recording started",
  "duration_ms": 120
}
```

~200 bytes, only after action completes.

### Traffic Budget

| Message | Size | Frequency | Bytes/s | Packets/s | Airtime (MCS0) |
|---|---|---|---|---|---|
| hello | ~200 B | Every 5s | 40 B/s | 0.2 pkt/s | ~100 µs/s |
| state | ~300 B | On change | ~0 idle | ~0 idle | ~0 idle |
| command | ~150 B | User click | ~0 idle | ~0 idle | ~0 idle |
| result | ~200 B | After cmd | ~0 idle | ~0 idle | ~0 idle |
| **Total idle** | | | **40 B/s** | **0.2 pkt/s** | **~100 µs/s** |

During active menu interaction (rapid up/down): ~300 B × 5 Hz = 1.5 KB/s,
5 packets/s, ~2.5 ms/s airtime — still negligible vs the ~50 pkt/s RC stream.

**Piggybacking opportunity (v2):** If the OSD JSON sender already sends a
UDP packet to the vehicle (port 7777), a sync state delta could be included
in the same datagram (under the 1280-byte limit) instead of sending a
separate packet. This would add zero extra airtime for state updates that
coincide with OSD sends.

### Peer Discovery

**v1: Manual configuration.** Add `sync_peer` to config.json:

```json
{
  "sync_peer": "10.6.0.1",
  "sync_port": 7777
}
```

Ground hub sends sync messages to `sync_peer:7777` (vehicle OSD port).
Vehicle hub sends sync messages to the source IP of received OSD packets
on port 5005 (ground OSD port), or to a configured `sync_peer`.

**v2 (future):** Auto-detect peer from incoming traffic. The vehicle already
receives OSD JSON from the ground on port 7777 — the source IP of those
packets identifies the ground station. Similarly, the ground receives RTP
video — the source IP identifies the vehicle.

### Reliability

- **Sequence numbers** per sender detect gaps (log warning, no retransmit)
- **Idempotent state pushes** — receiving the same seq twice is a no-op
- **Stale peer detection** — if no hello received for 15 seconds, mark peer
  offline and log. No automatic reconnect needed (UDP is connectionless)
- **No ACKs for state** — eventual consistency via next push
- **ACK for commands** — the `result` message serves as implicit ACK.
  If no result within action_timeout_ms + 1s, ground shows timeout error

---

## Phase 3: Implementation Plan

### Step 1: Protocol Document

Write `protocols/hub-sync-udp.md` in the coordination repo as the canonical
spec (same pattern as crsf-rc.md, osd-control-udp.md).

### Step 2: C Implementation (waybeam_hub.c)

Add to the existing poll-based main loop:

1. **Sync receive:** Check OSD UDP socket for incoming `"sync"` messages
   during the existing `recvfrom()` path (same socket, same port)
2. **Sync send:** New `sync_send_*()` functions that build JSON and
   `sendto()` the configured peer address
3. **Hello timer:** 5-second interval in main loop (trivial — already has
   monotonic clock)
4. **State push:** Hook into existing `dirty` flag — when OSD payload is
   sent, also send a `state` sync message to peer
5. **Command receive:** Parse `"sync":{"type":"command"}`, look up action
   by section+name, call existing `action_launch()`
6. **Result push:** Hook into existing `action_poll()` completion path

Estimated additions: ~200 lines of C (JSON build + parse for 4 message types).

### Step 3: Python Implementation (waybeam_hub.py)

Mirror the same logic:

1. **Sync receive:** In the existing UDP socket poll loop, detect `"sync"` key
2. **Sync send:** Build and send sync messages to peer
3. **Remote action display:** Show vehicle action results in ground WebUI
4. **Remote command UI:** Add "Vehicle Actions" section to WebUI that lists
   actions from the peer's last `hello` message and sends `command` on click

### Step 4: WebUI Integration

Extend both HTML UIs to show peer state:

- **Peer status card:** Online/offline, last seen, role, runtime
- **Peer menu state:** Current section, selected item, status message
- **Remote actions panel:** Buttons for each action in peer's allowlist
- **Action result display:** Exit code, output, duration

---

## Phase 4: Verification

1. **Unit fixtures:** Golden JSON messages (hello/state/command/result)
   validated by both C and Python parsers
2. **Loopback test:** Run both hubs on same machine, sync via localhost
3. **Cross-link test:** Ground Rockchip ↔ Vehicle SigmaStar over WiFi
   - Verify state propagates within 1 OSD update cycle
   - Verify remote action triggers and result display
   - Verify hello timeout detection on link loss
4. **Traffic measurement:** tcpdump during idle + active use, confirm
   sync overhead <500 B/s idle, <2 KB/s burst
5. **Packet loss test:** Use `tc netem` to simulate 10% loss, verify
   state converges within 2-3 update cycles

---

## Future Considerations (v2+)

- **Binary encoding:** If JSON overhead matters on very constrained links,
  switch to a fixed-format binary envelope (4-byte header + fields).
  Current analysis suggests JSON is fine for v1.
- **Multi-peer:** If a second ground station (e.g., observer/instructor)
  is added, extend to peer list. Still unicast, just to multiple targets.
- **Config sync:** Push config.json fragments (e.g., new radio rules) from
  ground to vehicle. Requires careful versioning and atomic apply.
- **Android native integration:** Beyond WebView access, the Android app
  could add a native Kotlin HTTP client that polls `/state` and posts to
  `/command`, enabling tighter UI integration (e.g., showing vehicle
  status in the HUD overlay, triggering actions from on-screen buttons
  without opening the browser). This would be a small addition since the
  HTTP API is already JSON and the app already has the VTX IP.
- **Multi-target browser:** Add a target selector to the Android WebView
  overlay so the user can switch between VTX browser (port 80), vehicle
  hub (port 8060), and ground hub (port 8060 on ground IP). Could be a
  simple dropdown or swipe gesture.
- **Streaming telemetry:** Forward FC telemetry (battery, GPS, attitude)
  from vehicle hub to ground hub via sync channel, so ground WebUI can
  display vehicle health. This would replace the need for a cross-link
  SSE endpoint.

---

## Resolved Questions (from original draft)

| Question | Resolution |
|---|---|
| Multicast availability? | **No.** Unreliable on embedded. Use unicast. |
| Centralized vs peer-elected lease? | **Neither.** Single-peer model, no leases in v1. |
| Binary encoding for constrained links? | **Not in v1.** JSON overhead is ~40 B/s. Revisit if needed. |
| How does Android fit? | **Via WebUI HTTP API.** Auto-detected VTX IP + existing WebView = full access to vehicle hub. No new protocol code needed. |
| How does ESP32 fit? | **It doesn't.** Separate WiFi AP, different network. |
