#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Touching a knob must not change its label.
#
# The knob card raises a magnified copy of the row you are touching, drawn by
# the SAME drawKnobRow at a narrower cell -- 29px against the grid's 32px. The
# label budget was min(cellW - 2, cap), so the card's came out 27px against the
# grid's 29px and 39% of fleet labels rendered differently in the two. The
# label changed under your finger, and the card -- which exists to show you
# MORE -- showed less.
#
# The cap now governs and the cell is only an overflow guard, so the two agree
# by construction. The cost, measured rather than waved past: at a 29px cell a
# full-width label leaves no gutter, and 24% of adjacent label pairs in the
# fleet come within 2px of each other in the card. That was accepted
# deliberately -- a label you have to re-read is worse than a tight one.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/knob_card.mjs"),
  import("./src/shared/param_pages/font4x5.mjs"),
]).then(([R, K, F]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* The card cell, derived the way the card derives it -- not a literal, so
   * this follows a card resize instead of going stale against one. */
  const cardCell = Math.floor(K.knobCardContentW() / 4);
  if (!(cardCell > 0 && cardCell < R.CELL_W))
    fail("the card cell (" + cardCell + ") is not narrower than the grid cell (" +
         R.CELL_W + ") -- this test has nothing to compare");

  const j = JSON.parse(require("fs").readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const labels = new Set();
  const walk = (o) => {
    if (!o || typeof o !== "object") return;
    if (Array.isArray(o)) return o.forEach(walk);
    for (const [k, v] of Object.entries(o)) {
      if ((k === "name" || k === "label") && typeof v === "string" && v) labels.add(v);
      walk(v);
    }
  };
  walk(j);

  /* ---- every fleet label, identical on both surfaces ----------------- */
  const diff = [];
  for (const t of labels) {
    const grid = R.labelForCell(t);
    const card = R.labelForCell(t, cardCell);
    if (grid !== card) diff.push(t + ": grid " + grid + " / card " + card);
  }
  if (diff.length)
    fail(diff.length + " of " + labels.size + " labels change when you touch the knob, e.g.\n      " +
         diff.slice(0, 5).join("\n      "));

  /* ---- and nothing may spill out of the narrower cell ----------------
   *
   * The min() against the cell is kept precisely as this guard. A surface
   * narrower than the cap must clip the label, not draw past its cell -- in
   * the card that would mean drawing through its own border. */
  for (const t of labels) {
    const w = F.fontWidth4x5(R.labelForCell(t, cardCell));
    if (w > cardCell)
      fail("\"" + t + "\" renders " + w + "px into a " + cardCell +
           "px card cell -- it would draw through the card border");
  }
  /* Explicitly at a cell narrower than the cap, which is the case the guard
   * exists for and which no current surface exercises. */
  for (const narrow of [20, 12, 6]) {
    for (const t of ["Rotation", "Blank B", "Polyphony"]) {
      const w = F.fontWidth4x5(R.labelForCell(t, narrow));
      if (w > narrow)
        fail("at a " + narrow + "px cell, \"" + t + "\" renders " + w + "px -- the " +
             "overflow guard is gone");
    }
  }

  console.log("  ok  all " + labels.size + " fleet labels identical on grid and card");
  console.log("  ok  nothing spills its cell, including at cells narrower than the cap");
  console.log("PASS: touching a knob does not change its label");
});
'
