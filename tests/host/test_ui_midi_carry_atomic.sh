#!/usr/bin/env bash
# A SNAPSHOT IS TAKEN WHOLE, OR DEFERRED WHOLE.
#
# ui_midi_carry_push() drops the NEWEST packet when the carry is full, one at a
# time, so a snapshot that overruns leaves a message's head queued and its tail
# discarded -- a truncated SysEx, which the receiver renders as a garbled
# screen. It is the same defect as the partial write in js_shadow_midi_send(),
# one buffer further along, and the carry's own comment warns of it in so many
# words: "Refusing the newest packet truncates one message".
#
# The pre-existing `wants_more` backpressure is necessary and NOT sufficient:
# it only asks whether the carry is below half, while a snapshot can be the
# full buffer, so half-full plus a full snapshot overruns -- mid-message.
#
# Found on hardware 2026-09-11: after the display's latency was cut, the extra
# traffic reached this path and the garbling came back ("MUCH better but
# garbles") with the SHM-level truncation already fixed.
#
# The deferral used to work by not advancing a `last_ready` counter. It now
# works by not committing read_idx (see src/host/ui_midi_out_ring.h) -- the
# same property, one buffer's ownership rules further on, so this pin is
# written against the COMMIT rather than against either spelling.
set -euo pipefail
cd "$(dirname "$0")/../.."

SRC=src/host/shadow_midi.c
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# The capacity check must exist...
fit_line=$(grep -n "UI_MIDI_CARRY_BYTES - ui_midi_carry.len" "$SRC" | head -1 | cut -d: -f1)
[ -n "$fit_line" ] || fail "no whole-snapshot capacity check -- a snapshot larger than the carry's free space is truncated mid-message"

# ...and it must come BEFORE the bytes are released, or the deferral discards
# the very packets it was trying to protect.
commit_line=$(grep -n "ui_midi_out_commit(" "$SRC" | head -1 | cut -d: -f1)
[ -n "$commit_line" ] || fail "the consumer no longer commits read_idx; this pin needs rewriting against whatever replaced it"

if [ -n "$fit_line" ] && [ -n "$commit_line" ] && [ "$fit_line" -gt "$commit_line" ]; then
    fail "the capacity check (line $fit_line) runs AFTER the bytes are released (line $commit_line) -- the deferred snapshot is already gone"
fi

# And it must DEFER, not drop: returning without committing is what makes the
# same snapshot arrive whole on a later frame.
if [ -n "$fit_line" ]; then
    body=$(sed -n "${fit_line},$((fit_line + 3))p" "$SRC")
    case "$body" in
        *"return"*) ;;
        *) fail "the capacity check does not defer -- it must return without committing read_idx" ;;
    esac
fi

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: a snapshot is taken whole or deferred whole"
