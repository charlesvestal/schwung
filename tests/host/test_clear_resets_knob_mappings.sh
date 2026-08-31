#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A slot `clear` must reset that slot's knob mappings — same defect class as
# test_clear_resets_lfos.sh, same fix shape, one component later.
#
# Knob mappings are per-SLOT state, not per-module, so unloading the synth and
# every FX used to leave a knob "assigned" to a component that no longer
# exists. Two-pass set switching only calls load_file when the new Set has
# SAVED slot state; an empty Set has none, so pass 2 never runs for that slot
# and `clear` is the only reset a mapping gets. Reported from the device: a
# knob assigned to a param in one Set still read as assigned (and, if turned,
# still forwarded to whatever now occupied that position) after switching to
# an empty Set.
#
# Zeroing is safe rather than lossy: loading a patch assigns the whole array
# from the patch, so nothing a patch defines can be lost by clearing first.

fail() { echo "FAIL: $1" >&2; exit 1; }
f="src/modules/chain/dsp/chain_host.c"

blk=$(awk '/strcmp\(key, "clear"\) == 0/,/malloc_trim\(0\)/' "$f")
[ -n "$blk" ] || fail "the clear handler is gone from $f"

command grep -q "memset(inst->knob_mappings, 0, sizeof(inst->knob_mappings))" <<<"$blk" || \
  fail "clear does not reset inst->knob_mappings — a new set keeps the old set's knob assignments"
command grep -q "inst->knob_mapping_count = 0;" <<<"$blk" || \
  fail "clear does not reset knob_mapping_count — a stale mapping stays iterable after the reset"

# It must clear AFTER the unloads, not before: a mapping's target is only
# meaningless once the modules are gone, and ordering it first would be a
# silent no-op the moment an unload path grows a knob-mapping write of its own.
u=$(command grep -n "v2_unload_synth(inst)" <<<"$blk" | head -n 1 | cut -d: -f1)
k=$(command grep -n "memset(inst->knob_mappings" <<<"$blk" | head -n 1 | cut -d: -f1)
[ -n "$u" ] && [ -n "$k" ] && [ "$u" -lt "$k" ] || \
  fail "the knob-mapping reset runs before the modules are unloaded"

# And a patch load must still assign the whole array, which is what makes
# zeroing safe. If that ever becomes a merge, clearing first LOSES settings.
command grep -q "memcpy(inst->knob_mappings, patch->knob_mappings," src/modules/chain/dsp/chain_patch.c || \
  fail "a patch load no longer assigns the knob_mappings array wholesale — clearing first is then lossy"

echo "  ok  clear resets knob_mappings, after the unloads"
echo "  ok  a patch load still assigns the array wholesale, so clearing first loses nothing"
echo "PASS: a cleared slot carries no knob assignment into the next set"
