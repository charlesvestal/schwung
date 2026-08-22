#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# ottx declares lgain / mgain / hgain, adjacent and all -30..30 dB -- a
# textbook three-band EQ that matched none of the band-word patterns, because
# those require a separator ((^|_)(low|lo|bass)($|_)) and "lgain" has none.
# The separator is what keeps "lowpass"/"highpass" out of the EQ detector, so
# it stays; the [lmh]gain forms are spelled out as an anchored exception.
#
# THIS CHANGES NOTHING IN THE FLEET TODAY, and that is worth saying out loud
# rather than hiding: ottx puts the three gains on knobs 3, 4 and 5, which
# straddles the row break between knob 4 and knob 5. A graphic cannot span the
# two rows of the grid, so isAdjacentRun rejects it on layout regardless of the
# vocabulary. Both have to be true, and only one of them is ours -- the module
# has to move the three gains into a single row.
#
# The vocabulary fix is still the right half to make: without it, moving the
# knobs would not help either.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/viz.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
]).then(([V, M]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const GAINS = [
    { key: "lgain", name: "Low", type: "float", min: -30, max: 30 },
    { key: "mgain", name: "Mid", type: "float", min: -30, max: 30 },
    { key: "hgain", name: "High", type: "float", min: -30, max: 30 },
  ];
  const groups = (keys, params) => {
    const idx = M.buildMetaIndex({
      hierarchy: { levels: { root: { knobs: keys } } },
      chainParams: params,
    });
    return (V.resolveViz({ keys, metaIndex: idx }).groups || []);
  };

  /* ---- the vocabulary --------------------------------------------------- */
  {
    const g = groups(["lgain", "mgain", "hgain"], GAINS).find((x) => x.kind === "eq");
    if (!g) fail("lgain/mgain/hgain adjacent and all -30..30 dB produced no EQ group");
    if (g.roles.low !== "lgain" || g.roles.mid !== "mgain" || g.roles.high !== "hgain")
      fail("the bands are bound to the wrong knobs: " + JSON.stringify(g.roles));
    console.log("  ok  lgain/mgain/hgain are recognised as low/mid/high");
  }

  /* ---- within one ROW, which is the other half ------------------------- *
   *
   * Slots 2,3,4 straddle the row break and must NOT group -- a graphic cannot
   * span the gap. This is ottx as it ships, and it is why the vocabulary fix
   * alone changes nothing for it. */
  {
    const pad = [{ key: "a", type: "float", min: 0, max: 1 },
                 { key: "b", type: "float", min: 0, max: 1 },
                 { key: "c", type: "float", min: 0, max: 1 },
                 { key: "d", type: "float", min: 0, max: 1 },
                 { key: "e", type: "float", min: 0, max: 1 }];
    const straddling = groups(["a", "b", "lgain", "mgain", "hgain", "c", "d", "e"], GAINS.concat(pad));
    if (straddling.some((x) => x.kind === "eq"))
      fail("three gains spanning the row break grouped as an EQ -- a graphic cannot " +
           "be drawn across the two rows of the grid");
    const inRow = groups(["a", "lgain", "mgain", "hgain", "b", "c", "d", "e"], GAINS.concat(pad));
    if (!inRow.some((x) => x.kind === "eq"))
      fail("three gains inside one row did NOT group -- the fix does not actually work");
    console.log("  ok  one row groups, straddling the row break does not");
  }

  /* ---- the exception must not overreach -------------------------------- *
   *
   * "^lgain$" is anchored precisely so it cannot reach for other _gain keys.
   * A pass-filter name must still be kept out by the separator rule. */
  {
    const other = [
      { key: "lfo_gain", type: "float", min: -30, max: 30 },
      { key: "make_gain", type: "float", min: -30, max: 30 },
      { key: "mgain", name: "Mid", type: "float", min: -30, max: 30 },
    ];
    const g = groups(["lfo_gain", "make_gain", "mgain"], other).find((x) => x.kind === "eq");
    if (g) fail("lfo_gain / make_gain were read as EQ bands: " + JSON.stringify(g.roles));
    console.log("  ok  lfo_gain and make_gain are not EQ bands");
  }
  {
    const passes = [
      { key: "lowpass", type: "float", min: -30, max: 30 },
      { key: "highpass", type: "float", min: -30, max: 30 },
    ];
    if (groups(["lowpass", "highpass"], passes).some((x) => x.kind === "eq"))
      fail("lowpass/highpass grouped as an EQ -- the separator rule was loosened");
    console.log("  ok  lowpass/highpass are still not EQ bands");
  }

  console.log("PASS: [lmh]gain names an EQ band; the row constraint is unchanged");
});
'
