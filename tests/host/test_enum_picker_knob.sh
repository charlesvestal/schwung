#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# While the enum option list is up, a knob must SCROLL IT.
#
# You reach the picker by holding a knob and clicking, so the hand is already
# on the knob and the reflex is to keep turning it — reported from the device
# as "I keep trying to keep turning it". Jog-only made the gesture change hands
# halfway through.
#
# It is also a correctness fix. Without the route the turn fell through to
# adjustKnobAndShow and moved the value BEHIND the list — invisibly, because
# the picker covers the grid — and Back then "cancelled" a change that had
# already been written. That is the half worth pinning: the ordering.

file="src/shadow/shadow_ui.js"

# The knob CC handler, from the CC test to the adjustKnobAndShow fallthrough.
block=$(awk '/Handle knob CCs \(71-78\) for parameter control/,/adjustKnobAndShow\(knobIndex, delta\)/' "$file")
if [ -z "$block" ]; then
  echo "FAIL: could not find the knob CC handler in $file" >&2
  exit 1
fi

if ! grep -q "VIEWS.ENUM_PICKER" <<<"$block"; then
  echo "FAIL: a knob turn is not routed to the enum picker." >&2
  echo "      The turn falls through to adjustKnobAndShow and edits the value" >&2
  echo "      behind the list, where the user cannot see it." >&2
  exit 1
fi

if ! grep -q "enumPickerJog" <<<"$block"; then
  echo "FAIL: the ENUM_PICKER branch does not call enumPickerJog" >&2
  exit 1
fi

# ORDER: the picker route must come BEFORE adjustKnobAndShow, or the value is
# edited first and the scroll is dead code.
pick_line=$(grep -n "VIEWS.ENUM_PICKER" <<<"$block" | head -n 1 | cut -d: -f1)
adj_line=$(grep -n "adjustKnobAndShow(knobIndex, delta)" <<<"$block" | head -n 1 | cut -d: -f1)
if [ -z "$pick_line" ] || [ -z "$adj_line" ] || [ "$pick_line" -ge "$adj_line" ]; then
  echo "FAIL: the ENUM_PICKER route must come BEFORE adjustKnobAndShow" >&2
  echo "      (picker at line $pick_line, adjustKnobAndShow at $adj_line)" >&2
  exit 1
fi

# And it must RETURN, or the turn does both.
tail_after=$(sed -n "${pick_line},\$p" <<<"$block" | sed -n '1,12p')
if ! grep -q "return;" <<<"$tail_after"; then
  echo "FAIL: the ENUM_PICKER branch does not return — the turn would scroll" >&2
  echo "      the list AND edit the value underneath it." >&2
  exit 1
fi

# enumPickerJog must still clamp rather than run off the ends: it is now driven
# by a knob, which can deliver a delta bigger than 1 on a fast turn.
jog=$(awk '/^function enumPickerJog\(/,/^}/' "$file")
if [ -z "$jog" ]; then
  echo "FAIL: enumPickerJog not found" >&2
  exit 1
fi
if ! grep -q "Math.max(0" <<<"$jog" || ! grep -q "Math.min(" <<<"$jog"; then
  echo "FAIL: enumPickerJog no longer clamps to the option range" >&2
  exit 1
fi

echo "PASS: a knob scrolls the enum picker, before adjustKnobAndShow, and returns"
