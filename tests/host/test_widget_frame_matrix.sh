#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE AXIS THIS DESIGN EXISTS TO HANDLE.
#
# A single-size snapshot passing proves nothing here, because the entire reason
# the frame ctx is stronger than clipping is that the rect VARIES:
#
#   render_page.mjs   cellW = floor(rect.w / COLS), caller-dependent, and rowH
#                     is dynamic -- computeGeom picks the whole render mode from
#                     it (dial -> shrinking radius -> bar-value -> bar-label ->
#                     bar-only)
#   render_page_movy  a fixed 32x15 unembedded, but band-driven when embedded
#   render_page.mjs   Math.min(g.slotSpan, COLS - col) silently CLAMPS a
#                     two-slot group near the right edge
#
# THE FRAMES ARE NOT WRITTEN DOWN HERE. computeGeom and its thresholds
# (DIAL_FULL_H, BAR_FULL_H) are module-private, so a hand-written table of
# heights would be a COPY of arithmetic that lives somewhere else, and would go
# stale the moment a threshold moved -- silently, and green. Instead this drives
# the REAL renderers across a sweep of rects and captures every frame a widget
# is actually handed. If the renderer changes what it hands out, this test sees
# the new frames rather than the old table.
#
# A green matrix only proves the axis you chose, so the greedy widget is driven
# through the SAME captured frames: containment has to hold everywhere, not
# somewhere.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the frame matrix tests" >&2
  exit 1
fi

node --input-type=module -e '
import { renderPage } from "./src/shared/param_pages/render_page.mjs";
import { renderPageMovy } from "./src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "./src/shared/param_pages/viz.mjs";
import { frameCtx } from "./src/shared/param_pages/frame_ctx.mjs";
import { registerWidget, clearWidgets } from "./src/shared/param_pages/widget_registry.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const sink = () => ({
  fillRect() {}, print() {}, textWidth(t) { return String(t).length * 4; },
});

/* One custom param, so every renderer hands our widget a frame. */
const CP = [{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
              viz: { kind: "custom:probe" } },
            { key: "tone",  name: "Tone",  type: "float", min: 0, max: 1 },
            { key: "mix",   name: "Mix",   type: "float", min: 0, max: 1 }];
const KEYS = ["drive", "tone", "mix", null, null, null, null, null];
const metaIndex = buildMetaIndex({
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: ["drive", "tone", "mix"],
    params: CP.map((p) => ({ key: p.key })) } } },
  chainParams: CP,
});
const page = { name: "P", keys: KEYS };
const values = { drive: "0.5", tone: "0.5", mix: "0.5" };

/* ---- Capture every frame the real renderers hand a widget. ---- */
const frames = [];
clearWidgets();
registerWidget("custom:probe", {
  draw: (c) => { frames.push({ w: c.width, h: c.height }); },
});

/* Resolved groups are a CALLER input (o.viz), so the sweep must resolve them
 * the way the shadow UI does -- with the widget registered, so the custom kind
 * actually claims its key. */
const VIZ = resolveViz({ keys: KEYS, metaIndex }).groups;
if (!VIZ.some((g) => g.kind === "custom:probe")) {
  ok(false, "setup: the custom kind did not resolve, so nothing downstream is meaningful");
}

const SWEEP = [];
for (let h = 20; h <= 64; h += 2) SWEEP.push({ x: 0, y: 0, w: 128, h });
for (const w of [64, 96, 100, 128]) SWEEP.push({ x: 0, y: 0, w, h: 64 });
/* Off-origin, to catch a renderer that forgets rect.x/rect.y. */
SWEEP.push({ x: 7, y: 3, w: 100, h: 50 });

for (const rect of SWEEP) {
  for (const layout of ["dial", "bar"]) {
    try { renderPage(sink(), { rect, page, values, metaIndex, layout, viz: VIZ,
                               title: "T", pageIndex: 0, pageCount: 1 }); }
    catch (e) { ok(false, `renderPage threw at ${rect.w}x${rect.h} ${layout}: ${e}`); }
  }
  try { renderPageMovy(sink(), { rect, page, values, metaIndex, viz: VIZ,
                                 title: "T", pageIndex: 0, pageCount: 1 }); }
  catch (e) { ok(false, `renderPageMovy threw at ${rect.w}x${rect.h}: ${e}`); }
}
/* The unembedded movy path too -- it takes a different geometry branch. */
try { renderPageMovy(sink(), { page, values, metaIndex, viz: VIZ, title: "T", pageIndex: 0, pageCount: 1 }); }
catch (e) { ok(false, `unembedded renderPageMovy threw: ${e}`); }

const uniq = [...new Map(frames.map((f) => [f.w + "x" + f.h, f])).values()];
ok(frames.length > 0, "the renderers handed the widget at least one frame");
ok(uniq.length >= 4,
   `the sweep produced several distinct frame sizes (got ${uniq.length}: ` +
   uniq.map((f) => f.w + "x" + f.h).join(" ") + ")");

/* ---- Drive both widgets through every captured frame. ---- */
const polite = (c) => {
  c.fillRect(0, 0, c.width, 1, 1);
  if (c.height > 1) c.fillRect(0, c.height - 1, c.width, 1, 1);
  if (c.width > 2 && c.height > 2) c.fillRect(1, 1, c.width - 2, c.height - 2, 0);
};
const greedy = (c) => {
  c.fillRect(-40, -40, 400, 400, 1);
  c.fillRect(200, 200, 40, 40, 1);
  c.print(0, 0, "OVERFLOWING TEXT THAT IS FAR TOO LONG FOR ANY CELL", 1);
};

const ORIGIN = { x: 11, y: 13 };
let politeBad = 0, politeClipped = 0, greedyBad = 0, greedyUncounted = 0;

for (const f of uniq) {
  const box = { ...ORIGIN, w: f.w, h: f.h };
  const inside = (calls) => calls.every(([x, y, w, h]) =>
    x >= ORIGIN.x && y >= ORIGIN.y &&
    x + w <= ORIGIN.x + f.w && y + h <= ORIGIN.y + f.h);

  let calls = [];
  let c = frameCtx({ fillRect: (...a) => calls.push(a), print() {},
                     textWidth: (t) => String(t).length * 4 }, box);
  polite(c);
  if (!inside(calls)) politeBad++;
  if (c.clipped() !== 0) politeClipped++;

  calls = [];
  c = frameCtx({ fillRect: (...a) => calls.push(a), print() {},
                 textWidth: (t) => String(t).length * 4 }, box);
  greedy(c);
  if (!inside(calls)) greedyBad++;
  if (c.clipped() === 0) greedyUncounted++;
}

ok(politeBad === 0, `a widget sized to width/height stays in frame in all ${uniq.length} frames`);
ok(politeClipped === 0, "such a widget clips nothing in any frame");
ok(greedyBad === 0, `a greedy widget is CONTAINED in all ${uniq.length} frames`);
ok(greedyUncounted === 0, "greedy overflow is COUNTED, not hidden, in every frame");

/* Degenerate frames a renderer could hand out at the low end. */
for (const f of [{ w: 1, h: 1 }, { w: 0, h: 12 }, { w: 12, h: 0 }]) {
  let calls = [];
  const c = frameCtx({ fillRect: (...a) => calls.push(a), print() {},
                       textWidth: (t) => String(t).length * 4 },
                     { ...ORIGIN, ...f });
  let threw = false;
  try { polite(c); greedy(c); } catch (e) { threw = true; }
  ok(!threw, `a ${f.w}x${f.h} frame does not throw`);
  ok(calls.every(([x, y, w, h]) =>
       x >= ORIGIN.x && y >= ORIGIN.y &&
       x + w <= ORIGIN.x + f.w && y + h <= ORIGIN.y + f.h),
     `a ${f.w}x${f.h} frame contains everything drawn into it`);
}

clearWidgets();
process.exit(fail ? 1 : 0);
'
