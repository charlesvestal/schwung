#!/usr/bin/env bash
# THE OUTBOUND PACE MAY NOT CLAIM THE WHOLE MAILBOX.
#
# MIDI_OUT is 20 slots per SPI frame and it is SHARED: Move's own output and
# the LED flush write into it too, which is why the drain's idea of a free slot
# is "all four bytes zero" rather than an index of its own. A pace of 20 fills
# every slot on every frame we have something to send.
#
# Measured on hardware 2026-09-11: pace 20 wedged the E16 and it had to be
# replugged. The cap was 64 then -- not a considered ceiling, just a number
# larger than anyone expected to type, written before anybody asked what a
# shared region can be asked to give away.
set -euo pipefail
cd "$(dirname "$0")/../.."

H=src/host/ui_midi_out_carry.h
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

max=$(grep -oE '#define UI_MIDI_CARRY_PACE_MAX +[0-9]+' "$H" | grep -oE '[0-9]+$')
res=$(grep -oE '#define UI_MIDI_CARRY_PACE_RESERVE +[0-9]+' "$H" | grep -oE '[0-9]+$')

[ -n "$max" ] || fail "no pace ceiling at all -- a typo in the pace file can claim the whole mailbox"
[ -n "$res" ] || fail "no declared reserve -- the ceiling would be a bare number with nothing saying what it protects"

# 20 slots, stated here rather than borrowed from the header, so a resize that
# forgets the cap fails here instead of on a device.
slots=20
if [ -n "$max" ] && [ "$max" -ge "$slots" ]; then
    fail "pace ceiling $max can fill all $slots mailbox slots -- nothing is left for Move (this wedged a device)"
fi
if [ -n "$max" ] && [ -n "$res" ] && [ $((max + res)) -gt "$slots" ]; then
    fail "ceiling $max plus reserve $res exceeds the $slots-slot mailbox -- the reserve is not reserved"
fi

grep -q "pace > UI_MIDI_CARRY_PACE_MAX" "$H" \
  || fail "the ceiling is declared but never enforced in ui_midi_carry_set_pace"

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: the pace leaves Move room in the shared mailbox (max $max, reserve $res of 20)"
