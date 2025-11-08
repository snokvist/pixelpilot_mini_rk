#!/bin/sh

GPU=/sys/class/misc/mali0/device
# sample period for utilisation; units are milliseconds on most kbase builds
echo 500 > "$GPU/utilisation_period" 2>/dev/null || true   # 0.5 s

watch -n0.5 'printf "util=%s%%  freq=%s Hz\n" \
  "$(cat /sys/class/misc/mali0/device/utilisation 2>/dev/null)" \
  "$(cat /sys/class/misc/mali0/device/devfreq/*/cur_freq 2>/dev/null)"'
