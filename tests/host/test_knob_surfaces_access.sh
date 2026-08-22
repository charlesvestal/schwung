#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Every knob surface must honour `access`, not just the param-pages grid.
#
# The axis landed on the param-pages knob grid and never reached shadow_ui.js.
# getKnobContext there serves the CHAIN EDITOR, MASTER FX and the hierarchy
# list editor alike, so on all three a trigger was an ordinary enum. That
# breadth is the point: the list editor is being deprecated, and if this were
# only about the list editor it would be near-dead code.
#
#   - TURNING its knob walked through the fire value and ran the action.
#     magneto's `clear` wipes the deck; euclidrum's `rnd_preset` randomises all
#     eight lanes. From a knob nudge, with no confirmation.
#   - CLICKING it opened the option picker -- a two-item list whose second item
#     is the action, i.e. another way to fire it by accident, and a "dive" into
#     something with nothing to browse.
#   - a READOUT was writable, and its picker discarded the choice in silence.
#
# Pinned at the source: shadow_ui.js cannot be imported off-device (absolute
# on-device import paths), and these are guard clauses whose absence is the
# whole bug.

fail() { echo "FAIL: $1" >&2; exit 1; }
file="src/shadow/shadow_ui.js"

command grep -q "function isTriggerParam" "$file" || fail "shadow_ui.js has no access:write test"
command grep -q "function isReadoutParam" "$file" || fail "shadow_ui.js has no access:read test"

# --- the knob turn must bail BEFORE anything is written ----------------------
turn=$(awk '/A trigger is FIRED, never scrubbed/,/^    if \(ctx.meta && ctx.meta.type === "enum"/' "$file")
[ -n "$turn" ] || fail "the knob-turn guard is gone from processPendingHierKnob"
command grep -q "isTriggerParam(ctx.meta) || isReadoutParam(ctx.meta)" <<<"$turn" || \
  fail "the knob turn does not check access"
# ORDER is the whole point: the guard must precede the value read and the write.
g=$(command grep -n "isTriggerParam(ctx.meta) || isReadoutParam" "$file" | head -n 1 | cut -d: -f1)
w=$(command grep -n "const currentVal = getKnobCachedValue" "$file" | head -n 1 | cut -d: -f1)
[ -n "$g" ] && [ -n "$w" ] && [ "$g" -lt "$w" ] || \
  fail "the access guard runs AFTER the value is read (guard $g, read $w) -- a turn still writes"

# --- the click must fire a trigger, not open a picker ------------------------
click=$(awk '/A TRIGGER is pushed, not opened/,/openEnumPicker\(\{/' "$file")
[ -n "$click" ] || fail "clicking a trigger no longer fires it"
command grep -q "triggerFireValue" <<<"$click" || fail "the click does not use the module wire value"
# The CONDITION, not just the code under it. Replacing the test with `if
# (false)` left every string below intact and this check green -- the mutation
# survived until this line existed.
command grep -q "if (!hierEditorEditMode && isTriggerParam(meta))" <<<"$click" || \
  fail "the trigger branch is no longer guarded by isTriggerParam -- it is dead code"
command grep -q "isReadoutParam(meta)) return" <<<"$click" || fail "clicking a readout still opens something"
# The trigger branch must come BEFORE the enum picker, or it is dead code.
tl=$(command grep -n "A TRIGGER is pushed, not opened" "$file" | head -n 1 | cut -d: -f1)
pl=$(command grep -n "openEnumPicker({" "$file" | head -n 1 | cut -d: -f1)
[ "$tl" -lt "$pl" ] || fail "the trigger branch is after openEnumPicker -- the picker wins"

# --- the fire value must be the module's own wire format ---------------------
fv=$(awk '/^function triggerFireValue\(/,/^}/' "$file")
command grep -q "opts\[1\]" <<<"$fv" || fail "triggerFireValue does not use option 1"
command grep -q "usesIndex" <<<"$fv" || fail "triggerFireValue ignores index-reporting modules"
# THE destructive case: a bare "0" MEANS the idle option, i.e. do nothing, and
# euclidrum fires on anything that is not it. Writing 0 must be unreachable.
command grep -q '"0"' <<<"$fv" && fail "triggerFireValue can emit \"0\" -- that means IDLE, not fire"

# --- a HELD trigger fires on the jog click, and does not dive ----------------
#
# Reported as the rule: "on the overlay, it should trigger but not dive.
# otherwise, diving of course." Holding a knob and clicking normally descends
# into the focused component; a trigger has nothing to descend into.
held=$(awk '/A held TRIGGER is fired by the click/,/Shift\+Click in chain edit/' "$file")
[ -n "$held" ] || fail "a held trigger no longer fires on the jog click"
command grep -q "knobCardKnob >= 0" <<<"$held" || fail "the held-knob branch does not check the card"
command grep -q "isTriggerParam(tctx.meta)" <<<"$held" || fail "the held-knob branch does not check access"
command grep -q "return;" <<<"$held" || fail "the click is not consumed -- it would fire AND dive"
# It must precede the dive, or the dive wins.
hl=$(command grep -n "A held TRIGGER is fired by the click" "$file" | head -n 1 | cut -d: -f1)
dl=$(command grep -n "Shift+Click in chain edit enters component edit mode" "$file" | head -n 1 | cut -d: -f1)
[ "$hl" -lt "$dl" ] || fail "the held-trigger branch runs after the dive"
echo "  ok  a held trigger fires on click and consumes it; anything else still dives"

echo "  ok  a knob turn cannot write a trigger or a readout, and bails before the read"
echo "  ok  a click fires a trigger through the module wire, and never opens a picker"
echo "  ok  a click on a readout opens nothing"
echo "PASS: every knob surface honours access"
