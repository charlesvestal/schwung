#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Geometry and frame tests for the chain-editor knob card.
#
# Two different KINDS of assertion live here on purpose.
#
# The card tests are BEHAVIOUR: does it draw the right thing. The baseline test
# is an INVARIANT: parameterising CELL_W must not disturb the knob grid, a
# screen that was tuned deliberately and whose regressions are invisible in
# review. Behaviour tests cannot express that, because the grid is not what
# they draw.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob card tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./tools/param-pages/cases.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
  import("node:fs"),
]).then(([H, C, P, M, RM, V, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));

  /* ---- 1. the geometry parameter exists and defaults to the grid ---- */
  if (typeof RM.drawKnobStrip !== "function") fail("drawKnobStrip is not exported");
  if (!RM.GRID_GEOM || RM.GRID_GEOM.x0 !== 0 || RM.GRID_GEOM.cellW !== RM.CELL_W)
    fail("GRID_GEOM must be {x0: 0, cellW: CELL_W}");
  console.log("PASS: geometry surface");

  /* ---- 2. INVARIANT: the default path is byte-identical ---- */
  const baseline = JSON.parse(fs.readFileSync("tests/fixtures/movy-geom-baseline.json", "utf8"));
  let checked = 0;
  for (const mod of fx.modules) {
    const r = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    r.pages.forEach((p, i) => {
      if (p.kind !== P.PAGE_KNOBS) return;
      const id = mod.id + ":" + i;
      if (!baseline[id]) return;
      const values = {};
      for (const k of (p.keys || [])) if (k) values[k] = 0.5;
      const fb = H.createFramebuffer();
      const g = V.resolveViz({ keys: p.keys || [], metaIndex }).groups;
      RM.renderPageMovy(H.drawContext(fb), {
        page: p, metaIndex, values, title: mod.id,
        pageIndex: i, pageCount: r.pages.length, touched: 2, viz: g,
        footer: [["MUTE", "DFLT"], ["SHFT", "FINE"]],
      });
      const now = Buffer.from(fb.pixels).toString("base64");
      if (now !== baseline[id])
        fail("renderPageMovy changed for " + id + " -- the CELL_W parameterisation " +
             "is NOT inert on the default path");
      checked++;
    });
  }
  if (checked === 0) fail("baseline covered no pages");
  console.log("PASS: default geometry inert across " + checked + " pages");

  /* ---- 3. a non-default geometry actually moves the cells ---- */
  {
    const params = [
      { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "b", name: "Beta",  type: "float", min: 0, max: 1, step: 0.01 },
      { key: "c", name: "Gamma", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "d", name: "Delta", type: "float", min: 0, max: 1, step: 0.01 },
    ];
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: params });
    const page = { kind: "knobs", name: "T", keys: ["a", "b", "c", "d"], authored: true };
    const values = { a: 0.2, b: 0.4, c: 0.6, d: 0.8 };
    const draw = (geom) => {
      const fb = H.createFramebuffer();
      RM.drawKnobStrip(H.drawContext(fb), { page, metaIndex: mi, values, touched: -1 },
                       0, RM.ROW0_Y, RM.LBL0_Y, geom);
      return fb;
    };
    const a = draw(RM.GRID_GEOM);
    const b = draw({ x0: 6, cellW: 29 });
    if (Buffer.from(a.pixels).equals(Buffer.from(b.pixels)))
      fail("a 29px strip at x0=6 drew identically to the 32px grid strip -- the " +
           "geometry parameter is being ignored");
    if (a.clipped() !== 0 || b.clipped() !== 0) fail("strip drew outside the display");
    if (a.missingGlyphs.size || b.missingGlyphs.size) fail("strip used a glyph the atlas lacks");
    console.log("PASS: non-default geometry moves the cells");
  }

  console.log("OK");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
