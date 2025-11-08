#!/bin/sh
# rk3566-gpu-dmc-max.sh

set -e

GPU=/sys/class/misc/mali0/device
DFGPU="$(ls -d "$GPU"/devfreq/* 2>/dev/null | head -n1)" || {
  echo "No Mali devfreq node"; exit 1; }

# --- Pin DMC (memory controller) ---
DMC=$(ls -d /sys/class/devfreq/*dmc* 2>/dev/null | head -n1 || true)
if [ -n "$DMC" ]; then
  echo performance > "$DMC/governor" 2>/dev/null || true
  TOP_DMC=$(tr ' ' '\n' < "$DMC/available_frequencies" | sort -n | tail -1)
  [ -n "$TOP_DMC" ] && {
    echo "$TOP_DMC" > "$DMC/min_freq"
    echo "$TOP_DMC" > "$DMC/max_freq" 2>/dev/null || true
  }
fi

# --- Pin GPU ---
# Keep GPU awake if supported
[ -w "$GPU/power_policy" ] && echo always_on > "$GPU/power_policy" 2>/dev/null || true

# Prefer performance/userspace; otherwise we’ll still clamp min=max
if [ -r "$DFGPU/available_governors" ]; then
  if grep -qw performance "$DFGPU/available_governors"; then
    echo performance > "$DFGPU/governor" 2>/dev/null || true
  elif grep -qw userspace "$DFGPU/available_governors"; then
    echo userspace > "$DFGPU/governor" 2>/dev/null || true
  fi
fi

TOP_GPU=$(tr ' ' '\n' < "$DFGPU/available_frequencies" | grep -E '^[0-9]+$' | sort -n | tail -1)
[ -z "$TOP_GPU" ] && TOP_GPU=$(cat "$DFGPU/max_freq")

echo "$TOP_GPU" > "$DFGPU/max_freq"
echo "$TOP_GPU" > "$DFGPU/min_freq"

# --- Status ---
echo "DMC: $( [ -n "$DMC" ] && cat "$DMC/cur_freq" || echo N/A )"
echo "GPU cur/min/max: $(cat "$DFGPU/cur_freq") / $(cat "$DFGPU/min_freq") / $(cat "$DFGPU/max_freq")"
[ -r "$GPU/utilisation" ] && echo "GPU util: $(cat "$GPU/utilisation")%"
