#!/usr/bin/env bash
# AN EMPTY SLOT STILL FOLLOWS ITS FADER.
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
echo "PASS: an empty slot follows its fader (volume, mute, solo, pan)"
