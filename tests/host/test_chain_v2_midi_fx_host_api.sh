#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

# v2 MIDI FX must be initialized with the instance-specific host API, not the
# global v1 host API object. This ensures get_clock_status is wired in v2.
if ! rg -q 'midi_fx_api_v1_t \*api = init_fn\(&inst->subplugin_host_api\);' "$file"; then
  echo "FAIL: v2 MIDI FX init is not using inst->subplugin_host_api" >&2
  exit 1
fi

if rg -q 'midi_fx_api_v1_t \*api = init_fn\(&g_subplugin_host_api\);' "$file"; then
  echo "FAIL: v2 MIDI FX init still references g_subplugin_host_api" >&2
  exit 1
fi

echo "PASS: v2 MIDI FX init uses instance host API"
