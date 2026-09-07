#!/usr/bin/env bash
# Source pin: the pixel volume-bar scanner reads MOVE'S MASTER VOLUME OVERLAY
# and nothing else.
#
# The scanner reconstructs Move's OLED and converts a bar position into
# shadow_master_volume — mailbox gain. Move draws a bar in those same rows for
# gestures that are not the master volume at all:
#
#   track + volume   track level
#   pad   + volume   pad gain
#   step  + volume   that step's VELOCITY
#
# Read as a volume bar, each drags the mailbox gain along with it. The first
# two were guarded when they bit; the third shipped unguarded and showed up as
# "editing step velocity in the Schwung view also turns the volume down".
#
# So the capture gate must carry all three held-state terms, and the step term
# must be a MASK keyed by note number rather than a counter: midi_monitor()
# only processes a MIDI_IN slot whose first four bytes changed, and events
# shift between slots, so the same note-on can be seen twice and a note-off can
# arrive for a slot already counted. A drifted counter latches the scanner off
# for the rest of the session.
set -u

SHIM="$(dirname "$0")/../../src/schwung_shim.c"
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

[ -f "$SHIM" ] || { echo "FAIL: cannot find $SHIM" >&2; exit 1; }

# --- 1. the capture gate carries track, pad AND step ----------------------
gate_line=$(grep -n 'if (shadow_volume_knob_touched && shadow_held_track < 0' "$SHIM" | head -1 | cut -d: -f1)
[ -n "$gate_line" ] || fail "cannot find the volume-capture gate — has it moved?"
if [ -n "${gate_line:-}" ]; then
    gate=$(sed -n "${gate_line},$((gate_line + 2))p" "$SHIM")
    case "$gate" in
        *shadow_pads_held\ ==\ 0*) ;;
        *) fail "volume-capture gate does not exclude a held pad" ;;
    esac
    case "$gate" in
        *shadow_steps_held_mask\ ==\ 0*) ;;
        *) fail "volume-capture gate does not exclude a held STEP — step+volume is Move's velocity edit, and its overlay reads as a low master volume" ;;
    esac
fi

# --- 2. the step hold is tracked, as a mask, in midi_monitor -------------
grep -q 'static volatile uint32_t shadow_steps_held_mask' "$SHIM" \
    || fail "shadow_steps_held_mask is not a uint32_t mask — a counter drifts when a slot is re-seen"

mon_line=$(grep -n '^void midi_monitor' "$SHIM" | head -1 | cut -d: -f1)
[ -n "$mon_line" ] || fail "cannot find midi_monitor()"
if [ -n "${mon_line:-}" ]; then
    body=$(sed -n "${mon_line},$((mon_line + 200))p" "$SHIM")
    case "$body" in
        *shadow_steps_held_mask\ \|=*) ;;
        *) fail "midi_monitor() never sets a step bit — the gate above can never fire" ;;
    esac
    case "$body" in
        *shadow_steps_held_mask\ \&=\ ~*) ;;
        *) fail "midi_monitor() never clears a step bit — the scanner would stay off after one step press" ;;
    esac
fi

# The mask is tracked from the hardware mailbox, like the pad counter and the
# volume touch: a shadow-buffer read would go blind exactly when the shadow UI
# filters the event, which is the state the bug was reported in.
case "$(sed -n "${mon_line:-1},$((${mon_line:-1} + 20))p" "$SHIM")" in
    *hardware_mmap_addr*) ;;
    *) fail "midi_monitor() no longer reads the hardware mailbox — the held-state tracking would go blind under the shadow UI's own MIDI_IN filtering" ;;
esac

if [ "$fails" -eq 0 ]; then
    echo "PASS: volume-bar scanner is gated on track, pad and step holds"
    exit 0
fi
echo "$fails check(s) failed" >&2
exit 1
