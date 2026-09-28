#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# src/shared/scenes_screen.mjs: the Scenes screen's gestures, driven through a
# fake io, and its picture in the real 128x64 framebuffer -- written as PNGs to
# build/tests/scenes_*.png so a person can LOOK at them, which is what finds
# the defects a pixel count cannot.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi
mkdir -p build/tests
node --input-type=module -e '
import fs from "node:fs";
import { createFramebuffer } from "./tools/param-pages/harness.mjs";
import * as S from "./src/shared/scenes_screen.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const eq = (what, got, want) => { const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) fail(what + ": got " + a + ", want " + b); };

/* ---- a fake device ---- */
let t = 1000;
const state = { a: -1, b: -1, edit: -1, xfade: 0, rev: 0, flash: 0 };
let bank = "", said = [], applied = [];
const io = {
  now: () => t,
  state: () => ({ ...state }),
  setAB: (a, b) => { state.a = a; state.b = b; },
  setXfade: (x) => { state.xfade = x; },
  setEdit: (n) => { state.edit = n; },
  applyAll: (verb, val) => { applied.push(verb + " " + val); bank = bank + verb; state.rev++; return true; },
  snapshot: () => ({ bank }),
  restore: (s) => { bank = s.bank; state.rev++; return true; },
  lockCounts: () => [3,0,5,0,0,0,0,0,0,0,0,0,0,0,0,1],
  announce: (s) => said.push(s),
};
const scr = S.createScenesScreen(io);
scr.enter();
const cc = (d1, d2, shift = false) => scr.onMidi(0xB0, d1, d2, shift);
const step = (n, on) => scr.onMidi(on ? 0x90 : 0x80, 16 + n, on ? 100 : 0, false);

/* the pad map: A on the top two rows, B on the bottom two, top-left = 1 */
eq("top-left pad is A1", S.padScene(92), { end: "a", n: 0 });
eq("second row, last pad is A16", S.padScene(91), { end: "a", n: 15 });
eq("third row first pad is B1", S.padScene(76), { end: "b", n: 0 });
eq("bottom-right pad is B16", S.padScene(75), { end: "b", n: 15 });
eq("a step is not a pad", S.padScene(16), null);
for (const end of ["a", "b"]) for (let n = 0; n < 16; n++) {
  const p = S.padScene(S.scenePad(end, n));
  if (!p || p.end !== end || p.n !== n) fail("scenePad/padScene round trip " + end + n);
}
const pad = (note, on) => scr.onMidi(on ? 0x90 : 0x80, note, on ? 100 : 0, false);
const tapPad = (note) => { pad(note, 1); t += 100; pad(note, 0); };

tapPad(S.scenePad("a", 2));
eq("a top-row pad sets A", [state.a, state.b], [2, -1]);
tapPad(S.scenePad("b", 9));
eq("a bottom-row pad sets B", [state.a, state.b], [2, 9]);
tapPad(S.scenePad("a", 2));
eq("the selected A pad again clears A", state.a, -1);
tapPad(S.scenePad("a", 0));
cc(14, 32); cc(14, 32);  /* +1.0 */
eq("jog moves the fader 1/64 per detent", state.xfade, 1);
cc(14, 127, true);
eq("shift+jog is fine", Math.round(state.xfade * 256), 255);
cc(3, 127);
eq("click snaps to the nearer end", state.xfade, 1);
cc(78, 64 + 64 - 16);    /* -16 detents on knob 8 */
eq("knob 8 is the fader too", state.xfade, 0.75);
scr.onMidi(0xB0, 71, 1, false);
eq("knob 1 no longer picks scenes", [state.a, state.b], [0, 9]);
eq("steps are not ours", scr.onMidi(0x90, 16, 100, false), false);

/* hold arms, tap on the armed pad disarms */
pad(S.scenePad("b", 5), 1); t += 499; scr.tick();
eq("not armed before 500 ms", state.edit, -1);
t += 2; scr.tick();
eq("armed at 500 ms, while still held", state.edit, 5);
pad(S.scenePad("b", 5), 0);
eq("the release after a hold is not also a tap", [state.a, state.b], [0, 9]);
tapPad(S.scenePad("a", 5));
eq("a tap on the armed scene (either row) disarms", state.edit, -1);

/* copy, clear, undo */
cc(60, 127); pad(S.scenePad("a", 0), 1); pad(S.scenePad("a", 0), 0);
pad(S.scenePad("b", 3), 1); pad(S.scenePad("b", 3), 0); cc(60, 0);
eq("copy source then destination, across rows", applied.pop(), "copy 0 3");
cc(119, 127); pad(S.scenePad("a", 3), 1); pad(S.scenePad("a", 3), 0); cc(119, 0);
eq("delete + pad clears", applied.pop(), "clear 3");
eq("delete + pad did not also arm or assign", [state.edit, state.a, state.b], [-1, 0, 9]);
const before = bank;
cc(56, 127);
eq("undo restores the bank as it was", bank, "copy");
cc(56, 127);
eq("undo again is redo", bank, before);

/* LEDs: every pad painted once, then only what changed */
{
  const leds = new Map(); let sends = 0;
  const io2 = { ...io, setPadLed: (n, c) => { leds.set(n, c); sends++; return true; } };
  Object.assign(state, { a: 0, b: 15, edit: -1 });
  const s3 = S.createScenesScreen(io2);
  s3.enter(); s3.paintPads(true);
  eq("all 32 pads painted", leds.size, 32);
  eq("selected A is bright", leds.get(S.scenePad("a", 0)), S.PAD_COLORS.a.selected);
  eq("selected B is bright", leds.get(S.scenePad("b", 15)), S.PAD_COLORS.b.selected);
  eq("a scene with locks is dim", leds.get(S.scenePad("a", 2)), S.PAD_COLORS.a.locked);
  eq("an empty scene is darkest", leds.get(S.scenePad("b", 1)), S.PAD_COLORS.b.empty);
  const n0 = sends; s3.paintPads(); eq("nothing changed, nothing sent", sends, n0);
  state.edit = 2; s3.paintPads();
  eq("the scene being edited is white on both rows", [leds.get(S.scenePad("a", 2)), leds.get(S.scenePad("b", 2))], [120, 120]);
  eq("...and only those two pads were sent", sends - n0, 2);
  state.edit = -1;
}

/* ---- pictures ---- */
function render(name, st, extra) {
  Object.assign(state, st);
  const fb = createFramebuffer();
  globalThis.fill_rect = fb.fillRect; globalThis.print = fb.print;
  globalThis.text_width = fb.textWidth; globalThis.set_pixel = (x, y, c) => fb.fillRect(x, y, 1, 1, c);
  return import("./src/shared/menu_layout.mjs").then((ML) => {
    const s2 = S.createScenesScreen(io);
    s2.enter();
    if (extra) extra(s2);
    s2.draw({ fillRect: fb.fillRect, print: fb.print, textWidth: fb.textWidth,
              drawHeader: ML.drawMenuHeader, drawFooter: ML.drawMenuFooter });
    fs.writeFileSync("build/tests/scenes_" + name + ".png", fb.toPng(4));
    if (fb.clipped()) fail(name + ": " + fb.clipped() + " pixels drawn off the panel");
    if (fb.missingGlyphs.size) fail(name + ": missing glyphs " + [...fb.missingGlyphs].join(""));
    return fb;
  });
}
await render("idle", { a: 0, b: 2, edit: -1, xfade: 0.4 });
await render("armed", { a: 0, b: 15, edit: 2, xfade: 0.0 });
await render("none", { a: -1, b: -1, edit: -1, xfade: 1 });
await render("copy", { a: 2, b: 2, edit: -1, xfade: 0.5 }, (s) => s.onMidi(0xB0, 60, 127, false));

/* badge */
{
  const fb = createFramebuffer();
  fb.fillRect(0, 0, 128, 64, 1);     /* a busy screen underneath */
  const r = S.drawArmBadge(fb, S.armBadgeText(11, 0));
  eq("badge text", S.armBadgeText(11, 0), "SCN 12");
  eq("flash replaces the text", [S.armBadgeText(3, 1), S.armBadgeText(3, 2), S.armBadgeText(-1, 0)], ["FULL", "N/A", ""]);
  if (!r || r.x + r.w > 128) fail("badge off panel");
  fs.writeFileSync("build/tests/scenes_badge.png", fb.toPng(4));
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: scenes screen (PNGs in build/tests/scenes_*.png)");
'
