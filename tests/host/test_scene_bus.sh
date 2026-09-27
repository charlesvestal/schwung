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
echo "PASS: scene bus wiring"
