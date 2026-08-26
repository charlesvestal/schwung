#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Structural invariants over the SCH-50 style option catalog.
#
# The catalog is data, not behaviour, so what can go wrong is structural: a
# set that lost an option, two options claiming the same axis position, a
# font option that is a copy of the shipping font. None of that is visible in
# a rendered contact sheet, which is exactly why it is asserted here.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the style catalog tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/styles/index.mjs"),
  import("./src/shared/param_pages/styles/dither.mjs"),
]).then(async ([S, D]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  const problems = S.validateAll();
  if (problems.length) fail("structural problems:\n  " + problems.join("\n  "));
  console.log("PASS: " + S.SETS.length + " set(s) structurally valid");

  /* ---- dither densities ---- */
  const density = (pred) => {
    let on = 0;
    for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) if (pred(x, y)) on++;
    return on / 4096;
  };
  const near = (got, want, tol, what) => {
    if (Math.abs(got - want) > tol) fail(what + " density " + got.toFixed(3) + ", want ~" + want);
  };
  near(density(D.SOLID), 1, 0.001, "SOLID");
  near(density(D.DIAG_HEAVY), 0.75, 0.03, "DIAG_HEAVY");
  near(density(D.CHECKER), 0.5, 0.03, "CHECKER");
  near(density(D.DIAG_THIRD), 1 / 3, 0.03, "DIAG_THIRD");
  near(density(D.DIAG_LIGHT), 0.25, 0.03, "DIAG_LIGHT");
  near(density(D.DOTS(3)), 1 / 9, 0.03, "DOTS3");

  /* ---- predicates are screen-space: same coords, same answer, and the
   * answer varies with position rather than being constant ---- */
  for (const [name, pred] of [["CHECKER", D.CHECKER], ["DIAG_LIGHT", D.DIAG_LIGHT], ["DIAG_THIRD", D.DIAG_THIRD]]) {
    if (pred(10, 10) !== pred(10, 10)) fail(name + " is not pure");
    const row = [0, 1, 2, 3, 4].map((x) => pred(x, 0));
    if (row.every((v) => v === row[0])) fail(name + " looks constant along x");
  }

  /* ---- the DIAGONAL is the point, and density cannot see it ----
   *
   * Every assertion above is invariant to orientation. Mistype DIAG_LIGHT as
   * (x % 4) === 0 and you get vertical stripes that still measure exactly
   * 0.25, are still pure, and still vary along x: the whole suite passes on a
   * visibly wrong fill. So assert the property these patterns exist FOR.
   *
   * The (x + y) family is constant along the anti-diagonal, because moving
   * +1 in x and -1 in y leaves the sum unchanged. No axis-aligned pattern
   * satisfies that, which is exactly what makes it the discriminating test. */
  for (const [name, pred] of [["CHECKER", D.CHECKER], ["DIAG_LIGHT", D.DIAG_LIGHT],
                              ["DIAG_HEAVY", D.DIAG_HEAVY], ["DIAG_THIRD", D.DIAG_THIRD]]) {
    for (let x = 3; x < 12; x++) for (let y = 3; y < 12; y++) {
      if (pred(x + 1, y - 1) !== pred(x, y))
        fail(name + " is not constant along the anti-diagonal at " + x + "," + y +
             " -- it is axis-aligned, not a diagonal hatch");
    }
  }

  /* DOTS is a lattice rather than a hatch, so it must NOT satisfy the above.
   * Asserting the negative keeps the check honest: a predicate that returned
   * a constant would pass the diagonal test for every pattern at once. */
  {
    const d = D.DOTS(3);
    let same = true;
    for (let x = 3; x < 12 && same; x++) for (let y = 3; y < 12; y++)
      if (d(x + 1, y - 1) !== d(x, y)) { same = false; break; }
    if (same) fail("DOTS(3) behaves like a diagonal hatch, so the diagonal test proves nothing");
  }

  /* ---- fillDithered never clears ---- */
  const H = await import("./tools/param-pages/harness.mjs");
  const fb = H.createFramebuffer(16, 16);
  const ctx = H.drawContext(fb);
  ctx.fillRect(0, 0, 16, 16, 1);
  const before = fb.countLit();
  D.fillDithered(ctx, 0, 0, 16, 16, D.CHECKER);
  if (fb.countLit() !== before) fail("fillDithered cleared pixels, it must only set them");

  /* ---- fillTerrain fills DOWN to the bottom edge ---- */
  const fb2 = H.createFramebuffer(8, 10);
  const ctx2 = H.drawContext(fb2);
  D.fillTerrain(ctx2, 0, 0, 8, 10, new Array(8).fill(0.5), D.SOLID, true);
  if (!fb2.pixels[9 * 8 + 0]) fail("fillTerrain did not reach the bottom edge");
  if (fb2.pixels[0 * 8 + 0]) fail("fillTerrain filled above the curve");

  console.log("PASS: dither densities");

  /* ---- every draw option stays inside its own surface ----
   *
   * For a knob that surface is the KW x BOX_H widget box: one row of overflow
   * lands on the label row, which the grid does not repaint, so it shows up on
   * hardware and nowhere else.
   *
   * Not every set replaces a knob, and the ones that do not have neither that
   * signature nor that surface -- a fader takes a viz rect and a metaIndex, a
   * footer takes a hint list, an opaque cell takes a value and an override. So
   * a set declares `probeSize` and `probe`, and this asserts against WHAT THE
   * SET SAYS ITS SURFACE IS. That is the same pair the catalog renders its
   * swatch through, deliberately: if the judged surface and the asserted
   * surface could differ, this test would prove nothing about what the contact
   * sheet shows.
   *
   * The footer set is where this earns its keep. Its surface is the nine rows
   * RULE_Y..63, so an option that reaches up into the label strip or down off
   * the bottom of the screen fails here rather than silently overprinting. */
  const RM = await import("./src/shared/param_pages/render_page_movy.mjs");
  for (const set of S.SETS) {
    if (set.kind !== S.KIND_DRAW) continue;
    const size = set.probeSize || { w: RM.KW, h: RM.BOX_H };
    for (const o of set.options) {
      for (const v of [0, 0.25, 0.5, 0.75, 1]) {
        const wfb = H.createFramebuffer(size.w, size.h);
        const wctx = H.drawContext(wfb);
        if (typeof set.probe === "function") set.probe(wctx, o.draw, v);
        else o.draw(wctx, 0, 0, v);
        if (wfb.clipped() !== 0)
          fail(set.id + "/" + o.id + " at v=" + v + " drew " + wfb.clipped() + " pixel(s) outside its box");
        if (wfb.countLit() === 0)
          fail(set.id + "/" + o.id + " at v=" + v + " drew nothing at all");
      }
    }
  }
  console.log("PASS: draw options stay in their boxes");

  /* ---- nobody may reproduce the nine RETIRED glyphs, SHIPPING INCLUDED ----
   *
   * font4x5.mjs used to draw nine letterforms that SCH-50 retired and replaced
   * (the `metric-matched` option was adopted). The shipping table therefore can
   * no longer serve as the definition of "the thing being replaced" -- comparing
   * against it would now assert that no catalog option matches the ADOPTED one,
   * which is backwards and would fail on the option that was chosen.
   *
   * The nine forms are therefore pinned LITERALLY here, copied out of the
   * pre-SCH-50 table, and the assertion runs over the catalog options AND over
   * font4x5 itself. That is the assertion the comment always meant: what must
   * not come back is those nine drawings, not whatever font4x5 holds today.
   * (No apostrophes in this file -- the node script is a single-quoted bash
   * string and one apostrophe ends it.) */
  const F4 = await import("./src/shared/param_pages/font4x5.mjs");
  const RETIRED_GLYPHS = {
    A: [5, 0, 4, 5, 6, 9, 15, 9, 9],
    D: [5, 0, 4, 5, 7, 9, 9, 9, 7],
    E: [5, 0, 4, 5, 15, 1, 7, 1, 15],
    I: [2, 0, 1, 5, 1, 1, 1, 1, 1],
    L: [5, 0, 4, 5, 1, 1, 1, 1, 15],
    M: [6, 0, 5, 5, 17, 27, 21, 17, 17],
    P: [5, 0, 4, 5, 7, 9, 7, 1, 1],
    T: [4, 0, 3, 5, 7, 2, 2, 2, 2],
    U: [5, 0, 4, 5, 9, 9, 9, 9, 6],
  };
  const fontSet = S.SETS.find((s) => s.kind === S.KIND_FONT);
  if (fontSet) {
    const CH = F4.CHARS;
    /*
     * THE SHIPPING TABLE IS EXEMPT, AND THAT IS THE POINT OF THIS COMMENT.
     *
     * This asserted non-identity over every table INCLUDING font4x5 itself,
     * and the assertion caused the damage it was meant to prevent. At 4x5 with
     * an advance pinned to the old value, these letters have essentially one
     * legible form: a 1px-wide I is a 5px vertical bar and there is no second
     * drawing of it. Forcing difference forced WRONGNESS -- shipped as a T with
     * a doubled crossbar that read as a 2, an I a row short of the baseline, an
     * A and an E with the middle bar one row high, an M that lost its apex and
     * a U squared into a box. Every defect was the same move, and every one
     * passed this suite, because the sibling assertion checks the ASCII picture
     * against the numbers and both were wrong together.
     *
     * The spec already carries the argument that resolves it, in the section on
     * convergent idioms: the 1px corner notch is kept because "the constraint,
     * not the designer, produced it". A letterform the cell admits only one of
     * is the same case. That reasoning was applied to notches and not to
     * glyphs, which is how an absolute got written here.
     *
     * So: the CATALOG options must still differ -- they exist to offer
     * alternatives, and an alternative identical to the incumbent is not one.
     * The shipping table is judged on legibility instead, which is what the
     * device actually cares about, and which the structural checks below and
     * the picture/number check cover.
     */
    const tables = fontSet.options.map((o) => [o.id, o.glyphs]);
    for (const [id, glyphs] of tables.concat([["font4x5.mjs (SHIPPING)", F4.GLYPHS_FOR_TEST]])) {
      if (glyphs.length !== CH.length)
        fail(id + ": " + glyphs.length + " glyphs, want " + CH.length);
      for (const g of glyphs) {
        if (!Array.isArray(g) || g.length < 4)
          fail(id + ": a glyph is malformed");
      }
    }
    for (const [id, glyphs] of tables) {
      for (const letter of Object.keys(RETIRED_GLYPHS)) {
        const i = CH.indexOf(letter);
        if (i < 0) continue;
        if (JSON.stringify(glyphs[i]) === JSON.stringify(RETIRED_GLYPHS[letter]))
          fail(id + ": glyph " + letter + " is byte-identical to the retired letterform; a catalog OPTION has design freedom and must use it");
      }
    }

    /*
     * What the shipping table is held to instead: no letter may sit off the
     * baseline, and no full-width bar may be doubled. Those are the two shapes
     * every one of the shipped defects took, so this is the assertion that
     * would have caught them.
     */
    const LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (const c of LETTERS) {
      const i = CH.indexOf(c);
      if (i < 0) continue;
      const g = F4.GLYPHS_FOR_TEST[i];
      const w = g[2], rows = g.slice(4);
      if (rows.length && rows[rows.length - 1] === 0)
        fail("shipping glyph " + c + " has a blank BOTTOM row, so it sits high off the shared baseline");
      if (rows.length && rows[0] === 0)
        fail("shipping glyph " + c + " has a blank TOP row, so it sits low against its neighbours");
      const full = (1 << w) - 1;
      for (let k = 1; k < rows.length; k++)
        if (rows[k] === full && rows[k - 1] === full && w >= 3)
          fail("shipping glyph " + c + " has a DOUBLED full-width bar at rows " + (k - 1) + "/" + k +
               "; that is what made T read as a 2");
    }
    console.log("PASS: catalog fonts differ from the retired forms, shipping font is legible");
  }

  /* ---- a metric-matched option must actually match the metrics ----
   *
   * One option in the set is defined by a promise about its NUMBERS rather
   * than about its shapes: every advance equal to the shipping advance for the
   * same character, so adopting it moves no text anywhere in the product. That
   * is the whole reason it exists, and it is invisible -- a specimen sheet
   * cannot show it, and a glyph nudged by one column looks fine and quietly
   * costs KEYTRIG the 32px cell it fits in today.
   *
   * Keyed off the flag rather than off the id, so a second metric-matched
   * option inherits the check instead of needing the test edited. */
  if (fontSet) {
    let checkedMM = 0;
    for (const o of fontSet.options) {
      if (!o.metricMatched) continue;
      for (let i = 0; i < F4.CHARS.length; i++) {
        const got = o.glyphs[i][0], want = F4.GLYPHS_FOR_TEST[i][0];
        if (got !== want)
          fail(o.id + " declares metricMatched but glyph " + JSON.stringify(F4.CHARS[i]) +
               " advances " + got + " where font4x5 advances " + want);
      }
      checkedMM++;
    }
    if (checkedMM === 0)
      fail("no font option declares metricMatched -- the option that pins the shipping advances is gone");
    console.log("PASS: " + checkedMM + " metric-matched font option(s) match font4x5 advances exactly");
  }

  /* ---- a glyph must be able to DRAW what it declares ----
   *
   * A table is data, and the two ways it goes wrong are both invisible in a
   * rendered specimen. A row bit set past the declared width draws nothing at
   * all, because the blit scans while col < w -- so the glyph silently loses
   * a stroke. A row count that disagrees with h reads undefined off the end,
   * which is falsy, so every glyph in the option quietly drops its last row
   * and a font one row short still looks like a font.
   *
   * The advance check is the third: an advance equal to the body width welds
   * every glyph to its neighbour, and the shipping measurement function
   * subtracts one from the total on the assumption that the advance already
   * carries the inter-glyph gap. */
  if (fontSet) {
    for (const o of fontSet.options) {
      for (let i = 0; i < o.glyphs.length; i++) {
        const g = o.glyphs[i];
        const adv = g[0], w = g[2], h = g[3];
        const at = o.id + ": glyph " + JSON.stringify(F4.CHARS[i]);
        /* A zero-width glyph is the space: font4x5 spells it [3, 0, 0, 5],
         * h rows of nothing, and the blit reads undefined for each of them
         * and skips. Only a glyph with a BODY has to carry its rows. */
        if (w > 0 && g.length !== 4 + h)
          fail(at + " declares h=" + h + " but carries " + (g.length - 4) + " row(s)");
        if (w > 0 && adv <= w)
          fail(at + " has advance " + adv + " for a " + w + "px body, so it would touch its neighbour");
        for (let r = 0; r < h; r++)
          if (g[4 + r] >> w)
            fail(at + " row " + r + " sets a bit past its declared width " + w);
      }
    }
    console.log("PASS: font glyph tables are self-consistent");
  }

  /* ---- the picture and the numbers must still agree ----
   *
   * Every row in those tables carries the authored form as a trailing
   * comment, and the legend at the top of each file says the numbers are
   * derived FROM the picture. That is a claim about two representations of
   * the same glyph living in one line, and the failure mode is the ordinary
   * one: somebody nudges a bit to fix a letterform and leaves the picture
   * describing the letter that used to be there. Nothing downstream reads
   * the comment, so the drift is silent and permanent, and the next person
   * to edit that glyph is working from a wrong drawing.
   *
   * Checked against the SOURCE rather than the imported table, because the
   * comment does not survive the import. */
  {
    const fs = await import("node:fs");
    const dir = "./src/shared/param_pages/styles/font";
    const files = fs.readdirSync(dir).filter((f) => f.endsWith(".mjs") &&
                                                    f !== "index.mjs" && f !== "blit.mjs");
    if (files.length !== 11) fail("expected 11 font variant files, found " + files.length);
    let checked = 0;
    for (const f of files) {
      const src = fs.readFileSync(dir + "/" + f, "utf8");
      for (const line of src.split("\n")) {
        const m = line.match(/^\s*\[([0-9,\s]+)\],\s*\/\*\s*(\S+)\s+([#.\s]+?)\s*\*\/\s*$/);
        if (!m) continue;
        const nums = m[1].split(",").map((n) => parseInt(n, 10));
        const rows = m[3].split(/\s+/);
        const w = nums[2], h = nums[3];
        if (rows.length !== h)
          fail(f + " " + m[2] + ": picture has " + rows.length + " row(s), table says h=" + h);
        for (let r = 0; r < h; r++) {
          let bits = 0;
          if (rows[r].length !== w)
            fail(f + " " + m[2] + ": picture row " + r + " is " + rows[r].length +
                 "px, table says w=" + w);
          for (let c = 0; c < w; c++) if (rows[r][c] === "#") bits |= (1 << c);
          if (bits !== nums[4 + r])
            fail(f + " " + m[2] + " row " + r + ": picture says " + bits +
                 ", table says " + nums[4 + r] + " -- the two have drifted apart");
        }
        checked++;
      }
    }
    /* 11 files x 58 glyphs: CHARS is 59 long and the space carries no
     * picture, so it is not one of them. */
    if (checked !== 638) fail("matched " + checked + " glyph lines, expected 638");
    console.log("PASS: font glyph pictures match their numbers");
  }

  /* ---- Bradley-Terry recovers a ranking it was given ----
   * A fit that silently returned input order, or noise, would otherwise look
   * fine on real data where nobody knows the true answer. */
  const R = await import("./tools/param-pages/rank.mjs");
  const rows = [];
  let seed = 12345;
  const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  for (let i = 0; i < 400; i++) {
    const a = Math.floor(rnd() * 5), b = Math.floor(rnd() * 5);
    if (a === b) continue;
    const better = a > b ? "a" : "b";
    const worse = better === "a" ? "b" : "a";
    rows.push({ set: "t", a: "o" + a, b: "o" + b, winner: rnd() < 0.9 ? better : worse });
  }
  const fitted = R.fit(rows);
  const order = fitted.map((f) => f.id);
  if (order[0] !== "o4" || order[order.length - 1] !== "o0")
    fail("bradley-terry did not recover the planted order, got " + order.join(" > "));

  /* skips must not count as evidence for either side */
  const withSkips = rows.concat(Array.from({ length: 200 }, () =>
    ({ set: "t", a: "o0", b: "o4", winner: "skip" })));
  const fitted2 = R.fit(withSkips);
  if (fitted2.map((f) => f.id).join() !== order.join())
    fail("skips changed the ranking, they must be counted but not fitted");

  /* zero judgements must not crash */
  const empty = R.fit([]);
  if (!Array.isArray(empty)) fail("fit([]) did not return an array");

  console.log("PASS: bradley-terry recovers a known ranking");

  /* ---- the four curve sets render ONE curve ----
   *
   * Sets 7-10 vary treatment only: the shape of an ADSR, a filter response, an
   * LFO wave and a sample waveform is a picture of the maths and is not this
   * catalog to change. The mechanism is that all four register the SAME
   * treatment functions from viz_treatments.mjs and inject their own heights,
   * so an option cannot reach the curve. Both the mechanism and the property
   * are asserted, because they fail differently: a wrapper added in a set
   * module breaks identity, and a treatment that drew from something other
   * than the array it was handed breaks the profile.
   */
  const HN = await import("./tools/param-pages/harness.mjs");
  const VT = await import("./src/shared/param_pages/styles/viz_treatments.mjs");
  const CURVE_SETS = ["viz_envelope", "viz_filter", "viz_lfo", "viz_sample"];
  const curveSets = CURVE_SETS.map((id) => S.setById(id) || fail("missing curve set " + id));

  const CW = 128, BAND = 13;
  const CH2 = Array.from({ length: CW }, (_, i) => 0.5 + 0.45 * Math.sin((i / (CW - 1)) * Math.PI * 2.5));
  const COPTS = { baseFrac: 0.5, mirror: false };

  const renderCurve = (draw) => {
    const cfb = HN.createFramebuffer(CW, 15);
    draw(HN.drawContext(cfb), { x: 0, y: 0, w: CW, h: BAND }, CH2, COPTS);
    if (cfb.clipped()) fail("a curve treatment drew outside its band");
    return cfb;
  };
  /* Topmost lit pixel per column: the curve, independent of whatever fill sits
   * under it. Two treatments that differ only in fill share this exactly. */
  const profileOf = (draw) => {
    const pfb = renderCurve(draw);
    const top = new Array(CW).fill(-1);
    for (let x = 0; x < CW; x++)
      for (let y = 0; y < 15; y++) if (pfb.pixels[y * CW + x]) { top[x] = y; break; }
    return top.join(",");
  };

  for (let pos = 1; pos <= S.OPTIONS_PER_SET; pos++) {
    const opts = curveSets.map((s) => s.options.find((o) => o.position === pos)
      || fail(s.id + " has no option at position " + pos));
    const ids = new Set(opts.map((o) => o.id));
    if (ids.size !== 1) fail("position " + pos + " is a different treatment per set: " + [...ids].join(", "));
    if (new Set(opts.map((o) => o.draw)).size !== 1)
      fail("position " + pos + " (" + [...ids][0] + ") is not one shared draw function");
    const profiles = new Set(opts.map((o) => profileOf(o.draw)));
    if (profiles.size !== 1)
      fail("position " + pos + " (" + [...ids][0] + ") draws a different curve in different sets");
    if ([...profiles][0].split(",").every((v) => v === "-1")) fail("position " + pos + " drew nothing");
  }

  /* All ten being one function would also satisfy everything above, which is
   * the failure mode of a copy-paste. Ten distinct functions, and enough
   * distinct ink for the set to be an axis rather than a repetition. */
  if (new Set(VT.VIZ_TREATMENTS.map((t) => t.draw)).size !== S.OPTIONS_PER_SET)
    fail("the ten treatments are not ten distinct functions");
  const inks = new Set(VT.VIZ_TREATMENTS.map((t) => renderCurve(t.draw).countLit()));
  if (inks.size < 5) fail("the ten treatments are barely distinguishable: " + inks.size + " distinct ink counts");
  console.log("PASS: four curve sets share one curve and ten distinct treatments");

  /* ---- the TWO TEXT-BEARING sets are measured against EVERY FACE ----
   *
   * enum_square and label_cell are the only sets whose options are sized
   * around a string, and set 12 replaces the letterforms outright. So a pick
   * in one set can break a pick in the other, silently and only on hardware:
   * the option that fitted in the catalog was measured against the shipping
   * face, and the face is exactly what the font set proposes to change.
   *
   * Measured, not estimated. Each option declares the pixel budget its draw
   * function actually leaves for text, and this walks every font variant with
   * that set realistic probe string. Where an option genuinely cannot hold the
   * string on a variant, the variant is DECLARED on the option, and the
   * comparison is an exact set match rather than a floor -- a floor would pass
   * by being loose, and an option that quietly started overflowing one more
   * face would never be noticed.
   *
   * Both directions fail. Declaring an overflow that is not real is as wrong
   * as missing one, because the note beside it tells a reviewer which faces
   * the option survives. */
  const BL = await import("./src/shared/param_pages/styles/font/blit.mjs");
  const F53 = await import("./src/shared/param_pages/font5x3.mjs");
  if (!fontSet) fail("the font set is required to check text fit across faces");
  const FONT_N = Number.isInteger(fontSet.optionCount) ? fontSet.optionCount : S.OPTIONS_PER_SET;
  if (fontSet.options.length !== FONT_N)
    fail("expected " + FONT_N + " font variants to measure against");

  /* An enum square does not print its value -- it prints the two short lines
   * enumSquareLines splits it into, so the string that has to fit is a LINE.
   * A label band prints the whole run, and it prints two different ones: the
   * name at rest and the value while held. */
  const probeStrings = (set) => {
    if (Array.isArray(set.fontProbe)) return set.fontProbe;
    return F53.enumSquareLines(set.fontProbe).filter((s) => s && s.length);
  };

  for (const id of ["enum_square", "label_cell"]) {
    const tset = S.setById(id) || fail("missing text-bearing set " + id);
    if (!tset.fontProbe) fail(id + ": no fontProbe declared, so nothing can be measured");
    const strings = probeStrings(tset);
    if (!strings.length) fail(id + ": fontProbe produced no strings");
    for (const o of tset.options) {
      if (!Number.isFinite(o.textW))
        fail(id + "/" + o.id + ": no textW budget declared");
      if (o.textW <= 0) fail(id + "/" + o.id + ": textW budget is not positive");
      const measured = [];
      for (const fo of fontSet.options) {
        let w = 0;
        for (const s of strings) w = Math.max(w, BL.fontWidth(fo.glyphs, s));
        if (w > o.textW) measured.push(fo.id);
      }
      const declared = Array.isArray(o.overflowFonts) ? o.overflowFonts.slice() : [];
      if (measured.slice().sort().join(",") !== declared.sort().join(","))
        fail(id + "/" + o.id + " (budget " + o.textW + "px) overflows [" +
             measured.join(",") + "] but declares [" + declared.join(",") + "]");
    }
  }
  console.log("PASS: text-bearing options measured against all " + FONT_N + " font variants");

  /* ---- a label option must not touch the row above its band ----
   *
   * There is no gutter: BOX_H ends and LBL_H begins, and the grid does not
   * repaint a widget when only its label changes. One row of overflow lands on
   * the bottom of a knob and STAYS there, which is a defect visible on
   * hardware and nowhere else -- not in a swatch, not in code review.
   *
   * The sentinel is a pattern rather than a solid row, and it is compared
   * pixel for pixel, because the two ways to damage it are opposite: a strip
   * SETS pixels there and a notch or a cleared plate CLEARS them. A row that
   * was all lit would only catch the second, and a row that was all dark would
   * only catch the first. Both states of the cell are driven, since the
   * touched treatment is where every option grows. */
  {
    const lset = S.setById("label_cell");
    const geom = { x0: 0, cellW: RM.CELL_W };
    const SURF_H = RM.LBL_H + 2;
    for (const o of lset.options) {
      for (const inv of [false, true]) {
        const lfb = H.createFramebuffer(RM.CELL_W, SURF_H);
        const lctx = H.drawContext(lfb);
        for (let x = 0; x < RM.CELL_W; x++) {
          if ((x % 3) !== 0) continue;
          lctx.fillRect(x, 0, 1, 1, 1);
          lctx.fillRect(x, SURF_H - 1, 1, 1, 1);
        }
        const above = [], below = [];
        for (let x = 0; x < RM.CELL_W; x++) {
          above.push(lfb.pixels[x]);
          below.push(lfb.pixels[(SURF_H - 1) * RM.CELL_W + x]);
        }
        o.draw(lctx, geom, 0, 1, "CUTOF", "-24.0", inv, inv, true);
        for (let x = 0; x < RM.CELL_W; x++) {
          if (lfb.pixels[x] !== above[x])
            fail("label_cell/" + o.id + (inv ? " touched" : " resting") +
                 " changed the widget row above its band at x=" + x);
          if (lfb.pixels[(SURF_H - 1) * RM.CELL_W + x] !== below[x])
            fail("label_cell/" + o.id + (inv ? " touched" : " resting") +
                 " changed the row below its band at x=" + x);
        }
        if (lfb.clipped() !== 0)
          fail("label_cell/" + o.id + " drew outside the surface entirely");
      }
    }
    console.log("PASS: no label option encroaches on the widget row above it");
  }

  /* ---- motion options: start where they were, land where they were sent ----
   *
   * A trajectory is data, so what goes wrong is arithmetic, and both failures
   * are invisible in a rendered strip. A first frame that is not the OLD value
   * means the widget teleports before it eases, which on a strip just looks
   * like a shorter animation. A last frame that only APPROACHES the target --
   * which is what 1 - exp(-t*d)*cos(t*f) does by construction -- leaves a
   * parameter resting a fraction off the number the encoder was turned to; a
   * knob one percent short is one pixel of pointer and looks correct.
   *
   * from greater than to, and from equal to to, are both in the matrix. A
   * trajectory built for a value going up is easy to write in a way that
   * misbehaves going down, and an easing applied to a change that did not
   * happen must still be a flat line rather than a wobble around nothing. */
  for (const set of S.SETS) {
    if (set.kind !== S.KIND_MOTION) continue;
    for (const o of set.options) {
      for (const [a, b, n] of [[0.15, 0.85, 12], [0.85, 0.15, 12], [0.5, 0.5, 12],
                               [0, 1, 3], [1, 0, 20], [0.2, 0.21, 6]]) {
        const at = set.id + "/" + o.id + " (" + a + " to " + b + ", n=" + n + ")";
        const f = o.frames(a, b, n);
        if (!Array.isArray(f) || f.length !== n)
          fail(at + " returned " + (Array.isArray(f) ? f.length : typeof f) + " frame(s), want " + n);
        if (f[0] !== a) fail(at + " starts at " + f[0] + ", must start at the old value");
        if (Math.abs(f[n - 1] - b) > 0.01)
          fail(at + " settles at " + f[n - 1] + ", which is not the target -- it needs a final-frame clamp");
        for (const v of f) if (typeof v !== "number" || v !== v) fail(at + " produced a non-number");
        if (JSON.stringify(o.frames(a, b, n)) !== JSON.stringify(f))
          fail(at + " is not deterministic");
      }
      /* A change that did not happen must not move. */
      const flat = o.frames(0.42, 0.42, 8);
      if (flat.some((v) => v !== 0.42))
        fail(set.id + "/" + o.id + " moves when the value did not change");
    }
    console.log("PASS: motion options start at the old value and settle on the target");
  }
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
