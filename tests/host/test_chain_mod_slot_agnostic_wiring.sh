#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'inst->subplugin_host_api.mod_emit_value = chain_mod_emit_value' "$file"; then
  echo "FAIL: instance host API missing mod_emit_value wiring" >&2
  exit 1
fi
if ! rg -q 'inst->subplugin_host_api.mod_clear_source = chain_mod_clear_source' "$file"; then
  echo "FAIL: instance host API missing mod_clear_source wiring" >&2
  exit 1
fi
if ! rg -q 'inst->subplugin_host_api.mod_host_ctx = inst' "$file"; then
  echo "FAIL: instance host API missing mod_host_ctx wiring" >&2
  exit 1
fi
if ! rg -q 'g_subplugin_host_api.mod_emit_value = chain_mod_emit_value' "$file"; then
  echo "FAIL: v1 host API copy missing mod_emit_value wiring" >&2
  exit 1
fi

echo "PASS: slot-agnostic modulation callback wiring is present"
