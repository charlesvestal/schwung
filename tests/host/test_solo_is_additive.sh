#!/usr/bin/env bash
# SEVERAL TRACKS CAN BE SOLOED TOGETHER. The slot:soloed parameter (the E16
# Mixer, Slot Settings) used to be exclusive -- soloing one slot un-soloed the
# others -- and un-soloing any slot zeroed the solo count while others stayed
# soloed. It is additive and COUNTED now; Move's Shift+Mute+Track combo stays
# exclusive (it solos Move's own track too) but recounts on the way off.
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/host/shadow_chain_mgmt.c
blk=$(awk '/if \(strcmp\(key, "slot:soloed"\) == 0\) \{/{f=1} f{print} f&&/shadow_solo_count = n;/{exit}' "$SRC")
echo "$blk" | grep -q "shadow_chain_slots\[i\].soloed = 0" \
  && { echo "FAIL: soloing a slot through the parameter still un-solos the others"; exit 1; }
echo "$blk" | grep -q "shadow_solo_count = n;" \
  || { echo "FAIL: the solo count is not counted from the slots"; exit 1; }
tog=$(awk '/^void shadow_toggle_solo/,/^}/' "$SRC")
echo "$tog" | grep -q "if (shadow_chain_slots\[i\].soloed) shadow_solo_count++;" \
  || { echo "FAIL: un-soloing via the Move combo zeroes the count while others are soloed"; exit 1; }
echo "PASS: solo is additive and counted"
