#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# SCENES on a chain slot: the real chain_scene.c + chain_mod.c against a fake
# synth, measured at the DESTINATION (what the module was sent), plus pins on
# the chain_host.c wiring that no unit here can reach.

bin="build/tests/test_chain_scene"
mkdir -p "$(dirname "$bin")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_scene.c \
  src/modules/chain/dsp/chain_scene.c \
  src/modules/chain/dsp/chain_mod.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -lm -o "$bin"

"$bin"

host=src/modules/chain/dsp/chain_host.c
fail() { echo "FAIL: $*"; exit 1; }

# The table verbs and the edit arm are routed AHEAD of the component routes --
# an armed write that reached "synth:" first would change the base instead.
scene_line=$(grep -n 'chain_scene_route_set(inst, key, val)' "$host" | head -1 | cut -d: -f1 || true)
synth_line=$(grep -n 'strncmp(key, "synth:", 6) == 0' "$host" | head -1 | cut -d: -f1 || true)
[ -n "$scene_line" ] && [ -n "$synth_line" ] && [ "$scene_line" -lt "$synth_line" ] \
  || fail "the edit arm must be routed before the synth: route"
grep -q 'strncmp(key, "scenes:", 7) == 0' "$host" || fail "scenes: verbs not routed"
grep -q 'chain_scene_get_param(inst, key + 7' "$host" || fail "scenes: reads not routed"

# The armed read sits in front of the plain-key BASE answer on all three
# component routes, or an armed knob would show the base.
[ "$(grep -c 'chain_scene_edit_read(inst' "$host")" -eq 3 ] || fail "armed read missing from a component route"

# The tick runs inside lfo_tick, which also runs on idle frames via mod:tick.
# Captured first, not piped into grep -q: grep exits on its first match, awk
# then dies of SIGPIPE, and pipefail reports a failure that is only timing.
lfo_body=$(awk '/^static void lfo_tick\(chain_instance_t \*inst, int frames\) \{/,/^}/' "$host")
grep -q 'chain_scene_tick(inst)' <<< "$lfo_body" || fail "chain_scene_tick not called from lfo_tick"

grep -q 'chain_scene_init(inst)' "$host" || fail "instance never initialises its scenes (zeroed = scene 1)"
grep -q '^uint32_t chain_set_scene_morph(void \*instance' src/modules/chain/dsp/chain_scene.c || fail "export missing"
grep -q 'src/modules/chain/dsp/chain_scene.c' scripts/build.sh || fail "chain_scene.c not built"

# 64 targets: 32 cannot hold a scene's 64 pairs next to the LFOs.
grep -q '^#define MAX_MOD_TARGETS 64' src/modules/chain/dsp/chain_internal.h || fail "MAX_MOD_TARGETS"

echo "PASS: chain scene wiring"
