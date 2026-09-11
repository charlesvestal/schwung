#!/usr/bin/env bash
# The pacing knob, end to end.
#
# 3 packets per SPI frame was never measured: it was the first value that
# stopped the garbling after sending a framebuffer all at once failed, and
# nobody searched upward from it. It decides the only latency the user feels --
# 394 packets at 3/frame is 383 ms, at 12 it would be 96 -- and trying a value
# used to cost a cross-compile and a device restart, which is exactly why the
# search never happened.
#
# Four links, and a break in any one is silent: the file is read, the value
# reaches the SHM, the drain consults it, and the clamp holds. A test that
# checked only the constant would pass with the wire cut.
set -u
cd "$(dirname "$0")/../.."
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# 1. JS reads the file and pushes it through the binding.
grep -q "e16_pace" src/shadow/shadow_ui.js \
  || fail "shadow_ui.js does not read the pace file"
grep -q "host_ui_midi_pace" src/shadow/shadow_ui.js \
  || fail "shadow_ui.js never calls the pace binding"
grep -q "e16ReconcilePace()" src/shadow/shadow_ui.js \
  || fail "the pace probe is never ticked -- the file is read by nobody"

# 2. The binding exists and is registered.
grep -q "js_host_ui_midi_pace" src/shadow/shadow_ui.c \
  || fail "no pace binding in shadow_ui.c"
grep -q '"host_ui_midi_pace"' src/shadow/shadow_ui.c \
  || fail "the pace binding is defined but never registered -- JS calls a name that is not there"

# 3. It crosses on the control block, and the DRAIN consults it. The drain runs
#    on the SPI callback, so the file read can never move here.
grep -q "ui_midi_pace" src/host/shadow_constants.h \
  || fail "shadow_control_t carries no pace field"
grep -q "ui_midi_carry_set_pace" src/host/shadow_midi.c \
  || fail "the drain does not consult the pace -- the knob turns nothing"

# 4. The drain paces by the VARIABLE, not the constant. Missing this is the
#    quietest break of the four: everything is wired, the log line prints the
#    new value, and the wire keeps running at 3.
grep -q "placed >= ui_midi_carry_pace" src/host/ui_midi_out_carry.h \
  || fail "the drain still paces by the compile-time constant"

# 5. Clamped, so a typo cannot stall the wire or restore the unpaced send that
#    lost packets in the first place.
grep -q "UI_MIDI_CARRY_PACE_MIN" src/host/ui_midi_out_carry.h \
  || fail "the pace is not clamped"

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: the pace is settable from a file and reaches the drain"
