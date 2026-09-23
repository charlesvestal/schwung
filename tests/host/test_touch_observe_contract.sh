#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# capabilities.touch_observe: knob/jog touch edges reach the opted-in SYNTH,
# and nothing else.
#
# The predicate (which slots are a touch) is a compiled unit below. The rest
# is shape, pinned at source level because the callers run on the SPI callback
# and cannot be built here:
#
#   1. The raw hardware scan publishes NOTHING to the shadow UI. It has no
#      cable test and runs in every mode; publishing from it turned an external
#      keyboard's note 9 into a jog touch and doubled every jog touch in
#      overtake, whose own walk already forwards everything.
#   2. Jog touch reaches the UI from the display-mode walk, which is cable 0.
#   3. The chain hands a MOVE_MIDI_SOURCE_TOUCH message to the synth and
#      returns BEFORE the LFO retrigger, the MIDI FX, Pre-mode injection and
#      the audio FX -- every one of which reads a note 0-9 as a played note.

fail() { echo "FAIL: $1"; exit 1; }
strip_comments() { perl -0pe 's{/\*.*?\*/}{}gs; s{//[^\n]*}{}g' "$1"; }

bin="build/tests/test_touch_observe"
mkdir -p "$(dirname "$bin")"
cc -std=gnu11 -Wall -Wextra -Isrc/host tests/host/test_touch_observe.c -o "$bin"
"$bin"

grep -q "#define MOVE_MIDI_SOURCE_TOUCH" src/host/plugin_api_v1.h \
  || fail "plugin_api_v1.h does not define MOVE_MIDI_SOURCE_TOUCH"

# --- dispatcher ------------------------------------------------------------
disp=$(strip_comments src/host/shadow_midi.c | awk '/^void shadow_chain_dispatch_touch_to_slots/,/^}/')
[ -n "$disp" ] || fail "shadow_chain_dispatch_touch_to_slots is gone"
grep -q "touch_observe_is_edge(slot8)" <<<"$disp" || fail "dispatcher does not use touch_observe_is_edge"
grep -q "event_dedup_check_and_record" <<<"$disp" || fail "dispatcher does not dedup"
grep -q "MOVE_MIDI_SOURCE_TOUCH" <<<"$disp" || fail "dispatcher does not tag touches MOVE_MIDI_SOURCE_TOUCH"
grep -q "MOVE_MIDI_SOURCE_HOST" <<<"$disp" && fail "dispatcher sends touches as HOST -- the chain cannot tell them from clock"

# --- shim: raw scan publishes nothing; display walk carries note 9 ---------
shim=$(strip_comments src/schwung_shim.c)
scan=$(awk '/shadow_chain_dispatch_touch_to_slots\(&tsrc\[j\]\)/{f=1} f{print} f&&/^    }$/{exit}' <<<"$shim")
[ -n "$scan" ] || fail "the raw touch scan no longer calls the dispatcher"
grep -q "shadow_ui_midi_publish" <<<"$scan" \
  && fail "the raw touch scan publishes to the shadow UI (no cable test, every mode)"
grep -Eq '\(d1 <= 7 \|\| d1 == 9\) && shadow_ui_midi_shm' <<<"$shim" \
  || fail "the display-mode walk no longer forwards jog touch (9)"

# --- chain: TOUCH goes to the synth and returns before everything else ------
midi=$(strip_comments src/modules/chain/dsp/chain_midi.c | awk '/^void v2_on_midi\(/,/^}/')
[ -n "$midi" ] || fail "v2_on_midi is gone"
touch_ln=$(grep -n "source == MOVE_MIDI_SOURCE_TOUCH" <<<"$midi" | head -1 | cut -d: -f1 || true)
[ -n "$touch_ln" ] || fail "v2_on_midi has no MOVE_MIDI_SOURCE_TOUCH branch"
for later in chain_update_clock_runtime lfo_process_midi pre_mode_is_echo v2_process_midi_fx fx_on_midi chain_record_synth_note; do
  ln=$(grep -n "$later" <<<"$midi" | head -1 | cut -d: -f1 || true)
  [ -n "$ln" ] || fail "v2_on_midi no longer calls $later -- update this test"
  [ "$touch_ln" -lt "$ln" ] || fail "the TOUCH branch runs after $later"
done
branch=$(awk -v s="$touch_ln" 'NR>=s{print} NR>s&&/^    }$/{exit}' <<<"$midi")
grep -q "synth_touch_observe" <<<"$branch" || fail "TOUCH branch does not check synth_touch_observe"
grep -q "return;" <<<"$branch" || fail "TOUCH branch falls through into the note paths"

echo "PASS: touch_observe reaches the opted-in synth only; jog touch reaches the UI from cable 0 only"
