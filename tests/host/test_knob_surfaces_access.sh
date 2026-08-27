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

# --- the knob turn must branch on ACCESS before the enum stepper -------------
#
# A READOUT still bails: there is nothing to set.
#
# A TRIGGER no longer bails -- it FIRES, once per cooldown, in either
# direction. That reverses half of the original fix and the reason is that the
# original reasoning was about the enum STEPPER ("turning walks through the
# fire value"), not about the gesture: a momentary has no value to walk past.
# What keeps a knob from running the action a dozen times per flick is the
# cooldown, so the cooldown is the thing this file has to pin, and it is
# pinned as an ORDER (guard, then window, then write) because a fire placed
# after the window check is a fire with no window at all.
turn=$(awk '/A TRIGGER fires on a detent, in either direction/,/^    if \(ctx.meta && ctx.meta.type === "enum"/' "$file")
[ -n "$turn" ] || fail "the knob-turn access branch is gone from processPendingHierKnob"
command grep -q "if (isTriggerParam(ctx.meta)) {" <<<"$turn" || \
  fail "the knob turn does not check access:write"
command grep -q "if (isReadoutParam(ctx.meta)) {" <<<"$turn" || \
  fail "the knob turn does not check access:read"
command grep -q "TRIGGER_KNOB_COOLDOWN_MS" <<<"$turn" || \
  fail "a knob detent fires a trigger with NO cooldown -- one flick runs the action a dozen times"
command grep -q "triggerFireValue(ctx.meta" <<<"$turn" || \
  fail "the knob fire does not use the module wire value -- a bare index destroys euclidrum kits"
# The cooldown must be checked BEFORE the write, not after it.
# `|| true` on every lookup: with `set -euo pipefail` a grep that finds nothing
# kills the script at the assignment, so the fail() message below -- the only
# thing that says WHICH invariant broke -- never prints. An unexplained exit 1
# is how a deliberate change gets mistaken for a broken harness.
cl=$( { command grep -n "TRIGGER_KNOB_COOLDOWN_MS) return;" "$file" || true; } | head -n 1 | cut -d: -f1)
fl=$( { command grep -n "setSlotParam(ctx.slot, ctx.fullKey, fire)" "$file" || true; } | head -n 1 | cut -d: -f1)
[ -n "$cl" ] && [ -n "$fl" ] && [ "$cl" -lt "$fl" ] || \
  fail "the knob cooldown is checked AFTER the fire (window $cl, write $fl) -- it gates nothing"
# ORDER is the whole point: both guards must precede the enum stepper's read.
g=$( { command grep -n "if (isTriggerParam(ctx.meta)) {" "$file" || true; } | head -n 1 | cut -d: -f1)
r=$( { command grep -n "if (isReadoutParam(ctx.meta)) {" "$file" || true; } | head -n 1 | cut -d: -f1)
w=$( { command grep -n "const currentVal = getKnobCachedValue" "$file" || true; } | head -n 1 | cut -d: -f1)
[ -n "$g" ] && [ -n "$r" ] && [ -n "$w" ] && [ "$g" -lt "$w" ] && [ "$r" -lt "$w" ] || \
  fail "an access guard runs AFTER the value is read (trigger $g, readout $r, read $w)"
# The COOLDOWN IS KNOB-ONLY. A click is one gesture per press, and the two
# click paths must not consult the window -- a shared timer is exactly how
# "clicking twice quickly only fired once" gets introduced.
for fn in "A held TRIGGER is fired by the click" "A TRIGGER is pushed, not opened"; do
  blk=$(awk -v pat="$fn" 'index($0, pat) {n=1} n && n++ <= 40' "$file")
  command grep -q "TRIGGER_KNOB_COOLDOWN_MS" <<<"$blk" && \
    fail "the click path \"$fn\" is rate-limited by the KNOB cooldown"
done

# --- and the two surfaces must agree on how long that window is --------------
#
# The knob grid (page_controller.mjs) and this file drive the SAME physical
# encoder against the SAME parameter; which one is on screen is a Param View
# setting the user can flip. Two copies of the number is two behaviours, and
# the disagreement would only ever be noticed as "it fires differently in List
# view", which nobody would think to report as a constant.
a=$( { command grep -oE "^const TRIGGER_KNOB_COOLDOWN_MS = [0-9]+" "$file" || true; } | head -n 1)
b=$( { command grep -oE "^const TRIGGER_KNOB_COOLDOWN_MS = [0-9]+" \
         src/shared/param_pages/page_controller.mjs || true; } | head -n 1)
[ -n "$a" ] || fail "shadow_ui.js does not declare TRIGGER_KNOB_COOLDOWN_MS"
[ -n "$b" ] || fail "page_controller.mjs does not declare TRIGGER_KNOB_COOLDOWN_MS"
[ "$a" = "$b" ] || fail "the knob trigger cooldown has drifted: shadow_ui \"$a\" vs grid \"$b\""

echo "  ok  a knob detent fires a trigger once per cooldown; a readout still writes nothing"
echo "  ok  both knob surfaces share one cooldown value"

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
echo "  ok  a held trigger fires on click and consumes it"

# --- the overlay shows the VALUE, not a sentence about the parameter ---------
#
# It said "Read only" and "Click to fire", which is the surface explaining
# itself in the slot where the reading goes. Reported from the device: "we show
# READ ONLY in the header, that doesnt make sense, it just is a static value".
# A readout exists to be READ -- keydetect is nothing but its value.
# Code lines only -- the comment above the fix quotes the old strings, and
# matching those made this fail on its own explanation.
if command grep -nE '^[^*/]*"(Read only|Click to fire)"' "$file" | command grep -qv '^\s*[0-9]*:\s*\*'; then
  echo "FAIL: the overlay still prints a sentence about the parameter instead of its value" >&2
  command grep -nE '^[^*/]*"(Read only|Click to fire)"' "$file" >&2
  exit 1
fi
command grep -q "formatParamForOverlay(cached, ctx.meta)" <<<"$turn" || \
  fail "the refused turn no longer shows the parameter value"
echo "  ok  a refused turn shows the value, not a sentence about the parameter"

# --- while the card is up, a click never dives -------------------------------
#
# "when the overlay is active clicking shouldnt take you into the module, its a
# hidden element that its still selected." Releasing the knob drops the card,
# so there is a way out that does not also act on something you cannot see.
held=$(awk '/A held TRIGGER is fired by the click/,/Shift\+Click in chain edit/' "$file")
command grep -q "the click STOPS HERE either way" <<<"$held" || \
  fail "a click while the card is up can still fall through and dive into the component"
echo "  ok  a click while the card is up never dives"

echo "  ok  a knob turn cannot write a readout, and both access guards bail before the read"
echo "  ok  a click fires a trigger through the module wire, and never opens a picker"
echo "  ok  a click on a readout opens nothing"
echo "PASS: every knob surface honours access"
