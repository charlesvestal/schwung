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

  /* A COARSE DASH: 2 on, 2 off. Asserted as the RHYTHM, because that is what
   * distinguishes it from the solid cursor and from the fine every-other-row
   * spray fence -- a plain "there is ink in this column" check passes for all
   * three. Runs of 1 (fine dither) or a single run spanning the band (solid)
   * both fail here. */
  const rows = [...new Set(lowMark.map((p) => Number(p.split(",")[1])))].sort((a, b) => a - b);
  if (rows.length < 4)
    fail("the base mark is only " + rows.length + "px tall; expected a dashed column");
  const runs = [];
  for (const y of rows) {
    const last = runs[runs.length - 1];
    if (last && y === last[last.length - 1] + 1) last.push(y);
    else runs.push([y]);
  }
  if (runs.length < 2)
    fail("the base mark is a SOLID column -- indistinguishable from the cursor");
  if (!runs.every((r) => r.length <= 2))
    fail("the base mark has a run of " + Math.max(...runs.map((r) => r.length))
       + "px; a coarse dash is 2 on, 2 off");
  if (runs.some((r) => r.length === 1) && runs.every((r) => r.length === 1))
    fail("the base mark is a FINE dither -- indistinguishable from a spray fence");

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


  /* ---- THE FADER carries the same mark, in the one place nothing else draws.
   *
   * A fader is a knob in a different costume and is covered by its group in
   * exactly the same way, so it loses the dot for exactly the same reason. The
   * mark sits OUTSIDE the rails (the bar spans cx-3..cx+3, rails at cx+-4)
   * because the interior is a dithered lattice that re-phases as the value
   * moves -- a mark inside it would be least readable precisely when the value
   * is moving, which is the only time it exists.
   */
  const fader = groups.find((g) => g.kind === "fader");
  if (!fader) fail("mrdrums Main no longer plans a fader group");
  const FK = fader.roles.value;

  const finkFader = (baseAt, liveAt) => {
    const vals = {};
    for (const k of page.keys) vals[k] = "0.5";
    vals[FK] = baseAt;
    const fb = H.createFramebuffer();
    R.renderPageMovy(H.drawContext(fb), {
      page, metaIndex: ix, values: vals,
      modValues: { [FK]: liveAt }, modulated: (k) => k === FK,
      viz: groups, pageIndex: 0, pageCount: 1, header: "L",
    });
    const px = fb.pixels || fb.px;
    const s = new Set();
    for (let y = 0; y < 64; y++)
      for (let x = 0; x < 128; x++) if (px[y * 128 + x]) s.add(x + "," + y);
    return s;
  };

  const fSame = finkFader("0.8", "0.8");
  const fLow  = finkFader("0.2", "0.8");
  const fMid  = finkFader("0.5", "0.8");
  const fLowM = only(fLow, fSame), fMidM = only(fMid, fSame);

  if (fLowM.length === 0)
    fail("a modulated fader draws NOTHING for its base -- the bar moves on its "
       + "own with no way to see what you set");
  /* Two stubs, one either side, on ONE row. */
  const fRows = [...new Set(fLowM.map((q) => Number(q.split(",")[1])))];
  if (fRows.length !== 1)
    fail("the fader base mark spans " + fRows.length + " rows; it must be one");
  const fCols = [...new Set(fLowM.map((q) => Number(q.split(",")[0])))].sort((a, b) => a - b);
  if (fCols.length !== 4)
    fail("expected two 2px stubs either side of the fader, got columns "
       + JSON.stringify(fCols));
  /* A GAP between them: contiguous would mean it crossed the bar. */
  if (fCols[2] - fCols[1] < 5)
    fail("the fader stubs are not clear of the bar (columns "
       + JSON.stringify(fCols) + ") -- they would fight the dithered fill");
  /* ...and it tracks the base: a HIGHER base marks a HIGHER row (smaller y). */
  const fMidRow = [...new Set(fMidM.map((q) => Number(q.split(",")[1])))];
  if (fMidRow.length !== 1 || !(fMidRow[0] < fRows[0]))
    fail("the fader base mark does not track the base value (0.2 -> row "
       + fRows[0] + ", 0.5 -> row " + JSON.stringify(fMidRow) + ")");

  console.log("  ok  a modulated fader marks its base, clear of the bar");

  console.log("  ok  a modulated sample cell marks its BASE, one column wide");
  console.log("  ok  the mark tracks the base and reads as a coarse dash");
  console.log("  ok  an unmodulated page is untouched");
  console.log("PASS: covered graphics carry the modulation base the knob dot used to");
});
'
