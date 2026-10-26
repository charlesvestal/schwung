#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# midi_monitor() TRACKS MOVE'S OWN CONTROLS, so it must read cable 0 only.
#
# It keeps Shift, Delete, the volume touch, the held-pad count and the
# held-STEP mask -- every one a physical control, every one on cable 0. It had
# no cable test, so a sequencer on USB-A (cable 2) playing notes 16-31 set
# step bits: one sounding note was "exactly one step held", and a knob turned
# under the shadow UI became a p-lock, with Record off (field report
# 2026-10-24, an OXI One sequencing all four tracks).
#
# The gate must come BEFORE the first state write in the loop body. A gate
# placed after any of them leaves that one leaking, so each is checked by
# line order rather than by the gate merely existing.

fail() { echo "FAIL: $1"; exit 1; }

SHIM=src/schwung_shim.c
body=$(awk '/^void midi_monitor\(\)/,/^\}/' "$SHIM")
[ -n "$body" ] || fail "cannot find midi_monitor()"

gate=$(grep -n 'if (cable != 0x00) continue;' <<<"$body" | head -1 | cut -d: -f1 || true)
[ -n "$gate" ] || fail "midi_monitor() has no cable-0 gate -- external notes 16-31 read as held steps"

for pat in 'shadow_steps_held_mask |=' 'shadow_pads_held++' 'shadow_shift_held = 1' \
           'shadow_volume_knob_touched = 1' 'delete_held ='; do
  at=$(grep -nF "$pat" <<<"$body" | head -1 | cut -d: -f1 || true)
  [ -n "$at" ] || fail "midi_monitor() no longer contains '$pat' -- update this pin"
  [ "$gate" -lt "$at" ] \
    || fail "'$pat' (line $at of midi_monitor) runs before the cable-0 gate (line $gate)"
done

echo "PASS: midi_monitor() tracks cable 0 only, ahead of every held-state write"
