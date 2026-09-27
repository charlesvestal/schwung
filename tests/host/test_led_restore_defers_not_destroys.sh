#!/usr/bin/env bash
# A SysEx LED restore must DEFER to other cable-0 SysEx, never destroy it.
#
# Both restore flushes need their 6 packets contiguous and un-interleaved, and
# both used to get that by zeroing every cable-0 SysEx packet already in
# MIDI_OUT. That is not "making room", it is destroying somebody else's message
# — and cable 0 is shared with MOVE'S OWN FIRMWARE: its RGB LED commands, and
# its 37-family XMOS control messages, which are how the USB-C audio-out source
# is set.
#
# A destroyed 37 pair is silent at every layer. Move's Settings screen shows the
# new value, the hardware never changed, and re-selecting can be eaten too —
# reported as "USB-C Main Out stops working until a reboot". Captured on
# hardware 2026-09-12: 9 of 13 of Move's 37-family messages never reached the
# wire (see docs/DIAGNOSTICS.md, "MIDI_OUT loss attribution").
#
# There were TWO copies of the clear. The first carried a comment reasoning
# about RNBO's SysEx; the second carried no comment at all, which is how it
# outlived the reasoning. This test covers both and any third.
set -uo pipefail
cd "$(dirname "$0")/../.."

SRC=src/host/shadow_led_queue.c
fail() { echo "FAIL: $*"; exit 1; }
[ -f "$SRC" ] || fail "missing $SRC"

# Every function that flushes a SysEx LED restore.
mapfile -t FNS < <(grep -n '^int led_queue_flush_[a-z_]*sysex_restore(' "$SRC" | cut -d: -f1)
[ "${#FNS[@]}" -ge 2 ] || fail "expected at least 2 sysex-restore flush functions, found ${#FNS[@]}"

for start in "${FNS[@]}"; do
    name=$(sed -n "${start}p" "$SRC" | sed 's/^int \([a-z_]*\)(.*/\1/')
    end=$(awk -v s="$start" 'NR>s && /^\}/ {print NR; exit}' "$SRC")
    [ -n "$end" ] || fail "$name: could not find the end of the function"

    body=$(sed -n "${start},${end}p" "$SRC")

    # The cable-0 SysEx scan must exist in each of them...
    grep -q 'cin_type >= 0x04 && cin_type <= 0x07' <<<"$body" ||
        fail "$name: no cable-0 SysEx check at all — it can no longer be deferring"

    # ...and must DEFER. The guard's body may not assign to midi_out[...]:
    # that is the destroying form, in any spelling.
    guard=$(awk '
        /cin_type >= 0x04 && cin_type <= 0x07/ { grab = 6 }
        grab { print; grab-- }
    ' <<<"$body")

    if grep -qE 'midi_out\[[^]]*\][[:space:]]*=[[:space:]]*0' <<<"$guard"; then
        echo "$name: the cable-0 SysEx guard writes to midi_out:"
        sed 's/^/    /' <<<"$guard"
        fail "$name: a SysEx LED restore must DEFER (return 0) on foreign cable-0 SysEx, not zero it"
    fi

    grep -qE 'cin_type <= 0x07\)?[[:space:]]*$|cin_type <= 0x07\)' <<<"$guard" || true
    grep -q 'return 0;' <<<"$guard" ||
        fail "$name: the cable-0 SysEx guard does not defer (no 'return 0;' within it)"
done

# memset is the other way to spell it.
for start in "${FNS[@]}"; do
    end=$(awk -v s="$start" 'NR>s && /^\}/ {print NR; exit}' "$SRC")
    name=$(sed -n "${start}p" "$SRC" | sed 's/^int \([a-z_]*\)(.*/\1/')
    if sed -n "${start},${end}p" "$SRC" | grep -qE 'memset\([[:space:]]*midi_out'; then
        fail "$name: memset over midi_out — same destruction, different spelling"
    fi
done

echo "PASS: all ${#FNS[@]} SysEx LED restore flushes defer to foreign cable-0 SysEx instead of zeroing it"
