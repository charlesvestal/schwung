#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/modules/chain/dsp/chain_host.c"

# chain_host's own v1 implementation was unreachable in deployment (every
# loader resolves move_plugin_init_v2 first, and chain_host exports it) and
# cost ~2,600 lines + ~7.6 MB of BSS per process. Deleted in the 2026-06
# dead-code sweep; this pins it out. The v1 *contract* for external modules
# (src/host/plugin_api_v1.h) is untouched.

if rg -q 'move_plugin_init_v1' "$file"; then
  echo "FAIL: chain_host still defines/references the v1 plugin entry" >&2
  exit 1
fi
if ! rg -q 'move_plugin_init_v2' "$file"; then
  echo "FAIL: chain_host v2 entry missing" >&2
  exit 1
fi

for relic in 'g_patches' 'static void start_recording' 'chord_type_t' \
             'g_subplugin_host_api' 'midi_fx_js' 'audio_fx_api_v1'; do
  if rg -q "$relic" "$file"; then
    echo "FAIL: v1 relic still present: $relic" >&2
    exit 1
  fi
done

echo "PASS: chain_host is v2-only"
