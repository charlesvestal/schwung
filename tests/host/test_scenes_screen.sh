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

/* far-end assignment */
step(2, 1); t += 100; step(2, 0);
eq("tap with fader at A sets B", [state.a, state.b], [-1, 2]);
cc(14, 32); cc(14, 32);  /* +1.0 */
eq("jog moves the fader 1/64 per detent", state.xfade, 1);
step(0, 1); t += 100; step(0, 0);
eq("tap with fader at B sets A", [state.a, state.b], [0, 2]);
cc(14, 127, true);
eq("shift+jog is fine", Math.round(state.xfade * 256), 255);
cc(3, 127);
eq("click snaps to the nearer end", state.xfade, 1);
cc(72, 127);
eq("knob 2 steps B down", state.b, 1);
cc(71, 127);
eq("knob 1 steps A down to none", state.a, -1);
cc(71, 1); cc(71, 1);
eq("knob 1 steps A back up", state.a, 1);
cc(78, 64 + 64 - 16);    /* -16 detents on knob 8 */
eq("knob 8 is the fader too", state.xfade, 0.75);

/* hold arms, tap on the armed step disarms */
step(5, 1); t += 499; scr.tick();
eq("not armed before 500 ms", state.edit, -1);
t += 2; scr.tick();
eq("armed at 500 ms, while still held", state.edit, 5);
step(5, 0);
eq("the release after a hold is not also a tap", [state.a, state.b], [1, 1]);
step(5, 1); t += 50; step(5, 0);
eq("a tap on the armed step disarms", state.edit, -1);

/* copy, clear, undo */
cc(60, 127); step(0, 1); step(0, 0); step(3, 1); step(3, 0); cc(60, 0);
eq("copy source then destination", applied.pop(), "copy 0 3");
cc(119, 127); step(3, 1); step(3, 0); cc(119, 0);
eq("delete + step clears", applied.pop(), "clear 3");
eq("delete + step did not also arm or assign", [state.edit, state.a, state.b], [-1, 1, 1]);
const before = bank;
cc(56, 127);
eq("undo restores the bank as it was", bank, "copy");
cc(56, 127);
eq("undo again is redo", bank, before);

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
