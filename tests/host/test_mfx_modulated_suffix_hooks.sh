#!/usr/bin/env bash
set -euo pipefail

file="src/host/shadow_chain_mgmt.c"

if ! rg -q 'static int mfx_param_strip_suffix' "$file"; then
  echo "FAIL: MFX param suffix parser helper is missing" >&2
  exit 1
fi

if ! rg -q 'mfx_param_strip_suffix\(param_key, ":modulated", bare_param, sizeof\(bare_param\)\)' "$file"; then
  echo "FAIL: MFX GET path does not handle :modulated suffix" >&2
  exit 1
fi

if ! rg -q 'mfx_param_strip_suffix\(param_key, ":base", bare_param, sizeof\(bare_param\)\)' "$file"; then
  echo "FAIL: MFX GET path does not handle :base suffix" >&2
  exit 1
fi

if ! rg -q 'strcmp\(lfo->target, target_key\) != 0' "$file"; then
  echo "FAIL: MFX suffix handlers do not filter by targeted FX slot" >&2
  exit 1
fi

echo "PASS: master FX get_param supports :base and :modulated suffixes"
