#!/usr/bin/env bash
set -euo pipefail

file="src/host/shadow_chain_mgmt.c"

if ! rg -q 'strcmp\(lfo_param, "polarity"\) == 0' "$file"; then
  echo "FAIL: master FX LFO param handler missing polarity key" >&2
  exit 1
fi

if ! rg -q '\\\"polarity\\\":%d' "$file"; then
  echo "FAIL: master FX LFO config JSON missing polarity persistence" >&2
  exit 1
fi

if ! rg -q 'if \(lfo->depth < -1.0f\) lfo->depth = -1.0f;' "$file"; then
  echo "FAIL: master FX LFO depth lower clamp is not negative" >&2
  exit 1
fi

if ! rg -q 'if \(lfo->depth > 1.0f\) lfo->depth = 1.0f;' "$file"; then
  echo "FAIL: master FX LFO depth upper clamp is missing" >&2
  exit 1
fi

if ! rg -q 'float mod_signal = signal;' "$file"; then
  echo "FAIL: master FX LFO tick missing modulation signal path" >&2
  exit 1
fi

if ! rg -q 'if \(!lfo->bipolar\)' "$file"; then
  echo "FAIL: master FX LFO tick missing unipolar mapping branch" >&2
  exit 1
fi

if ! rg -q 'float range_scale = lfo->bipolar \? \(0.5f \* range_span\) : range_span;' "$file"; then
  echo "FAIL: master FX LFO tick missing polarity-aware range scaling" >&2
  exit 1
fi

echo "PASS: master FX LFO supports polarity mode and negative depth range"
