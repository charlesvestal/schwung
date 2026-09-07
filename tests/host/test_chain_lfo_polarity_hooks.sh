#!/usr/bin/env bash
set -euo pipefail

# TWO FILES, because the mod-route cluster is split by role. The set/get
# ladders moved to chain_mod_routes.c when the source types were added
# (chain_host.c is pinned below 2900 lines by test_chain_host_file_split.sh);
# the per-block emit and the lfo_config save document stayed in chain_host.c
# with the render path. Point each check at the file that owns it -- a single
# variable here would pass only until the next split.
routes="src/modules/chain/dsp/chain_mod_routes.c"   # the param ladders
host="src/modules/chain/dsp/chain_host.c"           # emit + JSON save
file="$routes"
common="src/host/lfo_common.h"

if ! rg -q 'int bipolar;' "$common"; then
  echo "FAIL: lfo_state_t is missing bipolar field" >&2
  exit 1
fi

if ! rg -q 'strcmp\(subkey, "polarity"\) == 0' "$file"; then
  echo "FAIL: slot LFO set_param handler missing polarity key" >&2
  exit 1
fi

if ! rg -q 'if \(strcmp\(subkey, "polarity"\) == 0\)' "$file"; then
  echo "FAIL: slot LFO get_param handler missing polarity key" >&2
  exit 1
fi

if ! rg -q '\\\"polarity\\\":%d' "$host"; then
  echo "FAIL: slot LFO JSON config is missing polarity persistence" >&2
  exit 1
fi

if ! rg -q 'chain_mod_emit_value\(inst, source_id, lfo->target, lfo->param,' "$host"; then
  echo "FAIL: slot LFO modulation emit call is missing" >&2
  exit 1
fi

if ! rg -q 'signal, lfo->depth, 0.0f, lfo->bipolar, 1 /\*enabled\*/\);' "$host"; then
  echo "FAIL: slot LFO modulation emit does not forward polarity mode" >&2
  exit 1
fi

if ! rg -q 'if \(lfo->depth < -1.0f\) lfo->depth = -1.0f;' "$file"; then
  echo "FAIL: slot LFO depth lower clamp is not negative" >&2
  exit 1
fi

if ! rg -q 'if \(lfo->depth > 1.0f\) lfo->depth = 1.0f;' "$file"; then
  echo "FAIL: slot LFO depth upper clamp is missing" >&2
  exit 1
fi

echo "PASS: slot LFO supports polarity mode and negative depth range"
