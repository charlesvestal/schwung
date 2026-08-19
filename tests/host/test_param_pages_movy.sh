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
     *   - the GAP at the bottom: the last three rows empty, matching
     *     the Elektron track, which stops at dy=+3.5 of its 6.5 radius. A
     *     closed ring would draw track the pointer can never reach and imply
     *     the control wraps around. */
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

  /* ---- 4b. enum square text stays inside its frame, with a margin -------- */
  {
    /* This is what a "1px margin" has to mean mechanically: no text pixel on
     * the frame columns, and none on the column just inside them either. The
     * bug it guards was worse than a tight margin — a three-glyph line in a
     * proportional font can measure 15px in a 14px interior ("LOW" has a
     * 5-wide W), and the centring then rounded to a NEGATIVE offset that
     * started the first glyph on top of the left frame column. */
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
      /* The frame columns (bx, bx+W-1) are drawn by design. The margin is the
       * column just inside each of them, which must be clear on every row
       * between the top and bottom frame rows. */
      for (let y = by + 1; y < by + RM.BOX_H - 1; y++) {
        for (const x of [bx + 1, bx + RM.ENUM_W - 2]) {
          if (lit(x, y)) fail("enum square for " + JSON.stringify(shapes[i]) +
            ": text reaches the margin column x=" + x + " (row " + y + "), so it is " +
            "flush against the frame");
        }
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
     * leftover always goes right, so every widget disagrees the same way. */
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
      let left = 99, right = 99;
      for (let y = by + 1; y < by + RM.KW - 1; y++) {
        for (let x = bx + 1; x < bx + RM.KW - 1; x++) {
          if (fb.pixels[y * fb.width + x]) { left = Math.min(left, x - bx); break; }
        }
        for (let x = bx + RM.KW - 2; x > bx; x--) {
          if (fb.pixels[y * fb.width + x]) { right = Math.min(right, (bx + RM.KW - 1) - x); break; }
        }
      }
      if (left === 99) continue;                       /* nothing drawn */
      if (Math.abs(left - right) > 1) {
        fail("opaque box for " + JSON.stringify(v) + " is off centre: " + left +
             "px from the left frame, " + right + "px from the right");
      }
      if (left > right) {
        fail("opaque box for " + JSON.stringify(v) + " leans RIGHT (" + left + "/" + right +
             "); an odd leftover pixel must always go to the right, so every widget " +
             "rounds the same way");
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
     * "text row" here is one containing an UNLIT pixel. */
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
      const rows = [];
      for (let y = RM.LBL0_Y; y < RM.LBL0_Y + RM.LBL_H; y++) {
        let hasText = false;
        for (let x = cx; x < cx + RM.CELL_W; x++) if (!fb.pixels[y * fb.width + x]) { hasText = true; break; }
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

    /* A square wave must have VERTICAL edges. Before drawStepCurve its
     * transition was smeared diagonally across a whole ~5px sample step. */
    for (const rate of [0, 0.25, 0.5, 0.75, 1]) {
      const cols = profile(draw(3, rate), true);
      /* One interior edge at a single cycle (the wrap is off-screen), more as
       * rate raises the cycle count. */
      const tall = cols.filter((ys) => ys.length >= 10).length;
      if (tall < 1) {
        fail("the square LFO at rate " + rate + " has " + tall + " full-height column(s) — " +
             "its edges are being drawn as diagonals instead of vertical risers");
      }
      const wide = cols.filter((ys) => ys.length > 1 && ys.length < 10).length;
      if (wide > 8) {
        fail("the square LFO at rate " + rate + " has " + wide + " partially-stepped columns — " +
             "a square has two edges, not a staircase");
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
      const colHeight = (x) => {
        let n = 0;
        for (let y = 0; y < 16; y++) if (fb.pixels[y * fb.width + x]) n++;
        return n;
      };
      /* drawWaveform insets by 2, so the body runs x=2..29 */
      for (const x of [2, 29]) {
        if (colHeight(x) > 3) {
          fail("the " + name + " silhouette has a " + colHeight(x) + "px vertical bar at its " +
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
      let n = 0;
      for (let y = 0; y < 16; y++) for (let x = 0; x < 32; x++) if (fb.pixels[y * fb.width + x]) n++;
      if (n > body + 2) {
        fail("the saw silhouette lights " + n + " pixels across " + body + " columns — a 1px " +
             "staircase needs about one per column, so the line is being drawn double-width");
      }
      /* The same shape through the LFO renderer, which has its own riser. */
      const fb2 = H.createFramebuffer(128, 16);
      VD.drawVizGroup(H.drawContext(fb2), { x: 0, y: 0, w: 128, h: 16 },
        { kind: V.VIZ_LFO, roles: { shape: "sh", rate: "r", depth: "d" }, slotStart: 0, slotSpan: 4 },
        { sh: String(SAW), r: "0", d: "1" }, metaIndex);
      let m = 0;
      for (let y = 0; y < 16; y++) {
        for (let x = 0; x < 128; x++) {
          if (!fb2.pixels[y * fb2.width + x]) continue;
          if (y === AXIS_Y && x % 2 === 0) continue;   /* dotted axis */
          m++;
        }
      }
      if (m > 128 + VD.VIZ_ROWS + 8) {
        fail("the saw LFO lights " + m + " pixels across 128 columns — the riser is re-drawing " +
             "the row its run already covered, so the staircase is double-width");
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

    /* Coincident with the pointer it says nothing and just thickens it. */
    if (draw({ modValues: { cutoff: "0.5" } }).countLit() !== plain.countLit()) {
      fail("a modulation dot equal to the base value must be suppressed");
    }

    /* Both rails, every knob — the arc is what it rides, so an off-by-one in
     * the angle maths puts it off the display. */
    for (const v of ["0", "1"]) {
      const fb = draw({ modValues: Object.fromEntries(KEYS.map((k) => [k, v])) });
      if (fb.clipped() > 0) fail("modulation dots drew outside the display at value " + v);
    }
    console.log("PASS: Movy modulation dot — rides the arc, suppressed at base, never clipped");
  }

  console.log("PASS: Movy layout — " + rendered + " page renders through the native draw context, " +
              "nothing off-screen, knob ring is a true circle, filter roll-off reaches the axis at every detent");
});
'
