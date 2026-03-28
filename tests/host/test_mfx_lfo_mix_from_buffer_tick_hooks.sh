#!/usr/bin/env bash
set -euo pipefail

file="src/schwung_shim.c"

if ! rg -q 'static void shadow_inprocess_mix_from_buffer\(' "$file"; then
  echo "FAIL: missing shadow_inprocess_mix_from_buffer()" >&2
  exit 1
fi

if ! rg -q 'shadow_master_fx_lfo_tick\(FRAMES_PER_BLOCK\);' "$file"; then
  echo "FAIL: missing Master FX LFO tick call in shim audio path" >&2
  exit 1
fi

echo "PASS: shim mix_from_buffer path ticks Master FX LFOs"
