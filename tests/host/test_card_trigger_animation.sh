#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The press animation has to be WIRED by every host, not just implemented once.
#
# render_page_movy draws the trigger button and it is PURE: it takes the fire
# times and the clock off its options object. There are two hosts driving that
# renderer -- page_controller for the param-pages grid, and shadow_ui for the
# chain editor knob card -- and each assembles its own options.
#
# The grid shipped without them once and the button drew its idle phase
# forever. The card then shipped with exactly the same omission, reported from
# the device as "there's no fire animation for buttons". One renderer, two
# hosts, and the plumbing is the thing that diverges.
#
# So this pins the WIRING on the shadow_ui side, at the source: the file cannot
# be imported off-device.

fail() { echo "FAIL: $1" >&2; exit 1; }
file="src/shadow/shadow_ui.js"

blk=$(awk '/^function knobCardDrawState\(/,/^}/' "$file")
[ -n "$blk" ] || fail "knobCardDrawState is gone"

command grep -q "triggerFiredAt:" <<<"$blk" || \
  fail "the card hands the renderer no fire times -- the button draws its idle phase forever"
command grep -q "nowMs:" <<<"$blk" || \
  fail "the card hands the renderer no clock -- the animation cannot advance"

# The fire times must be RECORDED, or the map handed over is always empty and
# the two lines above are decoration.
command grep -q "function noteTriggerFired" "$file" || fail "nothing records when a trigger fired"
fires=$(command grep -c "noteTriggerFired(" "$file")
# One definition + every place that fires a trigger.
[ "$fires" -ge 3 ] || \
  fail "only $fires reference(s) to noteTriggerFired: a fire path is not recording, so it will not animate"

# Every setSlotParam that writes a trigger fire value must be followed by the
# record. Checked by pairing: the fire helper and the recorder appear together.
for blkname in "A TRIGGER is pushed, not opened" "A held TRIGGER is fired by the click"; do
  b=$(awk "/$blkname/,/^    }/" "$file")
  [ -n "$b" ] || fail "the '$blkname' path is gone"
  command grep -q "triggerFireValue" <<<"$b" || fail "'$blkname' no longer fires through the wire helper"
  command grep -q "noteTriggerFired" <<<"$b" || \
    fail "'$blkname' fires without recording the time -- it will not animate"
done

# And the map must be bounded: it is keyed by param and appended to per press.
note=$(awk '/^function noteTriggerFired\(/,/^}/' "$file")
command grep -q "filter" <<<"$note" || \
  fail "noteTriggerFired never drops old timestamps -- the array grows for the whole session"

echo "  ok  the card hands the renderer fire times and a clock"
echo "  ok  every fire path records, and old timestamps are dropped"
echo "PASS: the press animation is wired on the chain-editor card too"
