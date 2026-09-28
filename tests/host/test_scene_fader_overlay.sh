#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The scene fader's A-B slider, and the footer panel it shares with the
# automation lock map (src/shared/footer_panel.mjs): when it rises, that it
# slides both ways, that the Scenes screen suppresses it, and what it looks
# like over a real footer -- build/tests/scene_fader_*.png.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi
mkdir -p build/tests
node --input-type=module -e '
import fs from "node:fs";
import { createFramebuffer } from "./tools/param-pages/harness.mjs";
import * as P from "./src/shared/footer_panel.mjs";
import * as O from "./src/shared/scene_fader_overlay.mjs";
import * as ML from "./src/shared/menu_layout.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (c, m) => { if (!c) fail(m); };

/* ---- the shared panel ---- */
ok(P.PANEL_TOP === 55 && P.PANEL_H === 9, "the panel is the rule and the footer: " + P.PANEL_TOP + "+" + P.PANEL_H);
const p = P.createFooterPanel();
ok(p.update(0, null) === null && !p.busy(), "nothing wanted, nothing shown");
const f0 = p.update(0, { v: 1 });
const f1 = p.update(40, { v: 2 });
const f2 = p.update(200, { v: 3 });
ok(f0.y === 64 && f1.y < 64 && f1.y > 55 && f2.y === 55, "it slides UP onto the rule: " + [f0.y, f1.y, f2.y]);
ok(f2.payload.v === 3, "the payload follows while open");
const g1 = p.update(240, null);
ok(g1 && g1.payload.v === 3 && g1.y >= 55, "on the way OUT it keeps its last payload");
const g2 = p.update(280, null);
ok(g2 && g2.y > g1.y, "and slides down: " + [g1.y, g2.y]);
const g3 = p.update(281, { v: 4 });
ok(g3 && Math.abs(g3.y - g2.y) <= 1, "re-opened mid-slide it continues from where it was, not the edge: " + [g2.y, g3.y]);
p.update(1000, null); p.update(2000, null);
ok(!p.busy(), "closed, it is gone");

/* ---- the fader overlay ---- */
let t = 0;
const o = O.createSceneFaderOverlay({ now: () => t });
const L = { a: "A3", b: "B12" };
o.observe(0.2, L, false);
ok(!o.busy() && o.frame() === null, "the first value is a baseline, not a move");
o.observe(0.2, L, false); t += 16;
ok(!o.busy(), "an unchanged fader raises nothing");
o.observe(0.3, L, false);
ok(o.busy(), "a move raises it");
ok(o.frame().y === 64, "... starting at the edge");
t += 500; o.observe(0.3, L, false);
const up = o.frame();
ok(up && up.y === 55 && up.payload.x === 0.3, "up and showing the value");
t += O.HOLD_MS + 10; o.observe(0.3, L, false);
const down1 = o.frame(); t += 60; const down2 = o.frame();
ok(down1 && down2 && down2.y > 55, "held long enough, it slides away");
t += 500; o.frame();
ok(!o.busy(), "and is gone");
o.observe(0.5, L, false); ok(o.busy(), "moved again");
o.observe(0.6, L, true);
ok(!o.busy() && o.frame() === null, "the Scenes screen draws its own fader: dropped at once");

/* ---- pictures, over a real footer ---- */
function render(name, x, label, y, offEdgeOk = false) {
  const fb = createFramebuffer();
  globalThis.fill_rect = fb.fillRect; globalThis.print = fb.print;
  globalThis.text_width = fb.textWidth; globalThis.set_pixel = (px, py, c) => fb.fillRect(px, py, 1, 1, c);
  ML.drawMenuHeader("Chain", "", false);
  ML.drawMenuFooter(["Jog: select", "Click: edit"]);
  const r = O.drawSceneFaderOverlay({ fillRect: fb.fillRect, print: fb.print, textWidth: fb.textWidth },
                                    { y, payload: { x, label } });
  fs.writeFileSync("build/tests/scene_fader_" + name + ".png", fb.toPng(4));
  if (fb.clipped() && !offEdgeOk) fail(name + ": " + fb.clipped() + " pixels off the panel");
  if (fb.missingGlyphs.size) fail(name + ": missing glyphs " + [...fb.missingGlyphs].join(""));
  return r;
}
const r = render("mid", 0.62, { a: "A3", b: "B12" }, 55);
ok(r && r.y === 55 && r.h === 9 && r.w === 128, "the blit rect is exactly the panel: " + JSON.stringify(r));
render("ends", 0, { a: "A16", b: "B16" }, 55);
render("full", 1, { a: "A-", b: "B5" }, 55);
render("sliding", 0.4, { a: "A1", b: "B1" }, 60, true);   /* mid-slide it hangs off the bottom, by design */

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: scene fader overlay (PNGs in build/tests/scene_fader_*.png)");
'
