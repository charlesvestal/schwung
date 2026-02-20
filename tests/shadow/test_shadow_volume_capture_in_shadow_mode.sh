#!/usr/bin/env bash
set -euo pipefail

# Master-volume capture should run when Shadow UI is temporarily hidden by
# plain volume touch (not Shift+Vol shortcut), even if shadow_display_mode=1.

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -n "native_display_visible" "$file" >/dev/null 2>&1; then
  echo "FAIL: Missing native_display_visible gating for volume capture" >&2
  exit 1
fi

if ! rg -n "shadow_volume_knob_touched && !shadow_shift_held" "$file" >/dev/null 2>&1; then
  echo "FAIL: Volume capture path missing plain-volume-touch condition" >&2
  exit 1
fi

if ! rg -n "if \\(global_mmap_addr && native_display_visible\\)" "$file" >/dev/null 2>&1; then
  echo "FAIL: Slice capture still gated only by !shadow_display_mode" >&2
  exit 1
fi

echo "PASS: Volume capture can run in shadow mode when native display is visible"
exit 0
