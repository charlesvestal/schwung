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


/* ---------------------------------------------------------------------------
 * THE FIELD SHAPE: four slots, each one bare synth, no FX at all.
 *
 * Reported from hardware 2026-09-10 as "9w9 listed in slot 9", which would be
 * a component sitting in the slot row -- the one thing the top/bottom split
 * exists to prevent. It was not: the frame was truncated by a carry overflow
 * and the map was correct. Pinned anyway, against the ACTUAL device chain
 * rather than a synthetic one, because the claim was specific and the next
 * person to read that report deserves the answer rather than the anecdote.
 *
 * The invariant is the load-bearing half: cells 0-3 are ALWAYS slots and a
 * component is ALWAYS at 4 or beyond, whatever the chain holds.
 * ------------------------------------------------------------------------- */
{
  const LIVE = { slots: [
    { synth: "9w9" }, { synth: "hank" }, { synth: "hank" }, { synth: "bouba-kiki" },
  ]};
  for (let sel = 0; sel < 4; sel++) {
    const m = buildMap(LIVE, { slot: sel });
    for (let i = 0; i < 4; i++) {
      eq("live slot " + sel + ": cell " + i + " is a slot",
         m.cells[i] && m.cells[i].kind, "slot");
    }
    eq("live slot " + sel + ": synth is at cell 4 (encoder 5), never the slot row",
       m.cells[4] && m.cells[4].component, "synth");
    eq("live slot " + sel + ": and it names the module",
       m.cells[4] && m.cells[4].label, LIVE.slots[sel].synth);
    for (let i = 5; i < 16; i++) {
      eq("live slot " + sel + ": cell " + i + " is empty", m.cells[i], null);
    }
    eq("live slot " + sel + ": the selected slot is the current one",
       m.cells[sel].current, true);
  }
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
