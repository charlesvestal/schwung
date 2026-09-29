#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# src/shared/scenes_screen.mjs: a scene is an A/B PAIR. Steps pick the scene,
# the top half of the pads is its A and the bottom half its B (tap on/off, hold
# to edit, latched). Driven through a fake io, and drawn into the real 128x64
# framebuffer -- PNGs in build/tests/scenes_*.png, to LOOK at.

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
const scene = { active: -1, enables: D.defaultEnables() };
let bank = "", applied = [], leds = new Map(), sends = 0;
const counts = new Array(32).fill(0); counts[D.halfB(2)] = 3; counts[D.halfA(5)] = 1;
const io = {
  now: () => t,
  state: () => ({ ...state }),
  scene: () => scene,
  setActive: (k) => { scene.active = k; },
  setEnable: (k, side, on) => { scene.enables[k][side === "a" ? 0 : 1] = on; },
  copyEnables: (s, d) => { scene.enables[d] = scene.enables[s].slice(); },
  setXfade: (x) => { state.xfade = x; },
  setEdit: (n) => { state.edit = n; },
  applyAll: (verb, val) => { applied.push(verb + " " + val); bank += verb; state.rev++; return true; },
  snapshot: () => ({ bank }),
  restore: (s) => { bank = s.bank; state.rev++; return true; },
  lockCounts: () => counts.slice(),
  setLed: (n, c) => { leds.set(n, c); sends++; return true; },
  announce: () => {},
};
const scr = S.createScenesScreen(io);
scr.enter();
const cc = (d1, d2, shift = false) => scr.onMidi(0xB0, d1, d2, shift);
const step = (k) => { scr.onMidi(0x90, 16 + k, 100, false); scr.onMidi(0x80, 16 + k, 0, false); };
const padA = 92, padB = 70;
const pad = (note, on) => scr.onMidi(on ? 0x90 : 0x80, note, on ? 100 : 0, false);
const tapPad = (note) => { pad(note, 1); t += 100; pad(note, 0); };
const holdPad = (note) => { pad(note, 1); t += 501; scr.tick(); pad(note, 0); };

eq("pad halves", [S.padSide(99), S.padSide(84), S.padSide(83), S.padSide(68), S.padSide(20)], ["a", "a", "b", "b", null]);

tapPad(padA);
eq("a pad with no scene picked does nothing", scene.active, -1);
step(2);
eq("a step picks the active scene", scene.active, 2);
tapPad(padA);
eq("tap A: A off", scene.enables[2], [false, true]);
tapPad(padA);
eq("tap A again: A on", scene.enables[2], [true, true]);

/* editing */
holdPad(padB);
eq("hold B: editing scene 3 B (half 5), latched past the release", state.edit, D.halfB(2));
tapPad(padA);
eq("a tap on the OTHER side while editing toggles it, and editing stays on B", [scene.enables[2][0], state.edit], [false, 5]);
tapPad(padA);
tapPad(padB);
eq("a tap on the side being edited stops editing (and does not switch it off)", [state.edit, scene.enables[2][1]], [-1, true]);
scene.enables[2][0] = false;
holdPad(padA);
eq("hold A while A is off: switches it ON and edits it", [scene.enables[2][0], state.edit], [true, 4]);
step(5);
eq("picking another scene stops editing", [scene.active, state.edit], [5, -1]);

/* the global shortcut */
scr.toggleEdit("b");
eq("Shift+Vol+Down: edit the active scene B", state.edit, D.halfB(5));
scr.toggleEdit("b");
eq("again: stop", state.edit, -1);
scene.active = -1;
scr.toggleEdit("a");
eq("with no scene active: scene 1 becomes active and its A is edited", [scene.active, state.edit], [0, 0]);
scr.toggleEdit("a");

/* fader */
cc(14, 32); cc(14, 32);
eq("jog moves the fader", state.xfade, 1);
cc(3, 127);
eq("click snaps to the nearer end", state.xfade, 1);
cc(78, 64 + 64 - 16);
eq("knob 8 is the fader too", state.xfade, 0.75);

/* copy / clear / undo */
cc(60, 127); step(5); step(9); cc(60, 0);
eq("copy scene: both halves", applied.splice(-2), ["copy 10 18", "copy 11 19"]);
cc(119, 127); step(9); cc(119, 0);
eq("delete + step clears both halves", applied.splice(-2), ["clear 18", "clear 19"]);
scene.active = 9;
cc(119, 127); pad(padB, 1); pad(padB, 0); cc(119, 0);
eq("delete + pad clears that side only", applied.pop(), "clear 19");
const before = bank;
cc(56, 127); cc(56, 127);
eq("undo twice is redo", bank, before);

/* LEDs */
{
  scene.active = 2; scene.enables[2] = [true, false]; state.edit = -1;
  leds.clear(); scr.paintLeds(true);
  eq("the active scene step is white", leds.get(16 + 2), S.COLORS.stepActive);
  eq("a scene with locks is lit", leds.get(16 + 5), S.COLORS.stepLocked);
  eq("an empty scene is dark", leds.get(16 + 7), S.COLORS.stepEmpty);
  eq("A on with no locks: dim", leds.get(padA), S.COLORS.a.onEmpty);
  eq("B off: darkest", leds.get(padB), S.COLORS.b.off);
  const n0 = sends; scr.paintLeds(); eq("nothing changed, nothing sent", sends, n0);
  state.edit = D.halfA(2); scr.paintLeds();
  eq("the side being edited is white on all 16 of its pads", S.PADS_A.every(n => leds.get(n) === 120), true);
  eq("...and only those were sent", sends - n0, 16);
  state.edit = -1;
}
eq("badge", [S.editLabel(D.halfB(2)), S.editLabel(-1)], ["S3 B", ""]);

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
  if (fb.clipped()) fail(name + ": " + fb.clipped() + " pixels drawn off the panel");
  if (fb.missingGlyphs.size) fail(name + ": missing glyphs " + [...fb.missingGlyphs].join(""));
}
const en2 = D.defaultEnables(); en2[2] = [false, true];
await render("idle", { edit: -1, xfade: 0.4 }, { active: 2, enables: en2 });
await render("editing", { edit: D.halfB(2), xfade: 0.4 }, { active: 2, enables: en2 });
await render("none", { edit: -1, xfade: 0 }, { active: -1, enables: D.defaultEnables() });

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: scenes screen (PNGs in build/tests/scenes_*.png)");
'
