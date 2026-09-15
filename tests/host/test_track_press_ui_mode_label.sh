#!/usr/bin/env bash
# A Track press that OPENS the shadow UI must not relabel Move's view as Note.
#
# Found on hardware: the user was in Session view, opened the shadow UI with
# Shift+Vol+Track, then switched clips — and Schwung kept playing the old ones.
# The Shift+Vol+Track CC is swallowed, so Move never saw it and never left
# Session, but schwung_shim.c relabelled move_ui_mode = 2 (NOTE) on ANY track
# press. Only the exact "Session Mode" announcement clears that, and it never
# arrives when you were already in Session. clip_state_on_led's Session-only
# gate then rejected every pad event, so clip identity froze on the clip
# witnessed before the UI opened — a STALE answer, which the clip-awareness
# design says must never happen (it must surface as identity_valid = 0).
#
# The rule lives in src/host/move_ui_mode_label.h so it can be RUN, not just
# grepped; this pins both the rule and the shim's use of it. Widening
# clip_state_on_led's gate instead is not the fix: in Note view Move's pads are
# a keyboard, and decoding them as clip state invents clip launches.
set -euo pipefail
cd "$(dirname "$0")/../.."

SHIM=src/schwung_shim.c
HDR=src/host/move_ui_mode_label.h
fails=0
note() { echo "FAIL: $1"; fails=$((fails+1)); }

[ -f "$HDR" ] || note "$HDR is gone — the rule has no single home again"

# --- the rule, actually executed ------------------------------------------
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'C'
#include <stdio.h>
#include "move_ui_mode_label.h"
static int bad;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); bad = 1; } } while (0)
int main(void) {
    /* Shift+Vol+Track: swallowed, so Move's view cannot have changed. */
    CHECK(!move_ui_mode_track_press_relabels(1, 1),
          "a press withheld from Move must not relabel");
    /* A delivered press selects a track and puts its instrument on the pads. */
    CHECK(move_ui_mode_track_press_relabels(1, 0),
          "a press delivered to Move must relabel");
    /* A release changes nothing; the selection rode on the press. */
    CHECK(!move_ui_mode_track_press_relabels(0, 0),
          "a release must not relabel");
    CHECK(!move_ui_mode_track_press_relabels(0, 1),
          "a withheld release must not relabel");
    CHECK(MOVE_UI_MODE_NOTE == 2 && MOVE_UI_MODE_SESSION == 1,
          "the numbering must match shadow_control_t.move_ui_mode");
    if (bad) return 1;
    printf("  move_ui_mode_track_press_relabels: ok\n");
    return 0;
}
C
if cc -std=c11 -Wall -Werror -Isrc/host -o "$tmp/t" "$tmp/t.c" 2>"$tmp/cc.log"; then
    "$tmp/t" || note "the relabel predicate is wrong"
else
    sed -n '1,20p' "$tmp/cc.log"
    note "move_ui_mode_label.h does not compile"
fi

# --- the shim consults it -------------------------------------------------
# The whole Track-button branch: CC 40-43 through to the Mute handler.
branch=$(awk '/if \(d1 >= 40 && d1 <= 43\) \{/,/Mute button \(CC 88\)/' "$SHIM")
[ -n "$branch" ] || note "cannot find the CC 40-43 branch in $SHIM"

echo "$branch" | grep -q "move_ui_mode_track_press_relabels(" \
  || note "the Track branch does not consult move_ui_mode_track_press_relabels() — an unconditional relabel is the hardware bug"

# An unconditional relabel is the bug in its original form. Every assignment of
# move_ui_mode in this branch must sit under the predicate — checked against the
# lines just above it, because the guard does not have to fit on one line.
bad=$(echo "$branch" | python3 -c '
import sys
lines = sys.stdin.read().split("\n")
for i, l in enumerate(lines):
    if "move_ui_mode" in l and "=" in l.split("move_ui_mode", 1)[1][:3]:
        window = "".join(lines[max(0, i - 4):i + 1])
        if "move_ui_mode_track_press_relabels(" not in window:
            print("%d: %s" % (i + 1, l.strip()))
')
[ -z "$bad" ] || { echo "$bad"; note "move_ui_mode assigned in the Track branch without the predicate"; }

# The predicate's second argument must come from the swallow, not from a
# restated combo condition — a combo list rots the next time a shortcut lands.
echo "$branch" | grep -qE "withheld_from_move *= *1;" \
  || note "nothing in the Track branch marks a press as withheld from Move"
swallows=$(echo "$branch" | grep -c "midi_in_swallow(shadow + MIDI_IN_OFFSET, src, j);" || true)
[ "$swallows" -ge 1 ] || note "the Track branch no longer swallows anything — re-check the withheld flag"

if [ "$fails" -ne 0 ]; then
    echo "test_track_press_ui_mode_label: $fails failure(s)"
    exit 1
fi
echo "test_track_press_ui_mode_label: PASS"
