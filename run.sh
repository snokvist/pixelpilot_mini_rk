#!/bin/sh

OSD_CMD="./osd_ext_feed"
PIXEL_CMD="./pixelpilot_mini_rk"

cleanup() {
    exit_code=$1
    trap - INT TERM EXIT

    if [ -n "${PIXEL_PID:-}" ]; then
        if kill -0 "$PIXEL_PID" 2>/dev/null; then
            kill "$PIXEL_PID" 2>/dev/null
        fi
        wait "$PIXEL_PID" 2>/dev/null || true
    fi

    if kill -0 "$OSD_PID" 2>/dev/null; then
        kill "$OSD_PID" 2>/dev/null
    fi
    wait "$OSD_PID" 2>/dev/null || true

    exit "$exit_code"
}

trap 'cleanup $?' EXIT
trap 'cleanup 130' INT
trap 'cleanup 143' TERM

"$OSD_CMD" &
OSD_PID=$!

PIXEL_PID=
"$PIXEL_CMD" "$@" &
PIXEL_PID=$!

wait "$PIXEL_PID"
PIXEL_STATUS=$?

cleanup "$PIXEL_STATUS"
