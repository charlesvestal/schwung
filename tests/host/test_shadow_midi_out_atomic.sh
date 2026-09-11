#!/usr/bin/env bash
# A MESSAGE IS WRITTEN WHOLE OR NOT AT ALL.
#
# js_shadow_midi_send() already refused a message larger than the entire
# outbound buffer, with the right words on it -- "refusing rather than
# truncating". That guard only ever covered the WHOLE buffer, and the write
# loop then did the very thing it exists to prevent whenever a message was
# merely larger than the REMAINING ROOM: it wrote packets one at a time until
# the buffer filled and counted the rest as dropped.
#
# For an LED flush that costs a few LEDs. For a SysEx it is fatal and silent: a
# USB-MIDI SysEx is a RUN of packets the receiver assembles into ONE message,
# so a prefix landing and a tail vanishing is a truncated message and the
# device renders whatever the fragment decodes to.
#
# Measured on hardware 2026-09-11: among whole-frame refusals of 394 packets
# sat "dropped 158" and "dropped 3" -- the partial ones -- matching a screen
# that "sometimes garbles and then comes back". It survived every pacing change
# and was WORSE at the slowest pace, because a slower drain leaves less room,
# so more refusals land mid-message instead of at a message boundary.
set -euo pipefail
cd "$(dirname "$0")/../.."

SRC=src/shadow/shadow_ui.c
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# The room check must exist, and must come BEFORE the marshalling loop -- a
# check after it is the bug with a comment on top.
room_line=$(grep -n "ui_midi_out_free(shadow_midi_out)" "$SRC" | head -1 | cut -d: -f1)
loop_line=$(grep -n "ASSEMBLE LOCALLY, THEN PUBLISH ONCE" "$SRC" | head -1 | cut -d: -f1)

[ -n "$room_line" ] || fail "no remaining-room check -- a message larger than the free space is written as a prefix and truncated on the wire"
[ -n "$loop_line" ] || fail "the packet marshalling loop moved; this pin needs rewriting against whatever replaced it"

# The loop must stage into a LOCAL buffer, never into the SHM. Marshalling
# straight into the segment advances write_idx per packet, so the shim can
# snapshot a SysEx run whose tail has not been written yet -- a truncated
# message assembled from two snapshots with other traffic between them.
if grep -nE 'memcpy\(&shadow_midi_out->buffer' "$SRC"; then
    fail "the marshalling loop writes into the SHM buffer directly -- a multi-packet message becomes visible to the shim half-written"
fi

if [ -n "$room_line" ] && [ -n "$loop_line" ] && [ "$room_line" -gt "$loop_line" ]; then
    fail "the room check sits AFTER the write loop (line $room_line > $loop_line) -- the prefix is already written by then"
fi

# And it must REFUSE, not clamp. Returning success on a shortened write is the
# same defect one layer up: the caller caches what it believes it sent.
if [ -n "$room_line" ]; then
    body=$(sed -n "${room_line},$((room_line + 20))p" "$SRC")
    case "$body" in
        *"return JS_FALSE"*) ;;
        *) fail "the room check does not refuse the message -- a caller told 'sent' will never retry it" ;;
    esac
fi

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: an outbound message is written whole or refused whole"
