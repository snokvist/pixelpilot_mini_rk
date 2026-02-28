# Waybeam Hub

Waybeam Hub is a remote menu driver and WebUI companion for PixelPilot Mini RK. It consumes decoded CRSF channel values from an SSE stream (serial and joystick sources), renders a navigable on-screen menu via the external OSD UDP protocol, and serves a browser-based control panel over HTTP.

## Features

- **Radio/joystick menu navigation** — interpret CRSF channels with configurable debounce, deadband, and priority between serial and joystick sources.
- **Hierarchical OSD menu** — three-slot sliding window (prev / current / next) displayed through `ext.text6`–`ext.text8` on the PixelPilot OSD overlay.
- **Asset visibility control** — toggle any of the 8 OSD asset slots on or off, individually or all at once.
- **Zoom control** — step-based zoom in/out with configurable step size and maximum percentage.
- **Shell actions from INI** — execute arbitrary shell commands (reboot, record, gamma presets, etc.) defined in a simple INI file.
- **Radio rule triggers** — fire commands automatically when a CRSF channel value enters a configured range, with debounce and latch logic to prevent accidental repeats.
- **WebUI** — dark-themed browser dashboard with real-time state display, OSD text/value overrides, asset toggles, zoom, destination management, and debug recording/playback.
- **Multi-destination UDP** — send OSD payloads to multiple PixelPilot instances simultaneously.

## Quick start

```sh
# Run with defaults (connects to 127.0.0.1:5005, WebUI on port 8060)
python3 waybeam_hub.py

# Custom target and WebUI port
python3 waybeam_hub.py --host 192.168.1.100 --port 5005 --webui-port 8080

# Open the WebUI in a browser
# http://<device-ip>:8060
```

No external Python dependencies are required — the script uses only the standard library.

## CLI options

| Flag | Default | Description |
| --- | --- | --- |
| `--host` | `127.0.0.1` | PixelPilot external OSD host |
| `--port` | `5005` | External OSD UDP port |
| `--interval-ms` | `400` | OSD payload re-send interval (ms) |
| `--extra-destination` | `10.6.0.50:7777` | Additional `host:port` UDP targets (repeatable) |
| `--initial-off` | *(none)* | Comma-separated asset IDs to start disabled (e.g. `2,5,7`) |
| `--menu-asset-id` | `7` | Asset ID of the menu widget (force-disabled on exit) |
| `--zoom-step` | `25` | Zoom percentage increment |
| `--zoom-max` | `300` | Maximum zoom percentage |
| `--actions-ini` | `menu.ini` | Path to INI file with menu actions and radio rules |
| `--action-timeout-ms` | `5000` | Shell command timeout (ms) |
| `--action-shell` | `$SHELL` / `/bin/sh` | Shell used to execute action commands |
| `--webui-host` | `0.0.0.0` | WebUI HTTP bind address |
| `--webui-port` | `8060` | WebUI HTTP port |
| `--sse-url` | `http://127.0.0.1:8070/sse` | SSE endpoint providing CRSF channel data |
| `--priority` | `serial` | Preferred input source (`serial` or `joystick`) |
| `--priority-fallback-s` | `5.0` | Seconds before falling back to the non-priority source |
| `--verbose` | off | Enable verbose SSE and debug logging |

## Menu INI format

The actions INI file defines menu sections and radio trigger rules. Each section becomes a submenu; keys are action labels and values are shell commands.

```ini
[SYSTEM]
reboot = reboot
halt = halt

[PIXELPILOT]
record = kill -SIGUSR2 $(pidof pixelpilot_mini_rk)

[GAMMA]
milos1 = gamma milos1
reset = gamma reset

[RADIO]
; Trigger format: MIN<chN<MAX = command
1200<ch1<1500 = echo "CH1 mid range"
1600<ch10<2000 = echo "ARM high"
```

The built-in `[ASSETS]` and `[ZOOM]` sections are always present in the menu and do not need to be declared in the INI file. The `[RADIO]` section defines condition-based triggers that fire independently of menu navigation.

## Menu navigation (radio/joystick)

Navigation uses centered CRSF channel values:

| Input | Channel | Threshold | Debounce |
| --- | --- | --- | --- |
| Up / Down | CH2 (pitch) | Deadband 120 from center | 100 ms |
| Select | CH1 (roll) high | >= 1400 | 250 ms |
| Menu toggle | CH1-3 low + CH4 high | Hold >= 1.0 s | — |

The menu auto-hides after 30 seconds of inactivity.

## WebUI API

The WebUI serves `index.html` at the root and exposes a JSON command endpoint:

| Endpoint | Method | Description |
| --- | --- | --- |
| `/` | GET | WebUI HTML page |
| `/state` | GET | Current menu state as JSON |
| `/command` | POST | Send a JSON command |

### Command examples

```json
{"key": "up"}
{"key": "down"}
{"key": "select"}
{"key": "show_menu"}
{"key": "hide_menu"}
{"key": "all_on"}
{"key": "all_off"}
{"selected": 2}
{"asset_updates": [{"id": 0, "enabled": true}]}
{"texts": ["Line 1", null, null, null, null, null, null, null]}
{"values": [50.5, null, null, null, null, null, null, null]}
{"zoom": "150,150,50,50"}
{"zoom": "off"}
{"destinations": [{"host": "10.6.0.50", "port": 7777}]}
```

## OSD text slot layout

Waybeam Hub reserves the last three text slots for menu display:

| Slot | Token | Usage |
| --- | --- | --- |
| 1–5 | `{ext.text1}`–`{ext.text5}` | Available for custom overrides |
| 6 | `{ext.text6}` | Menu: previous item |
| 7 | `{ext.text7}` | Menu: current item (highlighted with `>`) |
| 8 | `{ext.text8}` | Menu: next item |

Configure a PixelPilot OSD text element with these three lines to display the menu:

```ini
[osd.element.menu]
type = text
id = 7
enabled = false
line = {ext.text6}
line = {ext.text7}
line = {ext.text8}
```

## Files

| File | Description |
| --- | --- |
| `waybeam_hub.py` | Main application — SSE client, menu engine, WebUI server, UDP sender |
| `index.html` | Browser-based control panel (served by the WebUI) |
| `menu.ini` | Default menu actions and radio rule definitions |
