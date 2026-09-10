#!/usr/bin/env bash
# A framebuffer must FIT the outbound path, and an oversized message must be
# refused rather than retried.
#
# Measured on hardware 2026-09-10. An E16 OLED FRAMEBUFFER is 1180 bytes ->
# 394 USB-MIDI packets -> 1576 bytes on the SHM, against a 1024-byte buffer.
# js_shadow_midi_send wrote the 256 packets that fit, dropped 138, and returned
# false. The caller reads false as "not sent" -- correctly -- so it re-owed the
# repaint and retried every tick, pushing another truncated burst at the device
# forever: six short frames a second and a wedged E16.
#
# Two independent things are pinned, because either alone leaves the failure:
# the buffer being large enough, and the refusal that stops a livelock if some
# future message is larger still.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0
note() { echo "FAIL: $1"; fail=1; }

buf=$(grep -oE '#define SHADOW_MIDI_OUT_BUFFER_SIZE [0-9]+' src/host/shadow_constants.h | grep -oE '[0-9]+$')
carry=$(grep -oE '#define UI_MIDI_CARRY_PACKETS [0-9]+' src/host/ui_midi_out_carry.h | grep -oE '[0-9]+$')

# 1180-byte message -> ceil(1180/3) packets -> x4 bytes on the SHM.
need=$(( ( (1180 + 2) / 3 ) * 4 ))
[ "$buf" -ge "$need" ] || note "outbound buffer $buf < $need bytes needed for one framebuffer"

# The header states these must stay equal: if the SHM side were larger, a flush
# it accepted could not fit the carry and would become a DROP instead of a
# return value -- the exact shape ui_midi_out_carry.h exists to remove.
[ "$(( carry * 4 ))" -eq "$buf" ] || note "carry ${carry}pkt ($((carry*4))B) != buffer ${buf}B"

grep -q "can never be sent" src/shadow/shadow_ui.c \
  || note "no oversize refusal -- an unsendable message would be retried forever"

# The refusal must precede the write loop, or truncated packets go out anyway.
awk '/exceeds the .*-byte/{g=NR} /Process 4 bytes at a time/{p=NR} END{exit !(g && p && g < p)}' \
  src/shadow/shadow_ui.c || note "the oversize refusal runs AFTER the write loop"

[ "$fail" -eq 0 ] && echo "PASS: a framebuffer fits, and an unsendable message is refused not retried"
exit $fail
