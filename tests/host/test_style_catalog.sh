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
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
