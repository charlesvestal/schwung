#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp"

if ! rg -q '(static )?int chain_mod_get_modulated_for_subkey' "$file"; then
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

# Every KIND of position must route through the helper. This used to name
# fx1, fx2, midi_fx1 and midi_fx2 literally, and that stopped being how the
# code is written when the chain became variable length: fx1..fx8 now share
# one handler that computes the component id from the parsed index, so there
# are no per-slot call sites left to pin. The literal list also silently
# stopped covering fx3..fx8 the moment the cap rose.
#
# The synth is still addressed by a literal because there is exactly one.
if ! rg -q 'chain_mod_get_modulated_for_subkey\(inst, "synth", subkey, buf, buf_len\);' "$file"; then
  echo "FAIL: get_param path is missing :modulated support for the synth" >&2
  exit 1
fi

for var in fx_id mfx_id; do
  if ! rg -q "chain_mod_get_modulated_for_subkey\\(inst, $var, subkey, buf, buf_len\\);" "$file"; then
    echo "FAIL: get_param path is missing :modulated support via $var" >&2
    exit 1
  fi
done

# And the id must be DERIVED from the parsed index, not a constant wearing a
# variable name -- otherwise the loop above would pass on a hardcoded fx1.
# The index variable is deliberately left open -- pinning a local name is how
# a test breaks on a rename that changed nothing. What matters is the SECTION.
if ! rg -q 'chain_fx_component_id\(fx_id, sizeof\(fx_id\), "fx", ' "$file"; then
  echo "FAIL: fx_id is not derived from the parsed position index" >&2
  exit 1
fi
if ! rg -q 'chain_fx_component_id\(mfx_id, sizeof\(mfx_id\), "midi_fx", ' "$file"; then
  echo "FAIL: mfx_id is not derived from the parsed position index" >&2
  exit 1
fi

echo "PASS: chain get_param supports :modulated suffix for UI modulation indicators"
