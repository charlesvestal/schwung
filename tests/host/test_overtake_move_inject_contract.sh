#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
UI="$ROOT/src/shadow/shadow_ui.c"
SHIM="$ROOT/src/schwung_shim.c"
API="$ROOT/docs/API.md"
GUIDE="$ROOT/docs/ADDRESSING_MOVE_SYNTHS.md"

grep -q 'js_shadow_overtake_move_inject_active' "$UI"
grep -q '"shadow_overtake_move_inject_active"' "$UI"
grep -q 'midi_inject_to_move = shadow_overtake_midi_send' "$SHIM"

if grep -q 'midi_inject_to_move = shadow_chain_midi_inject' "$SHIM"; then
    echo "FAIL: overtake DSP output is wired back to the shared inject queue" >&2
    exit 1
fi

grep -q 'shadow_overtake_move_inject_active()' "$API"
grep -q 'dedicated.*queue' "$GUIDE"

echo "PASS: overtake Move-injection capability and ownership contract"
