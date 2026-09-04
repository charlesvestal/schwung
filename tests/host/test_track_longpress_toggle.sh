#!/usr/bin/env bash
# Source pin: a Track LONG-PRESS is a toggle between the two worlds.
#
# From Move it opens the shadow UI on that slot. From the shadow UI it hands the
# screen back to Move ON THAT TRACK. One gesture, its own inverse, so you can
# long-press between the two without learning a second key for the way back.
#
# Three things hold it together and each is silent when it breaks:
#
#   1. The fire block must BRANCH on shadow_display_mode. It used to set
#      JUMP_TO_SLOT unconditionally and only open when hidden, so a long-press
#      with the UI already up was indistinguishable from a tap.
#
#   2. The Move-track tap must be injected on BOTH directions. It is the same
#      injection that has always made Move's selected track follow the slot, and
#      it is the entire reason the dismiss lands somewhere useful — dismissing
#      without it drops you back on whatever track Move happened to be on. So it
#      must stay OUTSIDE the branch, and track_swallow_release with it, or the
#      user's real release is handed to Move as a release for a press it never
#      saw open.
#
#   3. The escape hatches must survive. Shift+Track still dismisses (the one
#      gesture that works whatever Keep Schwung says), and a plain TAP still
#      means what it meant — switch slot with Keep Schwung on, dismiss with it
#      off. Both live in the press/release handler, not here.
#
# Overtake needs no guard and must not grow one: the overtake early-out above
# the track block `continue`s before any timer starts, so the fire block cannot
# run at all. Pinned so a future reader does not "fix" that with a second check.
set -u

ROOT="$(dirname "$0")/../.."
SHIM="$ROOT/src/schwung_shim.c"

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

[ -f "$SHIM" ] || { echo "FAIL: cannot find $SHIM" >&2; exit 1; }

# The fire block: from `track_longpress_fired[i] = 1;` to the shadow_log that
# closes it. Extracted so every assertion below is about THAT block and cannot
# be satisfied by a lookalike somewhere else in a 9000-line file.
block=$(awk '
    /track_longpress_fired\[i\] = 1;/ { inb = 1 }
    inb { print }
    inb && /Track long-press: dismissing shadow UI/ { exit }
' "$SHIM")

[ -n "$block" ] || { echo "FAIL: could not locate the track long-press fire block" >&2; exit 1; }

# --- 1. it branches on display mode, both ways ------------------------------
echo "$block" | grep -q 'if (shadow_display_mode) {' ||
    fail "the fire block does not branch on shadow_display_mode — a long-press with the UI
      already up cannot dismiss, which is the whole gesture"

echo "$block" | grep -q 'shadow_control->display_mode = 0;' ||
    fail "the fire block never clears display_mode — nothing hands the screen back to Move"

echo "$block" | grep -q 'shadow_control->display_mode = 1;' ||
    fail "the fire block never sets display_mode — the open direction is gone"

# The open direction is the one that jumps, and it must be the ONLY one: a
# JUMP_TO_SLOT written while dismissing is a flag set on the way out.
jumps=$(echo "$block" | grep -c 'SHADOW_UI_FLAG_JUMP_TO_SLOT')
[ "$jumps" = "1" ] ||
    fail "SHADOW_UI_FLAG_JUMP_TO_SLOT appears $jumps times in the fire block, expected 1 — it
      belongs to the OPEN direction only"

# ...and it must sit on the open side of the branch, i.e. after the `} else {`.
echo "$block" | awk '/} else {/ { seen = 1 } seen && /SHADOW_UI_FLAG_JUMP_TO_SLOT/ { found = 1 }
                     END { exit !found }' ||
    fail "JUMP_TO_SLOT is not in the else (open) arm — it is being set while dismissing"

# --- 2. the Move-track tap is injected on BOTH directions -------------------
# i.e. outside the branch entirely. The injection closes the real hold with a
# synthetic release, taps, and swallows the user's eventual release.
echo "$block" | grep -q 'shadow_midi_inject_push' ||
    fail "the Move-track tap injection is gone from the fire block — a dismiss would drop you
      on whatever track Move happened to be on, and an open would show one module while the
      pads play another"

echo "$block" | grep -q 'track_swallow_release\[i\] = 1;' ||
    fail "the fire block does not swallow the real release — Move is handed a release for a
      press it never saw open"

# The injection must not have been pulled inside either arm. Everything from the
# closing brace of the if/else to the end of the block is the shared tail.
tail_txt=$(echo "$block" | awk '/^ *} else {/ { arm = 1 } arm && /^ *}$/ { tail = 1; next }
                                tail { print }')
echo "$tail_txt" | grep -q 'shadow_midi_inject_push' ||
    fail "the Move-track tap injection moved INSIDE one arm of the branch — it must run on
      both, or one direction of the toggle stops following the track"

# --- 3. overtake is handled by the early-out, not by a second guard ---------
echo "$block" | grep -q 'overtake' &&
    fail "the fire block grew an overtake guard — the early-out above the track block already
      \`continue\`s before any timer starts, so this one is dead code that reads as load-bearing"

# --- 4. the escape hatches still exist --------------------------------------
grep -q 'Shift+Track: dismissing shadow UI' "$SHIM" ||
    fail "Shift+Track no longer dismisses — that is the escape hatch that works whatever
      Keep Schwung says"

grep -q 'Track tap: dismissing shadow UI' "$SHIM" ||
    fail "the plain-tap dismiss (Keep Schwung off) is gone"

grep -q 'STAY_IN_SHADOW() && shadow_display_mode' "$SHIM" ||
    fail "the Keep Schwung tap-switches-slot branch is gone"

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails assertion(s) failed" >&2
    exit 1
fi
echo "PASS: a track long-press toggles between Move and the shadow UI, the Move-track tap runs
      on both directions, and the tap/Shift escape hatches are intact"
