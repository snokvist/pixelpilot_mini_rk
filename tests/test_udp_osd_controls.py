#!/usr/bin/env python3
"""UDP OSD external-control harness.

Builds a tiny shared-library shim around src/osd_external.c and validates
text/value/zoom + waybeam-style asset_updates behavior over UDP.
"""

from __future__ import annotations

import ctypes
import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
INCLUDE_DIR = REPO_ROOT / "include"
SRC_FILE = REPO_ROOT / "src" / "osd_external.c"
LOGGING_SRC_FILE = REPO_ROOT / "src" / "logging.c"

OSD_EXTERNAL_MAX_TEXT = 8
OSD_EXTERNAL_TEXT_LEN = 64
OSD_EXTERNAL_MAX_VALUES = 8
OSD_EXTERNAL_MAX_ASSETS = 8


class OsdExternalFeedSnapshot(ctypes.Structure):
    _fields_ = [
        ("text", (ctypes.c_char * OSD_EXTERNAL_TEXT_LEN) * OSD_EXTERNAL_MAX_TEXT),
        ("value", ctypes.c_double * OSD_EXTERNAL_MAX_VALUES),
        ("last_update_ns", ctypes.c_uint64),
        ("expiry_ns", ctypes.c_uint64),
        ("zoom_expiry_ns", ctypes.c_uint64),
        ("zoom_command", ctypes.c_char * OSD_EXTERNAL_TEXT_LEN),
        ("asset_enabled", ctypes.c_uint8 * OSD_EXTERNAL_MAX_ASSETS),
        ("asset_active_mask", ctypes.c_uint8),
        ("status", ctypes.c_int),
    ]


class ExternalBridgeHarness:
    def __init__(self) -> None:
        self._tmpdir = tempfile.TemporaryDirectory(prefix="osd_ext_harness_")
        self._lib = self._build_and_load(Path(self._tmpdir.name))
        self._bridge = ctypes.c_void_p(self._lib.bridge_create())
        if not self._bridge:
            raise RuntimeError("failed to create bridge")

    @staticmethod
    def _build_and_load(tmp: Path):
        shim = tmp / "shim.c"
        so = tmp / "libosd_ext_harness.so"
        shim.write_text(
            """
#include <stdlib.h>
#include "osd_external.h"

void *bridge_create(void) {
    OsdExternalBridge *bridge = (OsdExternalBridge *)calloc(1, sizeof(OsdExternalBridge));
    if (!bridge) {
        return NULL;
    }
    osd_external_init(bridge);
    return bridge;
}

int bridge_start(void *bridge, const char *bind_addr, int udp_port) {
    return osd_external_start((OsdExternalBridge *)bridge, bind_addr, udp_port);
}

void bridge_stop(void *bridge) {
    if (!bridge) {
        return;
    }
    osd_external_stop((OsdExternalBridge *)bridge);
}

void bridge_destroy(void *bridge) {
    if (!bridge) {
        return;
    }
    osd_external_stop((OsdExternalBridge *)bridge);
    free(bridge);
}

void bridge_snapshot(void *bridge, OsdExternalFeedSnapshot *out) {
    osd_external_get_snapshot((OsdExternalBridge *)bridge, out);
}
""".strip()
            + "\n"
        )

        cmd = [
            "cc",
            "-shared",
            "-fPIC",
            "-O2",
            "-Wall",
            "-I",
            str(INCLUDE_DIR),
            str(SRC_FILE),
            str(LOGGING_SRC_FILE),
            str(shim),
            "-o",
            str(so),
            "-lpthread",
        ]
        subprocess.run(cmd, check=True, cwd=REPO_ROOT)
        lib = ctypes.CDLL(str(so))
        lib.bridge_create.restype = ctypes.c_void_p
        lib.bridge_start.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        lib.bridge_start.restype = ctypes.c_int
        lib.bridge_stop.argtypes = [ctypes.c_void_p]
        lib.bridge_destroy.argtypes = [ctypes.c_void_p]
        lib.bridge_snapshot.argtypes = [ctypes.c_void_p, ctypes.POINTER(OsdExternalFeedSnapshot)]
        return lib

    def start(self, port: int) -> None:
        rc = self._lib.bridge_start(self._bridge, b"127.0.0.1", port)
        if rc != 0:
            raise RuntimeError(f"bridge_start failed: {rc}")

    def stop(self) -> None:
        self._lib.bridge_stop(self._bridge)

    def destroy(self) -> None:
        self._lib.bridge_destroy(self._bridge)
        self._bridge = ctypes.c_void_p()
        self._tmpdir.cleanup()

    def snapshot(self) -> OsdExternalFeedSnapshot:
        snap = OsdExternalFeedSnapshot()
        self._lib.bridge_snapshot(self._bridge, ctypes.byref(snap))
        return snap


def allocate_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def send_payload(port: int, payload: dict) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(json.dumps(payload).encode("utf-8"), ("127.0.0.1", port))


class TestUdpOsdControls(unittest.TestCase):
    def setUp(self) -> None:
        self.bridge = ExternalBridgeHarness()
        self.port = allocate_udp_port()
        self.bridge.start(self.port)

    def tearDown(self) -> None:
        self.bridge.destroy()

    @staticmethod
    def _cstr(buf: ctypes.Array) -> str:
        return bytes(buf).split(b"\0", 1)[0].decode("utf-8", errors="replace")

    def test_text_value_zoom_update(self) -> None:
        send_payload(
            self.port,
            {
                "texts": ["hello"],
                "values": [12.5],
                "zoom": "200,200,50,50",
            },
        )
        time.sleep(0.08)
        snap = self.bridge.snapshot()
        self.assertEqual(self._cstr(snap.text[0]), "hello")
        self.assertAlmostEqual(snap.value[0], 12.5, places=2)
        self.assertEqual(self._cstr(snap.zoom_command), "200,200,50,50")

    def test_asset_updates_toggle_and_ttl(self) -> None:
        send_payload(
            self.port,
            {
                "asset_updates": [{"id": 3, "enabled": False}],
                "ttl_ms": 120,
            },
        )
        time.sleep(0.06)
        snap = self.bridge.snapshot()
        self.assertTrue(snap.asset_active_mask & (1 << 3))
        self.assertEqual(int(snap.asset_enabled[3]), 0)

        time.sleep(0.10)
        snap2 = self.bridge.snapshot()
        self.assertFalse(snap2.asset_active_mask & (1 << 3))

    def test_asset_update_enable_persists_without_ttl(self) -> None:
        send_payload(
            self.port,
            {
                "asset_updates": [{"id": 1, "enabled": True}],
            },
        )
        time.sleep(0.06)
        snap = self.bridge.snapshot()
        self.assertTrue(snap.asset_active_mask & (1 << 1))
        self.assertEqual(int(snap.asset_enabled[1]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
