#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A slot `clear` must reset that slot's LFOs.
#
# The LFOs are per-SLOT state, not per-module, so unloading the synth and every
# FX used to leave them running and aimed at a component that no longer exists.
# A set switch clears every slot through this path, so the routing outlived the
# set that defined it.
#
# Reported from the device: "when i created a new set, the LFO was still active
# and targeting the now empty synth slot."
#
# Zeroing is safe rather than lossy: loading a patch assigns the whole array
# from the patch, so nothing a patch defines can be lost by clearing first.

fail() { echo "FAIL: $1" >&2; exit 1; }
f="src/modules/chain/dsp/chain_host.c"

blk=$(awk '/strcmp\(key, "clear"\) == 0/,/malloc_trim\(0\)/' "$f")
[ -n "$blk" ] || fail "the clear handler is gone from $f"

command grep -q "memset(inst->lfos, 0, sizeof(inst->lfos))" <<<"$blk" || \
  fail "clear does not reset inst->lfos — a new set keeps the old set's LFO routing"
command grep -q "memset(inst->lfo_base_valid" <<<"$blk" || \
  fail "clear does not reset lfo_base_valid — a stale base would be re-applied to whatever loads next"

# It must clear AFTER the unloads, not before: the targets are only meaningless
# once the modules are gone, and ordering it first would be a silent no-op the
# moment an unload path grows an LFO write of its own.
u=$(command grep -n "v2_unload_synth(inst)" <<<"$blk" | head -n 1 | cut -d: -f1)
l=$(command grep -n "memset(inst->lfos" <<<"$blk" | head -n 1 | cut -d: -f1)
[ -n "$u" ] && [ -n "$l" ] && [ "$u" -lt "$l" ] || \
  fail "the LFO reset runs before the modules are unloaded"

# And a patch load must still assign the whole array, which is what makes
# zeroing safe. If that ever becomes a merge, clearing first LOSES settings.
command grep -q "inst->lfos\[i\] = patch->lfos\[i\]" src/modules/chain/dsp/chain_patch.c || \
  fail "a patch load no longer assigns the LFO array wholesale — clearing first is then lossy"

echo "  ok  clear resets the LFOs, after the unloads"
echo "  ok  a patch load still assigns the array wholesale, so clearing first loses nothing"
echo "PASS: a cleared slot carries no LFO routing into the next set"
