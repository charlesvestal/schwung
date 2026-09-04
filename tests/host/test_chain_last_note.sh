#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# synth:last_note -- the fallback input for "which voice is focused".
#
# Source-invariant pins, because the surrounding code is the SPI callback and
# building a chain instance in a host test would mean stubbing the whole plugin
# ABI. What is asserted is the shape that makes it correct and RT-safe:
#
#   - it records at the SYNTH's on_midi call site, so it sees the note after
#     MIDI FX, not the raw input a chord FX may have replaced
#   - note-offs do not clear it (a released pad is still the pad you edit)
#   - the record is an assignment and nothing else -- no malloc, no log, no
#     file I/O on the callback

MIDI=src/modules/chain/dsp/chain_midi.c
HOST=src/modules/chain/dsp/chain_host.c
HDR=src/modules/chain/dsp/chain_internal.h

fail() { echo "FAIL: $1"; exit 1; }

grep -q "int synth_last_note;" "$HDR" \
  || fail "chain_internal.h declares no synth_last_note"

grep -q 'strcmp(subkey, "last_note")' "$HOST" \
  || fail "chain_host.c does not serve synth:last_note"

# The record must be velocity-gated: 0x90 with velocity 0 is a note-OFF, and
# recording it would blank the focus every time a pad was released.
grep -q 'out_msgs\[i\]\[0\] & 0xF0) == 0x90 && out_msgs\[i\]\[2\] > 0' "$MIDI" \
  || fail "the last_note record is not gated on a real note-on (velocity > 0)"

# ...and it must sit in the loop that feeds the SYNTH, so it sees post-MIDI-FX
# notes. Assert on the code with comments stripped: an assertion that trips on
# its own documentation proves nothing.
STRIPPED=$(sed 's://.*::' "$MIDI" | sed 's:/\*.*\*/::')
echo "$STRIPPED" | grep -q "synth_last_note = out_msgs" \
  || fail "last_note is not recorded from the synth-bound message"

# Nothing unsafe on the callback.
LINE=$(grep -n "synth_last_note = out_msgs" "$MIDI" | cut -d: -f1)
CONTEXT=$(sed -n "$((LINE-3)),$((LINE+3))p" "$MIDI")
echo "$CONTEXT" | grep -Eq "malloc|calloc|free\(|fopen|fprintf|unified_log|LOG_" \
  && fail "unsafe call beside the last_note record -- this is the SPI callback"

echo "PASS: last_note"
