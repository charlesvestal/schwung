#!/usr/bin/env bash
# The control foundation ASSEMBLED: control_host.mjs (document, target io,
# learn broker) wired to a real E16 surface on the Custom layout, over a fake
# file system and param channel -- the path shadow_ui.js wires, run end to end
# because shadow_ui.js itself cannot be imported here.
#
#   - a page added on the surface is SAVED to the set directory
#   - hold a knob, move a parameter on Move: the knob is assigned, and saved
#   - the surface OWN writes are never learned (a turn while armed elsewhere)
#   - a key the module does not declare is never learned (a module swap)
#   - a knob whose module was swapped out goes DARK and is not written
#   - another writer (the web editor) changing the file is picked up
#   - an unreadable file is left untouched: edits are refused, not saved over
#   - a set change loads that set document
#   - set duplication copies controls.json (source pin)
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { createControlHost, RECONCILE_MS } from "./src/shared/control_host.mjs";
import { createSurface } from "./src/shared/e16_surface.mjs";
import { LEARN_HOLD_MS, PAGEMAP_SHOW_MS } from "./src/shared/layout_custom.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };
const ok = (n, c) => eq(n, !!c, true);

/* ---- a fake device ---- */
const files = {};
const fs = { exists: (p) => p in files, read: (p) => files[p], write: (p, t) => { files[p] = t; }, ensureDir: () => {} };
let dir = "/sets/A";
let t = 1000;
const chain = { slots: [{ synth: "obxd", fx: [], midiFx: [] }, {}, {}, {}] };
const CP = JSON.stringify([{ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1 },
                           { key: "reso", name: "Reso", type: "float", min: 0, max: 1 }]);
const params = { "0|synth:chain_params": CP, "0|synth:cutoff": "0.5", "0|synth:reso": "0.2" };
const writes = [];
let host = null;
/* setSlotParam as shadow_ui.js has it: write, then offer the write to learn. */
const setSlotParam = (slot, key, v) => {
  params[slot + "|" + key] = String(v); writes.push([slot, key, String(v)]);
  host.observeWrite(slot, key, v); return true;
};
host = createControlHost({ fs, stateDir: () => dir, now: () => t,
  getParam: (s, k) => (params[s + "|" + k] === undefined ? null : params[s + "|" + k]),
  setParam: setSlotParam, chainShape: () => chain, masterFx: () => ({}) });

const surface = createSurface({ now: () => t, send: () => true, chainOf: () => chain,
  navigationOf: () => "custom",
  controls: () => host.controls(), editControls: (fn) => host.edit(fn), targets: host.targets, learn: host.learn });
surface.setEnabled(true);
const ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7];
const run = (ms) => { for (let i = 0; i < ms / 16; i++) { t += 16; if (i % 60 === 0) surface.feedMidi(ACK); host.reconcile(); surface.tick(); } };
const shift = (d) => surface.feedMidi(d ? [0x90, 16, 0x7F] : [0x80, 16, 0]);
const push = (e, d) => surface.feedMidi(d ? [0x90, e, 0x7F] : [0x80, e, 0]);
const turn = (e, n) => surface.feedMidi([0xB0, e + 1, n > 0 ? 1 : 0x7F]);
const saved = () => JSON.parse(files["/sets/A/controls.json"] || "null");
run(100);

eq("the Custom layout is live, and says there are no pages", surface.layout.screen(t).kind, "message");
shift(true); run(PAGEMAP_SHOW_MS + 50); push(0, true); push(0, false); shift(false); run(50);
eq("a page added on the surface is SAVED to the set", saved() && saved().surface.pages.length, 1);

push(0, true); run(LEARN_HOLD_MS + 50); push(0, false);
ok("holding a knob arms learn", host.learn.armed);
setSlotParam(0, "synth:cutoff", "0.6");
eq("moving a parameter on Move assigns the armed knob, and saves it",
   saved().surface.pages[0].knobs[0] && [saved().surface.pages[0].knobs[0].key, saved().surface.pages[0].knobs[0].label], ["cutoff", "Cutoff"]);

run(100);
push(1, true); run(LEARN_HOLD_MS + 50); push(1, false);
t += 1000;
turn(0, 1);
ok("the surface OWN write is never learned: knob 2 is still waiting", host.learn.armed && !saved().surface.pages[0].knobs[1]);
setSlotParam(0, "synth:module", "dx7");
ok("a module swap is never learned", host.learn.armed && !saved().surface.pages[0].knobs[1]);
/* last_note is a real served key (a diagnostic), shaped like a parameter,
 * but the module does not DECLARE it -- only the declared check stops it. */
setSlotParam(0, "synth:last_note", "60");
ok("a key the module does not declare is never learned", host.learn.armed && !saved().surface.pages[0].knobs[1]);
setSlotParam(0, "synth:reso", "0.3");
eq("...and the next real parameter is", saved().surface.pages[0].knobs[1].key, "reso");

writes.length = 0;
t += 1000; turn(0, 1);
ok("a live knob writes its target", writes.some((w) => w[1] === "synth:cutoff"));
chain.slots[0].synth = "dx7";
writes.length = 0;
t += 1000; turn(0, 3);
eq("its module swapped out, the knob is DARK and not written", [surface.layout.screen(t).view.cells[0].status, writes.length], ["dark", 0]);
chain.slots[0].synth = "obxd";

/* another writer: the web editor renames the page */
const doc = saved(); doc.surface.pages[0].name = "Web"; files["/sets/A/controls.json"] = JSON.stringify(doc);
run(RECONCILE_MS + 100);
eq("an edit by another writer is picked up", host.controls().surface.pages[0].name, "Web");

/* an unreadable file */
files["/sets/A/controls.json"] = "{ broken";
run(RECONCILE_MS + 100);
shift(true); run(PAGEMAP_SHOW_MS + 50); push(3, true); push(3, false); shift(false);
eq("an unreadable file is left untouched: the edit is refused, not saved over it",
   [files["/sets/A/controls.json"], host.broken], ["{ broken", true]);

/* a set change */
files["/sets/B/controls.json"] = JSON.stringify({ version: 1, surface: { pages: [{ name: "B1", knobs: [] }, { name: "B2", knobs: [] }] } });
dir = "/sets/B";
run(100);
eq("a set change loads THAT set document", host.controls().surface.pages.map((p) => p.name), ["B1", "B2"]);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'

# The copy list on set DUPLICATION is by name: a file not named is lost.
UI=src/shadow/shadow_ui.js
perl -0ne 'exit(!/host_read_file\(copySourceDir \+ "\/controls\.json"\).*?host_write_file\(newDir \+ "\/controls\.json"/s)' "$UI" \
  || { echo "FAIL: set duplication does not copy controls.json"; exit 1; }
# The surface OWN writes go through the muted path, never straight to setSlotParam.
[ "$(grep -c 'setParam: (key, value) => surfaceSetParam(focus.slot, key, value),' "$UI")" = 2 ] \
  || { echo "FAIL: a surface page controller writes around the learn mute"; exit 1; }
grep -q 'try { controlHost.observeWrite(slot, key, value); } catch (e) {}' "$UI" \
  || { echo "FAIL: setSlotParam does not offer writes to the learn broker"; exit 1; }
echo "PASS: shadow_ui.js copies controls.json, mutes surface writes, and feeds learn"
