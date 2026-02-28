# Waybeam Hub — Implementation Plan

This file tracks completed work, architectural decisions, and planned future improvements for the Waybeam Hub component. Update it whenever a significant change lands or a new requirement is identified.

## Current architecture

```
waybeam_hub.py
├── Config loading ──── load_config() reads config.json, validates, applies defaults
│                       apply_tuning() sets module-level globals from tuning section
├── SSE reader ──────── SseReader thread consumes channel data from PixelPilot SSE
├── Input processing ── poll_source_remote_keys() interprets CRSF channels
│                       evaluate_radio_rules() triggers condition-based commands
├── Menu engine ─────── Hierarchical menu (ASSETS, ZOOM, custom INI sections)
│                       Three-slot sliding window sent as ext.text6–ext.text8
├── OSD sender ──────── build_payload() + send_payloads() over UDP to all destinations
├── WebUI server ────── WebUiBridge HTTP server (GET /, /state; POST /command)
└── Action executor ─── execute_action() runs shell commands with timeout
```

### Key design decisions

- **JSON-only config** — all settings live in `config.json`; the only CLI flag is `--config`. Tuning constants (deadband, debounce, timeouts) are exposed in an optional `tuning` section so they can be adjusted without editing source.
- **Module globals for tuning** — `apply_tuning()` writes directly to module-level constants (`CRSF_AXIS_DEADBAND`, etc.) so that all existing function references work without passing a config object through every call.
- **Menu actions from INI** — the `actions_ini` path in config points to a separate INI file (`menu.ini`) that defines submenus and radio rules. This keeps the JSON config focused on runtime settings while the INI handles the menu structure.
- **No external dependencies** — stdlib only (json, configparser, socket, threading, http.server, urllib).

## Completed work

| Date | Change |
|------|--------|
| 2026-02-28 | Initial waybeam_hub: SSE menu driver, WebUI, radio rules, asset/zoom control |
| 2026-02-28 | Replaced CLI arguments with JSON config file (`config.json`) |
| 2026-02-28 | Exposed tuning constants (debounce, deadband, timeouts) in config `tuning` section |

## Known issues

- SSE reconnection on connection loss is basic (retry loop with fixed delay). Could benefit from exponential backoff.
- WebUI uses polling via `/state` endpoint rather than WebSocket push. Works fine at current scale but limits real-time responsiveness.
- Radio rule conditions only support simple `MIN<chN<MAX` ranges. No support for boolean combinations or edge-triggered (rising/falling) events.

## Planned improvements

_Add future implementation ideas below as they come up._

- [ ] **WebSocket push for WebUI** — replace `/state` polling with a WebSocket connection for lower-latency state updates in the browser.
- [ ] **Config hot-reload** — watch `config.json` for changes and apply non-destructive updates (e.g. tuning constants, verbose flag) without restarting.
- [ ] **Multi-condition radio rules** — support AND/OR combinations of channel conditions and edge triggers (fire on channel entering vs. leaving range).
- [ ] **OSD layout presets** — allow config to define named layout presets (sets of asset visibility + text overrides) that can be toggled via menu or radio rule.
- [ ] **Telemetry logging** — optional file or UDP logging of channel values, menu interactions, and action executions for post-flight analysis.

## Conventions

- Keep `waybeam_hub.py` as a single file — no package split unless it exceeds ~3000 lines.
- All settings must have defaults so that an empty `{}` config file produces a working (if unconfigured) instance.
- Validate early in `load_config()` — fail with clear error messages before `run_controller()` starts.
- Update this PLAN.md and `README.md` when adding new config keys or changing behavior.
- Update `config.json` defaults to match any new settings added to `_CONFIG_DEFAULTS` or `_TUNING_DEFAULTS`.
