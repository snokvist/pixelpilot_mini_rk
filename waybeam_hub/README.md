# Waybeam Hub

Waybeam Hub is a remote menu driver and WebUI companion for PixelPilot Mini RK. It consumes decoded CRSF channel values from an SSE stream (serial and joystick sources), renders a navigable on-screen menu via the external OSD UDP protocol, and serves a browser-based control panel over HTTP.

## Features

- **Radio/joystick menu navigation** — interpret CRSF channels with configurable debounce, deadband, and priority between serial and joystick sources.
- **Hierarchical OSD menu** — three-slot sliding window (prev / current / next) displayed through `ext.text6`–`ext.text8` on the PixelPilot OSD overlay.
- **Asset visibility control** — toggle any of the 8 OSD asset slots on or off, individually or all at once.
- **Zoom control** — step-based zoom in/out with configurable step size and maximum percentage.
- **Shell actions from INI** — execute arbitrary shell commands (reboot, record, gamma presets, etc.) defined in a simple INI file.
- **Radio rule triggers** — fire commands automatically when a CRSF channel value enters a configured range, with debounce and latch logic to prevent accidental repeats.
- **WebUI** — dark-themed browser dashboard with real-time state display, OSD text/value overrides, asset toggles, zoom, live gamma LUT sliders, destination management, and debug recording/playback.
- **Multi-destination UDP** — send OSD payloads to multiple PixelPilot instances simultaneously.
- **JSON config file** — all settings (including tuning constants) are read from a single JSON file; no CLI flags required.

## Quick start

```sh
# Run with the bundled config.json (connects to 127.0.0.1:5005, WebUI on port 8060)
python3 waybeam_hub.py

# Use a custom config file
python3 waybeam_hub.py --config /path/to/my_config.json

# Open the WebUI in a browser
# http://<device-ip>:8060
```

No external Python dependencies are required — the script uses only the standard library.

## Configuration

All settings are read from a JSON config file. The only CLI argument is `--config` (defaults to `config.json` beside the script).

### Main settings

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `host` | string | `"127.0.0.1"` | PixelPilot external OSD host |
| `port` | int | `5005` | External OSD UDP port |
| `interval_ms` | int | `400` | OSD payload re-send interval (ms) |
| `asset_folder` | string | `""` | Directory containing bundled assets such as `index.html` and fallback `menu.ini`. Empty means "next to waybeam_hub.py". Relative paths are resolved from the JSON config file location. The bundled sample `config.json` sets this to `"/etc/waybeam_hub"` for deployment. |
| `extra_destinations` | array | `["10.6.0.50:7777"]` | Additional `"host:port"` UDP targets |
| `initial_off` | array | `[]` | Asset IDs to start disabled (e.g. `[2, 5, 7]`) |
| `menu_asset_id` | int | `7` | Asset ID of the menu widget (force-disabled on exit) |
| `zoom_step` | int | `25` | Zoom percentage increment |
| `zoom_max` | int | `300` | Maximum zoom percentage |
| `actions_ini` | string | `""` | Path to INI file with menu actions (defaults to bundled `menu.ini`) |
| `action_timeout_ms` | int | `5000` | Shell command timeout (ms) |
| `action_shell` | string | `""` | Shell for actions (`$SHELL` fallback `/bin/sh`) |
| `webui_host` | string | `"0.0.0.0"` | WebUI HTTP bind address |
| `webui_port` | int | `8060` | WebUI HTTP port |
| `sse_url` | string | `"http://127.0.0.1:8070/sse"` | SSE endpoint providing CRSF channel data |
| `priority` | string | `"serial"` | Preferred input source (`"serial"` or `"joystick"`) |
| `priority_fallback_s` | float | `5.0` | Seconds before falling back to the non-priority source |
| `verbose` | bool | `false` | Enable verbose SSE and debug logging |

### Tuning settings

The optional `tuning` object exposes internal constants for fine-tuning radio/joystick behaviour and menu timing. All keys have sensible defaults — omit the entire section to use them.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `crsf_axis_deadband` | int | `120` | Deadband around CRSF center for up/down navigation |
| `crsf_action_threshold` | int | `1400` | Channel value threshold for select action |
| `crsf_menu_toggle_ch_low_max` | int | `500` | Max channel value for "low" in menu-toggle combo |
| `crsf_menu_toggle_ch4_min` | int | `1500` | Min CH4 value for menu-toggle combo |
| `crsf_nav_debounce_ms` | int | `100` | Navigation key debounce (ms) |
| `crsf_select_debounce_ms` | int | `250` | Select key debounce (ms) |
| `crsf_menu_toggle_hold_s` | float | `1.0` | Hold time to toggle menu visibility (s) |
| `crsf_sample_interval_ms` | int | `40` | Minimum interval between SSE sample processing (ms) |
| `radio_trigger_debounce_ms` | int | `100` | Radio rule trigger debounce (ms) |
| `radio_reset_debounce_ms` | int | `0` | Radio rule re-arm debounce (ms) |
| `menu_inactivity_timeout_s` | float | `30.0` | Auto-hide menu after this many seconds of inactivity |
| `source_stale_s` | float | `1.0` | Mark input source as stale after this many seconds |

### Example config.json

```json
{
  "host": "127.0.0.1",
  "port": 5005,
  "interval_ms": 400,
  "asset_folder": "/etc/waybeam_hub",
  "initial_off": [],
  "zoom_step": 25,
  "zoom_max": 300,
  "actions_ini": "",
  "action_timeout_ms": 5000,
  "action_shell": "",
  "menu_asset_id": 7,
  "webui_host": "0.0.0.0",
  "webui_port": 8060,
  "sse_url": "http://127.0.0.1:8070/sse",
  "priority": "serial",
  "priority_fallback_s": 5.0,
  "extra_destinations": ["10.6.0.50:7777"],
  "verbose": false,
  "tuning": {
    "crsf_axis_deadband": 120,
    "crsf_action_threshold": 1400,
    "crsf_nav_debounce_ms": 100,
    "crsf_select_debounce_ms": 250,
    "menu_inactivity_timeout_s": 30.0
  }
}
```

Only include the keys you want to override — missing keys use the built-in runtime defaults.

When `actions_ini` is left empty, Waybeam Hub looks for `menu.ini` inside `asset_folder`. The WebUI also serves `index.html` from the same directory. The default deployment layout now assumes:

```text
/usr/bin/waybeam_hub.py
/etc/waybeam_hub/config.json
/etc/waybeam_hub/index.html
/etc/waybeam_hub/menu.ini
```

You can still install the assets elsewhere by setting:

```json
{
  "asset_folder": "/usr/share/waybeam_hub"
}
```

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

Navigation uses centered CRSF channel values. The thresholds below reflect the defaults and can be adjusted via the `tuning` config section.

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
{"gamma": "0.85,0.00,1.00,1.00,1.00,1.00"}
{"gamma": "off"}
{"destinations": [{"host": "10.6.0.50", "port": 7777}]}
```

The `OSD Control` tab includes a `Gamma LUT` card with six sliders (`gamma`, `lift`, `gain`, `R`, `G`, `B`) plus `Gregify`, `Send Neutral`, and `Disable Gamma`. Slider changes are debounced and sent as one-shot external OSD `gamma` commands. Accepted ranges are `gamma` 0.20-5.00, `lift` -0.50-0.50, `gain` 0.50-3.00, and `R/G/B` 0.50-1.50. The `Gregify` preset sends `1.00,-0.15,2.75,1.00,1.00,1.00`.

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
| `config.json` | Default JSON configuration (all settings and tuning constants) |
| `index.html` | Browser-based control panel (served by the WebUI) |
| `menu.ini` | Default menu actions and radio rule definitions |
| `S99waybeam_hub` | Example init-style startup script for deployment |
