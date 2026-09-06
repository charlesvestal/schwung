#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The chain editor must not FLATTEN a knob that drives several parameters.
#
# It shows one target and one param per knob and rebuilds the patch's knob
# array from what it holds. Reading one row per knob and writing one back
# deleted every extra destination and every window, permanently, the first time
# a chain built on the device was saved from here -- and it looked correct on
# both screens the whole time. That is the only silent data-loss path in this
# feature, so it is pinned by RUNNING the real round trip, not by grepping.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.cwd() + "/src/modules/chain/ui.js", "utf8");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => console.log("  ok  " + m);
const check = (c, m) => c ? ok(m) : fail(m);

/* Lift the two pure helpers and the two constructors they use. */
function lift(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return -1; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail(name + " has no end"); return -1; }
  return src.slice(at, end + 2);
}
const parts = ["createEmptyKnobs", "createEmptyKnobRows", "parseKnobMappings", "buildKnobMappings"].map(lift);
if (parts.some((p) => p === -1)) process.exit(1);

const mk = new Function("NUM_KNOBS", "KNOB_CC_START",
  parts.join("\n") + "\nreturn { parseKnobMappings, buildKnobMappings };");
const { parseKnobMappings, buildKnobMappings } = mk(8, 71);

/* A knob driving three parameters, one of them through an inverted window,
   plus an ordinary single-destination knob alongside it. */
const patchRows = [
  { cc: 71, target: "synth", param: "cutoff", value: 0.4, dest: 0, pos: 0.75 },
  { cc: 71, target: "fx1",   param: "mix",    value: 0.2, lo: 0.2, hi: 0.8, dest: 1, pos: 0.75 },
  { cc: 71, target: "fx2",   param: "drive",  value: 0.1, lo: 1.0, hi: 0.0, dest: 2, pos: 0.75 },
  { cc: 72, target: "fx1",   param: "gain",   value: 0.5 }
];

const parsed = parseKnobMappings(patchRows);
check(parsed.knobs[0].param === "cutoff", "the editor edits the FIRST destination");
check(parsed.knobs[1].param === "gain", "an ordinary knob is unaffected");

const out = buildKnobMappings({ knobs: parsed.knobs, knobRows: parsed.knobRows });
check(out.length === patchRows.length,
      "a save writes back as many rows as it read (" + out.length + " of " + patchRows.length + ")");
check(JSON.stringify(out) === JSON.stringify(patchRows),
      "and they are IDENTICAL -- windows, positions and destination indices all survive");

/* Re-pointing the first destination is a real edit and must be kept, while the
   others stay exactly as they were. */
const edited = { knobs: parsed.knobs.slice(), knobRows: parsed.knobRows };
edited.knobs[0] = { slot: "synth", param: "resonance" };
const out2 = buildKnobMappings(edited);
check(out2[0].param === "resonance", "re-pointing the first destination is kept");
check(out2[0].lo === undefined && out2[0].dest === 0,
      "...and the rest of its row rides along");
check(out2[1].param === "mix" && out2[1].lo === 0.2,
      "the second destination is untouched by an edit to the first");
check(out2[2].lo === 1.0 && out2[2].hi === 0.0,
      "and the inverted window is still inverted");

/* An older patch -- one bare row per knob -- must still round-trip, including
   the preset special case this editor has always added. */
const legacy = [{ cc: 71, target: "synth", param: "preset" }];
const lp = parseKnobMappings(legacy);
const lout = buildKnobMappings({ knobs: lp.knobs, knobRows: lp.knobRows });
check(lout.length === 1 && lout[0].param === "preset", "an old single-row knob round-trips");

/* A knob assigned in this editor, with no rows behind it, takes the original
   path -- including the preset metadata. */
const fresh = buildKnobMappings({
  knobs: [{ slot: "synth", param: "preset" }], knobRows: [null]
});
check(fresh.length === 1 && fresh[0].type === "int" && fresh[0].max_param === "preset_count",
      "a knob newly assigned here still gets the preset metadata");

if (failures) { console.error("\n" + failures + " check(s) failed"); process.exit(1); }
console.log("\nPASS: the chain editor carries a knob every destination it was given");
'
