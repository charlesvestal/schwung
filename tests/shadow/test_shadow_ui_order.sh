#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

capture_line=$(rg -n "shadow_capture_midi_for_ui\\(" "$file" | rg -v "static void" | head -n 1 | cut -d: -f1 || true)
ioctl_line=$(rg -n "real_ioctl\\(fd" "$file" | head -n 1 | cut -d: -f1 || true)

if [ -z "${capture_line}" ] || [ -z "${ioctl_line}" ]; then
  echo "Failed to locate shadow_capture_midi_for_ui or real_ioctl call in ${file}" >&2
  exit 1
fi

if [ "${capture_line}" -gt "${ioctl_line}" ]; then
  echo "PASS: shadow_capture_midi_for_ui runs after real_ioctl"
  exit 0
fi

echo "FAIL: shadow_capture_midi_for_ui runs before real_ioctl (line ${capture_line} <= ${ioctl_line})" >&2
exit 1
