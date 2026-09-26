#!/usr/bin/env bash
# The generic CC map (cc_map.mjs) through the real control host:
#   - an ABSOLUTE CC spans the parameter range; an enum lands on an option,
#     in the module own wire form
#   - a RELATIVE CC steps through the knob engine
#   - a burst of CCs in one tick is ONE write (IPC is the cost)
#   - a binding whose module is gone is owned but writes nothing
#   - LEARN is a MODE, parameter first: move a parameter, move a CC -- bound
#     and saved -- then another pair, without re-arming; the footer says
#     where it is at each step; one CC per parameter; Stop ends it
#   - claimPairs names exactly the bound CCs, for the shim claim table
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
import { createControlHost } from "./src/shared/control_host.mjs";
import { createCCMap, relativeTicks, absValue } from "./src/shared/cc_map.mjs";
import { drawFooter, hintPairWidth } from "./src/shared/param_pages/render_page_movy.mjs";
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

/* ---- learn mode, parameter first ---- */
const saved = () => JSON.parse(files["/s/controls.json"]);
cc.beginLearn();
ok("learn turns the shim learn flag on", shimLearn);
eq("the footer asks for a parameter first", cc.learnFooter(), "Learn: move a param");
cc.feed(0xB0, 74, 10);
eq("a CC with no parameter chosen binds nothing", files["/s/controls.json"], undefined);
setSlotParam(0, "synth:cutoff", "0.6");
eq("a parameter moved: the footer names it and asks for a CC", cc.learnFooter(), "Learn: Cutoff > CC?");
writes.length = 0;
cc.feed(0xB0, 74, 10);
eq("then a CC: bound, saved to the set", saved().cc.map((b) => [b.channel, b.cc, b.target.key]), [[0, 74, "cutoff"]]);
eq("...the footer says so", cc.learnFooter(), "Learn: Cutoff: CC74");
t += 16; cc.tick(t);
eq("...and the binding message moved nothing", writes, []);
cc.feed(0xB0, 74, 127); t += 16; cc.tick(t);
eq("the same CC again DRIVES it, learn still on", [writes.length, !!cc.learning, shimLearn], [1, true, true]);
setSlotParam(0, "synth:wave", "Sq");
eq("another parameter, without re-arming", cc.learnFooter(), "Learn: Wave > CC?");
cc.feed(0xB1, 20, 64);
eq("...another CC binds to it", saved().cc.map((b) => [b.channel, b.cc, b.target.key]), [[0, 74, "cutoff"], [1, 20, "wave"]]);
eq("...off channel 1 the footer names the channel", cc.learnFooter(), "Learn: Wave: Ch2 CC20");
cc.feed(0xB0, 21, 64);
eq("a second CC brushed MOVES the parameter binding, never duplicates it",
   saved().cc.map((b) => [b.channel, b.cc, b.target.key]), [[0, 74, "cutoff"], [0, 21, "wave"]]);
cc.feed(0xB1, 20, 64);
eq("...and the first one back", saved().cc.map((b) => [b.channel, b.cc, b.target.key]), [[0, 74, "cutoff"], [1, 20, "wave"]]);
/* The footer DROPS a hint that does not fit, so the host measures and the
 * NAME gives way -- widest name, longest CC, and it still draws. */
{
  const fits = (a) => hintPairWidth("Learn", a) <= 126;
  let armedCb = null;
  const doc2 = { cc: [{ channel: 15, cc: 127, mode: "abs", target: Object.assign({}, CUT, { label: "WWWWWWWWWWWWWWWWWWWW" }) }] };
  const m2 = createCCMap({ controls: () => doc2, targets: host.targets, now: () => t,
    learn: { arm(o, cb) { armedCb = cb; }, cancel() {} } });
  m2.beginLearn();
  armedCb(doc2.cc[0].target);
  const txt = m2.learnFooter(fits);
  const stub = { fillRect() {}, print() {}, setPixel() {}, textWidth: (x) => x.length * 6 };
  eq("the longest footer is still DRAWN", drawFooter(stub, [["Learn", txt.slice(7)]]), 1);
  eq("...the name gave way, the CC did not", [/Ch16 CC127$/.test(txt), txt.length < 40], [true, true]);
  eq("...and unmeasured it is whole", m2.learnFooter().includes("WWWWWWWWWWWWWWWWWWWW"), true);
}
cc.cancelLearn();
eq("Stop ends it: flag off, no footer", [shimLearn, cc.learnFooter()], [false, null]);
setSlotParam(0, "synth:cutoff", "0.2");
cc.feed(0xB0, 30, 64);
eq("...and nothing binds after", saved().cc.length, 2);
cc.beginLearn();
t += 119000; cc.tick(t);
eq("learn mode survives a long pause", !!cc.learning, true);
t += 2000; cc.tick(t);
eq("...and ends after two idle minutes", cc.learning, null);

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
setSlotParam(0, "synth:cutoff", "0.3");
cc.feed(0xB0, 5, 1, true);
eq("...and is never learned", cc.claimPairs().some((p) => p[1] === 5), false);
cc.cancelLearn();
eq("claimPairs names the bound CCs", cc.claimPairs(), [[0, 74], [1, 20]]);
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
