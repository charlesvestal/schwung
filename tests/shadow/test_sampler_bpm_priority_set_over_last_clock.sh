#!/usr/bin/env bash
set -euo pipefail

# Regression test: when MIDI clock is not active, sampler BPM fallback should
# prefer current Set tempo over stale last-known clock tempo.

file="src/host/shadow_sampler.c"

if ! command -v rg >/dev/null 2>&1; then
  echo "rg is required to run this test" >&2
  exit 1
fi

func_start=$(rg -n "^float sampler_get_bpm\\(tempo_source_t \\*source\\)" "$file" | cut -d: -f1 | head -n1 || true)
if [ -z "$func_start" ]; then
  echo "FAIL: could not locate sampler_get_bpm()" >&2
  exit 1
fi

func_block=$(sed -n "${func_start},$((func_start + 80))p" "$file")

set_line=$(printf "%s\n" "$func_block" | rg -n "Current Set's tempo|set_tempo" | head -n1 | cut -d: -f1 || true)
last_line=$(printf "%s\n" "$func_block" | rg -n "Last measured clock BPM|sampler_last_known_bpm" | head -n1 | cut -d: -f1 || true)

if [ -z "$set_line" ] || [ -z "$last_line" ]; then
  echo "FAIL: could not find set tempo or last clock checks in sampler_get_bpm()" >&2
  exit 1
fi

if [ "$set_line" -lt "$last_line" ]; then
  echo "PASS: sampler_get_bpm() prefers set tempo over last-known clock tempo"
  exit 0
fi

echo "FAIL: sampler_get_bpm() still prefers last-known clock tempo over set tempo" >&2
exit 1
