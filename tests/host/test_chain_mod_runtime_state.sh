#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'typedef struct mod_target_state' "$file"; then
  echo "FAIL: missing mod_target_state definition" >&2
  exit 1
fi
if ! rg -q 'mod_target_state_t mod_targets' "$file"; then
  echo "FAIL: chain instance missing mod_targets storage" >&2
  exit 1
fi
if ! rg -q 'static int chain_mod_emit_value' "$file"; then
  echo "FAIL: missing chain_mod_emit_value callback" >&2
  exit 1
fi
if ! rg -q 'static void chain_mod_clear_source' "$file"; then
  echo "FAIL: missing chain_mod_clear_source callback" >&2
  exit 1
fi

echo "PASS: modulation runtime state surface is present"
