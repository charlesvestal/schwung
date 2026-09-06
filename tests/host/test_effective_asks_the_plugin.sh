#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# `<key>:effective` MUST REACH THE PLUGIN.
#
# The chain owns one kind of modulation -- LFOs and macros routed to a target --
# and chain_mod_get_effective_for_subkey answers from that table. When the key
# is NOT in it, the old code stripped ":effective" and asked the plugin for the
# plain key.
#
# That is right only if the chain is the sole thing that can move a parameter,
# and it is not. A synth that drives its own value serves `<key>:effective`
# itself: MonkSynth sweeps its vowel from pad pressure, exactly as the original
# swept it from the pitch wheel. Its answer was thrown away here -- the suffix
# consumed, the plain key asked, the KNOB value returned -- so every picture of
# that vowel sat still while the sound moved. Reported from hardware twice: "the
# mouth shape should animate with pressure", then "vowel doesnt change with
# pressure".
#
# The convention is documented in CLAUDE.md ("the driven value is asked for as
# <key>:effective") and this is the one place that could honour it for a module.
#
# Source-pinned rather than unit-tested: chain_mod.c needs a live
# chain_instance_t with a loaded plugin, which is a device, not a fixture.

C=src/modules/chain/dsp/chain_mod.c
fail() { echo "FAIL: $1"; exit 1; }

[ -f "$C" ] || fail "chain_mod.c is missing"

body=$(awk '/int chain_mod_get_effective_for_subkey/,/^}/' "$C")
[ -n "$body" ] || fail "chain_mod_get_effective_for_subkey not found"

# It must build "<param>:effective" and ask the plugin with it.
echo "$body" | grep -q '%s:effective' \
    || fail "the fallback never asks the plugin for <key>:effective -- a module that drives its own value is ignored"

# And that ask must come BEFORE the plain-key fallback, or the plain key wins
# and the suffix is still being swallowed.
eff_line=$(echo "$body" | grep -n '%s:effective' | head -n 1 | cut -d: -f1)
plain_line=$(echo "$body" | grep -n 'chain_mod_get_param_string(inst, target, param,' | head -n 1 | cut -d: -f1)
[ -n "$eff_line" ] && [ -n "$plain_line" ] || fail "could not locate both fallbacks"
[ "$eff_line" -lt "$plain_line" ] \
    || fail "the plain-key fallback runs first, so the plugin is never asked"

# An empty answer is a MISS, not a value: an unserved key comes back as an empty
# buffer, and taking that as an answer would blank the reading.
echo "$body" | grep -q "buf\[0\] != '\\\\0'" \
    || fail "an empty answer is treated as a value -- an unserved key would blank the reading"

echo "PASS: an unmodulated <key>:effective asks the plugin before falling back"
