#!/usr/bin/env bash
# THE CONSUMER OWNS read_idx AND NOTHING ELSE IN /schwung-midi-out.
#
# shadow_inject_ui_midi_out() runs on the SPI callback; js_shadow_midi_send()
# runs in shadow_ui, a SEPARATE PROCESS. The consumer used to finish a drain
# with `write_idx = 0` and a memset of the whole buffer, so a packet the
# producer appended in that window was erased and its index discarded -- after
# js_shadow_midi_send() had already returned true.
#
# Nothing could see it. JS counted the packet as sent; the shim never received
# it. One lost packet inside a SysEx run corrupts the message, which the E16
# draws as a garbled screen.
#
# tests/host/test_ui_midi_out_ring.c pins the PROPERTY against the header's
# helpers. This pin covers the other way back in: a hand-rolled write to the
# producer's region at the call site, bypassing the helpers entirely.
set -euo pipefail
cd "$(dirname "$0")/../.."

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

CONSUMER=src/host/shadow_midi.c
PRODUCER=src/shadow/shadow_ui.c
RING=src/host/ui_midi_out_ring.h

# --- the consumer must not write write_idx or the buffer ---------------------
if grep -nE '(midi_out_shm|shadow_midi_out)[^;]*->write_idx[[:space:]]*=' "$CONSUMER"; then
    fail "the consumer ASSIGNS write_idx -- that index belongs to shadow_ui, and clearing it discards a concurrent append"
fi
if grep -nE 'memset\([^)]*(midi_out_shm|shadow_midi_out)' "$CONSUMER"; then
    fail "the consumer memsets the SHM -- it erases packets the producer has already been told were sent"
fi

# --- the producer must not write read_idx ------------------------------------
if grep -nE 'shadow_midi_out->read_idx[[:space:]]*=' "$PRODUCER"; then
    fail "the producer ASSIGNS read_idx -- that index belongs to the shim"
fi

# --- both sides must go through the shared helpers ---------------------------
grep -q 'ui_midi_out_copy(' "$CONSUMER" ||
    fail "the consumer does not use ui_midi_out_copy -- a hand-rolled memcpy will not handle the wrap"
grep -q 'ui_midi_out_commit(' "$CONSUMER" ||
    fail "the consumer does not use ui_midi_out_commit"
grep -q 'ui_midi_out_push(' "$PRODUCER" ||
    fail "the producer does not use ui_midi_out_push -- an in-place write loop publishes a half-written SysEx"

# --- the helpers are the only place the rule is written down -----------------
grep -q 'm->read_idx' "$RING" ||
    fail "$RING no longer touches read_idx; this pin needs rewriting"

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: the consumer writes only read_idx, the producer only write_idx"
