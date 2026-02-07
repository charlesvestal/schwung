#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Capture helper should copy snapshot verbatim.
capture_start=$(rg -n "static void native_capture_total_mix_snapshot_from_buffer\\(const int16_t \\*src\\)" "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${capture_start}" ]; then
  echo "FAIL: Could not locate snapshot capture helper" >&2
  exit 1
fi
capture_ctx=$(sed -n "${capture_start},$((capture_start + 40))p" "$file")

if ! echo "${capture_ctx}" | rg -q "memcpy\\(native_total_mix_snapshot, src, AUDIO_BUFFER_SIZE\\);"; then
  echo "FAIL: Snapshot capture helper should memcpy source verbatim" >&2
  exit 1
fi
if echo "${capture_ctx}" | rg -q "1\\.0f /"; then
  echo "FAIL: Snapshot capture should not perform inverse volume compensation" >&2
  exit 1
fi

# Gain compensation belongs in overwrite apply helper.
overwrite_start=$(rg -n "static void native_resample_bridge_apply_overwrite_makeup\\(const int16_t \\*src," "$file" | head -n 1 | cut -d: -f1 || true)
if [ -z "${overwrite_start}" ]; then
  echo "FAIL: Could not locate overwrite helper" >&2
  exit 1
fi
overwrite_ctx=$(sed -n "${overwrite_start},$((overwrite_start + 200))p" "$file")

if ! echo "${overwrite_ctx}" | rg -q "float inv_mv = 1\\.0f / mv;"; then
  echo "FAIL: Overwrite helper missing inverse master-volume compensation math" >&2
  exit 1
fi
if ! echo "${overwrite_ctx}" | rg -q "native_bridge_makeup_desired_gain = inv_mv;"; then
  echo "FAIL: Overwrite helper should expose desired inverse gain in diagnostics" >&2
  exit 1
fi

echo "PASS: Snapshot capture/apply gain staging separation present"
exit 0
