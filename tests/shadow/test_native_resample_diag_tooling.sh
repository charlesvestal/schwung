#!/usr/bin/env bash
set -euo pipefail

file="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

if ! rg -q "native_resample_diag_is_enabled\\(" "$file"; then
  echo "FAIL: Missing native resample diag enable helper" >&2
  exit 1
fi

if ! rg -q "native_compute_audio_metrics\\(" "$file"; then
  echo "FAIL: Missing native audio metrics helper for resample diagnostics" >&2
  exit 1
fi

if ! rg -q "/data/UserData/move-anything/native_resample_diag_on" "$file"; then
  echo "FAIL: Missing native resample diag flag path" >&2
  exit 1
fi

if ! rg -q "Native bridge diag:" "$file"; then
  echo "FAIL: Missing native bridge diagnostic log output" >&2
  exit 1
fi

echo "PASS: Native resample diagnostics tooling present"
exit 0
