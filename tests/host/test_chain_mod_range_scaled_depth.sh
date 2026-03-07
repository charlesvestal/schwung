#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'float range_span = entry->max_val - entry->min_val;' "$file"; then
  echo "FAIL: missing modulation range span computation" >&2
  exit 1
fi
if ! rg -Fq 'if (range_span <= 0.0f) {' "$file"; then
  echo "FAIL: missing range span fallback guard" >&2
  exit 1
fi
if ! rg -q 'float range_scale = bipolar \? \(0.5f \* range_span\) : range_span;' "$file"; then
  echo "FAIL: missing polarity-aware range scaling" >&2
  exit 1
fi
if ! rg -q 'entry->contribution = \(\(mod_signal \* depth\) \+ offset\) \* range_scale;' "$file"; then
  echo "FAIL: modulation contribution is not range-scaled" >&2
  exit 1
fi

echo "PASS: modulation depth/offset is scaled to target parameter range"
