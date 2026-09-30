#!/usr/bin/env bash
# The step menu over the shadow UI: eligible in both views, and on a screen
# that withholds steps from Move (p-lock grids) the held press is HANDED to
# Move when the menu opens, its release taken back and owed through the menu's
# queue. Source pins -- the runtime half is tests/host/test_step_menu.c and
# hardware (the withhold is SPI-callback state no unit can reach).
set -euo pipefail
cd "$(dirname "$0")/../.."
f=src/schwung_shim.c
fail() { echo "FAIL: $1"; exit 1; }

# eligible over the shadow UI too: the 1.6 gate was "!shadow_display_mode"
grep -q 'step_menu_eligibility()' "$f" || fail "call sites do not ask step_menu_eligibility()"
elig=$(awk '/static int step_menu_eligibility\(void\)/,/^}/' "$f")
echo "$elig" | grep -q 'shadow_display_mode' \
  && fail "step_menu_eligibility() is gated on the display again (Move-screen only)"
# ...but NOT in Session view or Set Overview: no step editor there, and Menu
# is Move's own view toggle
echo "$elig" | grep -q 'MOVE_UI_MODE_SESSION' || fail "step menu opens in Session view"
echo "$elig" | grep -q 'MOVE_UI_MODE_SET_OVERVIEW' || fail "step menu opens in Set Overview"
grep -q 'strcasecmp(text, "Note Mode")' src/host/shadow_dbus.c \
  || fail "the Note Mode announcement is not heard (the label would stick on Session)"

# the hand-off: opening on a withheld step queues the press and marks it used
grep -q 'step_hand_press\[ms\] = 1;' "$f" || fail "menu open does not hand the withheld press to Move"
grep -q 'step_used\[ms\] = 1;' "$f"       || fail "handed press not marked used (its release would replay a tap)"
# ...emitted after compaction
grep -q 'THE STEP MENU.S HAND-OFF PRESSES' "$f" || fail "no post-compaction emitter for handed presses"
# the handed release: owed to Move, the withhold's latch retired, the UI told
blk=$(awk '/A HANDED step.s release is Move.s now/,/continue;/' "$f")
echo "$blk" | grep -q 'step_menu_owe_release'    || fail "handed release not owed to Move"
echo "$blk" | grep -q 'SM_HOLD_SAFE_MS'          || fail "handed release not held past Move's tap window"
echo "$blk" | grep -q 'step_swallow_latch\[i\] = 0' || fail "handed release leaves the withhold latch set"
echo "$blk" | grep -q 'shadow_ui_midi_publish'   || fail "handed release not forwarded to the UI (p-lock held state)"
# the jog is the menu's while open -- not the UI's cursor too
grep -q 'd1 == 14 && step_menu_open_step() >= 0' "$f" || fail "jog still forwarded to the UI under an open menu"
# a rewrite restores the CIN the display-mode filter zeroed
grep -q 'sh\[0\] = src\[j\];' "$f" || fail "SM_REWRITE writes bytes 1-3 behind a zeroed CIN"
# the card is painted over the shadow UI
grep -q 'drawStepMenuOnTop();' src/shadow/shadow_ui.js || fail "card not drawn over the shadow UI"
echo "PASS: step menu over the shadow UI + hand-off wiring"
