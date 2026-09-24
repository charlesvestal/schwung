#!/usr/bin/env bash
# AN EMPTY SLOT STILL FOLLOWS ITS FADER, AND STILL SENDS.
#
# Under Move->Schwung a slot with no module passes Move's track through. It
# did so at unity, so the slot's volume, mute, solo and pan did nothing for it:
# muting track 3 on the E16 Mixer while slot 3 was empty changed nothing, and
# a solo elsewhere left it playing (hardware, 2026-09-24). The passthrough
# must apply the same gain an occupied slot gets -- effective volume (which is
# where mute and solo live) and pan.
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/schwung_shim.c
block=$(awk '/\} else if \(have_move_track\) \{/,/skip_la_rebuild:/' "$SRC")
[ -n "$block" ] || { echo "FAIL: the empty-slot passthrough branch is gone; re-pin"; exit 1; }
echo "$block" | grep -q "shadow_effective_volume(s)" \
  || { echo "FAIL: the empty-slot passthrough ignores the slot volume / mute / solo"; exit 1; }
echo "$block" | grep -q "shadow_pan_gains(s" \
  || { echo "FAIL: the empty-slot passthrough ignores the slot pan"; exit 1; }
if echo "$block" | grep -q "shadow_stem_store(s, move_track, 1.0f)"; then
  echo "FAIL: the empty-slot stem is still taken at unity"; exit 1; fi
# ...and FEEDS ITS SENDS: an empty slot has no chain to hold send levels, so
# the shim keeps them (empty_send) and the passthrough must use them.
echo "$block" | grep -q "bus_mix_send(send_accum\[sb\], move_track" \
  || { echo "FAIL: the empty-slot passthrough feeds no send bus"; exit 1; }
# A slot with no MODULE can still have a chain INSTANCE, which holds the send
# levels the UI writes: it must be drained like an active slot, or sends need
# a module (hardware, 2026-09-24).
echo "$block" | grep -q "shadow_chain_drain_main_send(shadow_chain_slots\[s\].instance" \
  || { echo "FAIL: an inactive slot with a chain instance does not drain its own send levels"; exit 1; }
echo "$block" | grep -q "empty_send\[sb\]" \
  || { echo "FAIL: the empty-slot sends do not use the levels the shim keeps"; exit 1; }
grep -q 'strcmp(key, "buses:main_send1") == 0) return 0;' src/host/shadow_chain_mgmt.c \
  || { echo "FAIL: buses:main_send does not reach the empty-slot levels"; exit 1; }
echo "PASS: an empty slot follows its fader (volume, mute, solo, pan) and feeds its sends"
