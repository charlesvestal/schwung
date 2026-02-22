#!/usr/bin/env bash
set -euo pipefail

shim_c="src/move_anything_shim.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# Copy detection should be gated by set-name intent (e.g. "copy") and
# should not use ambiguous fallback-to-current heuristics.
if ! rg -q "shadow_set_name_looks_like_copy\(" "$shim_c"; then
  echo "FAIL: missing set-name copy-intent gate" >&2
  exit 1
fi
if ! rg -q "shadow_detect_copy_source\(const char \*set_name, const char \*new_uuid" "$shim_c"; then
  echo "FAIL: copy detection API does not accept set_name" >&2
  exit 1
fi
if rg -q "match_count > 1 && sampler_current_set_uuid\[0\]" "$shim_c"; then
  echo "FAIL: ambiguous copy-source fallback to current set still present" >&2
  exit 1
fi
if ! rg -q "shadow_detect_copy_source\(set_name, uuid, source_uuid" "$shim_c"; then
  echo "FAIL: set load path does not pass set_name into copy detection" >&2
  exit 1
fi

echo "PASS: copy detection guarded against false-positive new sets"
exit 0
