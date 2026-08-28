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

# The JS inject must NOT push into the test bus's ring. While overtake is
# active the shim pops that ring onto the module, so a tool injecting a button
# CC there writes its own input — song-mode's Play CC came straight back and
# toggled its own playback on a loop.
if ! grep -q 'shadow_midi_inject_ui' "$UI"; then
    echo "FAIL: move_midi_inject_to_move does not use the shadow UI's own ring" >&2
    exit 1
fi
if grep -q 'shadow_midi_inject_push(shadow_midi_inject,' "$UI"; then
    echo "FAIL: the JS inject pushes straight into the test-bus ring" >&2
    exit 1
fi

# ...and the shim's overtake republish must claim only the test-bus ring.
if grep -q 'shadow_midi_inject_peek(shadow_midi_inject_ui_shm' "$SHIM"; then
    echo "FAIL: the overtake republish consumes the shadow UI's ring" >&2
    exit 1
fi
grep -q 'shadow_midi_inject_ui_shm' "$SHIM"

echo "PASS: overtake Move-injection capability and ownership contract"
