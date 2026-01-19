#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

display_line=$(rg -n "shadow_swap_display\\(" "$file" | rg -v "static void" | head -n 1 | cut -d: -f1 || true)
ioctl_line=$(rg -n "real_ioctl\\(fd" "$file" | head -n 1 | cut -d: -f1 || true)

if [ -z "${display_line}" ] || [ -z "${ioctl_line}" ]; then
  echo "Failed to locate shadow_swap_display or real_ioctl call in ${file}" >&2
  exit 1
fi

if [ "${display_line}" -lt "${ioctl_line}" ]; then
  echo "PASS: shadow_swap_display runs before real_ioctl"
  exit 0
fi

echo "FAIL: shadow_swap_display runs after real_ioctl (line ${display_line} >= ${ioctl_line})" >&2
exit 1
