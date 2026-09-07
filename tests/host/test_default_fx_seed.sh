#!/usr/bin/env bash
#
# default_fx: a module says what belongs in the chain behind it.
#
# A drum module voiced through a bus compressor has had two bad options -- keep
# the effect INSIDE the module (a second, worse copy of what the slot chain
# provides, reachable only from in there and persisted by hand), or make the
# user add it after every load, where it is not part of the sound.
#
# The whole design is in WHEN it fires:
#
#  - on an INTERACTIVE PICK only. Every restore path -- boot, set change, patch
#    load -- reconstructs a chain the user has already shaped, so seeding there
#    would put back an effect they deleted, every boot, with no way to refuse it.
#  - into an EMPTY FX section only. A slot carrying effects has been shaped by
#    somebody, and appending rewrites their signal path silently.
#  - never on a FAILED read of the count. Seeding because a read did not
#    complete is how a chain gets a module appended to it for no reason.
set -euo pipefail

cd "$(dirname "$0")/../.."
UI="src/shadow/shadow_ui.js"
[ -f "$UI" ] || { echo "FAIL: cannot find $UI"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.argv[1], "utf8");
let bad = 0;
const fail = (m) => { console.log("FAIL: " + m); bad = 1; };
const ok = (m) => console.log("  ok  " + m);
const grab = (n) => {
  const m = src.match(new RegExp("^function " + n + "\\([^)]*\\)\\s*\\{[^]*?^}", "m"));
  if (!m) { fail("could not lift " + n + "()"); process.exit(1); }
  return m[0];
};

function rig(meta, fxCount) {
  const body = [
    "const CHAIN_CAP = { fx: 8 };",
    "let writes = [];",
    "const META = " + JSON.stringify(meta) + ";",
    "function host_get_module_metadata() { return META; }",
    "function getSlotParam(slot, key) { return " + JSON.stringify(fxCount) + "; }",
    "function setSlotParam(slot, key, val) { writes.push(key + \"=\" + val); return true; }",
    "function debugLog() {}",
    grab("moduleDefaultFx"), grab("seedDefaultFxForSlot"),
    "return { seed: (id) => seedDefaultFxForSlot(0, id), writes,",
    "         decl: (id) => moduleDefaultFx(id) };",
  ].join("\n");
  return new Function(body)();
}

const DECL = { capabilities: { default_fx: [
  { module: "clap", params: { plugin_id: "PurestDrive" } },
] } };

/* An empty chain seeds, and the params land on the position just written. */
{
  const r = rig(DECL, "0");
  if (r.seed("dr32") !== 1) fail("an empty FX section did not seed");
  if (JSON.stringify(r.writes) !==
      JSON.stringify(["fx1:module=clap", "fx1:plugin_id=PurestDrive"])) {
    fail("seed wrote " + JSON.stringify(r.writes));
  }
  ok("an empty FX section is seeded, module first then its params");
}

/* A chain that already holds anything is LEFT ALONE. */
{
  const r = rig(DECL, "1");
  if (r.seed("dr32") !== 0 || r.writes.length) {
    fail("seeded into a chain that already held an effect: " + JSON.stringify(r.writes));
  }
  ok("a non-empty FX section is left alone");
}

/* A FAILED read is not a zero. */
for (const answer of [null, undefined, ""]) {
  const r = rig(DECL, answer);
  if (r.seed("dr32") !== 0 || r.writes.length) {
    fail("seeded on a " + JSON.stringify(answer) + " count read -- a read that did "
         + "not complete is not an empty chain");
  }
}
ok("a failed count read declines rather than seeding");

/* A module that declares nothing costs nothing and writes nothing. */
for (const meta of [{}, { capabilities: {} }, { capabilities: { default_fx: "clap" } },
                    { capabilities: { default_fx: [{}, { module: "" }] } }, null]) {
  const r = rig(meta, "0");
  if (r.seed("x") !== 0 || r.writes.length) {
    fail("a module declaring " + JSON.stringify(meta) + " produced writes");
  }
}
ok("no declaration, a malformed one, or entries with no module: nothing written");

/* Bounded by the section cap: a declaration longer than the slot can hold must
   not have its tail vanish without a word -- it is truncated at the cap. */
{
  const many = { capabilities: { default_fx:
    Array.from({ length: 20 }, (_, i) => ({ module: "m" + i })) } };
  const r = rig(many, "0");
  if (r.decl("x").length !== 8) {
    fail("a 20-entry declaration resolved to " + r.decl("x").length + ", expected the cap of 8");
  }
  ok("a declaration longer than the section cap is truncated to it");
}

/* metadata handed over as a JSON STRING is parsed -- the binding has returned
   both shapes across versions, and a string would otherwise read as "declares
   nothing" rather than as an error. */
{
  const r = rig(JSON.stringify(DECL), "0");
  if (r.seed("dr32") !== 1) fail("metadata delivered as a JSON string was not parsed");
  ok("metadata as a JSON string is parsed");
}

/* ---- THE CALL SITE, which is the actual design ------------------------ */
{
  const m = src.match(/^function applyComponentSelectionConfirmed\([^)]*\)\s*\{[^]*?^}/m);
  if (!m) fail("could not find applyComponentSelectionConfirmed");
  else {
    if (!/seedDefaultFxForSlot\(/.test(m[0])) {
      fail("the interactive pick never seeds -- default_fx would be inert");
    }
    if (!/comp\.key === "synth"/.test(m[0])) {
      fail("the seed is not gated on the SYNTH position; an audio FX pick would "
           + "seed the chain it was just added to");
    }
  }
  /* AND NOWHERE ELSE. A restore path that seeded would put back an effect the
     user deleted, on every boot. */
  const sites = [...src.matchAll(/seedDefaultFxForSlot\(/g)].length;
  if (sites !== 2) {   /* the definition, and the single call */
    fail("seedDefaultFxForSlot appears " + sites + " times; it must be defined "
         + "once and called from the interactive pick ALONE -- a restore path "
         + "that seeds re-adds an effect the user removed");
  }
  ok("seeded from the interactive synth pick, and from nowhere else");
}

if (bad) process.exit(1);
console.log("PASS");
' "$UI"
