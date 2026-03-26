#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"
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

if ! rg -q '\\\"polarity\\\":%d' "$file"; then
  echo "FAIL: slot LFO JSON config is missing polarity persistence" >&2
  exit 1
fi

if ! rg -q 'chain_mod_emit_value\(inst, source_id, lfo->target, lfo->param,' "$file"; then
  echo "FAIL: slot LFO modulation emit call is missing" >&2
  exit 1
fi

if ! rg -q 'signal, lfo->depth, 0.0f, lfo->bipolar, 1 /\*enabled\*/\);' "$file"; then
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
