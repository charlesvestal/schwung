#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q "native_bridge_premaster_gain" "$file"; then
  echo "FAIL: Missing native bridge pre-master gain state" >&2
  exit 1
fi

if ! rg -q "native_resample_refresh_premaster_gain_from_settings" "$file"; then
  echo "FAIL: Missing settings-driven pre-master gain refresh helper" >&2
  exit 1
fi

capture_fn_start=$(rg -n "static void native_capture_total_mix_snapshot_from_buffer\\(const int16_t \*src\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${capture_fn_start}" ]; then
  echo "FAIL: Could not locate native snapshot capture helper" >&2
  exit 1
fi
capture_ctx=$(sed -n "${capture_fn_start},$((capture_fn_start + 80))p" "$file")

if ! echo "${capture_ctx}" | rg -q "native_bridge_premaster_gain"; then
  echo "FAIL: Snapshot capture does not reference pre-master gain" >&2
  exit 1
fi

if ! echo "${capture_ctx}" | rg -q "1\.0f /"; then
  echo "FAIL: Snapshot capture does not undo master attenuation" >&2
  exit 1
fi

echo "PASS: Native bridge pre-master gain compensation wiring present"
exit 0
