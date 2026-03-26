#!/usr/bin/env bash
set -euo pipefail

file="src/host/shadow_chain_mgmt.c"

if ! rg -q 'static int mfx_chain_params_lookup_numeric_meta\(' "$file"; then
  echo "FAIL: missing helper to parse Master FX chain_params metadata" >&2
  exit 1
fi

if ! rg -q 'mfx_chain_params_lookup_numeric_meta\(' "$file" || \
   ! rg -q 'mfx->chain_params_cache' "$file"; then
  echo "FAIL: Master FX LFO tick is not using robust chain_params metadata lookup" >&2
  exit 1
fi

if rg -q 'snprintf\(needle, sizeof\(needle\), "\\\"key\\\":\\\"%s\\\""[^;]*lfo->param' "$file"; then
  echo "FAIL: brittle exact key needle search is still present in Master FX LFO metadata lookup" >&2
  exit 1
fi

echo "PASS: Master FX LFO metadata lookup parses chain_params without assuming exact JSON spacing"
