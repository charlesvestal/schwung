#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# synth:last_note -- the fallback input for "which voice is focused".
#
# Source-invariant pins, because the surrounding code is the SPI callback and
# building a chain instance in a host test would mean stubbing the whole plugin
# ABI. What is asserted is the shape that makes it correct and RT-safe:
#
#   - it records at the SYNTH's on_midi call sites -- BOTH of them -- so it
#     sees the note after MIDI FX, not the raw input a chord FX may have
#     replaced, and so an arp emitting from tick() is not invisible
#   - note-offs do not clear it (a released pad is still the pad you edit)
#   - the record is an assignment and nothing else -- no malloc, no log, no
#     file I/O on the callback
#
# The record lives in ONE named helper, chain_record_synth_note, precisely so
# these assertions can name it. v2_on_midi and v2_tick_midi_fx declare buffers
# with IDENTICAL names (out_msgs / out_lens), so an assertion phrased in terms
# of those matches at either site and pins neither: the record was once moved
# verbatim from one function to the other and this test still printed PASS.

MIDI=src/modules/chain/dsp/chain_midi.c
HOST=src/modules/chain/dsp/chain_host.c
HDR=src/modules/chain/dsp/chain_internal.h

fail() { echo "FAIL: $1"; exit 1; }

grep -q "int synth_last_note;" "$HDR" \
  || fail "chain_internal.h declares no synth_last_note"

grep -q 'strcmp(subkey, "last_note")' "$HOST" \
  || fail "chain_host.c does not serve synth:last_note"

# Assert on the code with comments stripped: an assertion that trips on its own
# documentation proves nothing. Block comments here are MULTI-LINE (the helper
# carries a nine-line one), so a line-wise `s:/\*.*\*/::` would leave the body
# of every one of them in the text being searched.
STRIPPED=$(perl -0pe 's{/\*.*?\*/}{}gs' "$MIDI" | sed 's://.*::')

# --- the helper exists, and is still the velocity-gated int store ------------
#
# 0x90 with velocity 0 is a note-OFF; recording it would blank the focus every
# time a pad was released.
BODY=$(printf '%s\n' "$STRIPPED" \
  | awk '/^static inline void chain_record_synth_note\(/ {on=1} on {print} on && /^}/ {exit}')

[ -n "$BODY" ] || fail "no chain_record_synth_note helper in chain_midi.c"

printf '%s\n' "$BODY" | grep -q 'msg\[0\] & 0xF0) == 0x90' \
  || fail "chain_record_synth_note does not gate on a note-on status"
printf '%s\n' "$BODY" | grep -q 'msg\[2\] > 0' \
  || fail "the last_note record is not gated on a real note-on (velocity > 0)"
printf '%s\n' "$BODY" | grep -q 'synth_last_note = msg\[1\]' \
  || fail "chain_record_synth_note does not store the note number"

# Nothing unsafe on the callback: the helper's whole body, not a line window.
printf '%s\n' "$BODY" | grep -Eq "malloc|calloc|free\(|fopen|fprintf|unified_log|LOG_|pthread_|access\(" \
  && fail "unsafe call inside chain_record_synth_note -- this is the SPI callback"

# --- and it is called at BOTH synth-feed sites ------------------------------
#
# v2_on_midi carries what a MIDI FX returned from process_midi; v2_tick_midi_fx
# carries what it emitted from tick() instead, which is where an arpeggiator's
# whole pattern comes from. Either site alone leaves a real slot configuration
# reporting no note at all.
called_in() {
  printf '%s\n' "$STRIPPED" \
    | awk -v fn="$1" '
        $0 ~ ("^(static )?void " fn "\\(") {on=1}
        on && /chain_record_synth_note\(inst,/ {found=1}
        on && /^}/ {exit}
        END {exit found ? 0 : 1}
      '
}

called_in v2_on_midi \
  || fail "v2_on_midi does not call chain_record_synth_note (post-MIDI-FX notes unrecorded)"
called_in v2_tick_midi_fx \
  || fail "v2_tick_midi_fx does not call chain_record_synth_note -- an arp emits its pattern from tick(), so last_note would never update"

# Each call must sit immediately before the on_midi that feeds the synth, so it
# records what the synth is actually given rather than something later dropped.
PRECEDES=$(printf '%s\n' "$STRIPPED" \
  | grep -A1 'chain_record_synth_note(inst,' \
  | grep -c 'synth_plugin_v2->on_midi(' || true)
[ "$PRECEDES" = "2" ] \
  || fail "expected 2 chain_record_synth_note calls each immediately before a synth on_midi, found $PRECEDES"

echo "PASS: last_note"
