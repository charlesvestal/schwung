#!/usr/bin/env bash
# The caps are 8 MIDI FX + 1 synth + 8 audio FX = 17 per slot, against 12
# cells -- so overflow is reachable in a normal rig and must not silently drop
# the tail. And the stability rule is behavioural, not cosmetic: recomputing
# per frame would move buttons under the user's fingers while they navigate.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { buildMap, shapeSignature } from "./src/shared/e16_map.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };

const chain = {
  slots: [
    { midiFx: ["arp"], synth: "braids", fx: ["freeverb", null, "tapescam"] },
    { midiFx: [], synth: null, fx: [] },
    { midiFx: [], synth: "dx7", fx: [] },
    { midiFx: [], synth: null, fx: [] },
  ],
};

const m = buildMap(chain, { slot: 0, page: 0 });
eq("four slot cells", m.cells.slice(0, 4).map(c => c.kind),
   ["slot","slot","slot","slot"]);
eq("current slot marked", m.cells[0].current, true);
eq("occupied only", m.cells.slice(4).filter(Boolean).map(c => c.label),
   ["arp","braids","freeverb","tapescam"]);
eq("single page", m.pageCount, 1);

const full = { slots: [{ midiFx: Array(8).fill("mfx"), synth: "s",
                         fx: Array(8).fill("fx") }, {}, {}, {}] };
const f = buildMap(full, { slot: 0, page: 0 });
eq("17 components paginate", f.pageCount, 2);
eq("page 0 holds twelve", f.cells.slice(4).filter(Boolean).length, 12);
const f2 = buildMap(full, { slot: 0, page: 1 });
eq("page 1 holds the rest", f2.cells.slice(4).filter(Boolean).length, 5);

eq("signature stable", shapeSignature(chain), shapeSignature(chain));
const changed = JSON.parse(JSON.stringify(chain));
changed.slots[0].fx[1] = "gate";
eq("signature moves on shape change",
   shapeSignature(changed) !== shapeSignature(chain), true);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
