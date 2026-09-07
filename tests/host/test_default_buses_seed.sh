#!/usr/bin/env bash
#
# default_buses: a module hands its own effects over as a BUS.
#
# dr32 is the case. Its Drum Bus is four stages in series over the whole kit,
# and shipping that as ONE insert loses the only thing anyone wants from it --
# swapping just the compressor, or putting a drive between two stages. A
# container of positions is the point.
#
# A BUS AND NOT THE SLOT CHAIN, and the difference is real: a bus catches the
# VOICES, while the slot chain catches the synth's whole output including any
# returns it sums internally. A bus also keeps the module glue out of the eight
# slot positions the user wants for their own effects.
#
# Same guards as default_fx and for the same reasons -- an interactive pick
# only, and only into a slot with NO buses, because a slot that has buses has
# been arranged by somebody and adding to it silently re-routes their voices.
set -euo pipefail

cd "$(dirname "$0")/../.."
UI="src/shadow/shadow_ui.js"
[ -f "$UI" ] || { echo "FAIL: cannot find $UI"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.argv[1], "utf8");
const BusModel = await import(process.cwd() + "/src/shared/bus_model.mjs");
let bad = 0;
const fail = (m) => { console.log("FAIL: " + m); bad = 1; };
const ok = (m) => console.log("  ok  " + m);
const grab = (n) => {
  const m = src.match(new RegExp("^function " + n + "\\([^)]*\\)\\s*\\{[^]*?^}", "m"));
  if (!m) { fail("could not lift " + n + "()"); process.exit(1); }
  return m[0];
};

/* A config with `n` buses already present, in the shape parseBusesConfig makes. */
const cfgJson = (n) => JSON.stringify({
  buses: Array.from({ length: BusModel.SLOT_BUSES }, (_, i) =>
    i < n ? { present: 1, name: "B" + i, orphans: 0, voices: [], sends: [0, 0], fx: [] }
          : { present: 0 }),
  main_sends: [0, 0],
});

function rig(meta, existingBuses, voicesJson) {
  const body = [
    "const BusModel = arguments[0];",
    "let writes = [];",
    "const META = " + JSON.stringify(meta) + ";",
    "function host_get_module_metadata() { return META; }",
    "function getSlotParam(slot, key) {",
    "  if (key === \"buses:config\") return " + JSON.stringify(existingBuses) + ";",
    "  if (key === \"synth:split_voices\") return " + JSON.stringify(voicesJson) + ";",
    "  return \"\"; }",
    "function setSlotParam(slot, key, val) { writes.push(key + \"=\" + val); return true; }",
    "function debugLog() {}",
    grab("moduleDefaultBuses"), grab("seedDefaultBusesForSlot"),
    "return { seed: (id) => seedDefaultBusesForSlot(0, id), writes };",
  ].join("\n");
  return new Function(body)(BusModel);
}

const VOICES = JSON.stringify([{ id: "pad0", label: "Kick" },
                               { id: "pad1", label: "Snare" },
                               { id: "pad2", label: "Hats" }]);

const DRUMBUS = { capabilities: { default_buses: [{
  name: "Drum Bus",
  voices: "*",
  fx: [ { module: "dr32-fx", params: { effect: "Crunch" } },
        { module: "clap",    params: { plugin_id: "Pop3" }, preset: "Glue" } ],
}] } };

/* ---- the whole kit, routed by default -------------------------------- */
{
  const r = rig(DRUMBUS, cfgJson(0), VOICES);
  if (r.seed("dr32") !== 1) fail("no bus was created");
  const w = r.writes;
  if (w[0] !== "bus1:create=1") fail("first write was " + w[0] + ", expected the create");
  /* ONE voices write carrying the WHOLE list -- every bus<N>:voices write is a
     replace, so a per-voice write would leave only the last one in the bus. */
  const vw = w.filter((x) => x.startsWith("bus1:voices="));
  if (vw.length !== 1 || vw[0] !== "bus1:voices=pad0,pad1,pad2") {
    fail("voices written as " + JSON.stringify(vw));
  }
  if (w.indexOf("bus1:name=Drum Bus") < 0) fail("the bus was not named");
  /* Inserts are POSITIONS: module first, then its params, then its preset. */
  const want = ["bus1:fx1:module=dr32-fx", "bus1:fx1:effect=Crunch",
                "bus1:fx2:module=clap", "bus1:fx2:plugin_id=Pop3",
                "bus1:fx2:preset_name=Glue"];
  for (const k of want) if (w.indexOf(k) < 0) fail("missing write: " + k);
  if (w.indexOf("bus1:fx2:preset_name=Glue") < w.indexOf("bus1:fx2:plugin_id=Pop3")) {
    fail("the preset was applied before the params it depends on");
  }
  ok("creates the bus, routes every voice in ONE write, fills its positions in order");
}

/* ---- a SUBSET, which is what a bus is really for --------------------- */
{
  const sub = { capabilities: { default_buses: [{
    name: "Low", voices: ["pad0", "pad1", "ghost"], fx: [] } ] } };
  const r = rig(sub, cfgJson(0), VOICES);
  r.seed("dr32");
  const vw = r.writes.filter((x) => x.startsWith("bus1:voices="));
  /* An id the synth does not publish is DROPPED, not passed through: the write
     is a whole-list replace and the chain host would keep it as an orphan. */
  if (vw[0] !== "bus1:voices=pad0,pad1") {
    fail("subset routing wrote " + JSON.stringify(vw) + " -- an unknown id must be dropped");
  }
  ok("a subset routes only its own voices, and an unpublished id is dropped");
}

/* ---- the guards ------------------------------------------------------ */
{
  /* A slot that already has a bus has been arranged by somebody. */
  const r = rig(DRUMBUS, cfgJson(1), VOICES);
  if (r.seed("dr32") !== 0 || r.writes.length) {
    fail("seeded into a slot that already had buses: " + JSON.stringify(r.writes));
  }
  ok("a slot with buses is left alone");

  /* A FAILED config read is not an empty slot. busCount answers -1 for a read
     that did not complete, never 0, exactly so this can tell them apart. */
  for (const answer of [null, "", "not json"]) {
    const q = rig(DRUMBUS, answer, VOICES);
    if (q.seed("dr32") !== 0 || q.writes.length) {
      fail("seeded on a " + JSON.stringify(answer) + " config read");
    }
  }
  ok("a failed config read declines rather than creating buses");

  /* An UNRESOLVED voice read is not "no voices". parseSplitVoices answers
     { unresolved: true } for a read that did not complete, and treating that as
     empty gives a slow synth a bus with nothing routed into it and no second
     chance to fill it. Covered by the null/"" cases below, which is what an
     incomplete read looks like on the wire. */
  /* No voices means nothing to route, and a bus with no voices does nothing. */
  for (const v of [null, "", "[]"]) {
    const q = rig(DRUMBUS, cfgJson(0), v);
    if (q.seed("dr32") !== 0 || q.writes.length) {
      fail("created a bus with no voices to put in it (" + JSON.stringify(v) + ")");
    }
  }
  ok("a synth that publishes no voices gets no bus");

  /* Nothing declared costs nothing. */
  for (const m of [{}, { capabilities: {} }, { capabilities: { default_buses: "x" } }]) {
    const q = rig(m, cfgJson(0), VOICES);
    if (q.seed("x") !== 0 || q.writes.length) fail("a module declaring nothing produced writes");
  }
  ok("no declaration writes nothing");
}

/* ---- caps ------------------------------------------------------------ */
{
  const many = { capabilities: { default_buses:
    Array.from({ length: 20 }, (_, i) => ({ name: "B" + i, voices: "*",
      fx: Array.from({ length: 20 }, (_, j) => ({ module: "m" + j })) })) } };
  const r = rig(many, cfgJson(0), VOICES);
  const made = r.seed("x");
  if (made !== BusModel.SLOT_BUSES) {
    fail("20 declared buses produced " + made + ", expected the cap of " + BusModel.SLOT_BUSES);
  }
  const f1 = r.writes.filter((x) => /^bus1:fx\d+:module=/.test(x));
  if (f1.length !== BusModel.BUS_FX_SLOTS) {
    fail("20 declared inserts produced " + f1.length + ", expected " + BusModel.BUS_FX_SLOTS);
  }
  ok("both caps truncate rather than letting the tail vanish silently");
}

/* ---- the call site --------------------------------------------------- */
{
  const m = src.match(/^function applyComponentSelectionConfirmed\([^)]*\)\s*\{[^]*?^}/m);
  if (!m || !/seedDefaultBusesForSlot\(/.test(m[0])) {
    fail("the interactive pick never seeds buses -- default_buses would be inert");
  }
  const sites = [...src.matchAll(/seedDefaultBusesForSlot\(/g)].length;
  if (sites !== 2) {
    fail("seedDefaultBusesForSlot appears " + sites + " times; defined once and called "
         + "from the interactive pick ALONE -- a restore path that seeds re-creates "
         + "buses the user deleted, on every boot");
  }
  ok("seeded from the interactive synth pick, and from nowhere else");
}

if (bad) process.exit(1);
console.log("PASS");
' "$UI"
