#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# External (cable-2) MIDI must reach a slot on the slot's FORWARD channel, the
# same as Move's MIDI_OUT echo of it does.
#
# Both paths deliver a played note to the slot, and the echo copy is DROPPED as
# a duplicate of the cable-2 one. So the cable-2 copy is the only one that
# plays, and when it kept the keyboard's own channel, a module that forces a
# channel (JV-880: default_forward_channel 1, Patch mode answers one channel)
# was silent for an external keyboard on any other channel while the pads
# played. Move's "Full velocity" setting made it work by accident: the echo
# then differs from the original in d2, escapes the dedup, and is remapped.
# schwung-jv880#4.

fail() { echo "FAIL: $1" >&2; exit 1; }

host="src/host/shadow_midi.c"
blk=$(awk '/^void shadow_dispatch_cable2_channeled_slots/,/^}/' "$host")
[ -n "$blk" ] || fail "shadow_dispatch_cable2_channeled_slots is gone from $host"

grep -q 'on_midi(host_chain_slots\[s\]' <<<"$blk" || \
  fail "the cable-2 dispatcher no longer delivers to slots; update this test"

# Every message built for on_midi in the dispatcher is remapped first.
bad=$(grep -nE 'uint8_t msg\[3\] = \{ *status' <<<"$blk" || true)
[ -z "$bad" ] || fail "cable-2 dispatch builds a slot message without shadow_chain_remap_channel:
$bad"
grep -q 'shadow_chain_remap_channel(s, status)' <<<"$blk" || \
  fail "cable-2 dispatch does not remap to the slot's forward channel"

echo "PASS: cable-2 MIDI is remapped to each slot's forward channel"
