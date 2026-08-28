#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# `short_name`: a cell label that differs from the full name.
#
# The cell is five characters and the header is the width of the screen, so
# they want different words. Without a way to say so the only lever is the name
# itself, and shortening that fixes the cell by damaging the header and the
# list -- "Osc 1 Pitch" has to become "Pitch" everywhere, and on a synth with
# four oscillators the header can then no longer tell you which one you hold.
#
# Same split as short_options, which does this for enum VALUES already.
#
# The three assertions that matter are: the cell takes it, the header does NOT,
# and a module that declares nothing is completely unaffected.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
]).then(([M, R]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const meta = (p) => M.buildMetaIndex({ chainParams: [p] }).getOrGuess(p.key);

  const full = { key: "osc1_pitch", name: "Osc 1 Pitch", type: "float", min: 0, max: 1 };
  const withShort = Object.assign({}, full, { short_name: "Pitch" });

  /* The cell takes the short one. */
  const m2 = meta(withShort);
  const cell = R.labelForCell(m2.short_name || m2.label || m2.key);
  if (cell !== "PITCH") fail("the cell should draw PITCH, got " + cell);

  /* The header does not: it has the room and it is what disambiguates. */
  if ((m2.label || m2.key) !== "Osc 1 Pitch")
    fail("the full name must survive for the header, got " + (m2.label || m2.key));

  /* Without it, nothing changes -- this is the whole fleet today. */
  const m1 = meta(full);
  if (m1.short_name !== undefined) fail("short_name should be absent when not declared");
  const plain = R.labelForCell(m1.short_name || m1.label || m1.key);
  if (plain !== R.labelForCell("Osc 1 Pitch"))
    fail("a module declaring nothing must be unaffected, got " + plain);
  if (plain === "PITCH") fail("the fallback must not silently shorten anything");

  /* It is a LABEL, so the same squeezing applies -- a long short_name is not a
   * way to smuggle six characters into a five-character cell. */
  const long = meta(Object.assign({}, full, { short_name: "Resonance Amount" }));
  const drawn = R.labelForCell(long.short_name);
  if (drawn.length > 8) fail("short_name must still be fitted to the cell, got " + drawn);

  console.log("PASS: short_name shortens the cell, leaves the header alone, "
            + "and is inert when absent");
});
'
