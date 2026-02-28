#!/usr/bin/env python3
"""SSE + WebUI remote menu driver for PixelPilot external UDP OSD control.

This variant removes ncurses input and consumes menu controls from an SSE stream
that already exposes decoded CRSF channel values for multiple sources.
"""

from __future__ import annotations

import argparse
import configparser
import copy
import json
import os
import queue
import select
import shutil
import signal
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
import re
from typing import Dict, List, Mapping, Optional, Sequence, Set, Tuple

MAX_OSD_SLOTS = 8
MAX_OSD_TEXT_CHARS = 63
ASSET_COUNT = 8
MENU_TEXT_SLOT_START = MAX_OSD_SLOTS - 3  # ext.text6 (1-based) / index 5

SECTION_ASSETS = "ASSETS"
SECTION_ZOOM = "ZOOM"
SECTION_RADIO = "RADIO"
RESERVED_SECTIONS = {SECTION_ASSETS, SECTION_ZOOM, SECTION_RADIO}

STOP_REQUESTED = False

# CRSF-compatible channel interpretation (same thresholds/mapping as the existing
# controller, but channel data now arrives via SSE JSON events).
CRSF_MIN = 172
CRSF_MAX = 1811
CRSF_CENTER = 992
CRSF_AXIS_DEADBAND = 120
CRSF_ACTION_THRESHOLD = 1400
CRSF_MENU_TOGGLE_CH_LOW_MAX = 500
CRSF_MENU_TOGGLE_CH4_MIN = 1500
CRSF_NAV_DEBOUNCE_MS = 100
CRSF_SELECT_DEBOUNCE_MS = 250
CRSF_MENU_TOGGLE_HOLD_S = 1.0
CRSF_SAMPLE_INTERVAL_MS = 40
RADIO_TRIGGER_DEBOUNCE_MS = CRSF_NAV_DEBOUNCE_MS
# Re-arm quickly for digital switch toggles (e.g. 191 <-> 1792) while still
# debouncing action execution on re-entry.
RADIO_RESET_DEBOUNCE_MS = 0
CRSF_MIN_CONTROL_CHANNELS = 4
CRSF_DISPLAY_CHANNELS = 16

MENU_INACTIVITY_TIMEOUT_S = 30.0
SOURCE_STALE_S = 1.0

KEY_UP = -201
KEY_DOWN = -202
KEY_SELECT = 10
KEY_MENU_TOGGLE = -1002

SSE_SOURCES = ("serial", "joystick")
DEFAULT_EXTRA_DESTINATION = "10.6.0.50:7777"

GAMMA_DEFAULT = 1.0
GAMMA_LIFT_DEFAULT = 0.0
GAMMA_GAIN_DEFAULT = 1.0
GAMMA_CHANNEL_DEFAULT = 1.0
GAMMA_MIN = 0.20
GAMMA_MAX = 5.00
GAMMA_LIFT_MIN = -0.50
GAMMA_LIFT_MAX = 0.50
GAMMA_GAIN_MIN = 0.50
GAMMA_GAIN_MAX = 1.50
GAMMA_CHANNEL_MIN = 0.50
GAMMA_CHANNEL_MAX = 1.50


@dataclass(frozen=True)
class UdpDestination:
    host: str
    port: int


@dataclass
class MenuAction:
    section: str
    name: str
    command: str


@dataclass
class MenuEntry:
    kind: str
    section: str = ""
    asset_id: int = -1
    action: Optional[MenuAction] = None


@dataclass
class RadioRule:
    name: str
    channel_index: int
    min_value: int
    max_value: int
    action: MenuAction


@dataclass
class RadioRuleState:
    enter_started_monotonic: float = 0.0
    outside_started_monotonic: float = 0.0
    latched_in_range: bool = False
    trigger_count: int = 0


@dataclass
class SourceInputState:
    name: str
    select_pressed: bool = False
    back_pressed: bool = False
    last_select_monotonic: float = 0.0
    last_sample_monotonic: float = 0.0
    last_update_monotonic: float = 0.0
    channels: Tuple[int, ...] = (CRSF_CENTER,) * CRSF_DISPLAY_CHANNELS
    nav_direction: str = "neutral"
    nav_candidate: str = "neutral"
    nav_candidate_since: float = 0.0
    nav_latched: bool = False
    combo_active: bool = False
    combo_started_monotonic: float = 0.0
    combo_latched: bool = False
    link_up: bool = False
    debug_last_print_monotonic: float = 0.0
    debug_last_signature: str = ""


def clamp_crsf_channel(value: int) -> int:
    if value < CRSF_MIN:
        return CRSF_MIN
    if value > CRSF_MAX:
        return CRSF_MAX
    return value


class SseReader(threading.Thread):
    """Background SSE reader that emits decoded source/channel samples."""

    def __init__(
        self,
        url: str,
        out_queue: "queue.Queue[dict]",
        stop_event: threading.Event,
        verbose: bool,
    ) -> None:
        super().__init__(daemon=True)
        self._url = url
        self._out_queue = out_queue
        self._stop_event = stop_event
        self._verbose = verbose
        self._dropped_event_count = 0
        self._last_drop_notice_monotonic = 0.0

    def _queue_event(self, event: dict) -> None:
        try:
            self._out_queue.put_nowait(event)
        except queue.Full:
            self._dropped_event_count += 1
            try:
                self._out_queue.get_nowait()
            except queue.Empty:
                pass
            try:
                self._out_queue.put_nowait(event)
            except queue.Full:
                pass
            if self._verbose:
                now = time.monotonic()
                if (now - self._last_drop_notice_monotonic) >= 1.0:
                    print(
                        f"[SSE] queue full; dropped {self._dropped_event_count} event(s)",
                        flush=True,
                    )
                    self._last_drop_notice_monotonic = now

    def _queue_status(self, message: str) -> None:
        self._queue_event({"type": "status", "message": message, "monotonic": time.monotonic()})

    def _handle_sse_event(self, event_name: str, data_lines: Sequence[str]) -> None:
        if not data_lines:
            return

        payload_text = "\n".join(data_lines)
        try:
            payload = json.loads(payload_text)
        except json.JSONDecodeError:
            if self._verbose:
                self._queue_status("SSE: ignored non-JSON data event")
            return

        if not isinstance(payload, dict):
            return

        stream = str(payload.get("stream", "")).strip().lower()
        if not stream:
            stream = event_name.strip().lower()
        if stream not in SSE_SOURCES:
            return

        channels = payload.get("channels")
        if not isinstance(channels, list) or len(channels) < CRSF_MIN_CONTROL_CHANNELS:
            return

        converted: List[int] = []
        max_count = min(CRSF_DISPLAY_CHANNELS, len(channels))
        for idx in range(max_count):
            try:
                converted.append(clamp_crsf_channel(int(channels[idx])))
            except (TypeError, ValueError):
                return
        if len(converted) < CRSF_DISPLAY_CHANNELS:
            converted.extend([CRSF_CENTER] * (CRSF_DISPLAY_CHANNELS - len(converted)))

        self._queue_event(
            {
                "type": "sample",
                "stream": stream,
                "channels": tuple(converted),
                "payload": payload,
                "monotonic": time.monotonic(),
            }
        )

    def run(self) -> None:
        while not self._stop_event.is_set():
            try:
                request = urllib.request.Request(
                    self._url,
                    headers={
                        "Accept": "text/event-stream",
                        "Cache-Control": "no-cache",
                        "Connection": "keep-alive",
                    },
                    method="GET",
                )
                with urllib.request.urlopen(request, timeout=20) as response:
                    self._queue_status(f"SSE connected: {self._url}")
                    event_name = ""
                    data_lines: List[str] = []

                    while not self._stop_event.is_set():
                        raw_line = response.readline()
                        if raw_line == b"":
                            # Flush any partially buffered event before reconnecting.
                            self._handle_sse_event(event_name, data_lines)
                            break

                        line = raw_line.decode("utf-8", errors="replace").rstrip("\r\n")
                        if line == "":
                            self._handle_sse_event(event_name, data_lines)
                            event_name = ""
                            data_lines = []
                            continue

                        if line.startswith(":"):
                            continue
                        if line.startswith("event:"):
                            event_name = line.split(":", 1)[1].strip()
                            continue
                        if line.startswith("data:"):
                            data_lines.append(line.split(":", 1)[1].lstrip())
                            continue

                    if not self._stop_event.is_set():
                        self._queue_status("SSE stream closed, reconnecting")
            except urllib.error.URLError as exc:
                self._queue_status(f"SSE connect failed: {exc}")
            except TimeoutError:
                self._queue_status("SSE timeout, reconnecting")
            except Exception as exc:  # defensive: keep reconnecting
                self._queue_status(f"SSE error: {exc}")

            if self._stop_event.wait(1.0):
                break


def _on_sigint(_signum: int, _frame) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def parse_asset_id_list(spec: str) -> Set[int]:
    if spec.strip() == "":
        return set()
    parsed: Set[int] = set()
    for token in spec.split(","):
        chunk = token.strip()
        if not chunk:
            continue
        try:
            asset_id = int(chunk)
        except ValueError as exc:
            raise ValueError(f"invalid asset id '{chunk}'") from exc
        if asset_id < 0 or asset_id >= ASSET_COUNT:
            raise ValueError(f"asset id {asset_id} out of range (expected 0..{ASSET_COUNT - 1})")
        parsed.add(asset_id)
    return parsed


def parse_radio_condition(spec: str) -> Optional[Tuple[int, int, int]]:
    text = spec.strip().lower()
    if not text:
        return None

    match = re.match(r"^(-?\d+)\s*<\s*ch\s*(\d+)\s*<\s*(-?\d+)$", text)
    if not match:
        return None

    minimum = int(match.group(1))
    channel_number = int(match.group(2))
    maximum = int(match.group(3))
    if channel_number < 1 or channel_number > CRSF_DISPLAY_CHANNELS:
        raise ValueError(f"RADIO rule channel out of range: ch{channel_number} (expected 1..{CRSF_DISPLAY_CHANNELS})")

    if minimum >= maximum:
        raise ValueError(
            f"RADIO rule range must be increasing for ch{channel_number}: "
            f"expected min<max, got {minimum} and {maximum}"
        )

    return channel_number - 1, minimum, maximum


def load_actions(path: str) -> Tuple[List[str], Dict[str, List[MenuAction]], List[RadioRule]]:
    if not path:
        return [], {}, []

    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    loaded = parser.read(path)
    if not loaded:
        raise ValueError(f"failed to read actions ini: {path}")

    section_order: List[str] = []
    actions_by_section: Dict[str, List[MenuAction]] = {}
    radio_rules: List[RadioRule] = []

    for section in parser.sections():
        section_name = section.strip()
        if not section_name:
            continue
        if section_name.upper() == SECTION_RADIO:
            for name, raw_value in parser.items(section):
                condition_text = name.strip()
                value_text = raw_value.strip()
                if not condition_text or not value_text:
                    continue

                parsed_condition = parse_radio_condition(condition_text)
                if parsed_condition is None:
                    raise ValueError(
                        "invalid RADIO rule "
                        f"'{condition_text}': expected format '1200<ch1<1500 = command'"
                    )

                command_text = value_text
                ch_idx, min_value, max_value = parsed_condition
                rule_name = f"{min_value}<ch{ch_idx + 1}<{max_value}"

                action = MenuAction(section=SECTION_RADIO, name=rule_name, command=command_text)
                radio_rules.append(
                    RadioRule(
                        name=rule_name,
                        channel_index=ch_idx,
                        min_value=min_value,
                        max_value=max_value,
                        action=action,
                    )
                )
            continue

        if section_name.upper() in RESERVED_SECTIONS:
            continue

        section_actions: List[MenuAction] = []
        for name, raw_command in parser.items(section):
            action_name = name.strip()
            command = raw_command.strip()
            if not action_name or not command:
                continue
            section_actions.append(MenuAction(section=section_name, name=action_name, command=command))

        if section_actions:
            section_order.append(section_name)
            actions_by_section[section_name] = section_actions

    return section_order, actions_by_section, radio_rules


def clamp_text(text: str) -> str:
    if len(text) <= MAX_OSD_TEXT_CHARS:
        return text
    return text[: MAX_OSD_TEXT_CHARS - 3] + "..."


def clamp_osd_value(value: float) -> float:
    return max(-100.0, min(100.0, value))


def normalize_manual_asset_updates(payload: object) -> List[dict]:
    if not isinstance(payload, list):
        return []

    allowed_type_values = {"bar", "text", "image"}
    allowed_orientation_values = {"right", "left", "center"}
    normalized: List[dict] = []

    def parse_int(value: object) -> Optional[int]:
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            return value
        if isinstance(value, float) and value.is_integer():
            return int(value)
        return None

    def parse_float(value: object) -> Optional[float]:
        if isinstance(value, bool):
            return None
        if isinstance(value, (int, float)):
            return float(value)
        return None

    for raw_update in payload:
        if not isinstance(raw_update, dict):
            continue

        asset_id = parse_int(raw_update.get("id"))
        if asset_id is None or asset_id < 0 or asset_id >= MAX_OSD_SLOTS:
            continue

        update: Dict[str, object] = {"id": asset_id}

        enabled = raw_update.get("enabled")
        if isinstance(enabled, bool):
            update["enabled"] = enabled

        asset_type = raw_update.get("type")
        if isinstance(asset_type, str):
            cleaned_type = asset_type.strip().lower()
            if cleaned_type in allowed_type_values:
                update["type"] = cleaned_type

        value_index = parse_int(raw_update.get("value_index"))
        if value_index is not None and 0 <= value_index < MAX_OSD_SLOTS:
            update["value_index"] = value_index

        text_index = parse_int(raw_update.get("text_index"))
        if text_index is not None and -1 <= text_index < MAX_OSD_SLOTS:
            update["text_index"] = text_index

        text_indices = raw_update.get("text_indices")
        if isinstance(text_indices, list):
            parsed_text_indices: List[int] = []
            for item in text_indices:
                parsed_item = parse_int(item)
                if parsed_item is None or parsed_item < 0 or parsed_item >= MAX_OSD_SLOTS:
                    parsed_text_indices = []
                    break
                parsed_text_indices.append(parsed_item)
            if parsed_text_indices:
                update["text_indices"] = parsed_text_indices
            elif text_indices == []:
                update["text_indices"] = []

        text_inline = raw_update.get("text_inline")
        if isinstance(text_inline, bool):
            update["text_inline"] = text_inline

        label = raw_update.get("label")
        if isinstance(label, str):
            update["label"] = clamp_text(label)

        orientation = raw_update.get("orientation")
        if isinstance(orientation, str):
            cleaned_orientation = orientation.strip().lower()
            if cleaned_orientation in allowed_orientation_values:
                update["orientation"] = cleaned_orientation

        for int_key in ("x", "y", "width", "height", "bar_color", "text_color", "segments"):
            parsed_value = parse_int(raw_update.get(int_key))
            if parsed_value is not None:
                update[int_key] = parsed_value

        scale_pct = parse_int(raw_update.get("scale_pct"))
        if scale_pct is not None and 25 <= scale_pct <= 400:
            update["scale_pct"] = scale_pct

        minimum = parse_float(raw_update.get("min"))
        if minimum is not None:
            update["min"] = minimum

        maximum = parse_float(raw_update.get("max"))
        if maximum is not None:
            update["max"] = maximum

        background = parse_int(raw_update.get("background"))
        if background is not None and -1 <= background <= 10:
            update["background"] = background

        background_opacity = parse_int(raw_update.get("background_opacity"))
        if background_opacity is not None and 0 <= background_opacity <= 100:
            update["background_opacity"] = background_opacity

        rounded_outline = raw_update.get("rounded_outline")
        if isinstance(rounded_outline, bool):
            update["rounded_outline"] = rounded_outline

        if len(update) > 1:
            normalized.append(update)

    return normalized


def zoom_state_text(zoom_enabled: bool, zoom_percent: int) -> str:
    if not zoom_enabled:
        return "OFF"
    return f"{zoom_percent}%"


def build_top_entries(action_sections: Sequence[str]) -> List[MenuEntry]:
    entries = [
        MenuEntry(kind="section", section=SECTION_ASSETS),
        MenuEntry(kind="section", section=SECTION_ZOOM),
    ]
    entries.extend(MenuEntry(kind="section", section=section) for section in action_sections)
    entries.append(MenuEntry(kind="exit"))
    return entries


def build_submenu_entries(current_section: str, actions_by_section: Mapping[str, List[MenuAction]]) -> List[MenuEntry]:
    entries: List[MenuEntry] = []

    if current_section == SECTION_ASSETS:
        entries.extend(MenuEntry(kind="asset", asset_id=asset_id) for asset_id in range(ASSET_COUNT))
        entries.append(MenuEntry(kind="return"))
        return entries

    if current_section == SECTION_ZOOM:
        entries.append(MenuEntry(kind="zoom_in"))
        entries.append(MenuEntry(kind="zoom_out"))
        entries.append(MenuEntry(kind="return"))
        return entries

    for action in actions_by_section.get(current_section, []):
        entries.append(MenuEntry(kind="action", action=action))
    entries.append(MenuEntry(kind="return"))
    return entries


def build_submenu_table(
    action_sections: Sequence[str],
    actions_by_section: Mapping[str, List[MenuAction]],
) -> Dict[str, List[MenuEntry]]:
    submenu_table: Dict[str, List[MenuEntry]] = {
        SECTION_ASSETS: build_submenu_entries(SECTION_ASSETS, actions_by_section),
        SECTION_ZOOM: build_submenu_entries(SECTION_ZOOM, actions_by_section),
    }
    for section in action_sections:
        submenu_table[section] = build_submenu_entries(section, actions_by_section)
    return submenu_table


def display_entry(entry: MenuEntry, asset_enabled: Sequence[Optional[bool]], zoom_enabled: bool, zoom_percent: int) -> str:
    if entry.kind == "section":
        return f"[{entry.section}]"
    if entry.kind == "exit":
        return "EXIT"
    if entry.kind == "return":
        return "RETURN"
    if entry.kind == "asset" and entry.asset_id >= 0:
        asset_state = asset_enabled[entry.asset_id]
        state = "?" if asset_state is None else ("ON" if asset_state else "OFF")
        return f"ASSET {entry.asset_id} {state}"
    if entry.kind == "zoom_in":
        return f"ZOOM IN ({zoom_state_text(zoom_enabled, zoom_percent)})"
    if entry.kind == "zoom_out":
        return f"ZOOM OUT ({zoom_state_text(zoom_enabled, zoom_percent)})"
    if entry.kind == "action" and entry.action is not None:
        return entry.action.name
    return "UNKNOWN"


def build_three_slot_menu_texts(
    entries: Sequence[MenuEntry],
    selected: int,
    asset_enabled: Sequence[Optional[bool]],
    zoom_enabled: bool,
    zoom_percent: int,
) -> Tuple[str, str, str]:
    if not entries:
        return "", "", ""

    count = len(entries)
    if count >= 3:
        window_start = min(max(0, selected - 1), count - 3)
        visible_indices = [window_start, window_start + 1, window_start + 2]
    else:
        visible_indices = list(range(count))

    lines: List[str] = []
    for idx in visible_indices:
        prefix = "> " if idx == selected else "  "
        lines.append(clamp_text(f"{prefix}{display_entry(entries[idx], asset_enabled, zoom_enabled, zoom_percent)}"))

    while len(lines) < 3:
        lines.append("")
    return lines[0], lines[1], lines[2]


def current_zoom_command(zoom_enabled: bool, zoom_percent: int) -> str:
    if not zoom_enabled or zoom_percent <= 100:
        return "off"
    return f"{zoom_percent},{zoom_percent},50,50"


def _format_gamma_value(value: float) -> str:
    return f"{value:.2f}"


def current_gamma_command(
    gamma_enabled: bool,
    gamma_value: float,
    gamma_lift: float,
    gamma_gain: float,
    gamma_r: float,
    gamma_g: float,
    gamma_b: float,
) -> str:
    if not gamma_enabled:
        return "off"
    values = (
        gamma_value,
        gamma_lift,
        gamma_gain,
        gamma_r,
        gamma_g,
        gamma_b,
    )
    return ",".join(_format_gamma_value(value) for value in values)


def parse_gamma_command(command: str) -> Tuple[bool, Optional[Tuple[float, float, float, float, float, float]]]:
    value = command.strip()
    if not value:
        return False, None

    lowered = value.lower()
    if lowered in ("off", "gamma=off"):
        return False, None
    if lowered.startswith("gamma="):
        value = value[6:]

    parts = [part.strip() for part in value.split(",")]
    if len(parts) != 6 or any(part == "" for part in parts):
        raise ValueError("expected gamma command as GAMMA,LIFT,GAIN,R,G,B")

    try:
        gamma_value, gamma_lift, gamma_gain, gamma_r, gamma_g, gamma_b = (float(part) for part in parts)
    except ValueError as exc:
        raise ValueError("gamma command contains non-numeric values") from exc

    if not (GAMMA_MIN <= gamma_value <= GAMMA_MAX):
        raise ValueError(f"gamma must be between {GAMMA_MIN:.2f} and {GAMMA_MAX:.2f}")
    if not (GAMMA_LIFT_MIN <= gamma_lift <= GAMMA_LIFT_MAX):
        raise ValueError(f"lift must be between {GAMMA_LIFT_MIN:.2f} and {GAMMA_LIFT_MAX:.2f}")
    if not (GAMMA_GAIN_MIN <= gamma_gain <= GAMMA_GAIN_MAX):
        raise ValueError(f"gain must be between {GAMMA_GAIN_MIN:.2f} and {GAMMA_GAIN_MAX:.2f}")
    if not (GAMMA_CHANNEL_MIN <= gamma_r <= GAMMA_CHANNEL_MAX):
        raise ValueError(f"R must be between {GAMMA_CHANNEL_MIN:.2f} and {GAMMA_CHANNEL_MAX:.2f}")
    if not (GAMMA_CHANNEL_MIN <= gamma_g <= GAMMA_CHANNEL_MAX):
        raise ValueError(f"G must be between {GAMMA_CHANNEL_MIN:.2f} and {GAMMA_CHANNEL_MAX:.2f}")
    if not (GAMMA_CHANNEL_MIN <= gamma_b <= GAMMA_CHANNEL_MAX):
        raise ValueError(f"B must be between {GAMMA_CHANNEL_MIN:.2f} and {GAMMA_CHANNEL_MAX:.2f}")

    return True, (gamma_value, gamma_lift, gamma_gain, gamma_r, gamma_g, gamma_b)


def build_payload(menu_window: Tuple[str, str, str], zoom_enabled: bool, zoom_percent: int) -> dict:
    texts: List[Optional[str]] = [None] * MAX_OSD_SLOTS
    texts[MENU_TEXT_SLOT_START + 0] = clamp_text(menu_window[0])
    texts[MENU_TEXT_SLOT_START + 1] = clamp_text(menu_window[1])
    texts[MENU_TEXT_SLOT_START + 2] = clamp_text(menu_window[2])
    payload = {"texts": texts, "zoom": current_zoom_command(zoom_enabled, zoom_percent)}
    return payload


def parse_udp_destination(payload: object) -> Optional[UdpDestination]:
    if not isinstance(payload, dict):
        return None
    host = str(payload.get("host", "")).strip()
    port_raw = payload.get("port")
    if not host:
        return None
    if isinstance(port_raw, str) and port_raw.isdigit():
        port_raw = int(port_raw)
    if not isinstance(port_raw, int) or not (1 <= port_raw <= 65535):
        return None
    return UdpDestination(host=host, port=port_raw)


def parse_destination_spec(spec: str) -> UdpDestination:
    text = spec.strip()
    if not text:
        raise ValueError("empty destination")
    host_part, sep, port_part = text.rpartition(":")
    host = host_part.strip()
    port_text = port_part.strip()
    if sep == "":
        raise ValueError(f"invalid destination '{spec}' (expected host:port)")
    if not host:
        raise ValueError(f"invalid destination '{spec}' (empty host)")
    if not port_text:
        raise ValueError(f"invalid destination '{spec}' (empty port)")
    try:
        port = int(port_text)
    except ValueError as exc:
        raise ValueError(f"invalid destination port in '{spec}'") from exc
    if port < 1 or port > 65535:
        raise ValueError(f"destination port out of range in '{spec}'")
    return UdpDestination(host=host, port=port)


def dedupe_destinations(destinations: Sequence[UdpDestination]) -> List[UdpDestination]:
    seen: Set[Tuple[str, int]] = set()
    unique: List[UdpDestination] = []
    for destination in destinations:
        key = (destination.host, destination.port)
        if key in seen:
            continue
        seen.add(key)
        unique.append(destination)
    return unique


def destination_label(destination: UdpDestination) -> str:
    return f"{destination.host}:{destination.port}"


def send_payloads(sock: socket.socket, destinations: Sequence[UdpDestination], payload: dict) -> List[str]:
    encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    failures: List[str] = []
    for destination in destinations:
        try:
            sock.sendto(encoded, (destination.host, destination.port))
        except OSError as exc:
            failures.append(f"{destination_label(destination)} ({exc})")
    return failures


class WebUiBridge:
    """Small non-blocking HTTP JSON server for browser control."""

    def __init__(self, host: str, port: int, ui_html_path: str) -> None:
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind((host, port))
        self._server.listen()
        self._server.setblocking(False)
        self._clients: List[socket.socket] = []
        self._buffers: Dict[socket.socket, bytes] = {}
        self._pending_commands: List[dict] = []
        self._latest_state: dict = {"type": "menu_state", "status": "starting"}
        self._ui_html = self._read_ui_html(ui_html_path)

    def _read_ui_html(self, path: str) -> str:
        try:
            with open(path, "r", encoding="utf-8") as handle:
                return handle.read()
        except OSError:
            return "<html><body><h1>WebUI not found</h1></body></html>"

    def close(self) -> None:
        for client in self._clients:
            try:
                client.close()
            except OSError:
                pass
        self._clients.clear()
        self._buffers.clear()
        self._server.close()

    def poll_messages(self) -> List[dict]:
        self._accept_new_clients()
        if self._clients:
            readable, _, _ = select.select(self._clients, [], [], 0)
            for client in readable:
                try:
                    chunk = client.recv(4096)
                except OSError:
                    chunk = b""
                if not chunk:
                    self._drop_client(client)
                    continue
                pending = self._buffers.get(client, b"") + chunk
                request, remainder = self._extract_request(pending)
                if request is None:
                    self._buffers[client] = pending
                    continue
                self._buffers[client] = remainder
                self._handle_request(client, request)
                self._drop_client(client)

        commands = self._pending_commands
        self._pending_commands = []
        return commands

    def broadcast(self, payload: dict) -> None:
        self._latest_state = payload

    def _accept_new_clients(self) -> None:
        while True:
            try:
                client, _addr = self._server.accept()
            except BlockingIOError:
                break
            client.setblocking(False)
            self._clients.append(client)
            self._buffers[client] = b""

    def _drop_client(self, client: socket.socket) -> None:
        try:
            client.close()
        except OSError:
            pass
        if client in self._clients:
            self._clients.remove(client)
        self._buffers.pop(client, None)

    def _extract_request(self, data: bytes) -> Tuple[Optional[bytes], bytes]:
        header_end = data.find(b"\r\n\r\n")
        if header_end < 0:
            return None, data
        headers = data[:header_end].decode("utf-8", errors="replace")
        content_length = 0
        for line in headers.split("\r\n")[1:]:
            if line.lower().startswith("content-length:"):
                try:
                    content_length = int(line.split(":", 1)[1].strip())
                except ValueError:
                    content_length = 0
                break
        total_len = header_end + 4 + content_length
        if len(data) < total_len:
            return None, data
        return data[:total_len], data[total_len:]

    def _send_response(self, client: socket.socket, status: str, content_type: str, body: bytes) -> None:
        response = (
            f"HTTP/1.1 {status}\r\n"
            f"Content-Type: {content_type}\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n"
        ).encode("utf-8") + body
        try:
            client.sendall(response)
        except OSError:
            pass

    def _handle_request(self, client: socket.socket, request: bytes) -> None:
        header_part, body_part = request.split(b"\r\n\r\n", 1)
        lines = header_part.decode("utf-8", errors="replace").split("\r\n")
        request_line = lines[0] if lines else ""
        parts = request_line.split()
        if len(parts) < 2:
            self._send_response(client, "400 Bad Request", "application/json", b'{"error":"bad request"}')
            return
        method, path = parts[0].upper(), parts[1]

        if method == "OPTIONS":
            self._send_response(client, "204 No Content", "text/plain", b"")
            return

        if method == "GET" and path in ("/", "/index.html"):
            self._send_response(client, "200 OK", "text/html; charset=utf-8", self._ui_html.encode("utf-8"))
            return

        if method == "GET" and path == "/state":
            payload = json.dumps(self._latest_state, separators=(",", ":")).encode("utf-8")
            self._send_response(client, "200 OK", "application/json", payload)
            return

        if method == "POST" and path == "/command":
            try:
                payload = json.loads(body_part.decode("utf-8", errors="replace"))
            except json.JSONDecodeError:
                self._send_response(client, "400 Bad Request", "application/json", b'{"error":"invalid json"}')
                return
            if isinstance(payload, dict):
                self._pending_commands.append(payload)
                self._send_response(client, "200 OK", "application/json", b'{"ok":true}')
            else:
                self._send_response(client, "400 Bad Request", "application/json", b'{"error":"object required"}')
            return

        self._send_response(client, "404 Not Found", "application/json", b'{"error":"not found"}')


def parse_zoom_command(command: str) -> Tuple[bool, int]:
    value = command.strip().lower()
    if not value or value == "off":
        return False, 100
    zoom_percent = int(value.split(",", 1)[0].strip())
    if zoom_percent <= 100:
        return False, 100
    return True, zoom_percent


def condense_output(text: str) -> str:
    for line in text.splitlines():
        stripped = line.strip()
        if stripped:
            return clamp_text(stripped)
    return ""


def resolve_action_shell(preferred: str) -> str:
    candidate = preferred.strip() or os.environ.get("SHELL", "").strip() or "/bin/sh"
    if os.path.sep not in candidate:
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    return candidate


def execute_action(action: MenuAction, timeout_ms: int, action_shell: str) -> str:
    timeout_s = max(0.1, timeout_ms / 1000.0)
    try:
        result = subprocess.run(
            action.command,
            shell=True,
            executable=action_shell,
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return f"Action timeout: [{action.section}] {action.name}"
    except Exception as exc:
        return f"Action error: {exc}"

    message = condense_output(result.stdout) or condense_output(result.stderr)
    if result.returncode == 0:
        return f"OK [{action.section}] {action.name}: {message}" if message else f"OK [{action.section}] {action.name}"
    return f"ERR {result.returncode} [{action.section}] {action.name}: {message}" if message else f"ERR {result.returncode} [{action.section}] {action.name}"


def evaluate_radio_rules(
    source_state: SourceInputState,
    radio_rules: Sequence[RadioRule],
    rule_states: Sequence[RadioRuleState],
    now: float,
    action_timeout_ms: int,
    action_shell: str,
) -> List[str]:
    if len(radio_rules) != len(rule_states):
        return []

    triggered_statuses: List[str] = []
    for index, rule in enumerate(radio_rules):
        rule_state = rule_states[index]

        if rule.channel_index >= len(source_state.channels):
            rule_state.enter_started_monotonic = 0.0
            rule_state.outside_started_monotonic = 0.0
            rule_state.latched_in_range = False
            continue

        channel_value = source_state.channels[rule.channel_index]
        in_range = rule.min_value < channel_value < rule.max_value

        if not in_range:
            rule_state.enter_started_monotonic = 0.0
            if rule_state.latched_in_range:
                if rule_state.outside_started_monotonic <= 0.0:
                    rule_state.outside_started_monotonic = now
                outside_ms = (now - rule_state.outside_started_monotonic) * 1000.0
                if outside_ms >= RADIO_RESET_DEBOUNCE_MS:
                    rule_state.latched_in_range = False
                    rule_state.outside_started_monotonic = 0.0
            else:
                rule_state.outside_started_monotonic = 0.0
            continue

        rule_state.outside_started_monotonic = 0.0
        if rule_state.latched_in_range:
            continue

        if rule_state.enter_started_monotonic <= 0.0:
            rule_state.enter_started_monotonic = now
            continue

        stable_ms = (now - rule_state.enter_started_monotonic) * 1000.0
        if stable_ms < RADIO_TRIGGER_DEBOUNCE_MS:
            continue

        rule_state.latched_in_range = True
        rule_state.enter_started_monotonic = 0.0
        rule_state.trigger_count += 1
        action_status = execute_action(rule.action, action_timeout_ms, action_shell)
        triggered_statuses.append(
            f"{action_status} (trigger #{rule_state.trigger_count} {rule.min_value}<ch{rule.channel_index + 1}={channel_value}<{rule.max_value})"
        )

    return triggered_statuses


def update_radio_rule_resets_for_source(
    source_state: SourceInputState,
    radio_rules: Sequence[RadioRule],
    rule_states: Sequence[RadioRuleState],
    now: float,
) -> None:
    if len(radio_rules) != len(rule_states):
        return

    for index, rule in enumerate(radio_rules):
        rule_state = rule_states[index]
        if rule.channel_index >= len(source_state.channels):
            rule_state.enter_started_monotonic = 0.0
            rule_state.outside_started_monotonic = 0.0
            rule_state.latched_in_range = False
            continue

        channel_value = source_state.channels[rule.channel_index]
        in_range = rule.min_value < channel_value < rule.max_value
        if not in_range:
            rule_state.enter_started_monotonic = 0.0
            if rule_state.latched_in_range:
                if rule_state.outside_started_monotonic <= 0.0:
                    rule_state.outside_started_monotonic = now
                outside_ms = (now - rule_state.outside_started_monotonic) * 1000.0
                if outside_ms >= RADIO_RESET_DEBOUNCE_MS:
                    rule_state.latched_in_range = False
                    rule_state.outside_started_monotonic = 0.0
            else:
                rule_state.outside_started_monotonic = 0.0
            continue

        rule_state.outside_started_monotonic = 0.0


def reset_radio_rule_states(rule_states: Sequence[RadioRuleState]) -> None:
    for rule_state in rule_states:
        rule_state.enter_started_monotonic = 0.0
        rule_state.outside_started_monotonic = 0.0
        rule_state.latched_in_range = False


def source_is_fresh(state: SourceInputState, now: float, fallback_timeout_s: float) -> bool:
    return state.last_update_monotonic > 0.0 and (now - state.last_update_monotonic) <= fallback_timeout_s


def refresh_source_links(
    source_states: Mapping[str, SourceInputState],
    now: float,
    stale_timeout_s: float,
) -> None:
    for state in source_states.values():
        if state.last_update_monotonic <= 0.0:
            state.link_up = False
            continue
        state.link_up = (now - state.last_update_monotonic) <= stale_timeout_s


def choose_active_source(
    priority_source: str,
    source_states: Mapping[str, SourceInputState],
    now: float,
    fallback_timeout_s: float,
) -> Optional[str]:
    other_source = "joystick" if priority_source == "serial" else "serial"
    priority_fresh = source_is_fresh(source_states[priority_source], now, fallback_timeout_s)
    other_fresh = source_is_fresh(source_states[other_source], now, fallback_timeout_s)

    if priority_fresh:
        return priority_source
    if other_fresh:
        return other_source
    return None


def poll_source_remote_keys(
    source_state: SourceInputState,
    menu_visible: bool,
    now: float,
) -> List[int]:
    keys: List[int] = []

    if (now - source_state.last_sample_monotonic) * 1000.0 < CRSF_SAMPLE_INTERVAL_MS:
        return keys
    source_state.last_sample_monotonic = now

    if source_state.link_up and (now - source_state.last_update_monotonic) > SOURCE_STALE_S:
        source_state.link_up = False
        source_state.nav_direction = "neutral"
        source_state.nav_candidate = "neutral"
        source_state.select_pressed = False
        source_state.back_pressed = False
        source_state.nav_latched = False
        source_state.combo_active = False
        source_state.combo_started_monotonic = 0.0
        source_state.combo_latched = False

    if not source_state.link_up:
        return keys

    axis_y = source_state.channels[1] - CRSF_CENTER
    nav_direction = "neutral"
    nav_key: Optional[int] = None
    if axis_y >= CRSF_AXIS_DEADBAND:
        nav_direction = "up"
        nav_key = KEY_UP
    elif axis_y <= -CRSF_AXIS_DEADBAND:
        nav_direction = "down"
        nav_key = KEY_DOWN

    if nav_direction != source_state.nav_candidate:
        source_state.nav_candidate = nav_direction
        source_state.nav_candidate_since = now

    if nav_direction == "neutral":
        source_state.nav_latched = False

    source_state.nav_direction = nav_direction

    # CH1 high is enter/select. CH2 is the only navigation axis for up/down.
    # CH2 high => up, CH2 low => down.
    # Menu toggle combo: CH1-CH3 low and CH4 high (held).
    select_active = source_state.channels[0] >= CRSF_ACTION_THRESHOLD
    source_state.back_pressed = False

    combo_active = (
        source_state.channels[0] < CRSF_MENU_TOGGLE_CH_LOW_MAX
        and source_state.channels[1] < CRSF_MENU_TOGGLE_CH_LOW_MAX
        and source_state.channels[2] < CRSF_MENU_TOGGLE_CH_LOW_MAX
        and source_state.channels[3] > CRSF_MENU_TOGGLE_CH4_MIN
    )
    source_state.combo_active = combo_active
    if combo_active:
        if source_state.combo_started_monotonic <= 0.0:
            source_state.combo_started_monotonic = now
        hold_time = now - source_state.combo_started_monotonic
        if hold_time >= CRSF_MENU_TOGGLE_HOLD_S and not source_state.combo_latched:
            keys.append(KEY_MENU_TOGGLE)
            source_state.combo_latched = True
    else:
        source_state.combo_started_monotonic = 0.0
        source_state.combo_latched = False

    # When menu overlay is hidden, ignore navigation/actions and only allow
    # explicit combo-based menu toggle.
    if not menu_visible:
        source_state.select_pressed = select_active
        return [key for key in keys if key == KEY_MENU_TOGGLE]

    if nav_key is not None and source_state.nav_candidate == nav_direction and not source_state.nav_latched:
        nav_candidate_age_ms = (now - source_state.nav_candidate_since) * 1000.0
        if nav_candidate_age_ms >= CRSF_NAV_DEBOUNCE_MS:
            keys.append(nav_key)
            source_state.nav_latched = True

    select_elapsed_ms = (now - source_state.last_select_monotonic) * 1000.0
    if select_active and not source_state.select_pressed and select_elapsed_ms >= CRSF_SELECT_DEBOUNCE_MS:
        keys.append(KEY_SELECT)
        source_state.last_select_monotonic = now
    source_state.select_pressed = select_active

    return keys


def maybe_emit_source_debug_log(
    source_state: SourceInputState,
    active_source: Optional[str],
    emitted_keys: Sequence[int],
) -> None:
    now = time.monotonic()
    signature = (
        f"source={source_state.name} active={active_source or 'none'} "
        f"link={'up' if source_state.link_up else 'down'} "
        f"ch={source_state.channels} "
        f"nav={source_state.nav_direction} "
        f"sel={int(source_state.select_pressed)} "
        f"combo={int(source_state.combo_active)} latch={int(source_state.combo_latched)} "
        f"keys={list(emitted_keys)}"
    )
    if emitted_keys or signature != source_state.debug_last_signature or (now - source_state.debug_last_print_monotonic) >= 1.0:
        print(f"[SSE DEBUG] {signature}", flush=True)
        source_state.debug_last_signature = signature
        source_state.debug_last_print_monotonic = now


def build_webui_state(
    menu_window: Tuple[str, str, str],
    entries: Sequence[MenuEntry],
    selected: int,
    current_section: str,
    status: str,
    asset_enabled: Sequence[Optional[bool]],
    zoom_enabled: bool,
    zoom_percent: int,
    gamma_enabled: bool,
    gamma_value: float,
    gamma_lift: float,
    gamma_gain: float,
    gamma_r: float,
    gamma_g: float,
    gamma_b: float,
    destinations: Sequence[UdpDestination],
    source_states: Mapping[str, SourceInputState],
    active_source: Optional[str],
    priority_source: str,
    fallback_timeout_s: float,
    sse_url: str,
    asset_update_tx_count: int,
    asset_update_tx_last_monotonic: float,
    asset_update_tx_last_batch_size: int,
) -> dict:
    now = time.monotonic()

    active_state = source_states.get(active_source) if active_source else None
    if active_state is None:
        active_state = SourceInputState(name=priority_source)

    def source_state_payload(name: str) -> dict:
        state = source_states[name]
        age_ms = int((now - state.last_update_monotonic) * 1000.0) if state.last_update_monotonic else -1
        return {
            "link_up": state.link_up,
            "channels": list(state.channels),
            "nav_direction": state.nav_direction,
            "select_pressed": state.select_pressed,
            "back_pressed": state.back_pressed,
            "combo_active": state.combo_active,
            "combo_latched": state.combo_latched,
            "last_update_age_ms": age_ms,
        }

    active_age_ms = int((now - active_state.last_update_monotonic) * 1000.0) if active_state.last_update_monotonic else -1
    asset_update_tx_last_age_ms = int((now - asset_update_tx_last_monotonic) * 1000.0) if asset_update_tx_last_monotonic > 0.0 else -1

    # Keep `crsf` payload for existing WebUI compatibility while adding richer SSE info.
    return {
        "type": "menu_state",
        "section": current_section or "ROOT",
        "selected": selected,
        "entries": [display_entry(entry, asset_enabled, zoom_enabled, zoom_percent) for entry in entries],
        "menu_window": list(menu_window),
        "status": status,
        "asset_enabled": list(asset_enabled),
        "zoom": current_zoom_command(zoom_enabled, zoom_percent),
        "gamma": current_gamma_command(
            gamma_enabled,
            gamma_value,
            gamma_lift,
            gamma_gain,
            gamma_r,
            gamma_g,
            gamma_b,
        ),
        "gamma_control": {
            "enabled": gamma_enabled,
            "gamma": gamma_value,
            "lift": gamma_lift,
            "gain": gamma_gain,
            "r": gamma_r,
            "g": gamma_g,
            "b": gamma_b,
        },
        "destinations": [{"host": destination.host, "port": destination.port} for destination in destinations],
        "asset_update_tx": {
            "count": asset_update_tx_count,
            "last_age_ms": asset_update_tx_last_age_ms,
            "last_batch_size": asset_update_tx_last_batch_size,
        },
        "crsf": {
            "enabled": True,
            "port": 0,
            "stream": active_source or "none",
            "priority": priority_source,
            "link_up": active_state.link_up,
            "channels": list(active_state.channels),
            "nav_direction": active_state.nav_direction,
            "select_pressed": active_state.select_pressed,
            "back_pressed": active_state.back_pressed,
            "last_update_age_ms": active_age_ms,
            "combo_active": active_state.combo_active,
            "combo_latched": active_state.combo_latched,
        },
        "sse": {
            "url": sse_url,
            "priority": priority_source,
            "active_source": active_source or "none",
            "fallback_timeout_s": fallback_timeout_s,
            "sources": {
                "serial": source_state_payload("serial"),
                "joystick": source_state_payload("joystick"),
            },
        },
    }


def run_controller(
    host: str,
    port: int,
    interval_ms: int,
    initial_off: Set[int],
    zoom_step: int,
    zoom_max: int,
    action_sections: Sequence[str],
    actions_by_section: Dict[str, List[MenuAction]],
    radio_rules: Sequence[RadioRule],
    action_timeout_ms: int,
    action_shell: str,
    menu_asset_id: int,
    webui_host: str,
    webui_port: int,
    sse_url: str,
    priority_source: str,
    fallback_timeout_s: float,
    extra_destinations: Sequence[UdpDestination],
    verbose: bool,
) -> int:
    global STOP_REQUESTED
    STOP_REQUESTED = False

    prev_sigint = signal.getsignal(signal.SIGINT)
    prev_sigterm = signal.getsignal(signal.SIGTERM)
    signal.signal(signal.SIGINT, _on_sigint)
    signal.signal(signal.SIGTERM, _on_sigint)

    def emit_status(message: str) -> None:
        print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)

    try:
        asset_enabled: List[Optional[bool]] = [None] * ASSET_COUNT
        pending_asset_updates: Dict[int, bool] = {}
        for asset_id in initial_off:
            asset_enabled[asset_id] = False
            pending_asset_updates[asset_id] = False

        # Console-only mode starts hidden; show via combo or WebUI command.
        asset_enabled[menu_asset_id] = False
        pending_asset_updates[menu_asset_id] = False

        zoom_enabled = False
        zoom_percent = 100
        gamma_enabled = False
        gamma_value = GAMMA_DEFAULT
        gamma_lift = GAMMA_LIFT_DEFAULT
        gamma_gain = GAMMA_GAIN_DEFAULT
        gamma_r = GAMMA_CHANNEL_DEFAULT
        gamma_g = GAMMA_CHANNEL_DEFAULT
        gamma_b = GAMMA_CHANNEL_DEFAULT
        top_entries = build_top_entries(action_sections)
        submenu_table = build_submenu_table(action_sections, actions_by_section)
        fallback_entries = [MenuEntry(kind="return")]
        current_section = ""
        entries = top_entries
        selected = 0

        destinations = dedupe_destinations([UdpDestination(host=host, port=port)] + list(extra_destinations))

        status = f"Ready (WebUI http://{webui_host}:{webui_port}, SSE {sse_url})"
        last_logged_status = ""
        dirty = True
        last_send_monotonic = 0.0
        last_menu_activity_monotonic = time.monotonic()
        remote_keys: List[int] = []
        manual_text_overrides: List[Optional[str]] = [None] * MAX_OSD_SLOTS
        manual_value_overrides: List[Optional[float]] = [None] * MAX_OSD_SLOTS
        manual_asset_updates: List[dict] = []
        manual_gamma_command = ""
        manual_text_overrides_pending = False
        manual_value_overrides_pending = False
        manual_asset_updates_pending = False
        manual_gamma_pending = False
        asset_update_tx_count = 0
        asset_update_tx_last_monotonic = 0.0
        asset_update_tx_last_batch_size = 0

        def set_asset_enabled(asset_id: int, enabled: bool) -> None:
            asset_enabled[asset_id] = enabled
            pending_asset_updates[asset_id] = enabled

        source_states: Dict[str, SourceInputState] = {
            "serial": SourceInputState(name="serial"),
            "joystick": SourceInputState(name="joystick"),
        }
        radio_rule_states_by_source: Dict[str, List[RadioRuleState]] = {
            source_name: [RadioRuleState() for _ in radio_rules] for source_name in source_states
        }
        active_source: Optional[str] = None

        ui_html_path = os.path.join(os.path.dirname(__file__), "index.html")
        webui_bridge = WebUiBridge(webui_host, webui_port, ui_html_path)

        sse_events: "queue.Queue[dict]" = queue.Queue(maxsize=1024)
        sse_stop = threading.Event()
        sse_reader = SseReader(sse_url, sse_events, sse_stop, verbose)
        sse_reader.start()

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            try:
                while not STOP_REQUESTED:
                    now = time.monotonic()

                    while True:
                        try:
                            event = sse_events.get_nowait()
                        except queue.Empty:
                            break

                        event_type = event.get("type")
                        if event_type == "status":
                            message = str(event.get("message", "")).strip()
                            if message:
                                status = message
                            continue

                        if event_type == "sample":
                            stream = str(event.get("stream", "")).strip().lower()
                            if stream not in source_states:
                                continue
                            channels = event.get("channels")
                            if not isinstance(channels, tuple) or len(channels) < CRSF_MIN_CONTROL_CHANNELS:
                                continue
                            normalized_channels = tuple(int(ch) for ch in channels[:CRSF_DISPLAY_CHANNELS])
                            if len(normalized_channels) < CRSF_DISPLAY_CHANNELS:
                                normalized_channels = normalized_channels + (CRSF_CENTER,) * (CRSF_DISPLAY_CHANNELS - len(normalized_channels))
                            state = source_states[stream]
                            sample_now = time.monotonic()
                            state.channels = normalized_channels
                            state.last_update_monotonic = sample_now
                            state.link_up = True
                            # Apply RADIO reset tracking on every sample so quick
                            # low/high switch transitions are not collapsed away
                            # when multiple SSE events are drained in one loop.
                            if radio_rules:
                                update_radio_rule_resets_for_source(
                                    state,
                                    radio_rules,
                                    radio_rule_states_by_source[stream],
                                    sample_now,
                                )
                            dirty = True

                    now = time.monotonic()
                    refresh_source_links(source_states, now, SOURCE_STALE_S)
                    if radio_rules:
                        for source_name, source_state in source_states.items():
                            if not source_state.link_up:
                                reset_radio_rule_states(radio_rule_states_by_source[source_name])
                    new_active_source = choose_active_source(priority_source, source_states, now, fallback_timeout_s)
                    if new_active_source != active_source:
                        active_source = new_active_source
                        if active_source is None:
                            status = f"No active SSE input; waiting for {priority_source} or fallback source"
                        else:
                            status = f"Active input source: {active_source} (priority={priority_source})"
                        dirty = True

                    entries = top_entries if current_section == "" else submenu_table.get(current_section, fallback_entries)
                    selected = min(max(selected, 0), max(0, len(entries) - 1))

                    menu_window = build_three_slot_menu_texts(entries, selected, asset_enabled, zoom_enabled, zoom_percent)

                    for message in webui_bridge.poll_messages():
                        key_map = {
                            "up": KEY_UP,
                            "down": KEY_DOWN,
                            "select": KEY_SELECT,
                            "enter": KEY_SELECT,
                        }
                        key_name = str(message.get("key", "")).strip().lower()
                        if key_name in key_map:
                            remote_keys.append(key_map[key_name])
                            last_menu_activity_monotonic = time.monotonic()
                        elif key_name in ("quit", "hide_menu"):
                            set_asset_enabled(menu_asset_id, False)
                            status = "Menu overlay hidden"
                            dirty = True
                        elif key_name == "show_menu":
                            set_asset_enabled(menu_asset_id, True)
                            status = "Menu overlay visible"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                        elif key_name == "all_on":
                            for asset_id in range(ASSET_COUNT):
                                set_asset_enabled(asset_id, True)
                            status = "All assets ON"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                        elif key_name == "all_off":
                            for asset_id in range(ASSET_COUNT):
                                set_asset_enabled(asset_id, False)
                            status = "All assets OFF"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True

                        selected_value = message.get("selected")
                        if isinstance(selected_value, int):
                            selected = min(max(selected_value, 0), max(0, len(entries) - 1))
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True

                        if isinstance(message.get("asset_updates"), list):
                            parsed_asset_updates = normalize_manual_asset_updates(message["asset_updates"])
                            if parsed_asset_updates:
                                manual_asset_updates = parsed_asset_updates
                                manual_asset_updates_pending = True
                                dirty = True
                                for update in parsed_asset_updates:
                                    asset_id = update.get("id")
                                    enabled = update.get("enabled")
                                    if isinstance(asset_id, int) and 0 <= asset_id < ASSET_COUNT and isinstance(enabled, bool):
                                        asset_enabled[asset_id] = enabled
                                        if enabled and asset_id == menu_asset_id:
                                            last_menu_activity_monotonic = time.monotonic()
                            elif manual_asset_updates_pending:
                                manual_asset_updates_pending = False

                        gamma_command = message.get("gamma")
                        if isinstance(gamma_command, str):
                            try:
                                parsed_gamma_enabled, parsed_gamma_values = parse_gamma_command(gamma_command)
                            except ValueError as exc:
                                status = f"Ignored invalid gamma command: {exc}"
                            else:
                                gamma_enabled = parsed_gamma_enabled
                                if parsed_gamma_values is not None:
                                    (
                                        gamma_value,
                                        gamma_lift,
                                        gamma_gain,
                                        gamma_r,
                                        gamma_g,
                                        gamma_b,
                                    ) = parsed_gamma_values
                                manual_gamma_command = current_gamma_command(
                                    gamma_enabled,
                                    gamma_value,
                                    gamma_lift,
                                    gamma_gain,
                                    gamma_r,
                                    gamma_g,
                                    gamma_b,
                                )
                                manual_gamma_pending = True
                                last_menu_activity_monotonic = time.monotonic()
                                dirty = True

                        if isinstance(message.get("destinations"), list):
                            destination_payload = message["destinations"]
                            requested_destinations = [
                                parsed
                                for item in destination_payload
                                for parsed in [parse_udp_destination(item)]
                                if parsed is not None
                            ]
                            if not destination_payload:
                                destinations = []
                                status = "UDP destinations updated (0)"
                                last_menu_activity_monotonic = time.monotonic()
                                dirty = True
                            elif requested_destinations:
                                destinations = dedupe_destinations(requested_destinations)
                                status = f"UDP destinations updated ({len(destinations)})"
                                last_menu_activity_monotonic = time.monotonic()
                                dirty = True
                            else:
                                status = "Ignored invalid UDP destinations payload"

                        text_overrides = message.get("texts")
                        if isinstance(text_overrides, list):
                            parsed_texts: List[Optional[str]] = [None] * MAX_OSD_SLOTS
                            for idx in range(min(MAX_OSD_SLOTS, len(text_overrides))):
                                item = text_overrides[idx]
                                if item is None:
                                    parsed_texts[idx] = None
                                elif isinstance(item, str):
                                    parsed_texts[idx] = clamp_text(item)
                                else:
                                    parsed_texts[idx] = clamp_text(str(item))
                            if parsed_texts != manual_text_overrides:
                                manual_text_overrides = parsed_texts
                                manual_text_overrides_pending = True
                                dirty = True

                        value_overrides = message.get("values")
                        if isinstance(value_overrides, list):
                            parsed_values: List[Optional[float]] = [None] * MAX_OSD_SLOTS
                            for idx in range(min(MAX_OSD_SLOTS, len(value_overrides))):
                                item = value_overrides[idx]
                                if item is None:
                                    parsed_values[idx] = None
                                elif isinstance(item, (int, float)):
                                    parsed_values[idx] = clamp_osd_value(float(item))
                            if parsed_values != manual_value_overrides:
                                manual_value_overrides = parsed_values
                                manual_value_overrides_pending = True
                                dirty = True

                        zoom_command = message.get("zoom")
                        if isinstance(zoom_command, str):
                            try:
                                parsed_enabled, parsed_percent = parse_zoom_command(zoom_command)
                            except ValueError:
                                pass
                            else:
                                zoom_enabled = parsed_enabled
                                zoom_percent = max(100, min(zoom_max, parsed_percent))
                                last_menu_activity_monotonic = time.monotonic()
                                dirty = True

                    menu_visible = asset_enabled[menu_asset_id] is True
                    if active_source is not None:
                        polled_keys = poll_source_remote_keys(source_states[active_source], menu_visible, now)
                        remote_keys.extend(polled_keys)
                        if polled_keys:
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                        if verbose:
                            maybe_emit_source_debug_log(source_states[active_source], active_source, polled_keys)
                        if radio_rules:
                            triggered_radio_statuses = evaluate_radio_rules(
                                source_states[active_source],
                                radio_rules,
                                radio_rule_states_by_source[active_source],
                                now,
                                action_timeout_ms,
                                action_shell,
                            )
                            if triggered_radio_statuses:
                                status = triggered_radio_statuses[-1]
                                dirty = True

                    now = time.monotonic()
                    menu_visible = asset_enabled[menu_asset_id] is True
                    if menu_visible and (now - last_menu_activity_monotonic) >= MENU_INACTIVITY_TIMEOUT_S:
                        set_asset_enabled(menu_asset_id, False)
                        current_section = ""
                        selected = 0
                        status = f"Menu auto-hidden after {int(MENU_INACTIVITY_TIMEOUT_S)}s inactivity"
                        dirty = True
                        last_menu_activity_monotonic = now

                    webui_bridge.broadcast(
                        build_webui_state(
                            menu_window,
                            entries,
                            selected,
                            current_section,
                            status,
                            asset_enabled,
                            zoom_enabled,
                            zoom_percent,
                            gamma_enabled,
                            gamma_value,
                            gamma_lift,
                            gamma_gain,
                            gamma_r,
                            gamma_g,
                            gamma_b,
                            destinations,
                            source_states,
                            active_source,
                            priority_source,
                            fallback_timeout_s,
                            sse_url,
                            asset_update_tx_count,
                            asset_update_tx_last_monotonic,
                            asset_update_tx_last_batch_size,
                        )
                    )

                    if dirty or (now - last_send_monotonic) * 1000.0 >= interval_ms:
                        payload = build_payload(menu_window, zoom_enabled, zoom_percent)
                        applied_text_overrides = False
                        applied_value_overrides = False
                        applied_state_asset_updates = False
                        applied_asset_updates = False
                        applied_gamma = False

                        if pending_asset_updates:
                            payload["asset_updates"] = [
                                {"id": asset_id, "enabled": enabled}
                                for asset_id, enabled in sorted(pending_asset_updates.items())
                            ]
                            applied_state_asset_updates = True

                        if manual_text_overrides_pending and any(item is not None for item in manual_text_overrides):
                            texts_payload = payload.get("texts")
                            if not isinstance(texts_payload, list):
                                texts_payload = [None] * MAX_OSD_SLOTS
                            if len(texts_payload) < MAX_OSD_SLOTS:
                                texts_payload = texts_payload + [None] * (MAX_OSD_SLOTS - len(texts_payload))
                            for idx, item in enumerate(manual_text_overrides):
                                if item is not None:
                                    texts_payload[idx] = item
                            payload["texts"] = texts_payload
                            applied_text_overrides = True

                        if manual_value_overrides_pending and any(item is not None for item in manual_value_overrides):
                            payload["values"] = [item for item in manual_value_overrides]
                            applied_value_overrides = True

                        if manual_asset_updates_pending and manual_asset_updates:
                            base_updates = payload.get("asset_updates")
                            merged_updates: List[dict] = []
                            if isinstance(base_updates, list):
                                for update in base_updates:
                                    if isinstance(update, dict):
                                        merged_updates.append(dict(update))
                            merged_updates.extend(dict(update) for update in manual_asset_updates)
                            payload["asset_updates"] = merged_updates
                            applied_asset_updates = True

                        if manual_gamma_pending and manual_gamma_command:
                            payload["gamma"] = manual_gamma_command
                            applied_gamma = True

                        failures = send_payloads(sock, destinations, payload)
                        last_send_monotonic = now
                        if failures:
                            status = f"Send partial failure ({len(failures)}/{len(destinations)}): {clamp_text(failures[0])}"
                        else:
                            sent_asset_updates = payload.get("asset_updates")
                            if isinstance(sent_asset_updates, list):
                                asset_update_tx_count += 1
                                asset_update_tx_last_monotonic = now
                                asset_update_tx_last_batch_size = len(sent_asset_updates)
                            if applied_text_overrides:
                                manual_text_overrides_pending = False
                            if applied_value_overrides:
                                manual_value_overrides_pending = False
                            if applied_state_asset_updates:
                                pending_asset_updates.clear()
                            if applied_asset_updates:
                                manual_asset_updates_pending = False
                            if applied_gamma:
                                manual_gamma_pending = False
                            dirty = False

                    if status != last_logged_status:
                        emit_status(status)
                        last_logged_status = status

                    if not remote_keys:
                        time.sleep(0.05)
                        continue

                    key = remote_keys.pop(0)

                    if key == KEY_MENU_TOGGLE:
                        current_state = asset_enabled[menu_asset_id]
                        next_state = True if current_state is None else (not current_state)
                        set_asset_enabled(menu_asset_id, next_state)
                        current_section = ""
                        selected = 0
                        status = "Menu overlay visible" if next_state else "Menu overlay hidden"
                        if next_state:
                            last_menu_activity_monotonic = time.monotonic()
                        dirty = True
                        continue

                    if key == KEY_UP:
                        if selected > 0:
                            selected -= 1
                        last_menu_activity_monotonic = time.monotonic()
                        dirty = True
                        continue

                    if key == KEY_DOWN:
                        if selected + 1 < len(entries):
                            selected += 1
                        last_menu_activity_monotonic = time.monotonic()
                        dirty = True
                        continue

                    if key in (KEY_SELECT, 13, ord(" ")):
                        entry = entries[selected]

                        if entry.kind == "exit":
                            set_asset_enabled(menu_asset_id, False)
                            current_section = ""
                            selected = 0
                            status = "Menu overlay hidden"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                            continue

                        if entry.kind == "section":
                            current_section = entry.section
                            selected = 0
                            status = f"Opened [{current_section}]"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                            continue

                        if entry.kind == "return":
                            if current_section:
                                prev_section = current_section
                                current_section = ""
                                selected = 0
                                for idx, top_entry in enumerate(top_entries):
                                    if top_entry.kind == "section" and top_entry.section == prev_section:
                                        selected = idx
                                        break
                                status = "Returned to ROOT"
                                last_menu_activity_monotonic = time.monotonic()
                                dirty = True
                            continue

                        if entry.kind == "asset" and entry.asset_id >= 0:
                            current_state = asset_enabled[entry.asset_id]
                            next_state = True if current_state is None else (not current_state)
                            set_asset_enabled(entry.asset_id, next_state)
                            status = f"Asset {entry.asset_id} {'ON' if next_state else 'OFF'}"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                            continue

                        if entry.kind == "zoom_in":
                            zoom_percent = min(zoom_max, zoom_percent + zoom_step)
                            zoom_enabled = zoom_percent > 100
                            status = f"Zoom set to {zoom_state_text(zoom_enabled, zoom_percent)}"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                            continue

                        if entry.kind == "zoom_out":
                            zoom_percent = max(100, zoom_percent - zoom_step)
                            if zoom_percent <= 100:
                                zoom_enabled = False
                            status = f"Zoom set to {zoom_state_text(zoom_enabled, zoom_percent)}"
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                            continue

                        if entry.kind == "action" and entry.action is not None:
                            status = execute_action(entry.action, action_timeout_ms, action_shell)
                            last_menu_activity_monotonic = time.monotonic()
                            dirty = True
                            continue
            finally:
                sse_stop.set()
                sse_reader.join(timeout=1.5)

                try:
                    clear_texts: List[Optional[str]] = [None] * MAX_OSD_SLOTS
                    clear_texts[MENU_TEXT_SLOT_START + 0] = ""
                    clear_texts[MENU_TEXT_SLOT_START + 1] = ""
                    clear_texts[MENU_TEXT_SLOT_START + 2] = ""
                    send_payloads(sock, destinations, {"texts": clear_texts})
                except OSError:
                    pass

                try:
                    send_payloads(sock, destinations, {"asset_updates": [{"id": menu_asset_id, "enabled": False}]})
                except OSError:
                    pass

                webui_bridge.close()

        return 0
    finally:
        signal.signal(signal.SIGINT, prev_sigint)
        signal.signal(signal.SIGTERM, prev_sigterm)


_CONFIG_DEFAULTS: Dict[str, object] = {
    "host": "127.0.0.1",
    "port": 5005,
    "interval_ms": 400,
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
    "verbose": False,
}

_TUNING_DEFAULTS: Dict[str, object] = {
    "crsf_axis_deadband": 120,
    "crsf_action_threshold": 1400,
    "crsf_menu_toggle_ch_low_max": 500,
    "crsf_menu_toggle_ch4_min": 1500,
    "crsf_nav_debounce_ms": 100,
    "crsf_select_debounce_ms": 250,
    "crsf_menu_toggle_hold_s": 1.0,
    "crsf_sample_interval_ms": 40,
    "radio_trigger_debounce_ms": 100,
    "radio_reset_debounce_ms": 0,
    "menu_inactivity_timeout_s": 30.0,
    "source_stale_s": 1.0,
}


def load_config(path: str) -> dict:
    """Read *path* as JSON and return a validated config dict with defaults applied."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw = json.load(fh)
    except FileNotFoundError:
        raise SystemExit(f"config file not found: {path}")
    except json.JSONDecodeError as exc:
        raise SystemExit(f"config JSON parse error in {path}: {exc}")

    if not isinstance(raw, dict):
        raise SystemExit(f"config file must contain a JSON object, got {type(raw).__name__}")

    cfg: dict = {}
    for key, default in _CONFIG_DEFAULTS.items():
        cfg[key] = raw.get(key, copy.deepcopy(default))

    tuning_raw = raw.get("tuning", {})
    if not isinstance(tuning_raw, dict):
        raise SystemExit("'tuning' must be a JSON object")
    tuning: dict = {}
    for key, default in _TUNING_DEFAULTS.items():
        tuning[key] = tuning_raw.get(key, default)
    cfg["tuning"] = tuning

    # --- type coercion and validation ---
    cfg["port"] = int(cfg["port"])
    if cfg["port"] <= 0 or cfg["port"] > 65535:
        raise SystemExit("'port' must be in range 1..65535")

    cfg["interval_ms"] = int(cfg["interval_ms"])
    if cfg["interval_ms"] <= 0:
        raise SystemExit("'interval_ms' must be > 0")

    cfg["zoom_step"] = int(cfg["zoom_step"])
    if cfg["zoom_step"] <= 0:
        raise SystemExit("'zoom_step' must be > 0")

    cfg["zoom_max"] = int(cfg["zoom_max"])
    if cfg["zoom_max"] < 100:
        raise SystemExit("'zoom_max' must be >= 100")

    cfg["action_timeout_ms"] = int(cfg["action_timeout_ms"])
    if cfg["action_timeout_ms"] <= 0:
        raise SystemExit("'action_timeout_ms' must be > 0")

    cfg["menu_asset_id"] = int(cfg["menu_asset_id"])
    if cfg["menu_asset_id"] < 0 or cfg["menu_asset_id"] >= ASSET_COUNT:
        raise SystemExit(f"'menu_asset_id' must be in range 0..{ASSET_COUNT - 1}")

    cfg["webui_port"] = int(cfg["webui_port"])
    if cfg["webui_port"] <= 0 or cfg["webui_port"] > 65535:
        raise SystemExit("'webui_port' must be in range 1..65535")

    cfg["priority_fallback_s"] = float(cfg["priority_fallback_s"])
    if cfg["priority_fallback_s"] <= 0:
        raise SystemExit("'priority_fallback_s' must be > 0")

    if cfg["priority"] not in ("serial", "joystick"):
        raise SystemExit("'priority' must be 'serial' or 'joystick'")

    cfg["verbose"] = bool(cfg["verbose"])

    # initial_off: accept JSON array of ints
    initial_off_raw = cfg["initial_off"]
    if isinstance(initial_off_raw, str):
        try:
            cfg["initial_off"] = parse_asset_id_list(initial_off_raw)
        except ValueError as exc:
            raise SystemExit(f"'initial_off' parse error: {exc}") from exc
    elif isinstance(initial_off_raw, list):
        parsed_off: Set[int] = set()
        for item in initial_off_raw:
            aid = int(item)
            if aid < 0 or aid >= ASSET_COUNT:
                raise SystemExit(f"'initial_off' asset id {aid} out of range (0..{ASSET_COUNT - 1})")
            parsed_off.add(aid)
        cfg["initial_off"] = parsed_off
    else:
        raise SystemExit("'initial_off' must be an array of asset ids")

    # extra_destinations: accept JSON array of "host:port" strings
    extra_raw = cfg["extra_destinations"]
    if not isinstance(extra_raw, list):
        raise SystemExit("'extra_destinations' must be an array of 'host:port' strings")
    try:
        cfg["extra_destinations"] = [parse_destination_spec(spec) for spec in extra_raw if str(spec).strip()]
    except ValueError as exc:
        raise SystemExit(f"'extra_destinations' parse error: {exc}") from exc

    return cfg


def apply_tuning(tuning: dict) -> None:
    """Override module-level tuning constants from the config's tuning section."""
    global CRSF_AXIS_DEADBAND, CRSF_ACTION_THRESHOLD
    global CRSF_MENU_TOGGLE_CH_LOW_MAX, CRSF_MENU_TOGGLE_CH4_MIN
    global CRSF_NAV_DEBOUNCE_MS, CRSF_SELECT_DEBOUNCE_MS
    global CRSF_MENU_TOGGLE_HOLD_S, CRSF_SAMPLE_INTERVAL_MS
    global RADIO_TRIGGER_DEBOUNCE_MS, RADIO_RESET_DEBOUNCE_MS
    global MENU_INACTIVITY_TIMEOUT_S, SOURCE_STALE_S

    CRSF_AXIS_DEADBAND = int(tuning["crsf_axis_deadband"])
    CRSF_ACTION_THRESHOLD = int(tuning["crsf_action_threshold"])
    CRSF_MENU_TOGGLE_CH_LOW_MAX = int(tuning["crsf_menu_toggle_ch_low_max"])
    CRSF_MENU_TOGGLE_CH4_MIN = int(tuning["crsf_menu_toggle_ch4_min"])
    CRSF_NAV_DEBOUNCE_MS = int(tuning["crsf_nav_debounce_ms"])
    CRSF_SELECT_DEBOUNCE_MS = int(tuning["crsf_select_debounce_ms"])
    CRSF_MENU_TOGGLE_HOLD_S = float(tuning["crsf_menu_toggle_hold_s"])
    CRSF_SAMPLE_INTERVAL_MS = int(tuning["crsf_sample_interval_ms"])
    RADIO_TRIGGER_DEBOUNCE_MS = int(tuning["radio_trigger_debounce_ms"])
    RADIO_RESET_DEBOUNCE_MS = int(tuning["radio_reset_debounce_ms"])
    MENU_INACTIVITY_TIMEOUT_S = float(tuning["menu_inactivity_timeout_s"])
    SOURCE_STALE_S = float(tuning["source_stale_s"])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "SSE-driven remote menu that sends PixelPilot external OSD texts + "
            "asset updates over UDP and serves an HTTP JSON WebUI endpoint. "
            "All settings are read from a JSON config file."
        )
    )
    default_config = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")
    parser.add_argument(
        "--config",
        default=default_config,
        help=f"Path to JSON config file (default: {default_config})",
    )
    args = parser.parse_args()

    cfg = load_config(args.config)
    apply_tuning(cfg["tuning"])

    actions_ini_path = str(cfg["actions_ini"]).strip()
    if not actions_ini_path:
        bundled_actions_ini = os.path.join(os.path.dirname(os.path.abspath(__file__)), "menu.ini")
        if os.path.isfile(bundled_actions_ini):
            actions_ini_path = bundled_actions_ini

    try:
        action_sections, actions_by_section, radio_rules = load_actions(actions_ini_path)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    action_shell = resolve_action_shell(str(cfg["action_shell"]))
    if not os.path.isfile(action_shell):
        raise SystemExit(f"action shell does not exist: {action_shell}")
    if not os.access(action_shell, os.X_OK):
        raise SystemExit(f"action shell is not executable: {action_shell}")

    return run_controller(
        str(cfg["host"]),
        cfg["port"],
        cfg["interval_ms"],
        cfg["initial_off"],
        cfg["zoom_step"],
        cfg["zoom_max"],
        action_sections,
        actions_by_section,
        radio_rules,
        cfg["action_timeout_ms"],
        action_shell,
        cfg["menu_asset_id"],
        str(cfg["webui_host"]),
        cfg["webui_port"],
        str(cfg["sse_url"]),
        str(cfg["priority"]),
        cfg["priority_fallback_s"],
        cfg["extra_destinations"],
        cfg["verbose"],
    )


if __name__ == "__main__":
    raise SystemExit(main())
