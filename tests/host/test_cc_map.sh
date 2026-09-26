#!/usr/bin/env bash
# The generic CC map (cc_map.mjs) through the real control host:
#   - an ABSOLUTE CC spans the parameter range; an enum lands on an option,
#     in the module own wire form
#   - a RELATIVE CC steps through the knob engine
#   - a burst of CCs in one tick is ONE write (IPC is the cost)
#   - a binding whose module is gone is owned but writes nothing
#   - LEARN binds a controller CC to a parameter moved on Move, in EITHER
#     order, saves it to the set, and turns the shim learn flag off after
#   - claimPairs names exactly the bound CCs, for the shim claim table
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
import { createControlHost } from "./src/shared/control_host.mjs";
import { createCCMap, relativeTicks, absValue } from "./src/shared/cc_map.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };
const ok = (n, c) => eq(n, !!c, true);

const files = {};
const fs = { exists: (p) => p in files, read: (p) => files[p], write: (p, t) => { files[p] = t; }, ensureDir: () => {} };
let t = 1000;
const chain = { slots: [{ synth: "obxd", fx: [], midiFx: [] }, {}, {}, {}] };
const CP = JSON.stringify([{ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1 },
                           { key: "wave", name: "Wave", type: "enum", options: ["Saw", "Sq", "Tri"] }]);
const params = { "0|synth:chain_params": CP, "0|synth:cutoff": "0.5", "0|synth:wave": "Saw" };
const writes = [];
let host = null;
const setSlotParam = (slot, key, v) => { params[slot + "|" + key] = String(v); writes.push([key, String(v)]); host.observeWrite(slot, key, v); return true; };
host = createControlHost({ fs, stateDir: () => "/s", now: () => t,
  getParam: (s, k) => (params[s + "|" + k] === undefined ? null : params[s + "|" + k]),
  setParam: setSlotParam, chainShape: () => chain, masterFx: () => ({}) });
let shimLearn = false;
const cc = createCCMap({ controls: () => host.controls(), edit: (fn) => host.edit(fn), targets: host.targets,
  learn: host.learn, setShimLearn: (on) => { shimLearn = on; }, now: () => t });
const CUT = { kind: "param", slot: 0, component: "synth", key: "cutoff", module: "obxd", label: "Cutoff" };
const WAVE = Object.assign({}, CUT, { key: "wave", label: "Wave" });

eq("relative decoding", [relativeTicks(1), relativeTicks(127), relativeTicks(64)], [1, -1, 0]);
eq("an absolute value spans a float range", absValue(127, { min: 0, max: 1 }), 1);

/* ---- learn, CC first ---- */
cc.beginLearn();
ok("learn turns the shim learn flag on", shimLearn);
cc.feed(0xB0, 74, 10);
setSlotParam(0, "synth:cutoff", "0.6");
const saved = () => JSON.parse(files["/s/controls.json"]);
eq("CC then parameter: bound, saved to the set", saved().cc.map((b) => [b.channel, b.cc, b.target.key]), [[0, 74, "cutoff"]]);
eq("...and the shim learn flag is off again", shimLearn, false);

/* ---- learn, parameter first ---- */
cc.beginLearn();
setSlotParam(0, "synth:wave", "Sq");
cc.feed(0xB1, 20, 64);
eq("parameter then CC: bound too", saved().cc.map((b) => [b.channel, b.cc, b.target.key]), [[0, 74, "cutoff"], [1, 20, "wave"]]);

/* ---- absolute ---- */
writes.length = 0;
for (let v = 0; v <= 64; v += 8) cc.feed(0xB0, 74, v);
eq("a burst of CCs writes nothing until the tick", writes.length, 0);
t += 16; cc.tick(t);
eq("...then ONE write, the latest value", writes.length === 1 && writes[0][0] === "synth:cutoff" && Math.abs(Number(writes[0][1]) - 64 / 127) < 0.01, true);
writes.length = 0;
cc.feed(0xB1, 20, 127); t += 16; cc.tick(t);
eq("an enum lands on its last option, in the module wire form", writes, [["synth:wave", "Tri"]]);

/* ---- relative ---- */
host.edit((doc) => { const d = JSON.parse(JSON.stringify(doc)); d.cc[0].mode = "rel"; return Object.assign(doc, {}) && { version: 1, surface: doc.surface, cc: d.cc, extra: doc.extra }; });
cc.reload();
params["0|synth:cutoff"] = "0.5";
writes.length = 0;
cc.feed(0xB0, 74, 1); cc.feed(0xB0, 74, 1); t += 16; cc.tick(t);
ok("a relative CC steps up from the current value", writes.length === 1 && Number(writes[0][1]) > 0.5 && Number(writes[0][1]) < 0.6);

/* ---- dark ---- */
chain.slots[0].synth = "dx7";
writes.length = 0;
eq("a binding whose module is gone is owned...", cc.feed(0xB0, 74, 1), true);
t += 16; cc.tick(t);
eq("...but writes nothing", writes.length, 0);
chain.slots[0].synth = "obxd";

eq("an unbound CC is not ours", cc.feed(0xB0, 1, 1), false);
writes.length = 0;
eq("a CC the active SURFACE claims is never the CC map s, even when bound", cc.feed(0xB0, 74, 100, true), false);
t += 16; cc.tick(t);
eq("...and writes nothing", writes.length, 0);
cc.beginLearn();
cc.feed(0xB0, 5, 1, true);
eq("...and is never learned", cc.learning.cc, null);
cc.cancelLearn();
eq("claimPairs names the bound CCs", cc.claimPairs(), [[0, 74], [1, 20]]);
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
