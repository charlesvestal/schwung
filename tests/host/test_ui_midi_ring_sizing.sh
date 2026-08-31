#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The shadow_ui MIDI ring is sized for SysEx bursts, and is NOT the mailbox.
#
# It used to be MIDI_BUFFER_SIZE, which made it 64 packets because that is how
# many fit in the hardware mailbox -- a number with nothing to do with how much
# MIDI a tool can be sent between two drains. A Yamaha 5F bulk dump message is
# 158 bytes on the wire = 53 packets, so ONE nearly filled the ring and two
# back-to-back could not: shadow_ui_midi_publish() fell off the end of its scan
# and dropped whole packets in silence. Measured on a QY-70 at 6.9% of 3164
# messages, every shortfall a multiple of 3 (#358).
#
# THE TRAP THIS EXISTS FOR. MIDI_BUFFER_SIZE still bounds the hardware mailbox
# copies -- `memcpy(sh_midi, hw_midi, ...)` and `hotkey_prev` -- so "just make
# the buffer bigger" would have read past MIDI_IN into the display status word.
# The two sizes must stay separate names, and this fails if the ring paths go
# back to sharing the mailbox constant.
#
# Producer and consumer must also agree: they are different binaries mapping
# one segment, and a mismatch is a silent partial drain, not an error.

fail() { echo "FAIL: $1" >&2; exit 1; }

H=src/host/shadow_constants.h
SHIM=src/schwung_shim.c
UI=src/shadow/shadow_ui.c

# ---- the two sizes are separate, and the ring is the bigger one ------------
ring=$(sed -n 's/^#define SHADOW_UI_MIDI_BYTES[[:space:]]*\([0-9]*\).*/\1/p' "$H")
mail=$(sed -n 's/^#define MIDI_BUFFER_SIZE[[:space:]]*\([0-9]*\).*/\1/p' "$H")
[ -n "$ring" ] || fail "SHADOW_UI_MIDI_BYTES is gone -- the ring is sharing the mailbox size again"
[ -n "$mail" ] || fail "MIDI_BUFFER_SIZE is gone"

# 53 packets is one dump message; two back-to-back is the case that overflowed.
packets=$((ring / 4))
[ "$packets" -ge 106 ] || fail "the ring holds $packets packets; two 53-packet dump \
messages need 106, which is the burst that was measured dropping"

# ---- the ring paths use the ring constant, not the mailbox one -------------
# Producer: the SHM it creates and the slot scan it fills.
grep -q 'SHM_SHADOW_UI_MIDI,$' "$SHIM" || grep -q 'SHM_SHADOW_UI_MIDI' "$SHIM" \
    || fail "$SHIM no longer creates the UI MIDI segment"
grep -q 'SHADOW_UI_MIDI_BYTES, 1, 1' "$SHIM" \
    || fail "$SHIM does not CREATE the UI ring at SHADOW_UI_MIDI_BYTES"
grep -q 'slot < SHADOW_UI_MIDI_BYTES' "$SHIM" \
    || fail "shadow_ui_midi_publish does not scan the whole ring"

# Consumer: the attach, the wholesale clear, and the drain bound.
grep -q 'SHM_SHADOW_UI_MIDI, SHADOW_UI_MIDI_BYTES' "$UI" \
    || fail "$UI does not ATTACH the UI ring at SHADOW_UI_MIDI_BYTES -- producer and \
consumer disagreeing is a silent partial drain"
grep -q 'i < SHADOW_UI_MIDI_BYTES' "$UI" \
    || fail "process_shadow_midi does not drain the whole ring"
grep -q 'memset(shadow_ui_midi_shm, 0, SHADOW_UI_MIDI_BYTES)' "$UI" \
    || fail "$UI clears only part of the ring"

# Nothing in the consumer should still reach for the mailbox size.
if grep -q 'MIDI_BUFFER_SIZE' "$UI"; then
    fail "$UI still uses MIDI_BUFFER_SIZE -- the UI ring and the hardware mailbox \
are different buffers and must not share a size"
fi

# ---- the mailbox copies still use the MAILBOX size -------------------------
# This is the half that would corrupt the display region if it drifted.
grep -q 'memcpy(sh_midi, hw_midi, MIDI_BUFFER_SIZE)' "$SHIM" \
    || fail "the hardware MIDI_IN copy no longer uses MIDI_BUFFER_SIZE -- if it \
were widened to the ring size it would read past MIDI_IN into the display status"
grep -q 'hotkey_prev\[MIDI_BUFFER_SIZE\]' "$SHIM" \
    || fail "hotkey_prev no longer uses MIDI_BUFFER_SIZE"

# ---- an overflow is counted, not silent ------------------------------------
grep -q 'shim_ui_midi_drops++' "$SHIM" \
    || fail "a full ring still drops in silence -- nothing counts it"
grep -q 'ui_midi_drop_tick' src/host/shim_worker.c \
    || fail "nothing reports the drop count; the counter alone is invisible"

# ---- a short attach must be refused, not mapped ----------------------------
# Enlarging a segment that outlives the process means a new consumer can meet a
# stale short one, and mapping past its end is SIGBUS at an arbitrary later
# moment in whichever process attached.
grep -q 'fstat' src/host/shadow_shm_util.c \
    || fail "shadow_shm_map does not check the segment length on attach -- a stale \
short segment would SIGBUS instead of failing cleanly"

echo "PASS: the UI MIDI ring is sized for bursts, separate from the mailbox, and counts drops"
