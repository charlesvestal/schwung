#!/usr/bin/env bash
set -euo pipefail

# After triggering shadow via Shift+Vol+Track, releasing Shift slightly before
# volume touch should NOT briefly reveal native volume UI. The shim should latch
# a guard until volume touch is fully released.

file="src/move_anything_shim.c"

if ! command -v perl >/dev/null 2>&1; then
  echo "perl is required to run this test" >&2
  exit 1
fi

if ! perl -0ne 'exit((/shadow_block_plain_volume_hide_until_release/s && /if\s*\(shadow_volume_knob_touched\s*&&\s*!shadow_shift_held\)\s*\{[\s\S]*shadow_block_plain_volume_hide_until_release[\s\S]*\}/s && /if\s*\(!shadow_volume_knob_touched\)\s*\{[\s\S]*shadow_block_plain_volume_hide_until_release\s*=\s*0\s*;/s) ? 0 : 1)' "$file"; then
  echo "FAIL: Missing latch logic to suppress plain volume hide right after Shift+Vol shortcut release" >&2
  exit 1
fi

if ! perl -0ne 'exit((/Shift \+ Volume \+ Track[\s\S]*shadow_block_plain_volume_hide_until_release\s*=\s*1\s*;/s) ? 0 : 1)' "$file"; then
  echo "FAIL: Shift+Vol+Track path does not arm the release-flash guard latch" >&2
  exit 1
fi

echo "PASS: Shift+Vol+Track release path guards against brief native volume flash"
exit 0
