#!/bin/sh

# Usage:
#   ./run.sh                          # starts pixelpilot with no config
#   ./run.sh /path/to/config.ini      # starts with --config /path/to/config.ini
#   ./run.sh /path/to/config.ini --verbose   # passes extra args to pixelpilot

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

    exit "$exit_code"
}

trap 'cleanup $?' EXIT
trap 'cleanup 130' INT
trap 'cleanup 143' TERM

# If an argument is provided, treat the FIRST as a config path for pixelpilot.
# Any additional args after the first are passed through to pixelpilot.
CONFIG_ARG=""
if [ "$#" -gt 0 ]; then
    CONFIG_ARG=$1
    shift
fi

PIXEL_PID=
if [ -n "$CONFIG_ARG" ]; then
    "$PIXEL_CMD" --config "$CONFIG_ARG" "$@" &
else
    "$PIXEL_CMD" "$@" &
fi
PIXEL_PID=$!

wait "$PIXEL_PID"
PIXEL_STATUS=$?

cleanup "$PIXEL_STATUS"
