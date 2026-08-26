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

  /* ---- font options must not reproduce the shipping Elektron glyphs ----
   * font4x5.mjs documents nine letterforms as read straight off Elektron
   * screen. Replacing them is the concrete part of this issue, so it is
   * asserted rather than assumed. */
  const F4 = await import("./src/shared/param_pages/font4x5.mjs");
  const fontSet = S.SETS.find((s) => s.kind === S.KIND_FONT);
  if (fontSet) {
    const CH = F4.CHARS;
    const ELEKTRON = "ADEILMPTU";
    for (const o of fontSet.options) {
      if (o.glyphs.length !== CH.length)
        fail(o.id + ": " + o.glyphs.length + " glyphs, want " + CH.length);
      for (const g of o.glyphs) {
        if (!Array.isArray(g) || g.length < 4)
          fail(o.id + ": a glyph is malformed");
      }
      for (const letter of ELEKTRON) {
        const i = CH.indexOf(letter);
        if (i < 0) continue;
        if (JSON.stringify(o.glyphs[i]) === JSON.stringify(F4.GLYPHS_FOR_TEST[i]))
          fail(o.id + ": glyph " + letter + " is byte-identical to font4x5, which is the thing being replaced");
      }
    }
    console.log("PASS: font sets differ from font4x5");
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
    if (files.length !== 10) fail("expected 10 font variant files, found " + files.length);
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
    /* 10 files x 58 glyphs: CHARS is 59 long and the space carries no
     * picture, so it is not one of them. */
    if (checked !== 580) fail("matched " + checked + " glyph lines, expected 580");
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
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
