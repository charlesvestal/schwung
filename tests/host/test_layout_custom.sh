#!/usr/bin/env bash
# The Custom layout (layout_custom.mjs), driven through a fake target io, a
# fake learn broker and a real control document:
#
#   - no pages says so; a Shift hold shows the PAGE MAP, and a push on an
#     empty cell adds a page and goes there; a push on a page jumps to it
#   - a knob turns its target through the knob engine and writes what the
#     grid would write (an enum by the wire form the module uses)
#   - a quick push is the parameter own click (a two-option flips); a HOLD
#     arms learn, and the next write on Move lands on that knob
#   - while armed, turning that knob CLOCKWISE clears it; anticlockwise or a
#     push cancels -- never a clear on a single press
#   - a DARK knob (module gone) draws, never writes, never reads
#   - values are read ONE per tick, not a page per frame
#   - Shift + turn pages; a Shift tap is the Mixer
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { createCustomLayout, LEARN_HOLD_MS, PAGEMAP_SHOW_MS } from "./src/shared/layout_custom.mjs";
import { createKnobFeel } from "./src/shared/surface_core.mjs";
import { emptyControls, addPage, assignKnob } from "./src/shared/control_map.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { targetAddress } from "./src/shared/control_target.mjs";
import { screenLabels } from "./src/shared/layout_common.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };
const ok = (n, c) => eq(n, !!c, true);

/* ---- a fake set: obxd in slot 1 with a float and a two-option enum ---- */
const index = buildMetaIndex({ chainParams: [
  { key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "sync", name: "Sync", type: "enum", options: ["Off", "On"] },
] });
const store = { "0|synth:cutoff": "0.5", "0|synth:sync": "Off", "2|synth:cutoff": "0.1" };
let modules = { 0: "obxd", 2: "dx7" };
const reads = [], writes = [];
const targets = {
  status: (t) => (t.kind !== "param" || modules[t.slot] === t.module ? "live" : "dark"),
  metaOf: (t) => index.getOrGuess(t.key),
  read: (t) => { const a = targetAddress(t); reads.push(a.key); const v = store[a.slot + "|" + a.key]; return v === undefined ? null : v; },
  write: (t, v) => { const a = targetAddress(t); writes.push([a.slot, a.key, String(v)]); store[a.slot + "|" + a.key] = String(v); return true; },
};
let doc = emptyControls();
let learnCb = null, learnCancelled = 0;
const learn = { arm: (owner, cb) => { learnCb = cb; }, cancel: () => { learnCb = null; learnCancelled++; } };
let t = 1000;
const L = createCustomLayout({ now: () => t, feel: createKnobFeel(), targets, learn,
  controls: () => doc, edit: (fn) => { doc = fn(doc); return doc; } });
const ev = (e) => L.handle(e, t);
const tick = (n) => { for (let i = 0; i < (n || 1); i++) { t += 16; L.tick(t); } };

/* ---- pages ---- */
eq("no pages says so, never a blank page", L.screen(t).kind, "message");
ev({ type: "shift", down: true });
tick(Math.ceil(PAGEMAP_SHOW_MS / 16) + 1);
eq("a Shift hold shows the page map", L.screen(t).kind, "pagemap");
ev({ type: "push", enc: 0 });
eq("a push on an empty cell adds a page and goes to it", [doc.surface.pages.length, L.pageIndex], [1, 0]);
ev({ type: "push", enc: 1 });
eq("...and another", [doc.surface.pages.length, L.pageIndex], [2, 1]);
ev({ type: "push", enc: 0 });
eq("a push on a page jumps to it", L.pageIndex, 0);
ev({ type: "shift", down: false });
eq("letting go shows the page", L.screen(t).kind, "custom");
eq("the page map as names", screenLabels({ kind: "pagemap", names: ["Page 1", "Page 2", null], current: 0 }).labels.slice(0, 3),
   [">P1", "P2", "+"]);

/* ---- assign two knobs, and a knob on a module that is gone ---- */
doc = assignKnob(doc, 0, 0, { kind: "param", slot: 0, component: "synth", key: "cutoff", module: "obxd" });
doc = assignKnob(doc, 0, 1, { kind: "param", slot: 0, component: "synth", key: "sync", module: "obxd" });
doc = assignKnob(doc, 0, 2, { kind: "param", slot: 2, component: "synth", key: "cutoff", module: "obxd" });
L.reload();
reads.length = 0;
tick(1);
eq("the page is warmed on arrival: every LIVE knob read once, the dark one never",
   reads.slice().sort(), ["synth:cutoff", "synth:sync"]);
reads.length = 0;
tick(1);
eq("then ONE read per tick", reads.length, 1);
const cells = L.screen(t).view.cells;
eq("values arrive", [cells[0].value, cells[1].value], ["0.5", "Off"]);
eq("a knob whose module has gone is DARK", cells[2].status, "dark");

/* ---- turning ---- */
writes.length = 0;
ev({ type: "turn", enc: 0, ticks: 1 });
ok("a turn writes the target through the knob engine",
   writes.length === 1 && writes[0][1] === "synth:cutoff" && Number(writes[0][2]) > 0.5);
ok("...and leaves a reading", L.reading(t) && L.reading(t).name === "Cutoff");
writes.length = 0;
ev({ type: "turn", enc: 2, ticks: 3 });
eq("a DARK knob is never written", writes.length, 0);

/* ---- push: click vs learn vs clear ---- */
writes.length = 0;
ev({ type: "push", enc: 1 }); t += 100; ev({ type: "release", enc: 1 });
eq("a quick push flips a two-option value, in its own wire form", writes, [[0, "synth:sync", "On"]]);
ev({ type: "push", enc: 3 });
tick(Math.ceil(LEARN_HOLD_MS / 16) + 1);
ok("a HOLD arms learn on that knob", L.armed && L.armed.knob === 3 && learnCb);
writes.length = 0;
ev({ type: "release", enc: 3 });
eq("...and its release is not a click", writes.length, 0);
learnCb({ kind: "param", slot: 0, component: "synth", key: "cutoff", module: "obxd", label: "Cutoff" });
eq("the next write on Move lands on the armed knob", doc.surface.pages[0].knobs[3] && doc.surface.pages[0].knobs[3].key, "cutoff");
eq("...and learn is disarmed", L.armed, null);
ev({ type: "push", enc: 3 });
tick(Math.ceil(LEARN_HOLD_MS / 16) + 1);
ev({ type: "release", enc: 3 });
ev({ type: "push", enc: 3 }); ev({ type: "release", enc: 3 });
ok("while armed, a push on the same knob CANCELS (never clears)", !L.armed && doc.surface.pages[0].knobs[3]);
ev({ type: "push", enc: 3 });
tick(Math.ceil(LEARN_HOLD_MS / 16) + 1);
ev({ type: "release", enc: 3 });
ev({ type: "turn", enc: 3, ticks: -1 });
ok("...an ANTICLOCKWISE turn cancels", !L.armed && doc.surface.pages[0].knobs[3]);
ev({ type: "push", enc: 3 });
tick(Math.ceil(LEARN_HOLD_MS / 16) + 1);
ev({ type: "release", enc: 3 });
ev({ type: "turn", enc: 3, ticks: 1 });
eq("...and a CLOCKWISE turn clears it", [L.armed, doc.surface.pages[0].knobs[3]], [null, null]);
ev({ type: "push", enc: 4 });
tick(Math.ceil(LEARN_HOLD_MS / 16) + 1);
ev({ type: "release", enc: 4 });
const cancelsBefore = learnCancelled;
ev({ type: "shift", down: true }); ev({ type: "shift", down: false });
eq("Shift cancels an armed learn", [L.armed, learnCancelled > cancelsBefore], [null, true]);

/* ---- paging and the Mixer ---- */
t += 1000;
ev({ type: "shift", down: true });
ev({ type: "turn", enc: 5, ticks: 1 });
eq("Shift + turn pages", L.pageIndex, 1);
ev({ type: "shift", down: false });
eq("...and that release is not a tap", L.mixerOn, false);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
