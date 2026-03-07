#!/usr/bin/env bash
set -euo pipefail

file_api="src/host/plugin_api_v1.h"
file_chain="src/modules/chain/dsp/chain_host.c"

# Host API should expose modulation callback hooks.
if ! rg -q 'mod_emit_value' "$file_api"; then
  echo "FAIL: host_api_v1 missing mod_emit_value callback" >&2
  exit 1
fi
if ! rg -q 'mod_clear_source' "$file_api"; then
  echo "FAIL: host_api_v1 missing mod_clear_source callback" >&2
  exit 1
fi
if ! rg -q 'mod_host_ctx' "$file_api"; then
  echo "FAIL: host_api_v1 missing mod_host_ctx pointer" >&2
  exit 1
fi

# Chain host should wire callbacks for instance host API.
if ! rg -q 'inst->subplugin_host_api.mod_emit_value' "$file_chain"; then
  echo "FAIL: chain host instance API missing mod_emit_value wiring" >&2
  exit 1
fi

echo "PASS: modulation host API surface is present"
