#!/usr/bin/env bash
# The three MIDI_OUT log views must stay where they are, or they measure nothing.
#
# xmos_log_slots() is called at three points that BRACKET every writer of
# MIDI_OUT:
#
#   PRE     early in shim_pre_transfer
#   PREEND  its LAST statement, immediately before the library's shadow->hw copy
#   POSThw  the hardware mailbox, after the ioctl
#
# The whole diagnostic value is in the boundaries. A MIDI_OUT writer added after
# the PREEND call is invisible to it — worse, it is EXONERATED by it, because a
# message it overwrites still appears intact at PREEND and the log then blames
# the ioctl. That is the opposite of what this instrument is for.
#
# Why it exists at all: captured on hardware 2026-09-12, 9 of 13 of Move's
# 37-family XMOS control messages were replaced in the mailbox by an RGB LED
# SysEx before the transfer, so the USB-C audio-out setting silently did
# nothing. PRE vs POSThw could see the loss but not attribute it.
set -uo pipefail
cd "$(dirname "$0")/../.."

SHIM=src/schwung_shim.c
fail() { echo "FAIL: $*"; exit 1; }
[ -f "$SHIM" ] || fail "missing $SHIM"

# --- all three views are present -------------------------------------------
for tag in PRE PREEND POSThw; do
    grep -q "xmos_log_slots(\"$tag\"" "$SHIM" || fail "no xmos_log_slots(\"$tag\", ...) call"
done

# --- exactly one call each, so "the last one" is unambiguous ----------------
for tag in PRE PREEND POSThw; do
    n=$(grep -c "xmos_log_slots(\"$tag\"" "$SHIM")
    [ "$n" -eq 1 ] || fail "expected 1 xmos_log_slots(\"$tag\") call, found $n"
done

# --- PREEND is the LAST STATEMENT of shim_pre_transfer ----------------------
# Find the function, find its closing brace at column 0, and require the last
# non-blank, non-comment line before it to be the PREEND call.
start=$(grep -n '^static void shim_pre_transfer(' "$SHIM" | cut -d: -f1)
[ -n "$start" ] || fail "could not find shim_pre_transfer definition"

end=$(awk -v s="$start" 'NR>s && /^\}/ {print NR; exit}' "$SHIM")
[ -n "$end" ] || fail "could not find the end of shim_pre_transfer"

last=$(awk -v s="$start" -v e="$end" '
    NR>s && NR<e {
        line=$0
        sub(/^[ \t]+/, "", line)
        if (line == "") next
        if (line ~ /^\/\*/ || line ~ /^\*/ || line ~ /^\*\//) next
        if (line ~ /^\/\//) next
        keep=line
    }
    END { print keep }
' "$SHIM")

case "$last" in
    'xmos_log_slots("PREEND"'*)
        ;;
    *)
        echo "shim_pre_transfer's last statement is:"
        echo "    $last"
        fail "PREEND is not the last statement of shim_pre_transfer — a MIDI_OUT writer after it is silently exonerated"
        ;;
esac

# --- PRE comes before PREEND, which comes before POSThw --------------------
pre=$(grep -n 'xmos_log_slots("PRE"' "$SHIM" | cut -d: -f1)
preend=$(grep -n 'xmos_log_slots("PREEND"' "$SHIM" | cut -d: -f1)
post=$(grep -n 'xmos_log_slots("POSThw"' "$SHIM" | cut -d: -f1)
[ "$pre" -lt "$preend" ] || fail "PRE (line $pre) must precede PREEND (line $preend)"
[ "$preend" -lt "$post" ] || fail "PREEND (line $preend) must precede POSThw (line $post)"

# --- PREEND must read the SHADOW buffer, POSThw the HARDWARE one -----------
# Reading the same buffer at both ends compares a value with itself and always
# reports "nothing changed during the ioctl".
grep -q 'xmos_log_slots("PREEND", shadow + MIDI_OUT_OFFSET' "$SHIM" ||
    fail "PREEND must read shadow + MIDI_OUT_OFFSET"
grep -q 'xmos_log_slots("POSThw", hw + MIDI_OUT_OFFSET' "$SHIM" ||
    fail "POSThw must read hw + MIDI_OUT_OFFSET (the hardware mailbox), not shadow"

echo "PASS: PRE / PREEND / POSThw bracket MIDI_OUT in order, PREEND is last in shim_pre_transfer, and the two ends read different buffers"
