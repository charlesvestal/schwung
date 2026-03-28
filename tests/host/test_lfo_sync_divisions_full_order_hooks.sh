#!/usr/bin/env bash
set -euo pipefail

host_file="src/host/lfo_common.h"
ui_file="src/shadow/shadow_ui.js"
chain_file="src/modules/chain/dsp/chain_host.c"
mfx_file="src/host/shadow_chain_mgmt.c"

expected_labels="16bar,15bar,14bar,13bar,12bar,11bar,10bar,9bar,8bar,7bar,6bar,5bar,4bar,3bar,2bar,1/1,1/1T,1/2,1/2T,1/4,1/4T,1/8,1/8T,1/16,1/16T,1/32,1/32T"

if ! rg -q '^#define LFO_NUM_DIVISIONS 27$' "$host_file"; then
  echo "FAIL: LFO_NUM_DIVISIONS should be 27 after removing duplicate 1bar/1/1" >&2
  exit 1
fi

host_labels="$(sed -n '/static const lfo_division_t lfo_divisions/,/};/p' "$host_file" \
  | rg -o '"[^"]+"' \
  | tr -d '"' \
  | paste -sd ',' -)"
if [[ "$host_labels" != "$expected_labels" ]]; then
  echo "FAIL: host LFO division labels are not in the expected full ordered list" >&2
  echo "Expected: $expected_labels" >&2
  echo "Actual:   $host_labels" >&2
  exit 1
fi

ui_labels="$(sed -n '/const LFO_DIVISIONS = \[/,/];/p' "$ui_file" \
  | rg -o '"[^"]+"' \
  | tr -d '"' \
  | paste -sd ',' -)"
if [[ "$ui_labels" != "$expected_labels" ]]; then
  echo "FAIL: UI LFO division labels are not in the expected full ordered list" >&2
  echo "Expected: $expected_labels" >&2
  echo "Actual:   $ui_labels" >&2
  exit 1
fi

if rg -q 'rate_div \|\| 16' "$ui_file"; then
  echo "FAIL: UI must not use falsy fallback for rate_div (breaks index 0 / 16bar)" >&2
  exit 1
fi

if ! rg -q 'lfoConfig\.rate_div\)\)\s*$' "$ui_file"; then
  echo "FAIL: preset restore should parse lfoConfig.rate_div numerically" >&2
  exit 1
fi

if ! rg -U -q 'Number\.isFinite\(Number\((cfg|lfoConfig)\.rate_div\)\)\s*\n\s*\?\s*Number\((cfg|lfoConfig)\.rate_div\)\s*\n\s*:\s*15;' "$ui_file"; then
  echo "FAIL: applied config fallback rate_div should be index 15 (1/1)" >&2
  exit 1
fi

if ! rg -q 'lfo->sync && lfo->rate_div == 0\) lfo->rate_div = 15;' "$mfx_file"; then
  echo "FAIL: Master FX LFO sync default should map to index 15 (1/1)" >&2
  exit 1
fi

if ! rg -q 'lfo->sync && lfo->rate_div == 0\) lfo->rate_div = 15;' "$chain_file"; then
  echo "FAIL: Slot LFO sync default should map to index 15 (1/1)" >&2
  exit 1
fi

echo "PASS: full ordered LFO sync timing list and 1/1 defaults are wired across host + UI"
