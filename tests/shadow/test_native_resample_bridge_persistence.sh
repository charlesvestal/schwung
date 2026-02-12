#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q "native_resample_bridge_load_mode_from_shadow_config\\(" "$file"; then
  echo "FAIL: Missing native resample bridge startup load helper" >&2
  exit 1
fi

if ! rg -q "/data/UserData/move-anything/shadow_config.json" "$file"; then
  echo "FAIL: Bridge startup load does not read shadow_config.json" >&2
  exit 1
fi

if ! rg -q "resample_bridge_mode" "$file"; then
  echo "FAIL: Bridge startup load does not parse resample_bridge_mode key" >&2
  exit 1
fi

startup_line=$(rg -n "native_resample_bridge_load_mode_from_shadow_config\\(" "$file" | tail -n 1 | cut -d: -f1 || true)
if [ -z "${startup_line}" ]; then
  echo "FAIL: Could not locate startup call to bridge load helper" >&2
  exit 1
fi

mmap_hook_line=$(rg -n "if \\(length == 4096\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${mmap_hook_line}" ]; then
  echo "FAIL: Could not locate mmap mailbox hook init path" >&2
  exit 1
fi

if [ "${startup_line}" -lt "${mmap_hook_line}" ]; then
  echo "FAIL: Bridge mode startup load helper is not called in mailbox init path" >&2
  exit 1
fi

echo "PASS: Native resample bridge persistence wiring present"
exit 0
