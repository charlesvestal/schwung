#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# WHILE A SNAPSHOT IS ARMED, A PLAIN READ ANSWERS THE LOCK -- deliberately, so
# the knob on screen shows what a turn changes. Every SAVE that shared that key
# wrote the lock into the set as the user's value (slot volume/pan into
# shadow_chain_config.json, the send returns into send_levels.json, a stateless
# Master FX module's params into its state file), indistinguishable from it
# after a reload. Saves read `:base`, which is the knob armed or not.
#
# The behaviour of `:base` on the buses is unit-tested in test_scene_bus.c
# (shadow_scene_bus_read_base); the host scope's `:base` is scene_host_view.
# These pins are on the save paths and the shim wiring no unit reaches.

ui=src/shadow/shadow_ui.js
mgmt=src/host/shadow_chain_mgmt.c
fail() { echo "FAIL: $*"; exit 1; }

body() {  # the body of a top-level JS function, by name
  awk -v f="$1" '$0 ~ "^function " f "\\(" {on=1} on {print} on && /^}/ {exit}' "$ui"
}

chaincfg=$(body saveChainConfigToDir)
[ -n "$chaincfg" ] || fail "saveChainConfigToDir not found"
grep -q '":base"' <<< "$chaincfg" || fail "saveChainConfigToDir must read the :base of volume/pan"
grep -q 'knob(i, "slot:volume")' <<< "$chaincfg" || fail "slot volume is saved from its plain (armed: LOCK) read"
grep -q 'knob(i, "slot:pan")' <<< "$chaincfg" || fail "slot pan is saved from its plain (armed: LOCK) read"
if grep -q 'getSlotParam(i, "slot:volume")\s*||' <<< "$chaincfg"; then
  fail "slot volume still saved straight from the plain read"
fi

sends=$(body saveSendLevels)
[ -n "$sends" ] || fail "saveSendLevels not found"
grep -q 'bus.prefix + k + ":base"' <<< "$sends" || fail "send levels must be saved from :base"

mfx=$(body saveMasterFxChainConfigOnMaster)
[ -n "$mfx" ] || fail "saveMasterFxChainConfigOnMaster not found"
grep -q 'master_fx:${key}:${p.key}:base' <<< "$mfx" \
  || fail "the stateless Master FX params fallback must read :base"

# The shim answers `:base` for a scene-driven bus param from the drive's base,
# on BOTH buses -- on a send it used to reach the plugin as "mix:base".
grep -q 'shadow_scene_bus_read_base(0, mfx_slot, bare_param' "$mgmt" \
  || fail "master_fx:fxN:<p>:base does not ask the scene's base"
grep -q 'shadow_scene_bus_read_base(send_idx + 1, send_fx, base_param' "$mgmt" \
  || fail "send<N>:fxM:<p>:base does not ask the scene's base"

echo "PASS: scene saves read the knob"
