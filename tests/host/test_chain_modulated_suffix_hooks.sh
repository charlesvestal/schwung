#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'static int chain_mod_get_modulated_for_subkey' "$file"; then
  echo "FAIL: modulation status helper is missing" >&2
  exit 1
fi

if ! rg -q 'const size_t suffix_len = 10; /\* ":modulated" \*/' "$file"; then
  echo "FAIL: modulation status helper does not parse :modulated suffix" >&2
  exit 1
fi

if ! rg -q 'if \(entry && entry->active && chain_mod_has_active_sources\(entry\)\)' "$file"; then
  echo "FAIL: modulation status helper does not check active modulation sources" >&2
  exit 1
fi

for target in synth fx1 fx2 midi_fx1 midi_fx2; do
  if ! rg -q "chain_mod_get_modulated_for_subkey\\(inst, \"$target\", subkey, buf, buf_len\\);" "$file"; then
    echo "FAIL: get_param path is missing :modulated support for $target" >&2
    exit 1
  fi
done

echo "PASS: chain get_param supports :modulated suffix for UI modulation indicators"
