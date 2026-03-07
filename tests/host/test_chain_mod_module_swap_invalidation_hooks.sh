#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'static void chain_mod_clear_target_entries\(' "$file"; then
  echo "FAIL: missing helper to clear modulation entries for a specific target" >&2
  exit 1
fi
if ! rg -q 'chain_mod_clear_target_entries\(inst, "synth", 0\);' "$file"; then
  echo "FAIL: synth unload path does not clear modulation entries without base restore" >&2
  exit 1
fi
if ! rg -q 'chain_mod_clear_target_entries\(inst, target_name, 0\);' "$file"; then
  echo "FAIL: audio FX slot unload path does not clear target modulation entries" >&2
  exit 1
fi
if ! rg -q 'chain_mod_clear_target_entries\(inst, target, 0\);' "$file"; then
  echo "FAIL: MIDI FX unload path does not clear per-slot modulation entries" >&2
  exit 1
fi

echo "PASS: module unload/swap paths clear modulation target entries"
