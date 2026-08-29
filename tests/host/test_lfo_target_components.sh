#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# What a slot LFO is allowed to MODULATE, and what asking costs.
#
# The picker used to stop at two of each section -- the old fixed chain shape.
# The DSP has held eight of each for a while, so a user could build an eight-FX
# chain and only ever reach FX 1 and FX 2 with an LFO. Every fixture below is
# RUN rather than pattern-matched, so a list that merely looks longer in the
# source but still omits a position fails here.
#
# The second half is the reason the fix is not "loop to eight". This runs inside
# a draw on a label-cache miss, and a reorder forces one, so each position
# walked is TWO IPC round trips at ~2.8ms. Walking the cap would cost ~45ms on
# the frame after every reorder. The count says how far to go; the test pins
# that it is actually used.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* Lift a top-level function out of shadow_ui.js and hand it its dependencies
   as parameters. The file cannot be imported -- it is a device UI module full
   of host globals -- but a function that closes over a few named things can be
   RUN, which is the difference between pinning the source text and pinning the
   behaviour. */
function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

const CHAIN_CAP = { midiFx: 8, fx: 8 };
const LFO_TARGET_PARAMS = [];
const VIEWS = { CHAIN_SETTINGS: "chain_settings" };

const makeCtx = lift("makeSlotLfoCtx",
  ["getSlotParam", "setSlotParam", "shadowSetParamBlocking", "CHAIN_CAP",
   "LFO_TARGET_PARAMS", "VIEWS", "debugLog", "collectChainTargetComponents"]);
if (!makeCtx) process.exit(1);

/* makeSlotLfoCtx.getTargetComponents() now delegates the synth/FX/MIDI FX
   walk to this extracted helper (shared with the Macro target picker) rather
   than doing it inline -- lifted the same way, and handed in as one more
   dependency, so this test still runs the REAL walk rather than a copy of it. */
const liftCollect = lift("collectChainTargetComponents", ["getSlotParam", "CHAIN_CAP"]);
if (!liftCollect) process.exit(1);

/* A fake slot that answers only what it was given. An unserved key answers ""
   the way the real param channel does -- null would read as a different kind
   of nothing and hide a missing guard. */
function device(state) {
  const reads = [];
  const get = (slot, key) => { reads.push(key); return state[key] !== undefined ? state[key] : ""; };
  return { get, reads, state };
}
const targetsOf = (state) => {
  const d = device(state);
  const collect = liftCollect(d.get, CHAIN_CAP);
  const ctx = makeCtx(d.get, () => true, () => true, CHAIN_CAP,
                      LFO_TARGET_PARAMS, VIEWS, () => {}, collect)(0, 0);
  return { comps: ctx.getTargetComponents(), reads: d.reads };
};
const labelFor = (comps, key) => {
  const hit = comps.find((c) => c.key === key);
  return hit ? hit.label : null;
};

/* 1. HOLES ARE LEGAL, and everything up to the high-water mark is offered.
      fx_count is a high-water mark (chain_host.c keeps `fx_count = slot + 1`
      and only trims a TRAILING null), so fx2, fx4 and fx5 being empty inside a
      count of 6 is a chain the DSP can really be in. */
{
  const { comps } = targetsOf({
    synth_module: "sf2", "synth:name": "SF2",
    fx_count: "6",
    fx1_module: "freeverb", "fx1:name": "Freeverb",
    fx3_module: "cloudseed", "fx3:name": "CloudSeed",
    fx6_module: "tapescam", "fx6:name": "Tapescam",
  });
  for (const [key, label] of [["fx1", "FX 1: Freeverb"],
                              ["fx3", "FX 3: CloudSeed"],
                              ["fx6", "FX 6: Tapescam"]]) {
    const got = labelFor(comps, key);
    if (got !== label)
      fail(key + " is not offered as an LFO target (or is mislabelled): " + got);
  }
  /* An empty position inside the count is NOT a target -- there is nothing
     there to modulate, and offering it would route an LFO into a hole. */
  for (const key of ["fx2", "fx4", "fx5"])
    if (labelFor(comps, key) !== null)
      fail(key + " is empty but was offered as a target");
  if (labelFor(comps, "synth") !== "Synth: SF2") fail("the synth stopped being a target");
}

/* 2. MIDI FX past the second are reachable too -- same defect, same fix. */
{
  const { comps } = targetsOf({
    midi_fx_count: "4",
    midi_fx1_module: "arp", "midi_fx1:name": "Arp",
    midi_fx3_module: "chord", "midi_fx3:name": "Chord",
    midi_fx4_module: "velocity", "midi_fx4:name": "Velocity",
  });
  for (const [key, label] of [["midi_fx1", "MIDI FX 1: Arp"],
                              ["midi_fx3", "MIDI FX 3: Chord"],
                              ["midi_fx4", "MIDI FX 4: Velocity"]]) {
    const got = labelFor(comps, key);
    if (got !== label)
      fail(key + " is not offered as an LFO target (or is mislabelled): " + got);
  }
}

/* 3. THE COST. A two-FX chain must not pay for the six positions it does not
      have: the bound is the published count, never the cap. Each extra
      position is two IPC round trips inside a draw. */
{
  const { comps, reads } = targetsOf({
    fx_count: "2", midi_fx_count: "0",
    fx1_module: "freeverb", "fx1:name": "Freeverb",
    fx2_module: "cloudseed", "fx2:name": "CloudSeed",
  });
  if (labelFor(comps, "fx2") !== "FX 2: CloudSeed") fail("fx2 stopped being a target");
  const strayed = reads.filter((k) => /^(fx|midi_fx)([3-8])[_:]/.test(k));
  if (strayed.length)
    fail("walked past the published count into positions that cannot hold anything: [" +
         strayed.join(" ") + "]");
  /* And the count itself is ASKED, once -- probing fx1..fx8 to discover the
     same thing is the eight-round-trip version of this question. */
  if (reads.filter((k) => k === "fx_count").length !== 1)
    fail("fx_count should be read exactly once, got " +
         reads.filter((k) => k === "fx_count").length);
}

/* 4. An EMPTY chain is cheap: two counts, one synth probe, and no walk. */
{
  const { comps, reads } = targetsOf({ fx_count: "0", midi_fx_count: "0" });
  const positionReads = reads.filter((k) => /^(fx|midi_fx)\d/.test(k));
  if (positionReads.length)
    fail("an empty chain still probed positions: [" + positionReads.join(" ") + "]");
  if (comps.some((c) => /^(fx|midi_fx)\d/.test(c.key)))
    fail("an empty chain offered a chain position as a target");
}

if (failures) process.exit(1);
console.log("PASS: slot LFO targets — every occupied position up to the published count is " +
            "offered (holes skipped, FX and MIDI FX alike), and the walk is bounded by that " +
            "count rather than the cap of 8");
'
