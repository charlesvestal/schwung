#!/usr/bin/env bash
set -euo pipefail

# Test: Verify shift CC (0x31) is NOT filtered so hotkey detection works
# The post-ioctl MIDI filter should skip shift CC to allow exiting shadow mode

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Look for the shift bypass in the post-ioctl filtering section
# Pattern: d1 != 0x31 (don't filter shift CC)
line=$(rg -n "d1 != 0x31" "$file" | head -n 1 || true)
if [ -z "${line}" ]; then
  echo "FAIL: No shift CC (0x31) bypass found in post-ioctl MIDI filter" >&2
  exit 1
fi

# Verify it's in the context of CC filtering (type == 0xB0)
context=$(rg -B 5 "d1 != 0x31" "$file" | head -n 6)
if echo "${context}" | rg -q "type == 0xB0"; then
  echo "PASS: Shift CC (0x31) is bypassed in CC filter for hotkey detection"
  exit 0
fi

echo "FAIL: Shift bypass not in CC filter context" >&2
exit 1
