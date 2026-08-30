#!/usr/bin/env bash
# Source pin: the cable drain's zero-payload skip must EXEMPT SysEx CINs.
#
# The drain loop lives inside main() and reads mapped_memory directly, so there
# is no seam to unit-test it through. This pin is the repo's usual answer to
# that, and the invariant it protects is one that is SILENT when wrong.
#
# The empty-slot test is the header-byte one (`mapped_memory[i] == 0`). The
# payload test that follows it is belt-and-braces, and for every CIN except
# SysEx it is harmless: no channel-voice or system-common message is three
# zero bytes. SysEx data is arbitrary 7-bit bytes, so `00 00 00` is ordinary
# payload -- a zeroed parameter run, which is most of a patch dump.
#
# Dropping such a packet corrupts the message without any sign: framing still
# holds and the device's own checksum still validates, because the removed
# bytes sum to zero. A receiver gets a short, well-formed, wrong message.
#
# Two things are asserted, and the second is the one that regresses:
#   1. the payload skip still exists (it is correct for non-SysEx);
#   2. it is GUARDED by a SysEx-CIN exemption on the same condition.
set -u

ROOT="$(dirname "$0")/../.."
HOST="$ROOT/src/schwung_host.c"

fails=0
note() { echo "FAIL: $1"; fails=$((fails + 1)); }

[ -f "$HOST" ] || { echo "FAIL: $HOST not found"; exit 1; }

# The condition, with whitespace collapsed, so reflowing the source is allowed.
flat="$(tr '\n' ' ' < "$HOST" | tr -s ' ')"

grep -q "byte\[1\] + byte\[2\] + byte\[3\] == 0" "$HOST" \
  || note "the zero-payload slot skip is gone entirely; it is still correct for non-SysEx CINs"

case "$flat" in
  *"code_index_number >= 0x04 && code_index_number <= 0x07"*") &&"*"byte[1] + byte[2] + byte[3] == 0"*)
    ;;
  *)
    note "the zero-payload skip is not exempted for SysEx CINs 0x04-0x07 -- a SysEx data packet of three zero bytes will be dropped, silently and with a valid checksum"
    ;;
esac

if [ "$fails" -eq 0 ]; then echo "PASS: SysEx data packets are exempt from the zero-payload slot skip"; fi
exit "$fails"
