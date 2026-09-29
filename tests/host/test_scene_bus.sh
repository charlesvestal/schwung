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
grep -q 'shadow_set_scene_pairs(flat, sceneActive)' src/shadow/shadow_ui.js || fail "the UI never mirrors its pairings to the shim"
grep -q 'scenesAdoptPc(sceneState())' src/shadow/shadow_ui.js || fail "the UI never adopts a PC's scene"
echo "PASS: scene bus wiring"
