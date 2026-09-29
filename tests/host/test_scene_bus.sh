#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# SCENES on Master FX and the send buses (src/host/shadow_scene_bus.c), run
# against fake plugins and measured at what each plugin was SENT, plus pins on
# the shim wiring that no unit here reaches.

bin="build/tests/test_scene_bus"
mkdir -p "$(dirname "$bin")"
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
  -Isrc/host \
  tests/host/test_scene_bus.c src/host/shadow_scene_bus.c \
  -lm -o "$bin"
"$bin"

fail() { echo "FAIL: $*"; exit 1; }
mgmt=src/host/shadow_chain_mgmt.c
grep -q 'shadow_scene_bus_bind(' "$mgmt" || fail "bus io never bound"
[ "$(grep -c 'shadow_scene_bus_edit_write(' "$mgmt")" -ge 3 ] \
  || fail "the edit arm must see all three write paths (param channel, param= form, web set ring) plus sends"
[ "$(grep -c 'shadow_scene_bus_note_write(' "$mgmt")" -ge 3 ] || fail "a driven param's base must follow every write path"
grep -q 'shadow_scene_bus_set_verb(' "$mgmt" || fail "bus verbs not routed"
grep -q 'shadow_scene_bus_read(' "$mgmt" || fail "a driven bus param must read back its base / lock"
grep -q 'src/host/shadow_scene_bus.c' scripts/build.sh || fail "shadow_scene_bus.c not built into the shim"
# PROGRAM CHANGE: applied in the shim's external walk, and withheld from the
# slots and Move, or a slot receiving All changes preset on the same message.
pc_body=$(grep -n 'scene_pc_select(shadow_control->scene_pc_channel' src/schwung_shim.c | head -1 | cut -d: -f1 || true)
[ -n "$pc_body" ] || fail "the shim never applies a scene Program Change"
sed -n "${pc_body},$((pc_body + 10))p" src/schwung_shim.c | grep -q 'midi_in_swallow(sh_midi, hw_midi, j)' \
  || fail "a scene PC must be taken out of BOTH buffers"
grep -q 'SHADOW_UI_FLAG_SNAPSHOT_TAKE : snapshot_recall_pc()' src/schwung_shim.c \
  || fail "PC 126/127 must raise the same flags as Shift+Copy / Shift+Delete"
grep -A3 '^static uint16_t snapshot_recall_pc(void)' src/schwung_shim.c | grep -q 'recall_pending_target >= 0) return 0' \
  || fail "a repeated PC 127 must never CANCEL a queued recall"
grep -q 'shadow_set_scene_pairs(flat, sceneActive)' src/shadow/shadow_ui.js || fail "the UI never mirrors its pairings to the shim"
grep -q 'scenesAdoptPc(sceneState())' src/shadow/shadow_ui.js || fail "the UI never adopts a PC's scene"
# A PC applied by the shim is adopted BEFORE any push of the ends, or a push in
# the same tick (a pairing edit, an undo) overwrites it and the UI then adopts
# its own stale scene.
push_body=$(awk '/^function scenePushEnds\(\) \{/,/^}/' src/shadow/shadow_ui.js)
adopt_ln=$(grep -n 'scenesAdoptPc(sceneState())' <<< "$push_body" | head -1 | cut -d: -f1 || true)
ab_ln=$(grep -n 'shadow_set_scene_ab(' <<< "$push_body" | head -1 | cut -d: -f1 || true)
[ -n "$adopt_ln" ] && [ -n "$ab_ln" ] && [ "$adopt_ln" -lt "$ab_ln" ] \
  || fail "scenePushEnds must adopt a pending PC before it pushes the ends"
grep -q 'setActive: (k) => { scenesAdoptPc(sceneState()); sceneActive = k; scenePushEnds(); }' src/shadow/shadow_ui.js \
  || fail "a scene tap must consume a pending PC before choosing, or the push adopts the PC over it"
# Shift+Vol+Step 3 swallows BOTH edges: the press, and the release it owes
# (latched, never gated on Shift/Vol still being held).
s3=$(grep -n 'Shift + Volume + Step 3 (note 18) = the Scenes screen' src/schwung_shim.c | head -1 | cut -d: -f1 || true)
[ -n "$s3" ] || fail "Shift+Vol+Step 3 handler not found"
s3_body=$(sed -n "${s3},$((s3 + 20))p" src/schwung_shim.c)
grep -q 'step3_release_owed = 1;' <<< "$s3_body" || fail "Shift+Vol+Step 3 must latch its owed release"
grep -A2 'if (d1 == 18 && step3_release_owed && (type == 0x80 || (type == 0x90 && d2 == 0)))' <<< "$s3_body" \
  | grep -q 'midi_in_swallow(shadow + MIDI_IN_OFFSET, src, j)' \
  || fail "the Step 3 release must be swallowed from BOTH buffers"
echo "PASS: scene bus wiring"
