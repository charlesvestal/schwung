#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# The debounce logic now uses per-control re-arming flags.
# Verify shift_armed and volume_armed are reset on their respective release paths.
shift_line=$(rg -n "shift_armed = 1;" "$file" | tail -n 1 || true)
if [ -z "${shift_line}" ]; then
  echo "FAIL: Could not locate shift_armed reset in ${file}" >&2
  exit 1
fi

shift_line_num=$(echo "${shift_line}" | cut -d: -f1)
shift_start=$((shift_line_num - 60))
if [ "${shift_start}" -lt 1 ]; then shift_start=1; fi
shift_ctx=$(sed -n "${shift_start},${shift_line_num}p" "$file")
if ! echo "${shift_ctx}" | rg -q "midi_1 == 0x31"; then
  echo "FAIL: shift_armed reset not in shift release path" >&2
  exit 1
fi

vol_line=$(rg -n "volume_armed = 1;" "$file" | tail -n 1 || true)
if [ -z "${vol_line}" ]; then
  echo "FAIL: Could not locate volume_armed reset in ${file}" >&2
  exit 1
fi

vol_line_num=$(echo "${vol_line}" | cut -d: -f1)
vol_start=$((vol_line_num - 60))
if [ "${vol_start}" -lt 1 ]; then vol_start=1; fi
vol_ctx=$(sed -n "${vol_start},${vol_line_num}p" "$file")
if ! echo "${vol_ctx}" | rg -q "midi_1 == 0x08"; then
  echo "FAIL: volume_armed reset not in volume release path" >&2
  exit 1
fi

echo "PASS: hotkey controls re-arm on release (shift and volume)"
exit 0
