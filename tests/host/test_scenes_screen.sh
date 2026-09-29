#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# src/shared/scenes_screen.mjs: 32 SNAPSHOTS on the pads (A1-16 top, B1-16
# bottom) and 16 SCENES on the steps, each a pairing of one A and one B.
# Driven through a fake io, and drawn into the real 128x64 framebuffer -- PNGs
# in build/tests/scenes_*.png, to LOOK at.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi
mkdir -p build/tests
node --input-type=module -e '
import fs from "node:fs";
import { createFramebuffer } from "./tools/param-pages/harness.mjs";
import * as S from "./src/shared/scenes_screen.mjs";
import * as D from "./src/shared/scene_doc.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const eq = (what, got, want) => { const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) fail(what + ": got " + a + ", want " + b); };

/* ---- a fake device ---- */
let t = 1000;
const state = { edit: -1, xfade: 0, rev: 0, flash: 0 };
const scene = { active: -1, pairs: D.defaultPairs() };
let bank = "", applied = [], leds = new Map(), sends = 0;
const counts = new Array(32).fill(0); counts[D.halfB(2)] = 3; counts[D.halfA(5)] = 1;
const io = {
  now: () => t,
  state: () => ({ ...state }),
  scene: () => scene,
  setActive: (k) => { scene.active = k; },
  setPair: (k, p) => { scene.pairs[k] = [p[0], p[1]]; },
  setXfade: (x) => { state.xfade = x; },
  setEdit: (n) => { state.edit = n; },
  applyAll: (verb, val) => { applied.push(verb + " " + val); bank += verb; state.rev++; return true; },
  snapshot: () => ({ bank, pairs: scene.pairs.map(p => p.slice()) }),
  restore: (s) => { bank = s.bank; scene.pairs = s.pairs.map(p => p.slice()); state.rev++; return true; },
  lockCounts: () => counts.slice(),
  setLed: (n, c) => { leds.set(n, c); sends++; return true; },
  announce: () => {},
};
const scr = S.createScenesScreen(io);
scr.enter();
const cc = (d1, d2, shift = false) => scr.onMidi(0xB0, d1, d2, shift);
const step = (k) => { scr.onMidi(0x90, 16 + k, 100, false); scr.onMidi(0x80, 16 + k, 0, false); };
const pad = (note, on) => scr.onMidi(on ? 0x90 : 0x80, note, on ? 100 : 0, false);
const tapPad = (note) => { pad(note, 1); t += 100; pad(note, 0); };
const holdPad = (note) => { pad(note, 1); t += 501; scr.tick(); pad(note, 0); };
const A = (i) => S.snapPad("a", i), B = (i) => S.snapPad("b", i);

eq("pad map: A1 top-left, A16 end of row 2, B1 row 3, B16 bottom-right",
   [S.padSnap(92), S.padSnap(91), S.padSnap(76), S.padSnap(75), S.padSnap(20)],
   [{ side: "a", i: 0 }, { side: "a", i: 15 }, { side: "b", i: 0 }, { side: "b", i: 15 }, null]);
for (const side of ["a", "b"]) for (let i = 0; i < 16; i++) {
  const r = S.padSnap(S.snapPad(side, i)); if (!r || r.side !== side || r.i !== i) fail("round trip " + side + i);
}

/* pairing */
step(2);
eq("a step picks the active scene", scene.active, 2);
eq("scene 3 starts as A3 + B3", scene.pairs[2], [2, 2]);
tapPad(A(8));
eq("tap A9: scene 3 is A9 + B3", scene.pairs[2], [8, 2]);
tapPad(A(8));
eq("tap the lit A again: A none (the knobs)", scene.pairs[2], [-1, 2]);
tapPad(B(4));
eq("tap B5: just B5", scene.pairs[2], [-1, 4]);
step(1);
tapPad(A(0));
eq("scene 2 can use A1 too -- snapshots are shared", [scene.pairs[1][0], scene.pairs[0][0]], [0, 0]);

/* editing */
holdPad(B(4));
eq("hold B5: editing B5 (half 20), latched past the release", state.edit, D.halfB(4));
eq("...and holding does not re-pair", scene.pairs[1], [0, 1]);
tapPad(B(4));
eq("tap the snapshot being edited: done", state.edit, -1);
scene.active = 3; scene.pairs[3] = [-1, -1];
scr.toggleEdit("a");
eq("Shift+- on a scene with no A: pairs A4 (its own number) and edits it", [scene.pairs[3], state.edit], [[3, -1], D.halfA(3)]);
scr.toggleEdit("a");
eq("again: done", state.edit, -1);
scr.toggleEdit("b");
eq("Shift++: B4 paired and edited", [scene.pairs[3], state.edit], [[3, 3], D.halfB(3)]);
scr.toggleEdit("b");

/* fader */
cc(14, 32); cc(14, 32);
eq("jog moves the fader", state.xfade, 1);
cc(3, 127);
eq("click snaps to the nearer end", state.xfade, 1);
cc(78, 64 + 64 - 16);
eq("knob 8 is the fader too", state.xfade, 0.75);

/* copy / clear / undo */
cc(60, 127); step(2); step(9); cc(60, 0);
eq("Copy + 2 steps copies the PAIRING", scene.pairs[9], scene.pairs[2]);
cc(60, 127); pad(A(5), 1); pad(A(5), 0); pad(B(7), 1); pad(B(7), 0); cc(60, 0);
eq("Copy + 2 pads copies a SNAPSHOT (A6 -> B8)", applied.pop(), "copy " + D.halfA(5) + " " + D.halfB(7));
cc(119, 127); step(9); cc(119, 0);
eq("Delete + step empties the pairing", scene.pairs[9], [-1, -1]);
cc(119, 127); pad(A(5), 1); pad(A(5), 0); cc(119, 0);
eq("Delete + pad clears the snapshot", applied.pop(), "clear " + D.halfA(5));
cc(56, 127);
eq("Undo brings the snapshot back", bank.endsWith("clear"), false);
cc(56, 127);

/* LEDs */
{
  scene.active = 2; scene.pairs[2] = [8, 2]; state.edit = -1;
  leds.clear(); scr.paintLeds(true);
  eq("the active scene step is yellow", leds.get(16 + 2), 7);
  eq("a paired scene is lit", leds.get(16 + 5), S.COLORS.stepPaired);
  eq("an empty pairing is dark", leds.get(16 + 9), S.COLORS.stepEmpty);
  eq("the scene A (A9) is bright", leds.get(A(8)), S.COLORS.a.inScene);
  eq("the scene B (B3) is bright", leds.get(B(2)), S.COLORS.b.inScene);
  eq("another A with locks: dim", leds.get(A(5)), S.COLORS.a.locked);
  eq("an empty snapshot: darkest", leds.get(B(7)), S.COLORS.b.empty);
  const n0 = sends; scr.paintLeds(); eq("nothing changed, nothing sent", sends, n0);
  state.edit = D.halfA(8); scr.paintLeds();
  eq("the snapshot being edited is white", leds.get(A(8)), 120);
  eq("...and only that pad was sent", sends - n0, 1);
  state.edit = -1;
}
eq("badge", [S.armBadgeText(D.halfB(11), 0), S.armBadgeText(D.halfA(2), 0), S.armBadgeText(-1, 0)], ["EDIT B12", "EDIT A3", ""]);

/* ---- pictures ---- */
async function render(name, st, sc) {
  Object.assign(state, st); Object.assign(scene, sc);
  const fb = createFramebuffer();
  globalThis.fill_rect = fb.fillRect; globalThis.print = fb.print;
  globalThis.text_width = fb.textWidth; globalThis.set_pixel = (x, y, c) => fb.fillRect(x, y, 1, 1, c);
  const ML = await import("./src/shared/menu_layout.mjs");
  const s2 = S.createScenesScreen(io);
  s2.enter();
  s2.draw({ fillRect: fb.fillRect, print: fb.print, textWidth: fb.textWidth,
            drawHeader: ML.drawMenuHeader, drawFooter: ML.drawMenuFooter });
  fs.writeFileSync("build/tests/scenes_" + name + ".png", fb.toPng(4));
  /* The edit hint sits right of the pad map and must not touch it (grid ends x=79). */
  const rows = fb.toAscii().split("\n");
  for (let y = 12; y < 42; y++) {
    const x = rows[y].slice(80, 82).indexOf("#");
    if (x >= 0) { fail(name + ": the hint touches the pad map at " + (80 + x) + "," + y); break; }
  }
  if (fb.clipped()) fail(name + ": " + fb.clipped() + " pixels drawn off the panel");
  if (fb.missingGlyphs.size) fail(name + ": missing glyphs " + [...fb.missingGlyphs].join(""));
}
const p2 = D.defaultPairs(); p2[2] = [8, 2];
await render("idle", { edit: -1, xfade: 0.4 }, { active: 2, pairs: p2 });
await render("editing", { edit: D.halfB(2), xfade: 0.4 }, { active: 2, pairs: p2 });
const p3 = D.defaultPairs(); p3[4] = [-1, 11];
await render("justb", { edit: -1, xfade: 0.9 }, { active: 4, pairs: p3 });

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: scenes screen (PNGs in build/tests/scenes_*.png)");
'
