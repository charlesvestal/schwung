#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'getSlotParam\(slotIndex, "lfo_config"\)' "$file"; then
  echo "FAIL: slot patch export does not fetch lfo_config from DSP" >&2
  exit 1
fi

if ! rg -q 'patch\.lfos = lfos;' "$file"; then
  echo "FAIL: slot patch export does not persist LFO config into chain JSON" >&2
  exit 1
fi

if ! rg -q 'activeSlotStateDir \+ "/slot_" \+ i \+ "\.json"' "$file"; then
  echo "FAIL: autosave path is not writing per-slot state files" >&2
  exit 1
fi

if ! rg -q 'setSlotParamWithTimeout\(i, "load_file", path, 1500\)' "$file"; then
  echo "FAIL: set restore flow is not loading slot state files" >&2
  exit 1
fi

if ! rg -q 'host_write_file\(newDir \+ "/slot_" \+ i \+ "\.json", "\{\}\\n"\);' "$file"; then
  echo "FAIL: new set initialization does not seed slot state files" >&2
  exit 1
fi

if ! rg -q 'const src = host_read_file\(srcDir \+ "/slot_" \+ i \+ "\.json"\);' "$file"; then
  echo "FAIL: duplicated set flow does not copy per-slot state files" >&2
  exit 1
fi

if rg -q 'function getSlotLfoStatePath\(slotIndex, stateDir\)' "$file"; then
  echo "FAIL: dedicated slot LFO file helper still present; expected main inline behavior" >&2
  exit 1
fi

if rg -q 'restoreSlotLfosFromStateDir\(activeSlotStateDir' "$file"; then
  echo "FAIL: dedicated slot LFO restore flow still present; expected main inline behavior" >&2
  exit 1
fi

echo "PASS: slot LFO persistence follows main inline slot state flow"
