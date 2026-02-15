#!/usr/bin/env python3
"""Ncurses remote menu driver for PixelPilot OSD external UDP control.

This script simulates a simple on-screen menu locally in ncurses and sends
menu rows + asset visibility states to PixelPilot's external OSD UDP port.

It supports optional INI-driven command actions that execute locally when
selected and activated.
"""

from __future__ import annotations

import argparse
import configparser
import curses
import json
import os
import shutil
import signal
import socket
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
    parser.optionxform = str  # Preserve key casing for display.
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


def build_submenu_entries(
    current_section: str,
    actions_by_section: Mapping[str, List[MenuAction]],
) -> List[MenuEntry]:
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


def display_entry(
    entry: MenuEntry,
    asset_enabled: Sequence[Optional[bool]],
    zoom_enabled: bool,
    zoom_percent: int,
) -> str:
    if entry.kind == "section":
        return f"[{entry.section}]"
    if entry.kind == "exit":
        return "EXIT"
    if entry.kind == "return":
        return "RETURN"
    if entry.kind == "asset" and entry.asset_id >= 0:
        asset_state = asset_enabled[entry.asset_id]
        if asset_state is None:
            state = "?"
        else:
            state = "ON" if asset_state else "OFF"
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

    prev_line = ""
    next_line = ""
    count = len(entries)
    prev_idx = selected - 1
    next_idx = selected + 1
    if prev_idx >= 0:
        prev_line = clamp_text(
            f"  {display_entry(entries[prev_idx], asset_enabled, zoom_enabled, zoom_percent)}"
        )
    if next_idx < count:
        next_line = clamp_text(
            f"  {display_entry(entries[next_idx], asset_enabled, zoom_enabled, zoom_percent)}"
        )

    current_line = clamp_text(f"> {display_entry(entries[selected], asset_enabled, zoom_enabled, zoom_percent)}")
    return prev_line, current_line, next_line


def current_zoom_command(zoom_enabled: bool, zoom_percent: int) -> str:
    if not zoom_enabled or zoom_percent <= 100:
        return "off"
    return f"{zoom_percent},{zoom_percent},50,50"


def build_payload(
    menu_window: Tuple[str, str, str],
    asset_enabled: Sequence[Optional[bool]],
    zoom_enabled: bool,
    zoom_percent: int,
) -> dict:
    texts: List[Optional[str]] = [None] * MAX_OSD_SLOTS
    texts[MENU_TEXT_SLOT_START + 0] = clamp_text(menu_window[0])
    texts[MENU_TEXT_SLOT_START + 1] = clamp_text(menu_window[1])
    texts[MENU_TEXT_SLOT_START + 2] = clamp_text(menu_window[2])

    asset_updates = [
        {"id": asset_id, "enabled": asset_enabled[asset_id]}
        for asset_id in range(ASSET_COUNT)
        if asset_enabled[asset_id] is not None
    ]

    payload = {
        "texts": texts,
        "zoom": current_zoom_command(zoom_enabled, zoom_percent),
    }
    if asset_updates:
        payload["asset_updates"] = asset_updates
    return payload


def send_payload(sock: socket.socket, host: str, port: int, payload: dict) -> None:
    encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    sock.sendto(encoded, (host, port))


def condense_output(text: str) -> str:
    for line in text.splitlines():
        stripped = line.strip()
        if stripped:
            return clamp_text(stripped)
    return ""


def resolve_action_shell(preferred: str) -> str:
    candidate = preferred.strip()
    if not candidate:
        candidate = os.environ.get("SHELL", "").strip()
    if not candidate:
        candidate = "/bin/sh"

    # Allow passing names such as "bash" and resolve them via PATH.
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
        if message:
            return f"OK [{action.section}] {action.name}: {message}"
        return f"OK [{action.section}] {action.name}"
    if message:
        return f"ERR {result.returncode} [{action.section}] {action.name}: {message}"
    return f"ERR {result.returncode} [{action.section}] {action.name}"


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


def run_menu(
    stdscr: "curses._CursesWindow",
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
) -> int:
    global STOP_REQUESTED
    STOP_REQUESTED = False

    prev_sigint = signal.getsignal(signal.SIGINT)
    signal.signal(signal.SIGINT, _on_sigint)

    try:
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        stdscr.nodelay(True)
        stdscr.timeout(50)

        asset_enabled: List[Optional[bool]] = [None] * ASSET_COUNT
        for asset_id in initial_off:
            asset_enabled[asset_id] = False
        # Ensure the menu widget is visible when the controller starts.
        asset_enabled[menu_asset_id] = True

        zoom_enabled = False
        zoom_percent = 100

        top_entries = build_top_entries(action_sections)
        submenu_table = build_submenu_table(action_sections, actions_by_section)
        fallback_entries = [MenuEntry(kind="return")]
        current_section = ""
        entries = top_entries

        selected = 0

        status = "Ready"
        dirty = True
        last_send_monotonic = 0.0

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            try:
                while not STOP_REQUESTED:
                    if current_section == "":
                        entries = top_entries
                    else:
                        entries = submenu_table.get(current_section, fallback_entries)

                    if selected >= len(entries):
                        selected = max(0, len(entries) - 1)
                    if selected < 0:
                        selected = 0

                    menu_window = build_three_slot_menu_texts(
                        entries,
                        selected,
                        asset_enabled,
                        zoom_enabled,
                        zoom_percent,
                    )

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

                    key = stdscr.getch()
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
                            if current_state is None:
                                next_state = True
                            else:
                                next_state = not current_state
                            asset_enabled[entry.asset_id] = next_state
                            state = "ON" if next_state else "OFF"
                            status = f"Asset {entry.asset_id} {state}"
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
                    clear_payload = {"texts": clear_texts}
                    send_payload(sock, host, port, clear_payload)
                except OSError:
                    pass

                # Always hide the menu widget itself on exit.
                try:
                    hide_menu_payload = {
                        "asset_updates": [{"id": menu_asset_id, "enabled": False}],
                    }
                    send_payload(sock, host, port, hide_menu_payload)
                except OSError:
                    pass

        return 0
    finally:
        signal.signal(signal.SIGINT, prev_sigint)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Ncurses remote menu that sends PixelPilot external OSD texts + "
            "asset_updates over UDP"
        )
    )
    parser.add_argument("--host", default="127.0.0.1", help="Target host running pixelpilot_mini_rk")
    parser.add_argument("--port", type=int, default=5005, help="External OSD UDP port (default: 5005)")
    parser.add_argument(
        "--interval-ms",
        type=int,
        default=400,
        help="Re-send interval in milliseconds to refresh OSD state (default: 400)",
    )
    parser.add_argument(
        "--initial-off",
        default="",
        help="Comma-separated list of asset ids to start OFF (for example '2,5,7')",
    )
    parser.add_argument(
        "--zoom-step",
        type=int,
        default=25,
        help="Zoom percentage step for the menu Zoom In/Out rows (default: 25)",
    )
    parser.add_argument(
        "--zoom-max",
        type=int,
        default=300,
        help="Maximum zoom percentage allowed by the menu (default: 300)",
    )
    parser.add_argument(
        "--actions-ini",
        default="",
        help="Optional INI file of local command actions exposed as submenu sections",
    )
    parser.add_argument(
        "--action-timeout-ms",
        type=int,
        default=5000,
        help="Timeout for each action command execution (default: 5000)",
    )
    parser.add_argument(
        "--action-shell",
        default="",
        help="Shell executable for actions (default: $SHELL, fallback /bin/sh)",
    )
    parser.add_argument(
        "--menu-asset-id",
        type=int,
        default=7,
        help="Asset id of the menu widget to force-disable on exit (default: 7)",
    )
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

    return curses.wrapper(
        run_menu,
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
    )


if __name__ == "__main__":
    raise SystemExit(main())
