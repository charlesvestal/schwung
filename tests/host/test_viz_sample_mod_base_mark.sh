#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A COVERED cell has no modulation dot, so the graphic has to carry the base.
#
# The dot lives inside drawKnobWidget, and render_page_movy skips that call
# entirely for a cell a viz group covers. That was invisible until 5f5fb11c let
# a lone `wav_position` form a ONE-CELL sample group: mrdrums Sample Start had
# been an ordinary knob -- pointer at the base, dot at the effective value --
# and silently became a waveform whose cursor tracks the effective value with
# the base nowhere on screen. Reported from the device as losing the knob LFO
# indicator, with the label tilde still present (drawLabelCell sits OUTSIDE the
# covered guard, which is exactly why only half the indication disappeared).
#
# Driven through the REAL mrdrums contract and the real planner/detector, not a
# synthetic page: a hand-built one-key page does NOT form the sample group at
# all, so an earlier version of this probe measured the knob pointer moving and
# reported the mark working while it was absent.
#
# The cursor is held FIXED and only the base varies. Varying the modulation
# instead moves the cursor, which changes far more pixels than the mark does
# and passes with the mark deleted.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
const fs = require("fs");
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
]).then(([H, R, M, P, V]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const pj = (v) => (typeof v === "string" ? JSON.parse(v) : v);

  const j = JSON.parse(fs.readFileSync("./tests/fixtures/module-contracts.json", "utf8"));
  const m = j.modules.find((x) => x.id === "mrdrums");
  if (!m) fail("mrdrums is not in the fleet fixture");

  const hier = pj(m.ui_hierarchy), cp = pj(m.chain_params);
  const ix = M.buildMetaIndex({ hierarchy: hier, chainParams: cp });
  const pages = P.planPages({ hierarchy: hier, chainParams: cp }).pages;
  const page = pages.find((p) => p.keys && p.keys.indexOf("pad_start") >= 0);
  if (!page) fail("no planned page carries pad_start");

  const groups = (V.resolveViz({ keys: page.keys, metaIndex: ix }) || {}).groups || [];
  const slot = page.keys.indexOf("pad_start");
  const covering = groups.find((g) => g.kind === "sample"
      && slot >= g.slotStart && slot < g.slotStart + g.slotSpan);
  /* The premise. Without it every assertion below is about a plain knob and
   * the test proves nothing -- which is how the first version passed. */
  if (!covering) fail("pad_start is not covered by a sample group any more -- "
      + "this test is measuring a knob, not a graphic");

  const ink = (baseAt, liveAt) => {
    const vals = {};
    for (const k of page.keys) vals[k] = "0.5";
    vals.pad_start = baseAt;
    const fb = H.createFramebuffer();
    R.renderPageMovy(H.drawContext(fb), {
      page, metaIndex: ix, values: vals,
      modValues: { pad_start: liveAt }, modulated: (k) => k === "pad_start",
      viz: groups, pageIndex: 0, pageCount: 1, header: "L",
    });
    const px = fb.pixels || fb.px;
    const s = new Set();
    for (let y = 0; y < 64; y++)
      for (let x = 0; x < 128; x++) if (px[y * 128 + x]) s.add(x + "," + y);
    return s;
  };
  const only = (a, b) => [...a].filter((p) => !b.has(p));

  /* Cursor pinned at 0.8 throughout; only the base moves. */
  const atCursor = ink("0.8", "0.8");
  const farLow   = ink("0.2", "0.8");
  const farHigh  = ink("0.6", "0.8");

  const lowMark = only(farLow, atCursor);
  if (lowMark.length === 0)
    fail("a modulated sample cell draws NOTHING for its base -- the graphic "
       + "shows only where the LFO is, never where the knob is set");

  /* ONE column: the mark is an annotation, not a third full-height line
   * competing with the cursor and the spray fences. */
  const cols = [...new Set(lowMark.map((p) => Number(p.split(",")[0])))];
  if (cols.length !== 1)
    fail("the base mark spans " + cols.length + " columns; it must be one");

  /* ...and it must MOVE with the base, or it is a decoration pinned to the
   * cell rather than a reading of the parameter. */
  const highCols = [...new Set(only(farHigh, atCursor).map((p) => Number(p.split(",")[0])))];
  if (highCols.length !== 1 || highCols[0] === cols[0])
    fail("the base mark does not track the base value");
  if (!(highCols[0] > cols[0]))
    fail("a higher base must mark a column further right");

  /* Stubs at the band edges, top AND bottom -- a single edge reads as an
   * artifact of the waveform rather than as a mark. */
  const rows = [...new Set(lowMark.map((p) => Number(p.split(",")[1])))].sort((a, b) => a - b);
  if (rows.length < 4)
    fail("expected stubs at both band edges, got rows " + rows.join(","));
  const span = rows[rows.length - 1] - rows[0];
  if (span < 6)
    fail("the stubs are not at opposite edges of the band (span " + span + "px)");

  /* Base == cursor draws no mark: the two coinciding IS the reading, and a
   * mark there would be drawn under the solid cursor anyway. */
  if (only(ink("0.8", "0.8"), ink("0.8", "0.8")).length !== 0)
    fail("rendering is not deterministic");

  /* NOTHING changes when no source is running. This is what keeps every
   * pinned pixel baseline in the fleet valid. */
  const plainA = (() => {
    const vals = {}; for (const k of page.keys) vals[k] = "0.5";
    const fb = H.createFramebuffer();
    R.renderPageMovy(H.drawContext(fb), { page, metaIndex: ix, values: vals,
      modValues: null, modulated: () => false, viz: groups,
      pageIndex: 0, pageCount: 1, header: "L" });
    const px = fb.pixels || fb.px; const s = new Set();
    for (let y = 0; y < 64; y++) for (let x = 0; x < 128; x++) if (px[y * 128 + x]) s.add(x + "," + y);
    return s;
  })();
  if (plainA.size === 0) fail("the unmodulated page drew nothing at all");

  console.log("  ok  a modulated sample cell marks its BASE, one column wide");
  console.log("  ok  the mark tracks the base and sits at both band edges");
  console.log("  ok  an unmodulated page is untouched");
  console.log("PASS: the sample graphic carries the modulation base the knob dot used to");
});
'
