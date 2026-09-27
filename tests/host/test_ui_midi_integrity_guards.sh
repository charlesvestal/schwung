#!/usr/bin/env bash
# Two integrity guards on the shim <-> shadow_ui MIDI paths (source pins: the
# shim and shadow_ui cannot be compiled on a dev Mac).
#
#  1. A SysEx that lost a packet on the way to shadow_ui loses the REST of
#     itself too, so JS never assembles a head and a tail with a hole between
#     into a well-framed wrong message.
#  2. js_shadow_midi_send refuses a length that is not whole packets: a 3- or
#     7-byte push would shift the outbound ring off its 4-byte grid for good.
set -euo pipefail
cd "$(dirname "$0")/../.."
pub=$(awk '/static inline void shadow_ui_midi_publish/,/^}/' src/schwung_shim.c)
echo "$pub" | grep -q "sysex_dropping\[cable\] = 1" \
  || { echo "FAIL: a dropped SysEx packet does not drop the rest of its message"; exit 1; }
echo "$pub" | grep -q "if (sysex_dropping\[cable\] && is_sysex && !starts)" \
  || { echo "FAIL: the rest of a damaged SysEx is still delivered"; exit 1; }
send=$(awk '/static JSValue js_shadow_midi_send/,/^}/' src/shadow/shadow_ui.c)
echo "$send" | grep -q "(len & 3) != 0) return JS_FALSE" \
  || { echo "FAIL: js_shadow_midi_send accepts a length that is not whole packets"; exit 1; }
echo "PASS: a damaged SysEx is dropped whole; sends are whole packets"
