#!/usr/bin/env bash
set -euo pipefail

file="src/host/shadow_chain_mgmt.c"

if ! rg -q 'static int mfx_lfo_try_runtime_chain_params_meta\(' "$file"; then
  echo "FAIL: missing Master FX runtime chain_params metadata fallback helper" >&2
  exit 1
fi

if ! rg -q 'mfx_lfo_try_runtime_chain_params_meta\(' "$file"; then
  echo "FAIL: master FX LFO tick does not call runtime chain_params fallback" >&2
  exit 1
fi

if ! rg -q 'get_param\(mfx->instance, "chain_params"' "$file"; then
  echo "FAIL: runtime chain_params fetch for Master FX metadata is missing" >&2
  exit 1
fi

echo "PASS: master FX LFO can fetch metadata from runtime chain_params when cache misses"
