#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A wav_position must be offerable as a modulation target.
#
# The picker filters chain_params by TYPE, and the list was written
# before wav_position existed. Excluding it does not merely hide a menu row: the
# param cannot be routed, so an LFO aimed at a sample start has to be pointed at
# the concrete per-pad key while the grid draws the ALIAS. `<alias>:modulated`
# then answers 0, the read cursor never asks for `:base`, and the key never
# enters refreshModulatedValues -- so the cell is fed the EFFECTIVE value by the
# ordinary rotation at ~6Hz instead of the base plus a 44Hz dot.
#
# Reported from the device as a lost LFO dot and a choppy cell, and confirmed
# there with param_tally: synth:pad_start and synth:pad_start:modulated both
# read 6x/window, synth:pad_start:base never read at all.
#
# Driven through the REAL mrdrums chain_params rather than a synthetic list, so
# the fixture cannot drift into declaring a type the fleet does not use.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import fs from "node:fs";
import { flatLfoTargetParams } from "./src/shared/lfo_target_groups.mjs";
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const src = fs.readFileSync("./src/shadow/shadow_ui.js", "utf8");
const m = src.match(/function lfoTargetParamsFor[\s\S]*?\n}/);
if (!m) fail("could not lift lfoTargetParamsFor out of shadow_ui.js");

const contracts = JSON.parse(fs.readFileSync("./tests/fixtures/module-contracts.json", "utf8"));
const mod = contracts.modules.find((x) => x.id === "mrdrums");
if (!mod) fail("mrdrums is not in the fleet fixture");
const cpRaw = typeof mod.chain_params === "string"
  ? mod.chain_params : JSON.stringify(mod.chain_params);
const cp = JSON.parse(cpRaw);

/* The premise: mrdrums really does declare its alias as wav_position. If the
 * module ever changes that, this test is measuring nothing and must say so
 * rather than passing. */
const alias = cp.find((p) => p.key === "pad_start");
if (!alias) fail("mrdrums no longer declares pad_start");
if (alias.type !== "wav_position")
  fail("pad_start is now type " + alias.type + " -- this test no longer covers wav_position");

/* The type allowlist itself now lives in shared/lfo_target_groups.mjs, so the
   real one is injected rather than a copy -- this still measures the filter
   the picker actually applies, one indirection further out. */
const lfoTargetParamsFor = new Function(
  "chainTargetGetParam", "debugLog", "LFO_TARGET_PARAMS", "flatLfoTargetParams",
  m[0] + "; return lfoTargetParamsFor;"
)(() => cpRaw, () => {}, [], flatLfoTargetParams);

const offered = lfoTargetParamsFor("synth", "synth", "test").map((x) => x.key);

if (offered.indexOf("pad_start") < 0)
  fail("a wav_position param is not offered as an LFO target -- sample start "
     + "cannot be modulated, so its cell can never show a base or a dot");

/* The ordinary types must still be there: widening the filter must not have
 * replaced it. */
for (const k of ["pad_vol", "pad_pan", "p01_start"])
  if (offered.indexOf(k) < 0) fail("regression: " + k + " is no longer offered");

/* And a NON-numeric param must still be excluded, or the picker fills with
 * things the modulation engine cannot scale -- find_param_by_key needs a
 * declared range. A filepath is the case in this very contract. */
if (offered.indexOf("pad_sample_path") >= 0)
  fail("a filepath is being offered as a modulation target");

console.log("  ok  a wav_position is offerable as an LFO target");
console.log("  ok  float/int/enum targets are unaffected");
console.log("  ok  a filepath is still excluded");
console.log("PASS: sample position can be modulated, so its cell can report it");
'
