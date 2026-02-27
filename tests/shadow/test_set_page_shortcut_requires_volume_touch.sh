#!/usr/bin/env bash
set -euo pipefail

# Set-page navigation should only trigger with Shift+Vol+Left/Right.
# Shift+Left/Right without volume touch must not trigger page changes.

file="src/move_anything_shim.c"

if ! command -v perl >/dev/null 2>&1; then
  echo "perl is required to run this test" >&2
  exit 1
fi

if ! perl -0ne 'exit((/Shift\+Vol\+Left\/Right:[\s\S]*if\s*\(shadow_control\s*&&\s*shadow_control->set_pages_enabled\s*&&\s*shadow_shift_held\s*&&\s*shadow_volume_knob_touched\s*&&\s*d2\s*>\s*0\)/s) ? 0 : 1)' "$file"; then
  echo "FAIL: Set-page shortcut is not gated by Shift+Vol+Left/Right in ${file}" >&2
  exit 1
fi

echo "PASS: Set-page shortcut requires Shift+Vol+Left/Right"
exit 0
