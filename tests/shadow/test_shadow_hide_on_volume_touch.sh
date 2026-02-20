#!/usr/bin/env bash
set -euo pipefail

# Test: When volume touch is active, shadow UI overlay should be hidden
# so native Move volume overlays remain visible (master and track volume lines),
# but Shift+Vol shortcuts should still show Shadow UI.

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

fn_start=$(rg -n "static void shadow_swap_display\\(void\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${fn_start}" ]; then
  echo "FAIL: Could not locate shadow_swap_display() in ${file}" >&2
  exit 1
fi

fn_block=$(sed -n "${fn_start},$((fn_start + 80))p" "$file")

if ! echo "${fn_block}" | rg -q "shadow_volume_knob_touched"; then
  echo "FAIL: shadow_swap_display() does not reference shadow_volume_knob_touched" >&2
  exit 1
fi

if ! echo "${fn_block}" | rg -q "if \\(shadow_volume_knob_touched && !shadow_shift_held\\)"; then
  echo "FAIL: Missing guarded volume-touch condition (must bypass while Shift is held)" >&2
  exit 1
fi

if ! echo "${fn_block}" | rg -q "return;[[:space:]]*/\\*.*volume|return;"; then
  echo "FAIL: Volume-touch guard in shadow_swap_display() does not early-return" >&2
  exit 1
fi

echo "PASS: shadow_swap_display hides on volume touch without blocking Shift+Vol shortcuts"
exit 0
