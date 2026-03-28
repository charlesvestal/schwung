#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'static int json_get_float\(' "$file"; then
  echo "FAIL: missing JSON float parser helper" >&2
  exit 1
fi

if ! rg -q 'json_get_float\(obj, "rate_hz", &lfo->rate_hz\)' "$file"; then
  echo "FAIL: slot LFO load does not parse rate_hz as numeric JSON" >&2
  exit 1
fi

if ! rg -q 'json_get_float\(obj, "depth", &lfo->depth\)' "$file"; then
  echo "FAIL: slot LFO load does not parse depth as numeric JSON" >&2
  exit 1
fi

if ! rg -q 'json_get_float\(obj, "phase_offset", &lfo->phase_offset\)' "$file"; then
  echo "FAIL: slot LFO load does not parse phase_offset as numeric JSON" >&2
  exit 1
fi

echo "PASS: slot LFO load_file parses numeric float fields correctly"
