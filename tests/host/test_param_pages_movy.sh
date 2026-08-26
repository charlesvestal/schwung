#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Rendering tests for the Movy knob-grid layout (render_page_movy.mjs).
#
# This layout had NO coverage until two bugs shipped in it, and both were
# invisible to the existing suite for the same reason: the headless draw
# context offered no native primitives, so every test and every preview only
# ever exercised the JS fallback paths, while the device
# (src/shadow/shadow_ui_param_pages.mjs) always supplies the native ones.
# H.drawContext now offers them by default, and these tests pin the two shapes
# that broke:
#
#   1. The knob ring. It was faked as a radius-r disk with a radius-(r-1) disk
#      punched out of it. That is not a ring: at each cardinal the two disks
#      reach the same column extent, so the pixel just inside the extreme one
#      is erased and the extreme pixel is stranded over a gap — four detached
#      dots outside a flat-sided outline. No integer radius avoids it. Pinned
#      here as exact art, so a return to the two-disk trick fails loudly.
#
#   2. The filter roll-off. drawColumnCurve truncated the curve at its last
#      SAMPLE above the floor rather than at the floor crossing, and the
#      roll-off is only ~11% of the span, so ~3 of 28 uniform samples land in
#      it. The tail therefore ended at an arbitrary height that sawtoothed as
#      cutoff moved: the bottom half of the curve blinking on and off, one
#      detent at a time.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the Movy layout render tests" >&2
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
  import("./src/shared/param_pages/viz_draw.mjs"),
  import("./src/shared/param_pages/font5x3.mjs"),
  import("./src/shared/param_pages/font4x5.mjs"),
  import("./src/shared/param_pages/font_tamzen6x12.mjs"),
  import("./src/shared/param_pages/render_page.mjs"),
  import("./src/shared/param_format.mjs"),
  import("node:fs"),
]).then(([H, C, P, M, RM, V, VD, F5, F4, TZ, RP, PF, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));

  /* ---- 1. the draw context must actually offer the native primitives ---- */
  {
    const ctx = H.drawContext(H.createFramebuffer());
    for (const fn of ["line", "fillCircle", "drawCircle", "drawArc"]) {
      if (typeof ctx[fn] !== "function") {
        fail("harness drawContext is missing " + fn + " — tests would silently " +
             "exercise the JS fallback while the device takes the native path");
      }
    }
    const bare = H.drawContext(H.createFramebuffer(), { native: false });
    if (typeof bare.drawArc === "function") fail("drawContext({native:false}) still offers native primitives");
  }

  /* ---- 2. fleet sweep in the Movy layout, through the native context ---- */
  const missing = new Set();
  let rendered = 0;
  const blank = [], spill = [];

  for (const mod of fx.modules) {
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    for (let i = 0; i < pages.length; i++) {
      const page = pages[i];
      if (page.kind !== P.PAGE_KNOBS) continue;
      const values = {};
      for (const k of page.keys) values[k] = C.fakeValue(k, metaIndex.getOrGuess(k));
      const { groups } = V.resolveViz({ keys: page.keys, metaIndex });

      const fb = H.createFramebuffer();
      RM.renderPageMovy(H.drawContext(fb), {
        page, metaIndex, values,
        title: "T1 > " + mod.id.toUpperCase(),
        pageIndex: i, pageCount: pages.length, viz: groups,
      });
      rendered++;
      for (const g of fb.missingGlyphs) missing.add(g);
      if (fb.countLit() < 50) blank.push(mod.id + "#" + i);
      if (fb.clipped() > 0) spill.push(mod.id + "#" + i + " (" + fb.clipped() + "px)");
    }
  }
  if (missing.size) fail("Movy layout draws characters the device font has no glyph for: " + [...missing].join(" "));
  if (blank.length) fail("near-blank Movy pages: " + blank.slice(0, 5).join(", "));
  if (spill.length) fail("Movy content drawn off-screen: " + spill.slice(0, 5).join(", "));

  /* ---- 2b. every fleet string is drawable in the 5x3 font ---------------- */
  {
    /* `fb.missingGlyphs` above only watches the DEVICE font, and this layout
     * no longer draws any text through it — so without this check, a label
     * containing a character font5x3 lacks would render as a silent GAP on
     * the OLED with nothing to catch it. Same failure mode, different font. */
    const gaps = new Map();
    let scanned = 0;
    for (const mod of fx.modules) {
      const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      for (const page of pages) {
        if (page.kind !== P.PAGE_KNOBS) continue;
        for (const k of (page.keys || [])) {
          if (!k) continue;
          const meta = metaIndex.getOrGuess(k);
          /* Two fonts, two sweeps. font4x5 draws labels, values and the
           * header AND the enum square. A character it lacks renders as
           * NOTHING on the OLED, and it is not the device font, so
           * fb.missingGlyphs above cannot see any of it. */
          for (const s of [String(meta.label || meta.key),
                           String(PF.formatParamValue(C.fakeValue(k, meta), meta))]) {
            scanned++;
            /* Labels, values and the header are Tamzen now; the enum square is
             * still font4x5. Both are checked — a glyph either lacks renders
             * as NOTHING on the OLED, and neither is the device font, so
             * fb.missingGlyphs cannot see it. */
            for (const ch of TZ.missingGlyphs(RP.asciiFold(s).toUpperCase())) {
              if (!gaps.has(ch)) gaps.set(ch, "tamzen: " + s);
            }
          }
          for (const o of (Array.isArray(meta.options) ? meta.options : [])) {
            scanned++;
            const t = RP.asciiFold(String(o)).toUpperCase();
            for (const ch of TZ.missingGlyphs(t)) if (!gaps.has(ch)) gaps.set(ch, "tamzen: " + o);
            for (const ch of F4.missingGlyphs4x5(t)) if (!gaps.has(ch)) gaps.set(ch, "4x5(enum): " + o);

          }
        }
      }
    }
    if (gaps.size) {
      fail("a font has no glyph for " + [...gaps.keys()].map((c) => JSON.stringify(c)).join(" ") +
           " — these render as nothing on the OLED. Seen in: " +
           [...gaps.values()].slice(0, 4).map((s) => JSON.stringify(s)).join(", ") +
           ". Add the glyph to font5x3.mjs rather than folding it away if it carries " +
           "meaning (C# is not C).");
    }
    if (scanned < 3000) fail("font coverage scan only saw " + scanned + " strings — did the fleet fixture shrink?");
  }

  /* ---- 3. the knob ring is a circle, not a difference of two disks ------ */
  {
    /* Render one knob at a value whose pointer leaves the top-left quadrant
     * clear, then read the ring back. */
    const fb = H.createFramebuffer();
    const page = { kind: P.PAGE_KNOBS, name: "K", level: "root", keys: ["cutoff"] };
    const metaIndex = { getOrGuess: () => ({ key: "cutoff", label: "Cut", type: "float", kind: "number", min: 0, max: 1, step: 0.01 }) };
    RM.renderPageMovy(H.drawContext(fb), {
      page, metaIndex, values: { cutoff: "0.5" }, title: "T", pageIndex: 0, pageCount: 1, viz: [],
    });

    /* The ring sample window is 2r+1 = 17 rows, but the widget band is BOX_H
     * (15) — so once the footer tightened the rhythm (LBL0_Y 28 -> 25) the last
     * rows of the window land on the LABEL band, and the label glyphs showed
     * up in the art. The knob itself still stops well short of it (its bottom
     * three rows are the deliberate track gap, asserted below), so clear the
     * label band before reading back: this assertion is about the ring outline,
     * and the label band has its own tests. */
    for (let y = RM.LBL0_Y; y < RM.LBL0_Y + RM.LBL_H; y++) {
      for (let x = 0; x < fb.width; x++) fb.pixels[y * fb.width + x] = 0;
    }

    /* Knob 0 sits at column 0 of row 0: kx = (CELL_W-KW)/2, ky = ROW0_Y. */
    const kx = Math.floor((RM.CELL_W - RM.KW) / 2), ky = RM.ROW0_Y;
    const r = RM.KNOB_R, cx = kx + r, cy = ky + r;
    let art = "";
    for (let dy = -r; dy <= r; dy++) {
      for (let dx = -r; dx <= r; dx++) {
        /* Ignore the pointer column so this pins the OUTLINE only. */
        const isPointer = dx === 0 && dy <= 0;
        art += (!isPointer && fb.pixels[(cy + dy) * fb.width + (cx + dx)]) ? "#" : ".";
      }
      art += "\n";
    }
    /* The row-union-column ring, opened over the 260-degree
     * sweep the pointer itself uses. Two things are being pinned:
     *   - ONE pixel thick everywhere except the flat caps. Selecting every
     *     pixel whose distance rounds to r instead gives a unit-wide annulus,
     *     which is 1.41px across at 45 degrees — at r=8 that put two
     *     consecutive 2-pixel runs at each shoulder, stacking into a small
     *     diagonal blob ("little triangles in the corners").
     *   - flat caps at the compass points, because that is where the tangent
     *     is flat. A midpoint walk puts a single pixel there instead, one row
     *     proud of the run behind it, which reads as a spike.
     *   - the GAP at the bottom: the last three rows empty, the track
     *     stopping at dy=+3.5 of its 6.5 radius. A closed ring would draw
     *     track the pointer can never reach and imply the control wraps
     *     around -- and at this radius it would not fit the box either. */
    const want = [
      "......##.##......",
      "....##.....##....",
      "...#.........#...",
      "..#...........#..",
      ".#.............#.",
      ".#.............#.",
      "#...............#",
      "#...............#",
      "#...............#",
      "#...............#",
      "#...............#",
      ".#.............#.",
      ".#.............#.",
      "..#...........#..",
      ".................",
      ".................",
      ".................",
    ].join("\n") + "\n";
    /* The pointer at 0.5 points straight up, so blank the centre column out of
     * the expectation the same way it is blanked out of the art. */
    const wantNoPointer = want.split("\n").map((row, i) =>
      i <= r ? row.slice(0, r) + "." + row.slice(r + 1) : row).join("\n");
    if (art !== wantNoPointer) {
      fail("the knob ring is not the row-union-column circle\n" +
           "got:\n" + art + "want:\n" + wantNoPointer);
    }

    /* Say the defect out loud, independently of the art: no lit ring pixel may
     * stand alone at a COMPASS point. This is the thing users actually see —
     * "the circles have little points on the compass positions" — and it is
     * what both the two-disk fake and the midpoint walk get wrong. */
    const lit = (dx, dy) => !!fb.pixels[(cy + dy) * fb.width + (cx + dx)];
    for (const [dx, dy] of [[0, -r], [0, r], [-r, 0], [r, 0]]) {
      if (!lit(dx, dy)) continue;
      const neighbours = [[1, 0], [-1, 0], [0, 1], [0, -1]]
        .filter(([ax, ay]) => lit(dx + ax, dy + ay)).length;
      if (neighbours === 0) {
        fail("the ring pixel at compass point (" + dx + "," + dy + ") is isolated — " +
             "it reads as a point sticking out of the circle. A flat tangent needs a " +
             "flat run, which is why this is not a midpoint/Bresenham circle.");
      }
    }
  }

  /* ---- 4. the filter roll-off reaches the axis at every value ----------- */
  {
    const meta = {
      cutoff:    { key: "cutoff", type: "float", kind: "number", min: 0, max: 1, step: 0.01 },
      resonance: { key: "resonance", type: "float", kind: "number", min: 0, max: 1, step: 0.01 },
    };
    const metaIndex = { getOrGuess: (k) => meta[k] };
    const W = 64, H16 = 16, botY = 1 + VD.VIZ_ROWS - 1;

    const bottomRowAt = (cut) => {
      const fb = H.createFramebuffer();
      VD.drawVizGroup(H.drawContext(fb), { x: 0, y: 0, w: W, h: H16 },
        { kind: V.VIZ_FILTER, roles: { cutoff: "cutoff", resonance: "resonance" }, slotStart: 0, slotSpan: 2 },
        { cutoff: String(cut), resonance: "0.3" }, metaIndex);
      let deepest = -1;
      for (let y = 0; y < botY; y++) {
        for (let x = 0; x < W; x++) if (fb.pixels[y * fb.width + x]) { deepest = Math.max(deepest, y); break; }
      }
      return deepest;
    };

    /* One detent of a 0..1 float under movy_knob moves 0.005 of the range. A
     * curve that reaches zero bottoms out on the row above the axis at EVERY
     * one of them; the old truncation gave a different row almost every time. */
    const rows = new Set();
    for (let i = 0; i <= 40; i++) rows.add(bottomRowAt(i * 0.005));
    if (rows.size !== 1 || !rows.has(botY - 1)) {
      fail("the filter roll-off does not reach the axis at every detent — " +
           "bottom rows seen: " + [...rows].sort((a, b) => a - b).join(",") +
           " (want only " + (botY - 1) + "). The tail is being truncated at a " +
           "sample point again, which reads as the curve flashing on and off.");
    }
  }

  /* ---- 4b. enum square text stays inside its frame ---------------------- */
  {
    /*
     * The bug this guards: a three-glyph line in a proportional font can
     * measure 15px in a 14px interior ("LOW" has a 5-wide W), and the centring
     * then rounds to a NEGATIVE offset that starts the first glyph ON TOP OF
     * the left frame column. That is a text run escaping its box, and it is
     * still forbidden.
     *
     * WHAT CHANGED. This used to also require the column just INSIDE each frame
     * column to be clear — a 1px margin on top of the frame. SCH-50
     * `thin-frame` spends exactly that margin: the text box now runs to the
     * inside of the border (18px instead of 16), which is the difference
     * between "PAS" and "PA" at three characters, and its own note records the
     * cost as "glyphs now sit one pixel off the frame". Asserting the margin
     * would be asserting the option was not adopted.
     *
     * So the assertion narrows to the invariant that survives: NO TEXT PIXEL ON
     * A FRAME COLUMN. It cannot be read off the page directly, because the
     * frame draws those columns by design — so the text is isolated by
     * differencing a render against a frame-only render of the same cell. What
     * is left is exactly the glyphs, and none of it may sit on x = bx or
     * x = bx + ENUM_W - 1.
     */
    const shapes = ["Low Pass", "High Quality", "Parallel", "Wow Wow", "MMM", "Off", "Sine", "-12"];
    const meta = { key: "m", label: "Mode", type: "enum", kind: "enum", options: shapes, min: 0, max: shapes.length - 1 };
    const page = { kind: P.PAGE_KNOBS, name: "E", level: "root", keys: ["m"] };
    for (let i = 0; i < shapes.length; i++) {
      const fb = H.createFramebuffer();
      RM.renderPageMovy(H.drawContext(fb), {
        page, metaIndex: { getOrGuess: () => meta }, values: { m: String(i) },
        title: "T", pageIndex: 0, pageCount: 1, viz: [],
      });
      const bx = Math.floor((RM.CELL_W - RM.ENUM_W) / 2), by = RM.ROW0_Y;
      const lit = (x, y) => !!fb.pixels[y * fb.width + x];

      /* The same cell drawn with an EMPTY value: frame, notches and nothing
       * else. Anything lit in the real render and not in this one is text. */
      const bare = H.createFramebuffer();
      RM.drawEnumSquare(H.drawContext(bare), bx, by, "");
      const bareLit = (x, y) => !!bare.pixels[y * bare.width + x];

      for (let y = by; y < by + RM.BOX_H; y++) {
        for (const x of [bx, bx + RM.ENUM_W - 1]) {
          if (lit(x, y) && !bareLit(x, y))
            fail("enum square for " + JSON.stringify(shapes[i]) +
              ": a text pixel landed ON the frame column x=" + x + " (row " + y +
              "), so the line is wider than the box and the centring went negative");
        }
      }
      /*
       * ...and the arithmetic behind it, over the WHOLE FLEET rather than these
       * eight strings.
       *
       * The pixel check above cannot currently fail, and saying so is the point.
       * `enumSquareLines` splits a value into lines of at most three characters,
       * and three of the widest glyph this face has ("MMM") measures 17px — one
       * short of the 18px interior. So no reachable value can reach the frame,
       * and a pixel assertion over any fixed list of strings is a check that
       * passes because its input is too weak, which is the failure mode this
       * repo has shipped before.
       *
       * What CAN change is the budget. `thin-frame` widened ENUM_TEXT_W from
       * ENUM_W - 4 to ENUM_W - 2, which is exactly the interior; one more step
       * and the fitter would permit a line the box cannot hold. So the invariant
       * is asserted directly, and the widest line the fleet can actually produce
       * is measured against it rather than assumed.
       */
      if (i === 0) {
        const interior = RM.ENUM_W - 2;
        if (RM.ENUM_TEXT_W > interior)
          fail("ENUM_TEXT_W is " + RM.ENUM_TEXT_W + " but the box interior is only " + interior +
               "px — the fitter will pass a line the frame cannot hold, and the centring goes negative");
        let widest = 0, worst = "";
        for (const mod of fx.modules) {
          const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
          const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
          for (const page of pages) {
            if (page.kind !== P.PAGE_KNOBS) continue;
            for (const k of (page.keys || [])) {
              if (!k) continue;
              for (const o of (metaIndex.getOrGuess(k).options || [])) {
                for (const ln of F5.enumSquareLines(String(o))) {
                  const w = F4.fontWidth4x5(ln);
                  if (w > widest) { widest = w; worst = ln; }
                }
              }
            }
          }
        }
        if (widest > interior)
          fail("the fleet produces an enum line " + JSON.stringify(worst) + " measuring " + widest +
               "px, wider than the " + interior + "px box interior");
        if (widest < 8)
          fail("the widest enum line in the fleet is only " + widest + "px — the sweep found nothing, " +
               "so this assertion is measuring an empty set");
      }
      /* Vertical margins must match. Both contents are an ODD number of rows
       * (one line 5, two lines 11), so an even interior can never split its
       * remainder evenly — that is why BOX_H is odd. */
      let top = -1, bot = -1;
      for (let y = by + 1; y < by + RM.BOX_H - 1; y++) {
        let any = false;
        for (let x = bx + 1; x < bx + RM.ENUM_W - 1; x++) if (lit(x, y)) { any = true; break; }
        if (any) { if (top < 0) top = y; bot = y; }
      }
      if (top >= 0) {
        const above = top - by - 1, below = (by + RM.BOX_H - 1) - bot - 1;
        if (above !== below) {
          fail("enum square for " + JSON.stringify(shapes[i]) + " is not vertically centred: " +
               above + "px above the text, " + below + "px below. BOX_H must be odd so an " +
               "odd content height splits its remainder evenly.");
        }
      }
    }
  }

  /* ---- 4c. boxed text is centred, consistently ---------------------------- */
  {
    /* Centring on a midpoint (kx + KW/2) rather than on the SPAN put the
     * extra pixel on whichever side rounding happened to fall: "KIC" sat 3px
     * from the left frame and 2px from the right. The rule now is that an odd
     * leftover always goes right, so every widget disagrees the same way.
     *
     * This assertion used to scan the whole box and take the extreme lit pixel
     * on each side. That measured the FRAME, not the text: the frame is
     * symmetric by construction, so it reported left=1 right=1 for every input
     * and could never fail. The opaque box has no frame now (the divable
     * brackets are its frame), which is what exposed it.
     *
     * So measure the INK against the text span it was centred into. The
     * tolerance is 2px rather than 1 because glyph side bearings are not
     * symmetric — a trailing "." inks the left of its 6px advance, so the
     * ink box is narrower than the advance box on that side, and no
     * placement rule can make the two agree. */
    const meta = { key: "p", label: "Sample", kind: "opaque", type: "string" };
    const page = { kind: P.PAGE_KNOBS, name: "O", level: "root", keys: ["p"] };
    const paths = ["/s/kick_01.wav", "/x/hall.wav", "/a/b.wav", "/q/ir.wav", "/z/mmm.wav"];
    for (const v of paths) {
      const fb = H.createFramebuffer();
      RM.renderPageMovy(H.drawContext(fb), {
        page, metaIndex: { getOrGuess: () => meta }, values: { p: v },
        title: "T", pageIndex: 0, pageCount: 1, viz: [],
      });
      const bx = Math.floor((RM.CELL_W - RM.KW) / 2), by = RM.ROW0_Y;
      /* The span drawOpaqueBox centres into: frame inset of 2 on each side. */
      const spanX = bx + 2, spanW = RM.KW - 4;
      /* Rows strictly inside the widget band, so the bracket runs on the first
       * and last row of the band are never counted as text. */
      let inkL = 99, inkR = -1;
      for (let y = by + 1; y < by + RM.BOX_H - 1; y++) {
        for (let x = spanX; x < spanX + spanW; x++) {
          if (fb.pixels[y * fb.width + x]) {
            if (x < inkL) inkL = x;
            if (x > inkR) inkR = x;
          }
        }
      }
      if (inkR < 0) continue;                          /* nothing drawn */
      if (inkL < spanX || inkR > spanX + spanW - 1) {
        fail("opaque box text for " + JSON.stringify(v) + " escapes its span: ink " +
             inkL + ".." + inkR + " against span " + spanX + ".." + (spanX + spanW - 1));
      }
      const inkMid = (inkL + inkR) / 2, spanMid = spanX + (spanW - 1) / 2;
      if (Math.abs(inkMid - spanMid) > 2) {
        fail("opaque box text for " + JSON.stringify(v) + " is off centre: ink centre " +
             inkMid + " against span centre " + spanMid);
      }
    }
  }

  /* ---- 4d. the touched highlight has a clear row above and below --------- */
  {
    /* The inverted strip is drawn behind the value while a knob is held. At
     * LBL_H=6 it inverted 6 rows for 5 rows of glyph, and the remainder landed
     * entirely BELOW: zero clear rows on top, so the letters ran into the edge
     * of the highlight and the strip read as a smudge. An odd band splits its
     * remainder evenly. Text sits in colour 0 on a filled band, so a
     * "text row" here is one containing an UNLIT pixel.
     *
     * THE SCAN IS BOUNDED BY THE STRIP, NOT BY THE CELL, and that distinction
     * is the whole reason this block needed rewriting for SCH-50 `half-strip`.
     * The strip used to span all 32 columns, so "an unlit pixel anywhere in the
     * cell" and "an unlit pixel inside the highlight" were the same question.
     * Now the strip is sized to the VALUE, so the cell is mostly unlit ground
     * either side of it — scanning the cell reported every row as a text row,
     * giving 0 clear above and 0 below on a layout that is in fact unchanged.
     * A measurement can break without the thing it measures breaking.
     *
     * The strip extent is recovered as the union of lit pixels in the band,
     * and the two outermost columns are then excluded: the corners are NOTCHED
     * (four deliberately-cleared pixels), and counting a notch as a glyph would
     * mark the strip own top and bottom rows as text and reintroduce exactly
     * the false negative above. */
    const keys = ["a", "b", "c", "d"];
    const metas = Object.fromEntries(keys.map((k) => [k,
      { key: k, label: k, type: "float", kind: "number", min: 0, max: 1, step: 0.01 }]));
    for (const [slot, val] of [[0, "0.43"], [3, "1.00"], [2, "0"]]) {
      const fb = H.createFramebuffer();
      RM.renderPageMovy(H.drawContext(fb), {
        page: { kind: P.PAGE_KNOBS, name: "M", level: "root", keys },
        metaIndex: { getOrGuess: (k) => metas[k] },
        values: { a: val, b: val, c: val, d: val },
        title: "T", pageIndex: 0, pageCount: 1, touched: slot, viz: [],
      });
      const cx = slot * RM.CELL_W;
      let sx = Infinity, ex = -Infinity;
      for (let y = RM.LBL0_Y; y < RM.LBL0_Y + RM.LBL_H; y++) {
        for (let x = cx; x < cx + RM.CELL_W; x++) {
          if (fb.pixels[y * fb.width + x]) { if (x < sx) sx = x; if (x > ex) ex = x; }
        }
      }
      if (!isFinite(sx)) fail("touched highlight for slot " + slot + " drew no strip at all");
      if (ex - sx < 2) fail("touched highlight for slot " + slot + " is only " +
        (ex - sx + 1) + " column(s) wide — too narrow to carry a value");
      const rows = [];
      for (let y = RM.LBL0_Y; y < RM.LBL0_Y + RM.LBL_H; y++) {
        let hasText = false;
        for (let x = sx + 1; x <= ex - 1; x++) if (!fb.pixels[y * fb.width + x]) { hasText = true; break; }
        rows.push(hasText);
      }
      const first = rows.indexOf(true), last = rows.lastIndexOf(true);
      if (first < 0) fail("touched highlight for slot " + slot + " drew no text at all");
      const above = first, below = rows.length - 1 - last;
      if (above < 1 || below < 1) {
        fail("touched highlight for slot " + slot + " has " + above + " clear row(s) above the " +
             "text and " + below + " below — the glyphs touch the edge of the inverted strip, " +
             "which is what makes it illegible. LBL_H must leave one on each side.");
      }
      if (above !== below) {
        fail("touched highlight for slot " + slot + " is not vertically centred: " +
             above + " above, " + below + " below. LBL_H must be odd.");
      }
    }

    /*
     * ---- 4e. the modulation tilde survives a cell that is BOTH held and
     * modulated, which is the state neither swatch nor page render shows.
     *
     * The tilde is drawn six pixels left of the value run. While the strip
     * spanned the whole cell it was always ON the strip, so drawing it in
     * colour 0 was right and `inverted` was the only question. SCH-50
     * `half-strip` sizes the strip to the value, so for anything shorter than
     * the cell the mark now lands on BLACK GROUND — and colour 0 there is
     * invisible. The catalog option has exactly that bug; the shipping widget
     * keys the polarity on the strip extent instead.
     *
     * Asserted as "some ink appears where the mark goes", against the same cell
     * rendered unmodulated, so it cannot pass by counting the strip.
     */
    {
      const keys2 = ["a", "b", "c", "d"];
      const metas2 = Object.fromEntries(keys2.map((k) => [k,
        { key: k, label: k, type: "float", kind: "number", min: 0, max: 1, step: 0.01 }]));
      const render = (mod) => {
        const fb = H.createFramebuffer();
        RM.renderPageMovy(H.drawContext(fb), {
          page: { kind: P.PAGE_KNOBS, name: "M", level: "root", keys: keys2 },
          metaIndex: { getOrGuess: (k) => metas2[k] },
          values: { a: "1.00", b: "1.00", c: "1.00", d: "1.00" },
          title: "T", pageIndex: 0, pageCount: 1, touched: 0, viz: [],
          modulated: mod ? () => true : undefined,
        });
        return fb;
      };
      const plain = render(false), marked2 = render(true);
      let extra = 0;
      for (let y = RM.LBL0_Y; y < RM.LBL0_Y + RM.LBL_H; y++) {
        for (let x = 0; x < RM.CELL_W; x++) {
          const i = y * plain.width + x;
          if (!!marked2.pixels[i] !== !!plain.pixels[i]) extra++;
        }
      }
      if (extra < 4) {
        fail("a cell that is both HELD and MODULATED shows only " + extra + " pixel(s) of " +
             "difference from the unmodulated one — the modulation tilde is being drawn in " +
             "the ground colour on ground, so it has vanished on exactly the cells that need it");
      }
    }
  }

  /* ---- 5. LFO waves are the shape they claim, at every rate -------------- */
  {
    /* An earlier version of this check measured only the wave EXTENT (topmost
     * and bottommost lit row). That is stable even when the shape is visibly
     * wrong, which is how "the LFOs are wiggly" got past it. These assertions
     * are about SHAPE. */
    const f = (k) => ({ key: k, label: k, type: "float", kind: "number", min: 0, max: 1, step: 0.01 });
    const shapes = ["Sine", "Triangle", "Saw", "Square"];
    const sh = { key: "sh", label: "sh", type: "enum", kind: "enum", options: shapes, min: 0, max: shapes.length - 1 };
    const metaIndex = { getOrGuess: (k) => (k === "sh" ? sh : f(k)) };
    const WIDTH = 128;

    const draw = (shapeIdx, rate) => {
      const fb = H.createFramebuffer();
      VD.drawVizGroup(H.drawContext(fb), { x: 0, y: 0, w: WIDTH, h: 16 },
        { kind: V.VIZ_LFO, roles: { shape: "sh", rate: "r", depth: "d" }, slotStart: 0, slotSpan: 4 },
        { sh: String(shapeIdx), r: String(rate), d: "1" }, metaIndex);
      return fb;
    };
    /* y of the CURVE in each column. drawLfo also lays a dotted centre axis
     * along the baseline — every second column at row AXIS_Y — which would
     * otherwise be counted as curve and mask the very stepping this is
     * looking for. It is drawn only on even columns, so dropping that one
     * pixel there removes it exactly; on odd columns a baseline pixel can
     * only be the curve, and is kept. */
    const AXIS_Y = 1 + ((VD.VIZ_ROWS - 1) >> 1);
    const profile = (fb, dropAxis) => {
      const out = [];
      for (let x = 0; x < WIDTH; x++) {
        const ys = [];
        for (let y = 0; y < 16; y++) {
          if (!fb.pixels[y * fb.width + x]) continue;
          if (dropAxis && y === AXIS_Y && x % 2 === 0) continue;
          ys.push(y);
        }
        out.push(ys);
      }
      return out;
    };
    /* Which profile is sound for which question: dropping the axis is right
     * for COUNTING steps (it would otherwise add a phantom one to every other
     * column) but wrong for finding GAPS, because where the curve crosses the
     * baseline its only pixel IS at the axis row and would be discarded as
     * one. For gaps the raw profile is the safe direction — a constant-row
     * axis can only ever mask a discontinuity, never invent one. */

    /*
     * A square wave must have VERTICAL edges. Before drawStepCurve its
     * transition was smeared diagonally across a whole ~5px sample step.
     *
     * SCH-50 `ghost-fill` BROKE THE MEASUREMENT, not the drawing. This used to
     * count LIT PIXELS PER COLUMN, on the assumption that a column`s lit set is
     * the curve and nothing else. The graph now fills its mass with CHECKER
     * between the curve and the baseline, so every column has a handful of lit
     * pixels and the old count reported 126 partial steps on a square that is
     * still perfectly square. Reading the old numbers as a regression and
     * "fixing" the renderer would have been the wrong move entirely.
     *
     * Two measures replace it, both valid with a fill and without one:
     *
     *   runLen  the longest CONTIGUOUS lit run in a column. A checker fill
     *           alternates, so it can never exceed 2; a vertical riser is drawn
     *           SOLID, so it is the band height. The two do not overlap.
     *   curveY  the lit pixel FURTHEST FROM THE AXIS in a column. The fill only
     *           ever lies between the curve and the axis, so the extreme is on
     *           the curve by construction — with a fill or without one.
     *
     * A true square puts every column at the same extreme (the two plateaux are
     * symmetric about the axis), so a diagonal smear shows up as columns whose
     * extreme sits short of it. That is the same defect the old count caught,
     * measured on the shape instead of on the ink.
     */
    const AMP_SLACK = 1;
    for (const rate of [0, 0.25, 0.5, 0.75, 1]) {
      const fb = draw(3, rate);
      const runs = [], dist = [];
      for (let x = 0; x < WIDTH; x++) {
        let best = 0, cur = 0, far = -1;
        for (let y = 0; y < 16; y++) {
          const on = !!fb.pixels[y * fb.width + x];
          cur = on ? cur + 1 : 0;
          if (cur > best) best = cur;
          if (!on) continue;
          if (y === AXIS_Y && x % 2 === 0) continue;   /* dotted axis */
          const d = Math.abs(y - AXIS_Y);
          if (d > far) far = d;
        }
        runs.push(best); dist.push(far);
      }
      /* One interior edge at a single cycle (the wrap is off-screen), more as
       * rate raises the cycle count. */
      const tall = runs.filter((n) => n >= 10).length;
      if (tall < 1) {
        fail("the square LFO at rate " + rate + " has " + tall + " full-height column(s) — " +
             "its edges are being drawn as diagonals instead of vertical risers");
      }
      const amp = Math.max.apply(null, dist);
      const short = dist.filter((d) => d >= 0 && d < amp - AMP_SLACK).length;
      if (short > 8) {
        fail("the square LFO at rate " + rate + " has " + short + " column(s) whose curve sits " +
             "short of the plateau (amplitude " + amp + ") — a square has two edges, not a staircase");
      }
    }

    /* At full depth a wave must fill its band EXACTLY: peak on topY, trough on
     * botY, nothing outside. The band used to be 14 rows with no centre row,
     * so `round((1+14)/2)` put the axis half a row low and the trough landed
     * at round(14.5)=15 — one row below the box, the stray jag under a
     * triangle. An odd band centres exactly. */
    for (const [name, idx] of [["sine", 0], ["triangle", 1], ["saw", 2]]) {
      for (const rate of [0, 0.5, 1]) {
        const fb = draw(idx, rate);
        const topY = 1, botY = 1 + VD.VIZ_ROWS - 1;
        let hi = 99, lo = -1;
        for (let y = 0; y < 16; y++) {
          for (let x = 0; x < WIDTH; x++) {
            if (!fb.pixels[y * fb.width + x]) continue;
            if (y === AXIS_Y && x % 2 === 0) continue;      /* dotted axis */
            if (y < hi) hi = y;
            if (y > lo) lo = y;
            break;
          }
        }
        if (hi < topY || lo > botY) {
          fail("the " + name + " LFO at rate " + rate + " draws outside its band: rows " +
               hi + ".." + lo + ", band is " + topY + ".." + botY);
        }
        if (hi !== topY || lo !== botY) {
          fail("the " + name + " LFO at rate " + rate + " does not fill its band at full " +
               "depth: rows " + hi + ".." + lo + ", band is " + topY + ".." + botY +
               ". VIZ_ROWS must be odd so the axis is a real row.");
        }
      }
    }

    /* The single-knob SILHOUETTE must not bracket itself. Closing the cycle at
     * both ends of the box drew a full-height bar down each side, so a saw
     * read as a ramp inside a frame and a square as a rectangle outline. The
     * edge columns of a waveform are single-valued; only a discontinuity
     * INSIDE the window is a riser. */
    for (const [name, idx] of [["sine", 0], ["triangle", 1], ["saw", 2], ["square", 3]]) {
      const wsh = { key: "w", label: "w", type: "enum", kind: "enum", options: shapes, min: 0, max: 3 };
      const fb = H.createFramebuffer(32, 16);
      VD.drawVizGroup(H.drawContext(fb), { x: 0, y: 0, w: 32, h: 16 },
        { kind: V.VIZ_WAVEFORM, roles: { value: "w" }, slotStart: 0, slotSpan: 1 },
        { w: String(idx) }, { getOrGuess: () => wsh });
      /*
       * ODD PARITY ONLY, for the same reason the LFO budget below is halved:
       * the silhouette now carries the CHECKER mass too, and CHECKER lights
       * only where (x + y) is EVEN — so every odd-parity pixel is stroke and
       * the fill cannot contribute one.
       *
       * Counting all ink here made the SAW fail: at the left edge its curve is
       * at the extreme, so the fill legitimately spans from there to the centre
       * line and reads as a bar to a total-ink check. What this assertion is
       * actually for is a CLOSED CYCLE — a full-height stroke down the box edge
       * framing the shape — and that is 13 rows, about 6 of them odd, so the
       * same threshold still catches it while ignoring the fill.
       */
      const colStroke = (x) => {
        let n = 0;
        for (let y = 0; y < 16; y++) if (fb.pixels[y * fb.width + x] && ((x + y) & 1)) n++;
        return n;
      };
      /* drawWaveform insets by 2, so the body runs x=2..29 */
      for (const x of [2, 29]) {
        if (colStroke(x) > 3) {
          fail("the " + name + " silhouette has a " + colStroke(x) + "px vertical STROKE at its " +
               (x === 2 ? "left" : "right") + " edge (column " + x + ") — the cycle is being " +
               "closed at the box edge, which frames the shape instead of drawing it");
        }
      }
    }

    /* Lines must be ONE pixel thick. A monotonic ramp drawn as a proper
     * staircase lights about one pixel per column; if a connector re-draws the
     * row the previous column already covered, every step comes out two
     * columns wide and the count jumps by roughly the row count instead. That
     * is what made a triangle read as a chunky zigzag rather than a line. */
    {
      const wsh = { key: "w", label: "w", type: "enum", kind: "enum", options: shapes, min: 0, max: 3 };
      const SAW = 2, body = 28;                       /* drawWaveform insets by 2 */
      const fb = H.createFramebuffer(32, 16);
      VD.drawVizGroup(H.drawContext(fb), { x: 0, y: 0, w: 32, h: 16 },
        { kind: V.VIZ_WAVEFORM, roles: { value: "w" }, slotStart: 0, slotSpan: 1 },
        { w: String(SAW) }, { getOrGuess: () => wsh });
      /* Odd parity, halved budget — same reasoning as the edge check above and
       * the LFO budget below: the fill is even-parity by construction, so this
       * counts stroke only and a double-width staircase still doubles it. */
      let n = 0;
      for (let y = 0; y < 16; y++) for (let x = 0; x < 32; x++)
        if (fb.pixels[y * fb.width + x] && ((x + y) & 1)) n++;
      if (n > (body + 2) / 2) {
        fail("the saw silhouette lights " + n + " odd-parity (stroke) pixels across " + body +
             " columns — a 1px staircase needs about one per two columns at this parity, so the " +
             "line is being drawn double-width");
      }
      /*
       * The same shape through the LFO renderer, which has its own riser.
       *
       * COUNTED BY PARITY, because SCH-50 `ghost-fill` put a CHECKER mass under
       * the curve and a total-ink budget can no longer tell a double-width
       * staircase from a fill. Checker lights only pixels where (x + y) is
       * EVEN, so every ODD-parity lit pixel is stroke — the fill cannot
       * contribute one. Measured across all four shapes the odd-parity stroke
       * ink is 58-68 where the whole stroke is ~149, i.e. almost exactly half,
       * which is what the budget below is halved against. A staircase drawn
       * double-width doubles the stroke and lands near 130, so the two stay far
       * apart.
       *
       * The silhouette check above needs none of this: drawWaveform is not one
       * of the four graphs that took a fill, so its ink is still all stroke.
       */
      const fb2 = H.createFramebuffer(128, 16);
      VD.drawVizGroup(H.drawContext(fb2), { x: 0, y: 0, w: 128, h: 16 },
        { kind: V.VIZ_LFO, roles: { shape: "sh", rate: "r", depth: "d" }, slotStart: 0, slotSpan: 4 },
        { sh: String(SAW), r: "0", d: "1" }, metaIndex);
      let m = 0;
      for (let y = 0; y < 16; y++) {
        for (let x = 0; x < 128; x++) {
          if (!fb2.pixels[y * fb2.width + x]) continue;
          if (y === AXIS_Y && x % 2 === 0) continue;   /* dotted axis */
          if ((x + y) % 2 === 0) continue;             /* could be the checker fill */
          m++;
        }
      }
      if (m > Math.ceil((128 + VD.VIZ_ROWS + 8) / 2) + 4) {
        fail("the saw LFO lights " + m + " odd-parity (stroke-only) pixels across 128 columns — " +
             "the riser is re-drawing the row its run already covered, so the staircase is double-width");
      }
    }

    /* A sine and a triangle must be single-valued and continuous in every
     * column: no column carrying two separate strokes, no horizontal gap. */
    for (const [name, idx] of [["sine", 0], ["triangle", 1]]) {
      for (const rate of [0, 0.2, 0.4, 0.6, 0.8, 1]) {
        const cols = profile(draw(idx, rate), false);
        for (let x = 1; x < WIDTH - 1; x++) {
          if (cols[x].length === 0) fail("the " + name + " LFO at rate " + rate + " has a gap at column " + x);
        }
        /* Contiguity: consecutive columns must overlap or touch, or the riser
         * is missing and the wave reads as broken dashes. */
        for (let x = 1; x < WIDTH - 1; x++) {
          const a = cols[x - 1], b = cols[x];
          const near = a.some((ya) => b.some((yb) => Math.abs(ya - yb) <= 1));
          if (!near) fail("the " + name + " LFO at rate " + rate + " jumps discontinuously at column " + x);
        }
      }
    }
  }

  /* ---- the modulation dot ---------------------------------------------
   *
   * A modulated knob shows TWO values: the pointer stays on the base you
   * dialled in, and a dot rides the arc at whatever a source is currently
   * driving it to. Without both you cannot see what you set, because turning
   * the knob edits the base and the pointer would be chasing the LFO.
   *
   * Cheap to draw and expensive to feed — 487ns for the fill_rect against
   * ~2.8ms to learn the value — so the cost lives in the controller fast
   * lane, not here.
   */
  {
    const KEYS = ["cutoff", "res", "timbre", "color", "attack", "decay", "tune", "gain"];
    const META = {};
    for (const k of KEYS) META[k] = { key: k, label: k, type: "float", kind: "number", min: 0, max: 1, step: 0.01 };
    const base = {
      page: { kind: P.PAGE_KNOBS, name: "MOD", level: "root", keys: KEYS },
      metaIndex: { getOrGuess: (k) => META[k] },
      values: Object.fromEntries(KEYS.map((k) => [k, "0.5"])),
      title: "S1 > MOD", pageIndex: 0, pageCount: 1, touched: -1, viz: [],
    };
    const draw = (extra) => {
      const fb = H.createFramebuffer();
      RM.renderPageMovy(H.drawContext(fb), Object.assign({}, base, extra));
      return fb;
    };

    const plain = draw({});
    const dotted = draw({ modValues: { cutoff: "0.9" } });
    const added = dotted.countLit() - plain.countLit();
    if (added <= 0) fail("the modulation dot drew nothing");
    if (added > 8) fail("the modulation dot drew " + added + " pixels — it should be a 2x2 mark, not a blob");

    /* Coincident with the pointer, the dot is drawn ANYWAY.
     *
     * This used to assert the opposite -- suppressed within 0.02 of the base,
     * because a dot under the pointer only thickens it. True about the pixels
     * and wrong about the meaning: with it gone the knob is pixel-identical to
     * one nothing is driving, so it reads as "there is no LFO" rather than
     * "the LFO is at its base". Reported from the device, where a bipolar LFO
     * on a knob at 0 clamps there for half of every cycle and the indicator
     * blinked out for that half.
     *
     * Pinned as a DIFFERENCE from the unmodulated render rather than as an
     * exact pixel count, which is what test_mod_dot_at_base.sh measures per
     * position. */
    if (draw({ modValues: { cutoff: "0.5" } }).countLit() === plain.countLit()) {
      fail("a modulation dot coincident with the base was suppressed — the knob " +
           "is then indistinguishable from an unmodulated one");
    }

    /* Both rails, every knob — the arc is what it rides, so an off-by-one in
     * the angle maths puts it off the display. */
    for (const v of ["0", "1"]) {
      const fb = draw({ modValues: Object.fromEntries(KEYS.map((k) => [k, v])) });
      if (fb.clipped() > 0) fail("modulation dots drew outside the display at value " + v);
    }
    console.log("PASS: Movy modulation dot — rides the arc, visible even AT the base, never clipped");
  }

  /* ---- displayFor: a value the HOST resolves, per surface ---------------
   *
   * An LFO target is stored as "fx1" and means "Room Size on the Freeverb in
   * FX 1". Only the host can know that, so the renderer takes an optional
   * formatter — and it has to reach BOTH the header (76px, which is the point
   * of resolving at all) and the cell (30px, which gets the short form). The
   * wiring is the risk: an override honoured in one place and not the other
   * looks fine on screen and is wrong in exactly the case it exists for.
   *
   * Compared as PIXELS against the same page drawn without a formatter, so
   * this cannot pass by the string being computed and then dropped.
   */
  {
    const key = "target";
    const meta = { key: key, label: "Targ", type: "string", kind: "opaque" };
    const metaIndex = { getOrGuess: () => meta };
    const page = { kind: "knobs", name: "LFO 1", level: "lfo1",
                   keys: [key, null, null, null, null, null, null, null] };
    const render = (opts) => {
      const fb = H.createFramebuffer();
      RM.renderPageMovy(H.drawContext(fb), Object.assign({
        page: page, metaIndex: metaIndex, values: { target: "fx1" },
        title: "S1", pageIndex: 0, pageCount: 3, touched: -1,
        modulated: () => false, modValues: {}, pageGroups: [], viz: [],
      }, opts));
      return fb;
    };
    const band = (fb, y0, y1) => {
      let n = 0;
      for (let y = y0; y < y1; y++) for (let x = 0; x < 128; x++) if (fb.pixels[y * fb.width + x]) n++;
      return n;
    };

    const fmt = (k, raw, surface) =>
      (k === key ? (surface === "header" ? "FX 1: Room Size" : "Room Size") : null);

    /* The CELL, untouched: the opaque box shows the value, and the label band
     * shows the value while held. Both must change. */
    const plainCell = render({});
    const fmtCell = render({ displayFor: fmt });
    if (band(plainCell, RM.ROW0_Y, RM.LBL0_Y + RM.LBL_H) === band(fmtCell, RM.ROW0_Y, RM.LBL0_Y + RM.LBL_H)) {
      fail("displayFor never reached the cell — it still drew the stored key");
    }

    /* The HEADER, while the knob is held. */
    const plainHdr = render({ touched: 0, touchedSlots: [0] });
    const fmtHdr = render({ touched: 0, touchedSlots: [0], displayFor: fmt });
    if (band(plainHdr, 0, RM.HEADER_H) === band(fmtHdr, 0, RM.HEADER_H)) {
      fail("displayFor never reached the held-knob header — the surface with the room for it");
    }

    /* Returning null must leave the ordinary path EXACTLY as it was: this is
     * an opt-in for one key, not a new display path for every param. */
    const nulled = render({ displayFor: () => null });
    if (nulled.countLit() !== plainCell.countLit()) {
      fail("a displayFor returning null changed the drawing — the fallback is not identical");
    }
    if (fmtCell.clipped() > 0 || fmtHdr.clipped() > 0) {
      fail("a host-resolved value drew outside the display");
    }
  }

  console.log("PASS: Movy layout — " + rendered + " page renders through the native draw context, " +
              "nothing off-screen, knob ring is a true circle, filter roll-off reaches the axis at every detent");
});
'
