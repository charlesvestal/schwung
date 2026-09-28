#!/usr/bin/env bash
# THE OUTBOUND CARRY IS DRAINED EXACTLY ONCE PER FRAME.
#
# Every ui_midi_carry_drain() opens by clearing "last frame's" packets still
# in the mailbox, so nothing is transmitted twice. shadow_inject_ui_midi_out()
# used to drain TWICE -- up front, and again after taking in new packets from
# shadow_ui -- so the second call cleared what the first had placed moments
# earlier, before the transfer. Whole messages vanished: on hardware
# (2026-09-24) E16 rows never reached the wire and showed as dead lines until
# re-sent, counted all along as "would have REPEATED". It also counted our own
# packets as Move's traffic and let one frame exceed the pace cap.
#
# test_ui_midi_out_carry.c proves a second drain wipes the first; this pins
# the caller to one, after the ingest.
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/host/shadow_midi.c
body=$(awk '/^void shadow_inject_ui_midi_out\(void\)/,/^}/' "$SRC")
[ -n "$body" ] || { echo "FAIL: shadow_inject_ui_midi_out not found"; exit 1; }
n=$(printf '%s\n' "$body" | grep -c 'ui_midi_carry_drain(' || true)
[ "$n" -eq 1 ] || { echo "FAIL: shadow_inject_ui_midi_out drains $n times per frame; it must be exactly 1"; exit 1; }
ing=$(printf '%s\n' "$body" | grep -n 'ui_midi_out_ingest(' | head -1 | cut -d: -f1)
dr=$(printf '%s\n' "$body" | grep -n 'ui_midi_carry_drain(' | head -1 | cut -d: -f1)
[ -n "$ing" ] && [ "$ing" -lt "$dr" ] || { echo "FAIL: the drain must come AFTER this frame's packets are taken in"; exit 1; }
# The ingest must not be able to skip the drain by returning out of the caller.
if printf '%s\n' "$body" | sed -n "$((ing)),$((dr))p" | grep -q 'return'; then
  echo "FAIL: a return between the ingest and the drain skips the frame's only drain"; exit 1; fi
echo "PASS: one outbound drain per frame, after the ingest"
