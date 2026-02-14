#!/usr/bin/env python3
"""External UDP OSD control harness.

This script does not link against project code. It only sends UDP JSON payloads
that exercise the new external `asset_updates` behavior so an operator can
visually confirm OSD behavior on-screen.
"""

from __future__ import annotations

import argparse
import json
import socket
import time
from typing import Any, Dict, List


def send_payload(host: str, port: int, payload: Dict[str, Any]) -> None:
    encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(encoded, (host, port))


def run_test_sequence(host: str, port: int, asset_id: int, cooldown_s: float) -> None:
    tests: List[Dict[str, Any]] = [
        {
            "name": "Disable asset",
            "payload": {"asset_updates": [{"id": asset_id, "enabled": False}]},
            "expectation": f"Asset id={asset_id} should disappear immediately.",
        },
        {
            "name": "Enable asset",
            "payload": {"asset_updates": [{"id": asset_id, "enabled": True}]},
            "expectation": f"Asset id={asset_id} should reappear immediately.",
        },
        {
            "name": "Disable asset with TTL",
            "payload": {"asset_updates": [{"id": asset_id, "enabled": False}], "ttl_ms": 3000},
            "expectation": (
                f"Asset id={asset_id} should disappear immediately, then revert after TTL (~3s)."
            ),
        },
    ]

    print("UDP OSD control harness")
    print(f"Target: {host}:{port}")
    print(f"Asset id under test: {asset_id}")
    print(f"Cooldown between tests: {cooldown_s:.1f}s")
    print("=" * 72)

    for index, test in enumerate(tests, start=1):
        print(f"[Test {index}/{len(tests)}] {test['name']}")
        print(f"  Payload: {json.dumps(test['payload'])}")
        print(f"  Expect:  {test['expectation']}")
        send_payload(host, port, test["payload"])

        if test["name"] == "Disable asset with TTL":
            print("  Waiting 3.5s so TTL expiration can be observed on-screen...")
            time.sleep(3.5)

        print(f"  Cooling down for {cooldown_s:.1f}s before next test...\n")
        time.sleep(cooldown_s)

    print("All tests sent.\n")
    print("What was tested:")
    print("  1) asset_updates enable=false")
    print("  2) asset_updates enable=true")
    print("  3) asset_updates enable=false with ttl_ms (auto-expiry)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="External UDP harness for OSD asset visibility controls")
    parser.add_argument("--host", default="127.0.0.1", help="Target host running pixelpilot_mini_rk")
    parser.add_argument("--port", type=int, default=5005, help="External OSD UDP port (default: 5005)")
    parser.add_argument("--asset-id", type=int, default=0, help="OSD asset id to toggle (default: 0)")
    parser.add_argument("--cooldown", type=float, default=1.0, help="Cooldown seconds between tests")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.asset_id < 0 or args.asset_id > 7:
        raise SystemExit("--asset-id must be in range 0..7")
    if args.cooldown < 1.0:
        raise SystemExit("--cooldown must be >= 1.0 seconds")

    run_test_sequence(args.host, args.port, args.asset_id, args.cooldown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
