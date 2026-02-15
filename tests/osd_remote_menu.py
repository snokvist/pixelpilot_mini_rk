#!/usr/bin/env python3
"""Ncurses + WebUI remote menu driver for PixelPilot OSD external UDP control."""

from __future__ import annotations

import argparse
import base64
import configparser
import curses
import hashlib
import json
import os
import select
import shutil
import signal
import socket
import struct
import subprocess
import time
from dataclasses import dataclass
from typing import Dict, List, Mapping, Optional, Sequence, Set, Tuple

MAX_OSD_SLOTS = 8
MAX_OSD_TEXT_CHARS = 63
ASSET_COUNT = 8
MENU_TEXT_SLOT_START = MAX_OSD_SLOTS - 3  # ext.text6 (1-based) / index 5

SECTION_ASSETS = "ASSETS"
SECTION_ZOOM = "ZOOM"
RESERVED_SECTIONS = {SECTION_ASSETS, SECTION_ZOOM}

STOP_REQUESTED = False
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


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
class WebSocketClient:
    sock: socket.socket
    handshake_done: bool = False
    handshake_buffer: bytes = b""
    frame_buffer: bytes = b""


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


def load_actions(path: str) -> Tuple[List[str], Dict[str, List[MenuAction]]]:
    if not path:
        return [], {}

    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    loaded = parser.read(path)
    if not loaded:
        raise ValueError(f"failed to read actions ini: {path}")

    section_order: List[str] = []
    actions_by_section: Dict[str, List[MenuAction]] = {}

    for section in parser.sections():
        section_name = section.strip()
        if not section_name:
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

    return section_order, actions_by_section


def clamp_text(text: str) -> str:
    if len(text) <= MAX_OSD_TEXT_CHARS:
        return text
    return text[: MAX_OSD_TEXT_CHARS - 3] + "..."


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


def build_payload(menu_window: Tuple[str, str, str], asset_enabled: Sequence[Optional[bool]], zoom_enabled: bool, zoom_percent: int) -> dict:
    texts: List[Optional[str]] = [None] * MAX_OSD_SLOTS
    texts[MENU_TEXT_SLOT_START + 0] = clamp_text(menu_window[0])
    texts[MENU_TEXT_SLOT_START + 1] = clamp_text(menu_window[1])
    texts[MENU_TEXT_SLOT_START + 2] = clamp_text(menu_window[2])

    asset_updates = [
        {"id": asset_id, "enabled": asset_enabled[asset_id]}
        for asset_id in range(ASSET_COUNT)
        if asset_enabled[asset_id] is not None
    ]

    payload = {"texts": texts, "zoom": current_zoom_command(zoom_enabled, zoom_percent)}
    if asset_updates:
        payload["asset_updates"] = asset_updates
    return payload


def send_payload(sock: socket.socket, host: str, port: int, payload: dict) -> None:
    sock.sendto(json.dumps(payload, separators=(",", ":")).encode("utf-8"), (host, port))


class WebUiBridge:
    """Small non-blocking WebSocket server for browser control."""

    def __init__(self, host: str, port: int) -> None:
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind((host, port))
        self._server.listen()
        self._server.setblocking(False)
        self._clients: List[WebSocketClient] = []

    def close(self) -> None:
        for client in self._clients:
            try:
                client.sock.close()
            except OSError:
                pass
        self._clients.clear()
        self._server.close()

    def poll_messages(self) -> List[dict]:
        messages: List[dict] = []
        self._accept_new_clients()
        if not self._clients:
            return messages

        readable, _, _ = select.select([client.sock for client in self._clients], [], [], 0)
        for sock_obj in readable:
            client = self._find_client(sock_obj)
            if client is None:
                continue
            try:
                chunk = sock_obj.recv(4096)
            except OSError:
                chunk = b""
            if not chunk:
                self._drop_client(client)
                continue

            if not client.handshake_done:
                client.handshake_buffer += chunk
                if b"\r\n\r\n" in client.handshake_buffer:
                    if not self._finish_handshake(client):
                        self._drop_client(client)
                continue

            client.frame_buffer += chunk
            frame_messages, remainder, should_close = self._decode_frames(client.frame_buffer)
            client.frame_buffer = remainder
            for frame_text in frame_messages:
                try:
                    payload = json.loads(frame_text)
                except json.JSONDecodeError:
                    continue
                if isinstance(payload, dict):
                    messages.append(payload)
            if should_close:
                self._drop_client(client)
        return messages

    def broadcast(self, payload: dict) -> None:
        if not self._clients:
            return
        encoded = json.dumps(payload, separators=(",", ":"))
        frame = self._encode_text_frame(encoded)
        for client in list(self._clients):
            if not client.handshake_done:
                continue
            try:
                client.sock.sendall(frame)
            except OSError:
                self._drop_client(client)

    def _accept_new_clients(self) -> None:
        while True:
            try:
                sock_obj, _addr = self._server.accept()
            except BlockingIOError:
                break
            sock_obj.setblocking(False)
            self._clients.append(WebSocketClient(sock=sock_obj))

    def _find_client(self, sock_obj: socket.socket) -> Optional[WebSocketClient]:
        for client in self._clients:
            if client.sock is sock_obj:
                return client
        return None

    def _drop_client(self, client: WebSocketClient) -> None:
        try:
            client.sock.close()
        except OSError:
            pass
        if client in self._clients:
            self._clients.remove(client)

    def _finish_handshake(self, client: WebSocketClient) -> bool:
        try:
            request = client.handshake_buffer.decode("utf-8", errors="replace")
            header_block = request.split("\r\n\r\n", 1)[0]
            headers = {}
            for line in header_block.split("\r\n")[1:]:
                if ":" not in line:
                    continue
                key, value = line.split(":", 1)
                headers[key.strip().lower()] = value.strip()
            key = headers.get("sec-websocket-key", "")
            if not key:
                return False
            accept_raw = hashlib.sha1((key + WS_GUID).encode("utf-8")).digest()
            accept_value = base64.b64encode(accept_raw).decode("ascii")
            response = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept_value}\r\n\r\n"
            )
            client.sock.sendall(response.encode("utf-8"))
            client.handshake_done = True
            client.handshake_buffer = b""
            return True
        except OSError:
            return False

    def _decode_frames(self, data: bytes) -> Tuple[List[str], bytes, bool]:
        messages: List[str] = []
        idx = 0
        should_close = False
        total = len(data)

        while idx + 2 <= total:
            b1 = data[idx]
            b2 = data[idx + 1]
            opcode = b1 & 0x0F
            masked = (b2 & 0x80) != 0
            payload_len = b2 & 0x7F
            idx += 2

            if payload_len == 126:
                if idx + 2 > total:
                    idx -= 2
                    break
                payload_len = struct.unpack("!H", data[idx : idx + 2])[0]
                idx += 2
            elif payload_len == 127:
                if idx + 8 > total:
                    idx -= 2
                    break
                payload_len = struct.unpack("!Q", data[idx : idx + 8])[0]
                idx += 8

            if not masked:
                should_close = True
                break

            if idx + 4 + payload_len > total:
                idx -= 2
                if payload_len == 126:
                    idx -= 2
                elif payload_len == 127:
                    idx -= 8
                break

            mask = data[idx : idx + 4]
            idx += 4
            payload = data[idx : idx + payload_len]
            idx += payload_len
            decoded = bytes(payload[i] ^ mask[i % 4] for i in range(payload_len))

            if opcode == 0x8:
                should_close = True
                break
            if opcode == 0x1:
                messages.append(decoded.decode("utf-8", errors="replace"))

        return messages, data[idx:], should_close

    def _encode_text_frame(self, text: str) -> bytes:
        payload = text.encode("utf-8")
        length = len(payload)
        if length <= 125:
            header = bytes([0x81, length])
        elif length <= 65535:
            header = bytes([0x81, 126]) + struct.pack("!H", length)
        else:
            header = bytes([0x81, 127]) + struct.pack("!Q", length)
        return header + payload


def parse_zoom_command(command: str) -> Tuple[bool, int]:
    value = command.strip().lower()
    if not value or value == "off":
        return False, 100
    zoom_percent = int(value.split(",", 1)[0].strip())
    if zoom_percent <= 100:
        return False, 100
    return True, zoom_percent


def build_webui_state(
    menu_window: Tuple[str, str, str],
    entries: Sequence[MenuEntry],
    selected: int,
    current_section: str,
    status: str,
    asset_enabled: Sequence[Optional[bool]],
    zoom_enabled: bool,
    zoom_percent: int,
) -> dict:
    return {
        "type": "menu_state",
        "section": current_section or "ROOT",
        "selected": selected,
        "entries": [display_entry(entry, asset_enabled, zoom_enabled, zoom_percent) for entry in entries],
        "menu_window": list(menu_window),
        "status": status,
        "asset_enabled": list(asset_enabled),
        "zoom": current_zoom_command(zoom_enabled, zoom_percent),
    }


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


def draw_ui(
    stdscr: "curses._CursesWindow",
    menu_window: Tuple[str, str, str],
    status: str,
    host: str,
    port: int,
    interval_ms: int,
    zoom_enabled: bool,
    zoom_percent: int,
    section_count: int,
    current_section: str,
) -> None:
    stdscr.erase()
    height, width = stdscr.getmaxyx()

    def safe_addnstr(row: int, col: int, text: str) -> None:
        if row < 0 or row >= height:
            return
        try:
            stdscr.addnstr(row, col, text, max(1, width - 1))
        except curses.error:
            pass

    level = "ROOT" if current_section == "" else f"[{current_section}]"
    header = (
        f"OSD menu {level} -> {host}:{port} tx={interval_ms}ms "
        f"zoom={zoom_state_text(zoom_enabled, zoom_percent)} sections={section_count}"
    )
    safe_addnstr(0, 0, header)
    safe_addnstr(1, 0, "-" * max(1, width - 1))
    safe_addnstr(2, 0, "3-row menu view (matches OSD ext.text6/7/8):")
    safe_addnstr(4, 0, f"6: {menu_window[0]}")
    safe_addnstr(5, 0, f"7: {menu_window[1]}")
    safe_addnstr(6, 0, f"8: {menu_window[2]}")
    safe_addnstr(height - 2, 0, status)
    safe_addnstr(height - 1, 0, "Keys: Up/Down or J/K, Enter/Space select, A/Z in [ASSETS], Q/Ctrl+C or EXIT")
    stdscr.refresh()


def run_controller(
    stdscr: Optional["curses._CursesWindow"],
    host: str,
    port: int,
    interval_ms: int,
    initial_off: Set[int],
    zoom_step: int,
    zoom_max: int,
    action_sections: Sequence[str],
    actions_by_section: Dict[str, List[MenuAction]],
    action_timeout_ms: int,
    action_shell: str,
    menu_asset_id: int,
    webui_host: str,
    webui_port: int,
) -> int:
    global STOP_REQUESTED
    STOP_REQUESTED = False

    prev_sigint = signal.getsignal(signal.SIGINT)
    signal.signal(signal.SIGINT, _on_sigint)

    try:
        if stdscr is not None:
            try:
                curses.curs_set(0)
            except curses.error:
                pass
            stdscr.nodelay(True)
            stdscr.timeout(50)

        asset_enabled: List[Optional[bool]] = [None] * ASSET_COUNT
        for asset_id in initial_off:
            asset_enabled[asset_id] = False
        asset_enabled[menu_asset_id] = True

        zoom_enabled = False
        zoom_percent = 100
        top_entries = build_top_entries(action_sections)
        submenu_table = build_submenu_table(action_sections, actions_by_section)
        fallback_entries = [MenuEntry(kind="return")]
        current_section = ""
        entries = top_entries
        selected = 0
        status = f"Ready (WebUI ws://{webui_host}:{webui_port})"
        dirty = True
        last_send_monotonic = 0.0
        remote_keys: List[int] = []

        webui_bridge = WebUiBridge(webui_host, webui_port)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            try:
                while not STOP_REQUESTED:
                    entries = top_entries if current_section == "" else submenu_table.get(current_section, fallback_entries)
                    selected = min(max(selected, 0), max(0, len(entries) - 1))

                    menu_window = build_three_slot_menu_texts(entries, selected, asset_enabled, zoom_enabled, zoom_percent)

                    if stdscr is not None:
                        draw_ui(
                            stdscr,
                            menu_window,
                            status,
                            host,
                            port,
                            interval_ms,
                            zoom_enabled,
                            zoom_percent,
                            len(top_entries),
                            current_section,
                        )

                    for message in webui_bridge.poll_messages():
                        key_map = {
                            "up": curses.KEY_UP,
                            "down": curses.KEY_DOWN,
                            "select": 10,
                            "enter": 10,
                            "quit": ord("q"),
                            "all_on": ord("a"),
                            "all_off": ord("z"),
                        }
                        key_name = str(message.get("key", "")).strip().lower()
                        if key_name in key_map:
                            remote_keys.append(key_map[key_name])

                        selected_value = message.get("selected")
                        if isinstance(selected_value, int):
                            selected = min(max(selected_value, 0), max(0, len(entries) - 1))
                            dirty = True

                        if isinstance(message.get("asset_updates"), list):
                            for update in message["asset_updates"]:
                                if not isinstance(update, dict):
                                    continue
                                asset_id = update.get("id")
                                enabled = update.get("enabled")
                                if isinstance(asset_id, int) and 0 <= asset_id < ASSET_COUNT and isinstance(enabled, bool):
                                    asset_enabled[asset_id] = enabled
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
                                dirty = True

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
                        )
                    )

                    now = time.monotonic()
                    if dirty or (now - last_send_monotonic) * 1000.0 >= interval_ms:
                        payload = build_payload(menu_window, asset_enabled, zoom_enabled, zoom_percent)
                        try:
                            send_payload(sock, host, port, payload)
                            last_send_monotonic = now
                            dirty = False
                        except OSError as exc:
                            status = f"Send failed: {exc}"
                            last_send_monotonic = now

                    if remote_keys:
                        key = remote_keys.pop(0)
                    elif stdscr is not None:
                        key = stdscr.getch()
                    else:
                        time.sleep(0.05)
                        continue

                    if key < 0:
                        continue

                    if key in (ord("q"), ord("Q"), 27, 3):
                        STOP_REQUESTED = True
                        break

                    if key in (curses.KEY_UP, ord("k"), ord("K")):
                        if selected > 0:
                            selected -= 1
                        dirty = True
                        continue

                    if key in (curses.KEY_DOWN, ord("j"), ord("J")):
                        if selected + 1 < len(entries):
                            selected += 1
                        dirty = True
                        continue

                    if key in (ord("a"), ord("A")):
                        if current_section == SECTION_ASSETS:
                            for asset_id in range(ASSET_COUNT):
                                asset_enabled[asset_id] = True
                            status = "All assets ON"
                            dirty = True
                        else:
                            status = "A/Z available in [ASSETS]"
                        continue

                    if key in (ord("z"), ord("Z")):
                        if current_section == SECTION_ASSETS:
                            for asset_id in range(ASSET_COUNT):
                                asset_enabled[asset_id] = False
                            status = "All assets OFF"
                            dirty = True
                        else:
                            status = "A/Z available in [ASSETS]"
                        continue

                    if key in (curses.KEY_ENTER, 10, 13, ord(" ")):
                        entry = entries[selected]

                        if entry.kind == "exit":
                            STOP_REQUESTED = True
                            break

                        if entry.kind == "section":
                            current_section = entry.section
                            selected = 0
                            status = f"Opened [{current_section}]"
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
                                dirty = True
                            continue

                        if entry.kind == "asset" and entry.asset_id >= 0:
                            current_state = asset_enabled[entry.asset_id]
                            next_state = True if current_state is None else (not current_state)
                            asset_enabled[entry.asset_id] = next_state
                            status = f"Asset {entry.asset_id} {'ON' if next_state else 'OFF'}"
                            dirty = True
                            continue

                        if entry.kind == "zoom_in":
                            zoom_percent = min(zoom_max, zoom_percent + zoom_step)
                            zoom_enabled = zoom_percent > 100
                            status = f"Zoom set to {zoom_state_text(zoom_enabled, zoom_percent)}"
                            dirty = True
                            continue

                        if entry.kind == "zoom_out":
                            zoom_percent = max(100, zoom_percent - zoom_step)
                            if zoom_percent <= 100:
                                zoom_enabled = False
                            status = f"Zoom set to {zoom_state_text(zoom_enabled, zoom_percent)}"
                            dirty = True
                            continue

                        if entry.kind == "action" and entry.action is not None:
                            status = execute_action(entry.action, action_timeout_ms, action_shell)
                            dirty = True
                            continue
            finally:
                try:
                    clear_texts: List[Optional[str]] = [None] * MAX_OSD_SLOTS
                    clear_texts[MENU_TEXT_SLOT_START + 0] = ""
                    clear_texts[MENU_TEXT_SLOT_START + 1] = ""
                    clear_texts[MENU_TEXT_SLOT_START + 2] = ""
                    send_payload(sock, host, port, {"texts": clear_texts})
                except OSError:
                    pass

                try:
                    send_payload(sock, host, port, {"asset_updates": [{"id": menu_asset_id, "enabled": False}]})
                except OSError:
                    pass

                webui_bridge.close()

        return 0
    finally:
        signal.signal(signal.SIGINT, prev_sigint)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Remote menu that sends PixelPilot external OSD texts + asset updates over UDP, "
            "and always hosts a WebSocket WebUI control endpoint"
        )
    )
    parser.add_argument("--host", default="127.0.0.1", help="Target host running pixelpilot_mini_rk")
    parser.add_argument("--port", type=int, default=5005, help="External OSD UDP port (default: 5005)")
    parser.add_argument("--interval-ms", type=int, default=400, help="Re-send interval in milliseconds (default: 400)")
    parser.add_argument("--initial-off", default="", help="Comma-separated asset ids to start OFF (example: '2,5,7')")
    parser.add_argument("--zoom-step", type=int, default=25, help="Zoom percentage step (default: 25)")
    parser.add_argument("--zoom-max", type=int, default=300, help="Maximum zoom percentage (default: 300)")
    parser.add_argument("--actions-ini", default="", help="Optional INI file of local command actions")
    parser.add_argument("--action-timeout-ms", type=int, default=5000, help="Action command timeout in ms (default: 5000)")
    parser.add_argument("--action-shell", default="", help="Shell executable for actions (default: $SHELL, fallback /bin/sh)")
    parser.add_argument("--menu-asset-id", type=int, default=7, help="Asset id of menu widget to force-disable on exit (default: 7)")
    parser.add_argument("--webui-host", default="0.0.0.0", help="WebUI bind host (default: 0.0.0.0)")
    parser.add_argument("--webui-port", type=int, default=6666, help="WebUI WebSocket bind port (default: 6666)")
    parser.add_argument("--webui-only", action="store_true", help="Run without ncurses and serve as WebUI-driven background daemon")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.port <= 0 or args.port > 65535:
        raise SystemExit("--port must be in range 1..65535")
    if args.interval_ms <= 0:
        raise SystemExit("--interval-ms must be > 0")
    if args.zoom_step <= 0:
        raise SystemExit("--zoom-step must be > 0")
    if args.zoom_max < 100:
        raise SystemExit("--zoom-max must be >= 100")
    if args.action_timeout_ms <= 0:
        raise SystemExit("--action-timeout-ms must be > 0")
    if args.menu_asset_id < 0 or args.menu_asset_id >= ASSET_COUNT:
        raise SystemExit(f"--menu-asset-id must be in range 0..{ASSET_COUNT - 1}")
    if args.webui_port <= 0 or args.webui_port > 65535:
        raise SystemExit("--webui-port must be in range 1..65535")

    try:
        initial_off = parse_asset_id_list(args.initial_off)
    except ValueError as exc:
        raise SystemExit(f"--initial-off parse error: {exc}") from exc

    try:
        action_sections, actions_by_section = load_actions(args.actions_ini)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    action_shell = resolve_action_shell(args.action_shell)
    if not os.path.isfile(action_shell):
        raise SystemExit(f"action shell does not exist: {action_shell}")
    if not os.access(action_shell, os.X_OK):
        raise SystemExit(f"action shell is not executable: {action_shell}")

    if args.webui_only:
        return run_controller(
            None,
            args.host,
            args.port,
            args.interval_ms,
            initial_off,
            args.zoom_step,
            args.zoom_max,
            action_sections,
            actions_by_section,
            args.action_timeout_ms,
            action_shell,
            args.menu_asset_id,
            args.webui_host,
            args.webui_port,
        )

    return curses.wrapper(
        run_controller,
        args.host,
        args.port,
        args.interval_ms,
        initial_off,
        args.zoom_step,
        args.zoom_max,
        action_sections,
        actions_by_section,
        args.action_timeout_ms,
        action_shell,
        args.menu_asset_id,
        args.webui_host,
        args.webui_port,
    )


if __name__ == "__main__":
    raise SystemExit(main())
