#!/usr/bin/env bash
set -euo pipefail

# A module's saved state must be handed back COMPACT, never as the raw
# pretty-printed slot-file slice.
#
# slot_N.json and master_fx_N.json are written by JSON.stringify(w, null, 2),
# and the loaders do not re-serialize: chain_patch.c used to brace-match the
# "state" object in the FILE TEXT and strncpy the slice straight into
# synth_state / cfg->state, so set_param("state") received `"key": "` where
# the module's own emission — the thing its parser was written against — says
# `"key":"`. Whitespace-exact parsers (strstr of `"patch":"` and friends)
# silently missed, while whitespace-tolerant number parsers in the same
# modules kept landing, so the headline fields restored and the payload
# vanished. Field-confirmed in minijv (every working-patch edit lost on set
# reload), mono (entire state ignored), work (patterns), smack (locks),
# noisemaker (bank). The fix is json_object_compact_copy (json_compact.h),
# applied at every site that extracts a state object from a stored file.
#
# chain_patch.c cannot be compiled standalone (see test_chain_knob_cc_out.sh),
# so its three sites are pinned by source; the helper's behavior itself is
# covered by tests/host/test_json_compact.c, and master_fx_saved_state.h's
# use of it by test_master_fx_saved_state.c.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CHAIN_PATCH="$REPO_ROOT/src/modules/chain/dsp/chain_patch.c"
MFX_STATE="$REPO_ROOT/src/host/master_fx_saved_state.h"

fails=0
fail() { echo "FAIL: $1"; fails=$((fails + 1)); }

# The three chain_patch.c extraction sites (synth, midi_fx, audio_fx) all
# route through the compactor.
n=$(command grep -c 'json_object_compact_copy(' "$CHAIN_PATCH" || true)
[ "$n" -ge 3 ] || fail "chain_patch.c has $n json_object_compact_copy call(s), want the 3 state sites"

# No raw slice copy of a state object survives. The opaque-STRING branches
# (json_decode_quoted_string) are fine — a string decode is already compact.
if command grep -n 'strncpy(patch->synth_state, sv' "$CHAIN_PATCH"; then
    fail "synth state is strncpy'd raw from the file slice again"
fi
if command grep -n 'strncpy(cfg->state, state_start' "$CHAIN_PATCH"; then
    fail "fx state is strncpy'd raw from the file slice again"
fi

# Master FX boot restore compacts too.
command grep -q 'json_object_compact_copy(' "$MFX_STATE" \
    || fail "master_fx_saved_state.h no longer routes objects through json_object_compact_copy"

[ "$fails" -eq 0 ] || exit 1
echo "PASS: stored state objects are handed to modules compact"
