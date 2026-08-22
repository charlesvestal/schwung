#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# WORD_ABBREV — the per-word mnemonic table the knob grid applies before it
# squeezes a label into a cell.
#
# The table exists because the fallback is worse than truncation. shortenLabel
# drops vowels, so an uncovered word becomes a non-word: "Rotation" -> ROTATN,
# "Mod Sens A" -> MOSEA, "Wave Group" -> WGROU. Widening the cell does not fix
# that — it buys a longer non-word — which is why the answer to a cramped label
# is an entry here and not another pixel.
#
# Pinned as PROPERTIES, not as a fixture of expected strings. A fixture would
# have to be regenerated on every addition, and regenerating is exactly how a
# careless entry gets blessed.
#
# The test measures through the renderer's own labelForCell(), not a
# reimplementation of it, so the table and the assertions about it cannot drift.

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
  const T = R.WORD_ABBREV;
  const budgetOf = () => Math.min(R.CELL_W, F.fontWidth4x5("M".repeat(R.LABEL_CHARS)));
  const words = Object.keys(T);

  /* ---- 1. an abbreviation must abbreviate ---------------------------- */
  for (const w of words) {
    const a = T[w];
    if (typeof a !== "string" || !a) fail("`" + w + "` maps to nothing");
    /* Not LONGER. Equal length is legitimate: `env` -> ENV and `amt` -> AMT
     * are canonicalisations that exist so a module spelling it the short way
     * lands on the same mnemonic as one spelling it out. */
    if (a.length > w.length)
      fail("`" + w + "` -> `" + a + "` is LONGER than the word it replaces");
    if (a !== a.toUpperCase())
      fail("`" + w + "` -> `" + a + "` is not upper case; the cell draws caps");
    if (!/^[A-Z0-9.]+$/.test(a))
      fail("`" + w + "` -> `" + a + "` has a character the 4x5 atlas cannot draw");
    if (w !== w.toLowerCase())
      fail("key `" + w + "` is not lower case; preAbbreviate looks up lowercase");
  }

  /* ---- 2. a collision must be DECLARED --------------------------------
   *
   * Two different words sharing one mnemonic means the label stops
   * identifying the parameter. Some groups are deliberate — portamento,
   * porta and glide are three spellings of one control — so they are listed
   * rather than inferred. A prefix rule would not catch that group and would
   * wrongly bless others. */
  const group = new Map();
  for (const g of R.ABBREV_SYNONYMS) for (const w of g) {
    if (!T[w]) fail("ABBREV_SYNONYMS names `" + w + "`, which is not in the table");
    group.set(w, g[0]);
  }
  for (const g of R.ABBREV_SYNONYMS) {
    const abbrevs = new Set(g.map((w) => T[w]));
    if (abbrevs.size !== 1)
      fail("declared synonyms " + JSON.stringify(g) + " do not share an abbreviation: " +
           JSON.stringify([...abbrevs]));
  }
  const byAbbrev = new Map();
  for (const w of words) {
    const a = T[w];
    if (!byAbbrev.has(a)) byAbbrev.set(a, []);
    byAbbrev.get(a).push(w);
  }
  for (const [a, ws] of byAbbrev) {
    if (ws.length === 1) continue;
    const roots = new Set(ws.map((w) => group.get(w) || w));
    if (roots.size !== 1)
      fail("`" + a + "` is claimed by unrelated words " + JSON.stringify(ws) +
           " — declare them in ABBREV_SYNONYMS if that is deliberate");
  }

  /* ---- 3. every mnemonic must FIT when it is used --------------------
   *
   * A mnemonic still goes through shortenLabel afterwards, and one that gets
   * squeezed again is worse than no entry: it looks covered in the table and
   * renders as a stump on the device.
   *
   * Probed at a budget sized to THIS entry -- exactly the width of the mnemonic
   * itself -- which is the condition under which the table fires for it. A
   * single fixed narrow budget does not work: it has to be wider than every
   * mnemonic and narrower than every full word, and those overlap ("FINE" is
   * 14px, "FIN" is 11px).
   *
   * This used to assert labelForCell(word) === mnemonic at the DEFAULT cell,
   * which the group-aware rule breaks by design: a word whose full form fits
   * now renders that instead. */
  for (const w of words) {
    const probe = F.fontWidth4x5(T[w]);
    const shown = R.labelForCell(w, probe + 2);
    if (shown !== T[w])
      fail("at a " + probe + "px budget `" + w + "` is tabled as `" + T[w] +
           "` but the cell draws `" + shown + "` -- the mnemonic does not fit");
  }

  /* ---- 3b. a synonym group must agree ON SCREEN ----------------------
   *
   * Property 2 pins the TABLE; this pins the RENDER, and they are not the
   * same check. A per-word "expand it if it fits" rule keeps the table
   * perfectly consistent while drawing Amount as AMOUNT and Amt as AMT --
   * one control reading two ways depending on which spelling its module
   * used. That is the regression this catches and property 2 does not. */
  for (const g of R.ABBREV_SYNONYMS) {
    const shown = new Set(g.map((w) => R.labelForCell(w)));
    if (shown.size !== 1)
      fail("the synonym group " + JSON.stringify(g) + " renders as " +
           JSON.stringify([...shown]) + " -- the same control reads differently " +
           "depending on the spelling a module happened to use");
  }

  /* ---- 4. the table must EARN its place on the real fleet -------------
   *
   * Properties 1-3 are all satisfiable by a table that helps nobody. This is
   * the one that fails if the vocabulary stops matching the modules: measured
   * against every declared label in the fleet fixture, a floor on how many
   * render as a complete, unsqueezed abbreviation.
   *
   * A FLOOR, not an equality. Equality would fail whenever a module is added
   * to the fixture, and the fix for that is always to re-record the number,
   * which teaches nothing. */
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
  /* "Complete" means the cell did not have to squeeze: what it draws is what
   * the word-level pass produced, with nothing cut.
   *
   * The reference is labelUnsqueezed, NOT labelForCell with a huge cell --
   * the width is min(cellW - 2, LABEL_CHARS worth of Ms), so a huge cell
   * clamps to the same cap and the comparison is a value against itself.
   * That version reported 2665/2665 and asserted nothing. */
  let complete = 0, vowelStripped = 0;
  for (const t of labels) {
    const shown = R.labelForCell(t);
    if (shown === R.labelUnsqueezed(t)) complete++;
    /* A crude but honest proxy for the failure this table exists to stop: a
     * six-plus character run of letters with no vowel in it is what dropping
     * vowels produces, and is never a mnemonic anybody chose. */
    if (/[BCDFGHJKLMNPQRSTVWXZ]{6,}/.test(shown)) vowelStripped++;
  }
  const FLOOR = 360;
  if (complete < FLOOR)
    fail("only " + complete + " of " + labels.size + " fleet labels render complete, " +
         "below the floor of " + FLOOR + " — the table stopped matching the fleet");
  /* The vowel-strip count is SLACK at the current cap -- a four-M cell cuts a
   * label before six consonants can pile up, so it reads 0 and cannot fail.
   * It is here for the wider cell (LABEL_CHARS 5, on its own branch), where it
   * reads 54 and a careless removal from the table pushes it up. Recorded as
   * slack rather than deleted, and said out loud rather than presented as a
   * passing check. */
  /* 63 on the 95-module fleet, against 54 on the 76-module one. The count grew
   * by less than the fleet did (54 x 95/76 would be ~68) because the table was
   * extended alongside the recapture -- BNDPSS, DSTNTN, CMPLXT and 17 others
   * are now declared words.
   *
   * What is LEFT is mostly not vowel-stripping at all: FLTDRV, FBKDPT, RNDGRN,
   * DLFDBK are two good mnemonics joined, and the proxy regex cannot tell that
   * from a stripped word. So the ceiling is a tripwire against the table
   * rotting, not a target to drive to zero -- 65 leaves a little headroom
   * without leaving room for a whole module of regressions. */
  const VOWEL_CEILING = 65;
  if (vowelStripped > VOWEL_CEILING)
    fail(vowelStripped + " fleet labels render as a vowel-stripped run, " +
         "over the ceiling of " + VOWEL_CEILING);

  /* ---- 5. the reported labels, by name --------------------------------
   *
   * The aggregate above can stay above its floor while the specific labels
   * that motivated the table regress. These are the ones actually complained
   * about or quoted in review. */
  /* Asserted as "contains the mnemonic, does not contain the vowel-stripped
   * run", NOT as an exact string. What shortenLabel does with the REST of a
   * multi-word label depends on the cell width, so an exact expectation is
   * cap-dependent: "Wave Group" draws WGRP at the four-M cap and WAGRP at the
   * five-M one, and both are correct. Pinning WGRP made this test fail the
   * moment the two label branches were merged together, which is a fixture
   * masquerading as a property. */
  const CASES = [
    ["Rotation",   /ROT/,  /ROTATN/],
    ["Wave Group", /GRP/,  /GROU/],
    ["Polyphony",  /POLY/, /PLYPHN/],
    ["Expression", /EXP/,  /EXPR/],   /* EXPR, not EXPRSS: the un-tabled form is EXPR at the narrow cap */
  ];
  for (const [label, want, bad] of CASES) {
    const got = R.labelForCell(label);
    if (!want.test(got))
      fail("\"" + label + "\" draws `" + got + "`, expected it to contain " + want);
    if (bad.test(got))
      fail("\"" + label + "\" draws `" + got + "` -- still the vowel-stripped form");
  }

  /* And the other direction: a word that FITS must render as the WORD.
   *
   * This used to assert those words were ABSENT from the table, because the
   * table applied unconditionally and tabling ACCENT meant drawing ACC. The
   * rule is now conditional, so presence in the table is harmless and the
   * thing to check is the render. Nineteen words were removed under the old
   * rule and could safely come back under this one.
   *
   * Includes `color`, which is tabled precisely BECAUSE it fits: without an
   * entry it has no synonym group, and Color/Colour render two different
   * ways. A word can need a table entry for agreement rather than for
   * length. */
  const FITS_WHOLE = ["Accent", "Patch", "Preset", "Scale", "Tempo", "Drift",
                      "Morph", "Clock", "Model", "Choke", "Color", "Pulse",
                      "Chord", "Curve", "Range", "Scene", "Chance",
                      "Pitch", "Attack", "Decay", "Cutoff", "Octave", "Drive"];
  for (const w of FITS_WHOLE) {
    const upper = w.toUpperCase();
    if (F.fontWidth4x5(upper) > budgetOf()) continue;    /* does not fit: mnemonic is right */
    const shown = R.labelForCell(w);
    if (shown !== upper)
      fail("\"" + w + "\" fits the cell whole but draws `" + shown +
           "` -- a mnemonic must never replace a word that fits");
  }

  console.log("  ok  " + words.length + " entries, all shorter, upper case, drawable");
  console.log("  ok  every collision is declared in ABBREV_SYNONYMS");
  console.log("  ok  every mnemonic fits when it fires, and every synonym group renders as one word");
  console.log("  ok  " + complete + "/" + labels.size + " fleet labels complete (floor " + FLOOR + ")");
  console.log("  ok  the reported labels draw their mnemonic; " + FITS_WHOLE.length + " words that fit render whole");
  console.log("  .. " + vowelStripped + " vowel-stripped (ceiling " + VOWEL_CEILING +
              " -- slack at this cap, bites at the wider one)");
  console.log("PASS: the abbreviation table abbreviates, does not collide, and fits");
}).catch((e) => { console.log("FAIL: " + e); process.exit(1); });
'
