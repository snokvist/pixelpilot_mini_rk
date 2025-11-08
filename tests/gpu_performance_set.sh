#!/bin/sh
# rk3566-mali-perf.sh — performance toggle for Mali (kbase) on RK3566

set -e

GPU=/sys/class/misc/mali0/device
DFNODE="$(ls -d "$GPU"/devfreq/* 2>/dev/null | head -n1)" || {
  echo "No devfreq node under $GPU"; exit 1; }

GOV="$DFNODE/governor"
AVF="$DFNODE/available_frequencies"
AVG="$DFNODE/available_governors"
CUR="$DFNODE/cur_freq"
MIN="$DFNODE/min_freq"
MAX="$DFNODE/max_freq"
POL="$GPU/power_policy"

STATEDIR=/tmp/mali-perf-state
mkdir -p "$STATEDIR"

# Return the numeric highest OPP; fallback to current max_freq if needed
highest_freq() {
  if [ -r "$AVF" ]; then
    tr ' ' '\n' < "$AVF" | grep -E '^[0-9]+$' | sort -n | tail -1
  fi
}

have_gov() { [ -r "$AVG" ] && grep -qw "$1" "$AVG"; }

status() {
  echo "GPU devfreq node: $DFNODE"
  [ -r "$POL" ] && echo "power_policy: $(cat "$POL")"
  echo "governor    : $(cat "$GOV")"
  echo "cur/min/max : $(cat "$CUR") / $(cat "$MIN") / $(cat "$MAX")"
  [ -r "$GPU/utilisation" ] && echo "utilisation : $(cat "$GPU/utilisation")%"
}

perf_on() {
  # save current state
  cat "$GOV" > "$STATEDIR/governor" 2>/dev/null || true
  cat "$MIN" > "$STATEDIR/min" 2>/dev/null || true
  cat "$MAX" > "$STATEDIR/max" 2>/dev/null || true
  [ -r "$POL" ] && cat "$POL" > "$STATEDIR/power_policy" 2>/dev/null || true

  HIGHEST="$(highest_freq)"
  [ -n "$HIGHEST" ] || HIGHEST="$(cat "$MAX")"  # fallback to current max clamp

  # keep GPU from power-gating if supported
  [ -w "$POL" ] && echo always_on > "$POL" 2>/dev/null || true

  # Prefer a governor that won’t fight us
  if have_gov performance; then
    echo performance > "$GOV" 2>/dev/null || true
  elif have_gov userspace; then
    echo userspace > "$GOV" 2>/dev/null || true
  fi

  # Pin frequency at the top OPP (order matters on some builds)
  echo "$HIGHEST" > "$MAX"
  echo "$HIGHEST" > "$MIN"

  # If simple_ondemand is still active, make it eager (best-effort)
  if [ "$(cat "$GOV")" = simple_ondemand ] && [ -d "$DFNODE/simple_ondemand" ]; then
    echo 1  > "$DFNODE/simple_ondemand/upthreshold"       2>/dev/null || true
    echo 0  > "$DFNODE/simple_ondemand/downdifferential"   2>/dev/null || true
    echo 0  > "$DFNODE/simple_ondemand/ignore_idle_time"   2>/dev/null || true
  fi

  echo "Performance mode: ON (target=${HIGHEST} Hz)"
  status
}

perf_off() {
  [ -r "$STATEDIR/min" ] && cat "$STATEDIR/min" > "$MIN" 2>/dev/null || true
  [ -r "$STATEDIR/max" ] && cat "$STATEDIR/max" > "$MAX" 2>/dev/null || true

  if [ -r "$STATEDIR/governor" ]; then
    cat "$STATEDIR/governor" > "$GOV" 2>/dev/null || true
  else
    if have_gov simple_ondemand; then echo simple_ondemand > "$GOV" 2>/dev/null || true
    elif have_gov ondemand; then echo ondemand > "$GOV" 2>/dev/null || true
    elif have_gov powersave; then echo powersave > "$GOV" 2>/dev/null || true
    fi
  fi

  [ -r "$STATEDIR/power_policy" ] && cat "$STATEDIR/power_policy" > "$POL" 2>/dev/null || true
  echo "Performance mode: OFF"
  status
}

case "$1" in
  on)     perf_on ;;
  off)    perf_off ;;
  status|'') status ;;
  *) echo "Usage: $0 {on|off|status}"; exit 2 ;;
esac
