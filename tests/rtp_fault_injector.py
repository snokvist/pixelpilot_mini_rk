#!/usr/bin/env python3
"""Interactive RTP/UDP fault injector for manual stream recovery testing.

Default behavior:
- listen on UDP :5601
- forward to 127.0.0.1:5600

Use the ncurses-style menu to switch fault modes while watching output.
"""

from __future__ import annotations

import argparse
import curses
import heapq
import random
import socket
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, Tuple


class FaultMode(str, Enum):
    PASSTHROUGH = "passthrough"
    DROP_EVERY_N_PACKETS = "drop_every_n_packets"
    DROP_RANDOM_PACKETS = "drop_random_packets"
    FIXED_PACKET_DELAY = "fixed_packet_delay"
    PACKET_JITTER = "packet_jitter"
    BURST_PACKET_LOSS = "burst_packet_loss"
    DROP_EVERY_N_FRAMES = "drop_every_n_frames"
    DELAY_WHOLE_FRAMES = "delay_whole_frames"


MODE_LIST: List[FaultMode] = [
    FaultMode.PASSTHROUGH,
    FaultMode.DROP_EVERY_N_PACKETS,
    FaultMode.DROP_RANDOM_PACKETS,
    FaultMode.FIXED_PACKET_DELAY,
    FaultMode.PACKET_JITTER,
    FaultMode.BURST_PACKET_LOSS,
    FaultMode.DROP_EVERY_N_FRAMES,
    FaultMode.DELAY_WHOLE_FRAMES,
]


@dataclass(order=True)
class ScheduledPacket:
    send_at: float
    serial: int
    payload: bytes = field(compare=False)


@dataclass
class Stats:
    packets_in: int = 0
    packets_out: int = 0
    packets_dropped: int = 0
    frames_seen: int = 0
    frames_dropped: int = 0


@dataclass
class Config:
    in_port: int
    out_host: str
    out_port: int


@dataclass
class Parameters:
    drop_every_n_packets: int = 30
    random_drop_percent: float = 3.0
    fixed_delay_ms: int = 30
    jitter_base_delay_ms: int = 10
    jitter_delta_ms: int = 20
    burst_period_packets: int = 250
    burst_length_packets: int = 30
    drop_every_n_frames: int = 30
    whole_frame_delay_ms: int = 40


class Injector:
    def __init__(self, cfg: Config, params: Parameters) -> None:
        self.cfg = cfg
        self.params = params
        self.mode_index = 0
        self.mode = MODE_LIST[self.mode_index]

        self.stats = Stats()
        self.packet_counter = 0
        self.current_timestamp: Optional[int] = None
        self.drop_current_frame = False

        self.queue: List[ScheduledPacket] = []
        self._serial = 0

        self.sock_in = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock_in.bind(("0.0.0.0", cfg.in_port))
        self.sock_in.setblocking(False)

        self.sock_out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.out_addr = (cfg.out_host, cfg.out_port)

    def close(self) -> None:
        self.sock_in.close()
        self.sock_out.close()

    def set_mode(self, idx: int) -> None:
        self.mode_index = max(0, min(len(MODE_LIST) - 1, idx))
        self.mode = MODE_LIST[self.mode_index]

    @staticmethod
    def _parse_rtp(packet: bytes) -> Tuple[bool, Optional[int], bool]:
        if len(packet) < 12:
            return False, None, False
        b0 = packet[0]
        version = b0 >> 6
        if version != 2:
            return False, None, False
        csrc_count = b0 & 0x0F
        b1 = packet[1]
        marker = (b1 & 0x80) != 0
        timestamp = int.from_bytes(packet[4:8], "big")

        offset = 12 + csrc_count * 4
        if len(packet) < offset:
            return False, None, marker
        extension = (b0 & 0x10) != 0
        if extension:
            if len(packet) < offset + 4:
                return False, None, marker
            ext_words = int.from_bytes(packet[offset + 2 : offset + 4], "big")
            offset += 4 + ext_words * 4
            if len(packet) < offset:
                return False, None, marker

        return True, timestamp, marker

    def _schedule(self, payload: bytes, delay_ms: int = 0) -> None:
        send_at = time.monotonic() + max(0, delay_ms) / 1000.0
        self._serial += 1
        heapq.heappush(self.queue, ScheduledPacket(send_at=send_at, serial=self._serial, payload=payload))

    def _handle_frame_boundary(self, timestamp: Optional[int]) -> None:
        if timestamp is None:
            return
        if self.current_timestamp is None or timestamp != self.current_timestamp:
            self.current_timestamp = timestamp
            self.stats.frames_seen += 1
            if self.mode == FaultMode.DROP_EVERY_N_FRAMES:
                n = max(1, self.params.drop_every_n_frames)
                self.drop_current_frame = (self.stats.frames_seen % n) == 0
                if self.drop_current_frame:
                    self.stats.frames_dropped += 1
            else:
                self.drop_current_frame = False

    def process_packet(self, packet: bytes) -> None:
        self.stats.packets_in += 1
        self.packet_counter += 1

        is_rtp, timestamp, _marker = self._parse_rtp(packet)
        self._handle_frame_boundary(timestamp if is_rtp else None)

        if self.mode == FaultMode.PASSTHROUGH:
            self._schedule(packet, 0)
            return

        if self.mode == FaultMode.DROP_EVERY_N_PACKETS:
            n = max(1, self.params.drop_every_n_packets)
            if self.packet_counter % n == 0:
                self.stats.packets_dropped += 1
                return
            self._schedule(packet, 0)
            return

        if self.mode == FaultMode.DROP_RANDOM_PACKETS:
            prob = max(0.0, min(100.0, self.params.random_drop_percent)) / 100.0
            if random.random() < prob:
                self.stats.packets_dropped += 1
                return
            self._schedule(packet, 0)
            return

        if self.mode == FaultMode.FIXED_PACKET_DELAY:
            self._schedule(packet, self.params.fixed_delay_ms)
            return

        if self.mode == FaultMode.PACKET_JITTER:
            base = self.params.jitter_base_delay_ms
            delta = self.params.jitter_delta_ms
            self._schedule(packet, base + random.randint(-delta, delta))
            return

        if self.mode == FaultMode.BURST_PACKET_LOSS:
            period = max(1, self.params.burst_period_packets)
            length = max(0, min(period, self.params.burst_length_packets))
            idx = self.packet_counter % period
            if idx < length:
                self.stats.packets_dropped += 1
                return
            self._schedule(packet, 0)
            return

        if self.mode == FaultMode.DROP_EVERY_N_FRAMES:
            if self.drop_current_frame:
                self.stats.packets_dropped += 1
                return
            self._schedule(packet, 0)
            return

        if self.mode == FaultMode.DELAY_WHOLE_FRAMES:
            self._schedule(packet, self.params.whole_frame_delay_ms)
            return

        self._schedule(packet, 0)

    def flush_due(self) -> None:
        now = time.monotonic()
        while self.queue and self.queue[0].send_at <= now:
            item = heapq.heappop(self.queue)
            self.sock_out.sendto(item.payload, self.out_addr)
            self.stats.packets_out += 1


def draw_ui(stdscr: curses.window, injector: Injector) -> None:
    max_y, max_x = stdscr.getmaxyx()

    def safe_add(row: int, col: int, text: str, attr: int = curses.A_NORMAL) -> None:
        if row < 0 or row >= max_y or col >= max_x:
            return
        available = max_x - col
        if available <= 0:
            return
        clipped = text[: max(0, available - 1)]
        try:
            stdscr.addstr(row, col, clipped, attr)
        except curses.error:
            # Terminals can still reject writes at lower-right corner.
            pass

    stdscr.erase()
    safe_add(0, 0, "RTP Fault Injector (manual visual test)", curses.A_BOLD)
    safe_add(1, 0, f"In: 0.0.0.0:{injector.cfg.in_port}  ->  Out: {injector.cfg.out_host}:{injector.cfg.out_port}")

    safe_add(3, 0, "Modes (use Up/Down or number keys 1-8):")
    for idx, mode in enumerate(MODE_LIST):
        prefix = ">" if idx == injector.mode_index else " "
        attr = curses.A_REVERSE if idx == injector.mode_index else curses.A_NORMAL
        safe_add(4 + idx, 0, f"{prefix} {idx + 1}. {mode.value}", attr)

    row = 14
    safe_add(row, 0, "Stats:")
    safe_add(row + 1, 2, f"packets in/out: {injector.stats.packets_in} / {injector.stats.packets_out}")
    safe_add(row + 2, 2, f"packets dropped: {injector.stats.packets_dropped}")
    safe_add(row + 3, 2, f"frames seen/dropped: {injector.stats.frames_seen} / {injector.stats.frames_dropped}")
    safe_add(row + 4, 2, f"queue depth: {len(injector.queue)}")

    p = injector.params
    safe_add(row + 6, 0, "Current parameters:")
    safe_add(row + 7, 2, f"drop_every_n_packets={p.drop_every_n_packets}")
    safe_add(row + 8, 2, f"random_drop_percent={p.random_drop_percent:.1f}")
    safe_add(row + 9, 2, f"fixed_delay_ms={p.fixed_delay_ms}")
    safe_add(row + 10, 2, f"jitter_base_delay_ms={p.jitter_base_delay_ms}, jitter_delta_ms={p.jitter_delta_ms}")
    safe_add(row + 11, 2, f"burst_period_packets={p.burst_period_packets}, burst_length_packets={p.burst_length_packets}")
    safe_add(row + 12, 2, f"drop_every_n_frames={p.drop_every_n_frames}")
    safe_add(row + 13, 2, f"whole_frame_delay_ms={p.whole_frame_delay_ms}")

    safe_add(row + 15, 0, "Keys: q quit | r reset stats | [ ] random-drop +/-0.5 | -/= delay -/+5ms")
    safe_add(row + 16, 0, "      ,/. frame-drop N -/+1 | ;/' drop-every-N-packets -/+1")

    if max_y < 32:
        safe_add(max_y - 1, 0, "[Terminal is small: UI truncated, injector still active]", curses.A_DIM)

    stdscr.refresh()


def handle_key(key: int, injector: Injector) -> bool:
    if key in (ord("q"), ord("Q")):
        return False

    if key == curses.KEY_UP:
        injector.set_mode(injector.mode_index - 1)
    elif key == curses.KEY_DOWN:
        injector.set_mode(injector.mode_index + 1)
    elif ord("1") <= key <= ord("8"):
        injector.set_mode(key - ord("1"))
    elif key in (ord("r"), ord("R")):
        injector.stats = Stats()
        injector.packet_counter = 0
    elif key == ord("["):
        injector.params.random_drop_percent = max(0.0, injector.params.random_drop_percent - 0.5)
    elif key == ord("]"):
        injector.params.random_drop_percent = min(100.0, injector.params.random_drop_percent + 0.5)
    elif key == ord("-"):
        injector.params.fixed_delay_ms = max(0, injector.params.fixed_delay_ms - 5)
    elif key == ord("="):
        injector.params.fixed_delay_ms += 5
    elif key == ord(","):
        injector.params.drop_every_n_frames = max(1, injector.params.drop_every_n_frames - 1)
    elif key == ord("."):
        injector.params.drop_every_n_frames += 1
    elif key == ord(";"):
        injector.params.drop_every_n_packets = max(1, injector.params.drop_every_n_packets - 1)
    elif key == ord("'"):
        injector.params.drop_every_n_packets += 1

    return True


def run_menu(stdscr: curses.window, injector: Injector) -> None:
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(50)

    keep_running = True
    while keep_running:
        while True:
            try:
                packet, _addr = injector.sock_in.recvfrom(65535)
            except BlockingIOError:
                break
            injector.process_packet(packet)

        injector.flush_due()
        draw_ui(stdscr, injector)

        key = stdscr.getch()
        if key != -1:
            keep_running = handle_key(key, injector)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Interactive RTP fault injector for manual stream testing")
    parser.add_argument("--in-port", type=int, default=5601, help="UDP input port (default: 5601)")
    parser.add_argument("--out-host", default="127.0.0.1", help="UDP output host (default: 127.0.0.1)")
    parser.add_argument("--out-port", type=int, default=5600, help="UDP output port (default: 5600)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cfg = Config(in_port=args.in_port, out_host=args.out_host, out_port=args.out_port)
    params = Parameters()
    injector = Injector(cfg, params)
    try:
        curses.wrapper(run_menu, injector)
        return 0
    finally:
        injector.close()


if __name__ == "__main__":
    raise SystemExit(main())
