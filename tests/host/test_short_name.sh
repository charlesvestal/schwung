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
]).then(async ([M, R]) => {
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
  const drawn = R.labelVerbatim(long.short_name);
  if (drawn.length > 8) fail("short_name must still be fitted to the cell, got " + drawn);

  /*
   * FITTED, BUT NEVER RE-WORDED.
   *
   * labelForCell runs the word pass, which EXPANDS a known mnemonic when the
   * full word fits -- right for a label we derived, wrong for one an author
   * typed. Asked for "Amt", the grid drew AMOUNT: the author said the short
   * form and got the long one back. Reported by Charles from the review.
   */
  if (R.labelForCell("Amt") !== "AMOUNT")
    fail("precondition: the derived path is expected to expand Amt");
  if (R.labelVerbatim("Amt") !== "AMT")
    fail("a declared short_name must not be re-worded, got " + R.labelVerbatim("Amt"));

  /*
   * VERBATIM IF IT FITS. Two wrong answers preceded this, both from treating
   * the word pass as all-or-nothing: with a budget it EXPANDED (Amt -> AMOUNT),
   * with none it ABBREVIATED unconditionally (Noise -> NSE, Color -> CLR), and
   * skipping it lost the good mnemonics (Sustain tail-truncated to SUSTAI).
   */
  for (const w of ["Noise", "Color", "Shape", "Ring", "Output"])
    if (R.labelVerbatim(w) !== w.toUpperCase())
      fail("a declared word that FITS must be drawn as typed: " + w + " -> " + R.labelVerbatim(w));
  /* ...and the table still rescues one that does not fit. */
  if (R.labelVerbatim("Sustain") !== "SUS")
    fail("a declared word too wide for the cell should take its mnemonic, got " + R.labelVerbatim("Sustain"));
  if (R.labelVerbatim("Release") !== "REL")
    fail("Release should shorten to REL, got " + R.labelVerbatim("Release"));
  /* A space-separated pair survives: NS VOL reads better than NSVOL. */
  if (R.labelVerbatim("NS VOL") !== "NS VOL")
    fail("a declared two-word label should survive, got " + R.labelVerbatim("NS VOL"));

  /*
   * A PAGE can override it, because the page is the narrower context.
   *
   * filter has env_amount on BOTH Main and Envelope. On Envelope the page has
   * already said "envelope" so the cell wants "Amt"; on Main an LFO Amt sits
   * beside it and "Amt" would name them both. One value per param cannot say
   * that, which is why the level carries its own.
   */
  {
    const P = await import("./src/shared/param_pages/page_plan.mjs");
    const hier = { levels: {
      root: { name: "Main", knobs: ["env_amount", "lfo_amount"],
              params: ["env_amount", "lfo_amount", { level: "envelope", label: "Envelope" }] },
      envelope: { name: "Envelope", knobs: ["env_amount"],
                  params: [{ key: "env_amount", short_name: "Amt" }] } } };
    const cp = [{ key: "env_amount", name: "Env Amt", short_name: "Env Amt", type: "float", min: 0, max: 1 },
                { key: "lfo_amount", name: "LFO Amt", type: "float", min: 0, max: 1 }];
    const ix = M.buildMetaIndex({ hierarchy: hier, chainParams: cp });
    const { pages } = P.planPages({ hierarchy: hier, chainParams: cp });
    const drawn = (p, k) => R.labelForCell((p.shortNames && p.shortNames[k]) ||
                                           ix.getOrGuess(k).short_name || ix.getOrGuess(k).label || k);
    const main = pages.find((p) => p.name === "Main");
    const env  = pages.find((p) => p.name === "Envelope");
    if (!main || !env) fail("expected a Main and an Envelope page");

    if (env.shortNames === null || !env.shortNames || env.shortNames.env_amount !== "Amt")
      fail("the level did not carry its own short_name onto its page");
    if (main.shortNames) fail("a level declaring none must carry none");

    if (drawn(env, "env_amount") !== "AMOUNT")
      fail("the page override should win on its own page, got " + drawn(env, "env_amount"));
    if (drawn(main, "env_amount") === drawn(main, "lfo_amount"))
      fail("the override must NOT leak to Main, where it would name two cells: " + drawn(main, "env_amount"));
    if (drawn(main, "env_amount") !== "ENAMT")
      fail("Main should keep the param-level spelling, got " + drawn(main, "env_amount"));
  }

  console.log("PASS: short_name shortens the cell, leaves the header alone, "
            + "is inert when absent, and a PAGE can override it");
});
'
