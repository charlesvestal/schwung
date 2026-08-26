#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Geometry tests for the Movy row renderer (render_page_movy.mjs).
#
# The chain editor's knob card lands in a later commit and reuses this same
# row, drawn narrower and at an offset. Today there is no card and nothing
# here tests one -- this file holds only the geometry invariant the card will
# depend on: render_page_movy.mjs can draw a row at any origin and cell width,
# and the default {x0: 0, cellW: CELL_W} path is untouched by that ability.
#
# The default-path claim cannot be expressed as a behaviour test, because the
# knob grid it must leave alone is not what this file draws -- it is a
# screen tuned deliberately elsewhere, whose regressions are invisible in
# code review. So it is asserted as an INVARIANT instead: a per-page hash of
# the pre-refactor render, captured before the geometry parameter existed.

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
  import("node:crypto"),
]).then(async ([H, C, P, M, RM, V, fs, crypto]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));
  const BASELINE_PATH = "tests/fixtures/movy-geom-baseline.txt";
  const sha1 = (buf) => crypto.createHash("sha1").update(buf).digest("hex");

  /* ---- 1. the geometry parameter exists and defaults to the grid ---- */
  if (typeof RM.drawKnobRow !== "function") fail("drawKnobRow is not exported");
  if (!RM.GRID_GEOM || RM.GRID_GEOM.x0 !== 0 || RM.GRID_GEOM.cellW !== RM.CELL_W)
    fail("GRID_GEOM must be {x0: 0, cellW: CELL_W}");
  console.log("PASS: geometry surface");

  /* ---- 2. INVARIANT: the default path matches the pre-refactor baseline ----
   *
   * One line per page, "<id> <sha1-of-pixels>", sorted -- so a mismatch names
   * exactly which pages moved instead of only failing a byte count, and a
   * deliberate refresh (UPDATE_GEOM_BASELINE=1) is a reviewed diff, the same
   * convention as tests/fixtures/snapshots/param_pages.txt. */
  const current = {};
  for (const mod of fx.modules) {
    const r = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    r.pages.forEach((p, i) => {
      if (p.kind !== P.PAGE_KNOBS) return;
      const id = mod.id + ":" + i;
      const values = {};
      for (const k of (p.keys || [])) if (k) values[k] = 0.5;
      const fb = H.createFramebuffer();
      const groups = V.resolveViz({ keys: p.keys || [], metaIndex }).groups;
      RM.renderPageMovy(H.drawContext(fb), {
        page: p, metaIndex, values, title: mod.id,
        pageIndex: i, pageCount: r.pages.length, touched: 2, viz: groups,
        footer: [["MUTE", "DFLT"], ["SHFT", "FINE"]],
      });
      current[id] = { sha: sha1(Buffer.from(fb.pixels)), fb };
    });
  }
  const ids = Object.keys(current);
  if (ids.length === 0) fail("the fixture plans no knob pages -- this test is not testing anything");

  if (process.env.UPDATE_GEOM_BASELINE) {
    const lines = ids.slice().sort().map((id) => id + " " + current[id].sha);
    const header = [
      "# One line per fixture-plan knob page: <id> <sha1-of-pixels>, sorted.",
      "#",
      "# This is the render_page_movy.mjs INERTNESS baseline -- proof that",
      "# parameterising cell geometry left the default {x0: 0, cellW: CELL_W}",
      "# path byte-identical. A refresh here is a reviewed change: the diff",
      "# names exactly which pages moved. Same convention as",
      "# tests/fixtures/snapshots/param_pages.txt.",
      "#",
      "# Regenerate with:",
      "#     UPDATE_GEOM_BASELINE=1 bash tests/host/test_knob_card.sh",
      "",
    ].join("\n");
    fs.writeFileSync(BASELINE_PATH, header + lines.join("\n") + "\n");
    console.log("UPDATED " + BASELINE_PATH + " (" + lines.length + " pages)");
  }

  if (!fs.existsSync(BASELINE_PATH)) fail("no baseline file -- run with UPDATE_GEOM_BASELINE=1");
  const baseline = {};
  for (const line of fs.readFileSync(BASELINE_PATH, "utf8").split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const sp = line.lastIndexOf(" ");
    if (sp < 0) continue;
    baseline[line.slice(0, sp)] = line.slice(sp + 1);
  }

  let checked = 0, firstOffender = null;
  for (const id of ids) {
    if (!(id in baseline)) continue;
    checked++;
    if (baseline[id] !== current[id].sha && !firstOffender) firstOffender = id;
  }
  if (firstOffender) {
    console.log(current[firstOffender].fb.toBlocks());
    fail("renderPageMovy changed for " + firstOffender + " -- the CELL_W parameterisation " +
         "is NOT inert on the default path (render above; only regenerate with " +
         "UPDATE_GEOM_BASELINE=1 if this change is intended)");
  }
  if (checked === 0) fail("baseline covered no pages");
  console.log("PASS: default geometry inert across " + checked + " pages");

  /* ---- 2b. coverage cannot silently evaporate ----
   * A baseline entry the fixture no longer plans, or a planned page absent
   * from the baseline, both defeat the invariant while this test keeps
   * printing PASS -- assert set equality both ways. */
  const seen = new Set(ids);
  const missing = Object.keys(baseline).filter((k) => !seen.has(k));
  if (missing.length) fail("the baseline names " + missing.length +
      " pages the fixture no longer plans: " + missing.slice(0, 5).join(", "));
  const extra = ids.filter((k) => !(k in baseline));
  if (extra.length) fail(extra.length + " planned pages are not in the baseline -- " +
      "regenerate it with UPDATE_GEOM_BASELINE=1: " + extra.slice(0, 5).join(", "));
  console.log("PASS: baseline covers exactly the pages the fixture plans");

  /* ---- 3. cell origin AND width are both honoured ----
   *
   * Diffing two framebuffers does not prove this: a strip drawn at
   * {x0: 6, cellW: 29} already differs from the grid as soon as x0 alone is
   * honoured, so cellW could be dropped on the floor -- width ignored, only
   * origin threaded -- and a pixel-inequality check would still pass. That
   * exact shape has shipped green on this branch before.
   *
   * So the wide multi-key page below keeps its clip/glyph coverage (a case
   * the single-cell probe does not reach), then a single populated column
   * measures cellW directly: a touched cell fills its label band with a
   * solid run of exactly g.cellW pixels starting at its cell origin
   * (drawLabelCell, row lblY), so with only column 3 populated that run is
   * the only lit run on the row and its bounds ARE the geometry. */
  {
    const params = [
      { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "b", name: "Beta",  type: "float", min: 0, max: 1, step: 0.01 },
      { key: "c", name: "Gamma", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "d", name: "Delta", type: "float", min: 0, max: 1, step: 0.01 },
    ];
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: params });
    const page4 = { kind: "knobs", name: "T", keys: ["a", "b", "c", "d"], authored: true };
    const values4 = { a: 0.2, b: 0.4, c: 0.6, d: 0.8 };
    const drawWide = (geom) => {
      const fb = H.createFramebuffer();
      RM.drawKnobRow(H.drawContext(fb), { page: page4, metaIndex: mi, values: values4, touched: -1 },
                     0, RM.ROW0_Y, RM.LBL0_Y, geom);
      return fb;
    };
    const wa = drawWide(RM.GRID_GEOM), wb = drawWide({ x0: 6, cellW: 29 });
    if (wa.clipped() !== 0 || wb.clipped() !== 0) fail("strip drew outside the display");
    if (wa.missingGlyphs.size || wb.missingGlyphs.size) fail("strip used a glyph the atlas lacks");

    const single = M.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "d", name: "Delta", type: "float", min: 0, max: 1, step: 0.01 },
    ] });
    const page1 = { kind: "knobs", name: "T", keys: [null, null, null, "d"], authored: true };
    const bandRun = (geom) => {
      const fb = H.createFramebuffer();
      RM.drawKnobRow(H.drawContext(fb), { page: page1, metaIndex: single, values: { d: 0.7 }, touched: 3 },
                     0, RM.ROW0_Y, RM.LBL0_Y, geom);
      if (fb.clipped() !== 0) fail("strip drew outside the display");
      if (fb.missingGlyphs.size) fail("strip used a glyph the atlas lacks");
      let lo = -1, hi = -1;
      for (let x = 0; x < fb.width; x++) {
        if (fb.pixels[RM.LBL0_Y * fb.width + x]) { if (lo < 0) lo = x; hi = x; }
      }
      if (lo < 0) fail("the touched cell drew no inverted band");
      for (let x = lo; x <= hi; x++) {
        if (!fb.pixels[RM.LBL0_Y * fb.width + x]) fail("the inverted band has a hole in it");
      }
      return { x: lo, w: hi - lo + 1 };
    };
    /*
     * THE PROBE IS THE STRIP CENTRING, not the strip width.
     *
     * This used to assert `bandWidth === cellW`, which worked because the
     * touched strip spanned the whole cell: measuring the band measured the
     * cell. SCH-50 `half-strip` sizes the strip to the VALUE plus one pixel
     * each side, so the band no longer reports the cell width and that
     * assertion measured the wrong thing rather than a broken thing.
     *
     * What still tests both halves of the geometry is WHERE the strip sits.
     * It is centred in its cell, so:
     *
     *   x0     wrong -> the strip lands outside the cell entirely
     *   cellW  wrong -> the strip is centred against the wrong width, so the
     *                   clear margins either side of it stop being equal
     *
     * A one-pixel asymmetry is legal (an odd leftover cannot split evenly, and
     * centreX always gives the extra pixel to the right), so the tolerance is
     * 1 -- and centring against CELL_W=32 instead of cellW=29 skews it by 3,
     * which is well outside that.
     */
    for (const geo of [{ x0: 0, cellW: RM.CELL_W }, { x0: 0, cellW: 29 },
                       { x0: 6, cellW: 29 }, { x0: 3, cellW: 30 }]) {
      const got = bandRun(geo);
      const cellX = geo.x0 + 3 * geo.cellW;
      if (got.x < cellX || got.x + got.w > cellX + geo.cellW)
        fail("cell 3 strip spans " + got.x + ".." + (got.x + got.w - 1) + " but its cell is " +
             cellX + ".." + (cellX + geo.cellW - 1) + " -- the origin or width is not threaded through");
      if (got.w >= geo.cellW)
        fail("cell 3 strip is " + got.w + "px at cellW=" + geo.cellW +
             " -- it is spanning the cell, so it is not sized to its value");
      const left = got.x - cellX, right = (cellX + geo.cellW) - (got.x + got.w);
      if (Math.abs(left - right) > 1)
        fail("cell 3 strip is not centred at x0=" + geo.x0 + " cellW=" + geo.cellW +
             ": " + left + "px clear on the left, " + right + " on the right -- " +
             "it is being centred against some other width");
    }
    const dflt = bandRun(undefined);
    const dfltCellX = 3 * RM.CELL_W;
    if (dflt.x < dfltCellX || dflt.x + dflt.w > dfltCellX + RM.CELL_W)
      fail("an omitted geom did not fall back to the grid");
    console.log("PASS: cell origin and width both honoured");
  }

  /* ---- 4. CRITICAL: a partial geometry throws, it does not hang ----
   *
   * {cellW: 29} alone leaves x0 undefined, every cell position NaN, and the
   * knob pointer reaches the line() function in render_page.mjs, a
   * for (;;) loop whose only exit is x0 === x1 && y0 === y1 -- which NaN
   * never satisfies, so it spins the UI tick forever. Confirmed empirically:
   * the call did not return in 8 seconds before this guard existed. Assert
   * the throw instead of ever drawing a hang. */
  {
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
    ] });
    const page1 = { kind: "knobs", name: "T", keys: ["a", null, null, null], authored: true };
    const values1 = { a: 0.5 };
    const mustThrow = (geom, what) => {
      const fb = H.createFramebuffer();
      let threw = false;
      try {
        RM.drawKnobRow(H.drawContext(fb), { page: page1, metaIndex: mi, values: values1, touched: -1 },
                       0, RM.ROW0_Y, RM.LBL0_Y, geom);
      } catch (e) { threw = true; }
      if (!threw) fail("a partial geom (" + what + ") did not throw -- it can hang the UI tick");
    };
    mustThrow({ cellW: 29 }, "missing x0");
    mustThrow({ x0: 6 }, "missing cellW");
    mustThrow({ x: 6, cellW: 30 }, "misspelled x0");
    console.log("PASS: a partial geometry is rejected, not silently drawn");
  }

  /* ---- 4. the card rect ---- */
  const KC = await import("./src/shared/param_pages/knob_card.mjs");
  {
    const full = KC.knobCardRect(true), short = KC.knobCardRect(false);
    const eq = (a, b, what) => { if (a !== b) fail(what + ": expected " + b + ", got " + a); };
    eq(full.x, 3, "full.x"); eq(full.y, 12, "full.y"); eq(full.w, 122, "full.w"); eq(full.h, 38, "full.h");
    /* short.y tracks HEADER_H: the card is CENTRED in HEADER_H+1..RULE_Y, so a
       one-row-shorter header moves the odd remainder by one while the full
       card, whose remainder is even, does not move at all. It read 23 for the
       one release the header band was 6 rows, and 24 either side of that.
       Both numbers stay literal on purpose -- deriving them here would restate
       knobCardRect rather than pin it. */
    eq(short.x, 3, "short.x"); eq(short.y, 24, "short.y"); eq(short.w, 122, "short.w"); eq(short.h, 15, "short.h");
    if (full.y + full.h > RM.RULE_Y) fail("full card overlaps the footer rule");
    if (full.y < RM.HEADER_H + 1) fail("full card overlaps the screen header");
    eq(KC.knobCardContentW(), 116, "knobCardContentW");
    console.log("PASS: card rect");
  }

  /* ---- 4b. the cell is 29px, measured, not inferred ----
   *
   * The gap and clip checks below are ASYMMETRIC and that is easy to miss: a
   * cell too WIDE laps into the gap column or runs off the screen and is
   * caught, but a cell one or two pixels too NARROW is invisible to every
   * other assertion in this file. Driving the card through cellW of 27 and 28
   * left the whole suite green.
   *
   * So measure it -- but measure the PITCH, not one cell.
   *
   * This used to lean on the touched cell filling its label band with a solid
   * run of exactly cellW pixels, so the run START was the cell origin and its
   * LENGTH was the cell width. SCH-50 `half-strip` sizes that strip to the
   * VALUE instead of to the cell, so its length is now a fact about the text
   * and says nothing about the geometry.
   *
   * Two cells carrying the SAME label are immune to that. Whatever each label
   * measures, both sit at the same offset inside their own cell, so the
   * distance between where they start IS cellW exactly -- and it stays exact
   * whether the strip spans the cell, hugs the value, or is not drawn at all.
   * Neither cell is touched, so what is being measured is plain label text and
   * the assertion no longer depends on the inversion existing.
   *
   * A cell one pixel narrow still moves the second label one pixel left, which
   * is the failure the old check was written to catch (cellW 27 and 28 left the
   * whole suite green) and this one catches it the same way. */
  {
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "a", name: "AA", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "b", name: "AA", type: "float", min: 0, max: 1, step: 0.01 },
    ] });
    const fb = H.createFramebuffer();
    KC.drawKnobCard(H.drawContext(fb), {
      name: "AA", value: "0.20", row: 0, touched: -1,
      page: { kind: "knobs", keys: ["a", "b", null, null, null, null, null, null] },
      metaIndex: mi, values: { a: 0.2, b: 0.2 },
    });
    const r = KC.knobCardRect(true);
    /* content top + header band + the gap row = the widget row; its label band
     * sits LBL0_Y - ROW0_Y further down, plus the one clear row inside it */
    const lblRow = r.y + KC.BORDER_W + KC.GAP_W + KC.HEADER_BAND_H + KC.GAP_W
                 + (RM.LBL0_Y - RM.ROW0_Y) + 1;
    /* Scan the CONTENT span only. The cards own border columns are lit on
     * every interior row, so a full-width scan finds the border too. */
    const contentX = r.x + KC.BORDER_W + KC.GAP_W;
    /* Cluster the lit columns: glyphs are not contiguous, but the gap BETWEEN
     * two cells labels is far wider than any gap inside a word. */
    const GAP = 6;
    const runs = [];
    let start = -1, last = -2;
    for (let x = contentX; x < contentX + KC.knobCardContentW(); x++) {
      if (!fb.pixels[lblRow * fb.width + x]) continue;
      if (x - last > GAP) { if (start >= 0) runs.push([start, last]); start = x; }
      last = x;
    }
    if (start >= 0) runs.push([start, last]);
    if (runs.length !== 2)
      fail("expected two label runs at row " + lblRow + ", found " + runs.length +
           " -- the probe is not measuring what it thinks it is");
    const pitch = runs[1][0] - runs[0][0];
    if (pitch !== 29)
      fail("cell pitch is " + pitch + "px, expected 29 -- four cells must fill " +
           "the 116px content exactly");
    /* ...and the first cell is where the content starts, which pitch alone
     * cannot tell you: a card whose cells were all shifted right would keep a
     * correct pitch. The label is centred, so allow its own clear margin. */
    const label0Off = runs[0][0] - contentX;
    if (label0Off < 0 || label0Off > 29)
      fail("cell 0 label starts " + label0Off + "px into the cell -- the first " +
           "cell is not at the content origin " + contentX);
    console.log("PASS: cell width measured at 29px");
  }

  /* ---- 4c. an opaque param keeps its divable brackets ----
   *
   * Nothing DIVES from the card -- it is transient feedback while a finger is
   * on a knob, and the door to a params own editor is one level up, on the
   * chain editors jog click. The obvious simplification is therefore to drop
   * drawDivableMark inside the card, since the brackets look like an
   * affordance the card does not offer.
   *
   * That would be a bug, and render_page_movy.mjs says why at drawOpaqueBox:
   * the opaque box draws NO FRAME OF ITS OWN -- the divable brackets ARE its
   * frame. Suppress them and a filepath value floats in the cell with no
   * container.
   *
   * Pinned here because the reasoning that leads to removing them is sound
   * right up until the last step. */
  {
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "f", name: "File", type: "filepath" },
    ] });
    if (!mi.getOrGuess("f").divable) fail("a filepath should be divable");
    const fb = H.createFramebuffer();
    KC.drawKnobCard(H.drawContext(fb), {
      name: "FILE", value: "kick.wav", row: 0, touched: 0,
      page: { kind: "knobs", keys: ["f", null, null, null, null, null, null, null] },
      metaIndex: mi, values: { f: "/x/y/kick.wav" },
    });
    const r = KC.knobCardRect(true);
    const contentX = r.x + KC.BORDER_W + KC.GAP_W;
    const rowY = r.y + KC.BORDER_W + KC.GAP_W + KC.HEADER_BAND_H + KC.GAP_W;
    /* drawDivableMark insets by one and spans BOX_H, so these four are the
     * bracket corners -- the only frame this widget has */
    const bx = contentX + 1, by = rowY, bw = 29 - 2, bh = RM.BOX_H;
    const at = (x, y) => fb.pixels[y * fb.width + x];
    for (const [x, y, which] of [[bx, by, "top left"], [bx + bw - 1, by, "top right"],
                                 [bx, by + bh - 1, "bottom left"],
                                 [bx + bw - 1, by + bh - 1, "bottom right"]]) {
      if (!at(x, y))
        fail("the " + which + " divable bracket is missing -- an opaque box has " +
             "no frame of its own, so the brackets are the only thing containing it");
    }
    if (fb.clipped() !== 0) fail("opaque cell drew outside the display");
    console.log("PASS: opaque param keeps the brackets that frame it");
  }

  /* ---- 5. INVARIANT: a black gap separates the border from the band ----
   *
   * The border is white and so is the inverted header band. Where they touch,
   * the border stops existing: a short card without the gap reads as one fat
   * stripe across sliced-off diagram boxes, with no left, right or top. This
   * is invisible in code review, so it is asserted on the pixels.
   *
   * The arithmetic, worked on paper with CARD_X=3, CARD_W=122, BORDER_W=2: the
   * card spans x 3..124, border columns are 3,4 and 123,124, the gap columns
   * are 5 and 122, and content runs 6..121 (116 wide). checkGap below computes
   * this generically from the rect it is handed (gx0 = r.x + BORDER_W, etc.)
   * rather than hardcoding those numbers, so it holds for both card heights. */
  {
    const params = [
      { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "b", name: "Beta",  type: "float", min: 0, max: 1, step: 0.01 },
      { key: "c", name: "Gamma", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "d", name: "Delta", type: "enum",  options: ["Hall", "Room", "Plate"] },
      { key: "e", name: "Eps",   type: "int",   min: 0, max: 127 },
      { key: "f", name: "File",  type: "filepath" },
    ];
    const mi = M.buildMetaIndex({ hierarchy: null, chainParams: params });
    const keys = ["a", "b", "c", "d", "e", "f", null, null];
    const values = { a: 0.2, b: 0.4, c: 0.6, d: 2, e: 64, f: "/x/y/kick.wav" };

    const draw = (o) => {
      const fb = H.createFramebuffer();
      KC.drawKnobCard(H.drawContext(fb), o);
      return fb;
    };
    const px = (fb, x, y) => fb.pixels[y * fb.width + x];

    const checkGap = (fb, r, what) => {
      const gx0 = r.x + KC.BORDER_W, gx1 = r.x + r.w - 1 - KC.BORDER_W;
      const gy0 = r.y + KC.BORDER_W, gy1 = r.y + r.h - 1 - KC.BORDER_W;
      for (let y = gy0; y <= gy1; y++) {
        if (px(fb, gx0, y)) fail(what + ": left gap column lit at y=" + y);
        if (px(fb, gx1, y)) fail(what + ": right gap column lit at y=" + y);
      }
      for (let x = gx0; x <= gx1; x++) {
        if (px(fb, x, gy0)) fail(what + ": top gap row lit at x=" + x);
        if (px(fb, x, gy1)) fail(what + ": bottom gap row lit at x=" + x);
      }
      /* and the border itself must actually BE there */
      for (let i = 0; i < KC.BORDER_W; i++) {
        if (!px(fb, r.x + i, r.y + Math.floor(r.h / 2))) fail(what + ": left border missing");
        if (!px(fb, r.x + r.w - 1 - i, r.y + Math.floor(r.h / 2))) fail(what + ": right border missing");
      }
    };

    /* every knob touched in turn -- cols 0 and 3 fill their label band to the
     * cell edge, which is the case that eats a border without the gap */
    for (let k = 0; k < 6; k++) {
      const fb = draw({ name: "ALPHA", value: "0.62", row: k >> 2, touched: k,
                        page: { kind: "knobs", keys }, metaIndex: mi, values });
      checkGap(fb, KC.knobCardRect(true), "full card, knob " + k);
      if (fb.clipped() !== 0) fail("card drew outside the display, knob " + k);
      if (fb.missingGlyphs.size) fail("card used a glyph the atlas lacks, knob " + k);
    }
    const sfb = draw({ name: "S1: CUTOFF", value: "72" });
    checkGap(sfb, KC.knobCardRect(false), "short card");
    if (sfb.clipped() !== 0) fail("short card drew outside the display");
    console.log("PASS: frame invariant, gap holds against every cell");
  }

  /* ---- 6. the NAME loses a collision, never the value ---- */
  {
    const fb = H.createFramebuffer();
    const ctx = H.drawContext(fb);
    KC.drawKnobCard(ctx, { name: "A Ludicrously Long Parameter Name", value: "12345" });
    const r = KC.knobCardRect(false);
    const vw = ctx.textWidth("12345");
    /* The band is white and the glyphs are knocked out of it, so the value
     * being present means UNLIT pixels in the columns it should occupy. */
    let knocked = 0;
    for (let x = r.x + r.w - 3 - vw; x < r.x + r.w - 3; x++)
      for (let y = r.y + 3; y < r.y + 3 + 9; y++) if (!fb.pixels[y * fb.width + x]) knocked++;
    if (knocked === 0) fail("value was squeezed out of the header band by a long name");
    if (fb.clipped() !== 0) fail("long name drew outside the display");
    console.log("PASS: long name truncates, value survives");
  }

  console.log("OK");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
