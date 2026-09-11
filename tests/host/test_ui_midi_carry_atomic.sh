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
set -euo pipefail
cd "$(dirname "$0")/../.."

SRC=src/host/shadow_midi.c
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# The capacity check must exist...
fit_line=$(grep -n "UI_MIDI_CARRY_BYTES - ui_midi_carry.len" "$SRC" | head -1 | cut -d: -f1)
[ -n "$fit_line" ] || fail "no whole-snapshot capacity check -- a snapshot larger than the carry's free space is truncated mid-message"

# ...and it must come BEFORE write_idx is cleared, or the deferral discards the
# very packets it was trying to protect.
reset_line=$(grep -n "midi_out_shm->write_idx = 0;" "$SRC" | head -1 | cut -d: -f1)
[ -n "$reset_line" ] || fail "the write_idx reset moved; this pin needs rewriting against whatever replaced it"

if [ -n "$fit_line" ] && [ -n "$reset_line" ] && [ "$fit_line" -gt "$reset_line" ]; then
    fail "the capacity check (line $fit_line) runs AFTER write_idx is cleared (line $reset_line) -- the deferred snapshot is already gone"
fi

# And it must DEFER, not drop: returning without advancing last_ready is what
# makes the same snapshot arrive whole on a later frame.
if [ -n "$fit_line" ]; then
    body=$(sed -n "${fit_line},$((fit_line + 3))p" "$SRC")
    case "$body" in
        *"return"*) ;;
        *) fail "the capacity check does not defer -- it must return without advancing last_ready" ;;
    esac
fi

ready_line=$(grep -n "last_ready = midi_out_shm->ready;" "$SRC" | head -1 | cut -d: -f1)
if [ -n "$fit_line" ] && [ -n "$ready_line" ] && [ "$fit_line" -gt "$ready_line" ]; then
    fail "the check runs after last_ready is advanced -- the deferred snapshot is never retaken"
fi

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: a snapshot is taken whole or deferred whole"
