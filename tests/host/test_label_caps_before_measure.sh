#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The cell draws UPPER CASE, so the width that matters is the upper-case one.
#
# labelForCell used to capitalise AFTER shortenLabel had measured the
# mixed-case string against the cell budget. The two widths differ in BOTH
# directions in font4x5, which is why this was a bug and not a rounding
# detail:
#
#   - upper case is narrower for an ordinary word -- "Pitch" is 24px where
#     "PITCH" is 20px -- so a label was cut to fit a width it would have fitted
#     anyway. 351 fleet labels lost a character (STEP for STEPS).
#   - upper case is WIDER once shortenLabel has devowelled, because the letters
#     devowel keeps are the narrow lowercase ones: "Ecldrm" fits, "ECLDRM" is
#     30px in a 29px cell. 68 fleet labels overran their budget and ate the
#     gutter between neighbouring cells.
#
# The second half is invisible in code review and is the reason this is pinned
# by MEASURING every fleet label rather than by checking a handful of strings.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/font4x5.mjs"),
]).then(([R, F]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* The budget labelForCell itself uses, derived the same way rather than
   * hard-coded, so this test follows a cap change instead of breaking on it. */
  const budget = Math.min(R.CELL_W - 2, F.fontWidth4x5("M".repeat(R.LABEL_CHARS)));

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

  /* ---- 1. NOTHING may render wider than the budget --------------------
   *
   * This is the assertion that catches the devowel half. It cannot be
   * satisfied by measuring the mixed-case string, because the string that is
   * actually drawn is the capitalised one. */
  const over = [];
  for (const t of labels) {
    const shown = R.labelForCell(t);
    const w = F.fontWidth4x5(shown);
    if (w > budget) over.push(t + " -> " + shown + " (" + w + "px > " + budget + ")");
  }
  if (over.length)
    fail(over.length + " fleet labels render wider than the " + budget +
         "px budget, e.g.\n      " + over.slice(0, 5).join("\n      "));

  /* ---- 2. a word that FITS in caps must not be cut --------------------
   *
   * Chosen from words with no WORD_ABBREV entry, so this measures the
   * capitalisation order and not the mnemonic table. Asserted as "not a
   * truncation of itself" rather than as an exact string, so it holds at
   * either cap. */
  for (const word of ["Steps", "Voicing", "Evolve"]) {
    const shown = R.labelForCell(word);
    const upper = word.toUpperCase();
    if (F.fontWidth4x5(upper) <= budget && shown !== upper)
      fail("\"" + word + "\" fits as " + upper + " (" + F.fontWidth4x5(upper) +
           "px in " + budget + "px) but renders as " + shown +
           " -- it was measured in mixed case");
  }

  /* ---- 3. and the fold really is happening before the fit ------------
   *
   * A direct probe: a string whose mixed-case width exceeds the budget while
   * its upper-case width does not must survive whole. */
  {
    const probe = ["Pitch", "Grain", "Drive", "Steps"].find(
      (w) => F.fontWidth4x5(w) > budget && F.fontWidth4x5(w.toUpperCase()) <= budget);
    if (!probe)
      console.log("  .. no probe word at this cap: every candidate fits in both cases");
    else if (R.labelForCell(probe).length < probe.length &&
             !R.WORD_ABBREV[probe.toLowerCase()])
      fail("\"" + probe + "\" is wider than the budget in mixed case and fits in " +
           "caps, but still came back shortened -- the fold is after the fit");
  }

  console.log("  ok  " + labels.size + " fleet labels, none wider than the " + budget + "px budget");
  console.log("  ok  a word that fits in caps is not cut for its mixed-case width");
  console.log("PASS: labels are measured in the case they are drawn in");
});
'
