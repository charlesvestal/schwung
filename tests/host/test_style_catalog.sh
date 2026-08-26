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
