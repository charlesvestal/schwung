#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A graphic must sit inside ONE ROW: row 0's knobs draw at y=10 with their
# LABELS at y=25..32, and row 1 starts at y=33, so a shape spanning both would
# draw through the label band. That constraint is real and stays.
#
# Its consequence was not. Measured on the 95-module fleet, 26 groups were
# rejected for LAYOUT alone -- the ADSR on the Main page of obxd, hush1,
# minijv, moog, surge, rex and osirus, plus twelve surge LFO pages. An author
# writing attack/decay/sustain/release in the obvious order lands on slots 3..6
# and gets four separate dials.
#
# planPages now nudges such a block into a row. What it must NOT do is as
# important as what it does, and all of it is pinned below.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
  import("node:fs"),
]).then(async ([P, M, V, fs]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const planOf = (id) => {
    const mod = fx.modules.find((x) => x.id === id);
    if (!mod) fail("fixture has no module \"" + id + "\"");
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    return { plan: P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params }), metaIndex };
  };
  const pageNamed = (plan, name) => plan.pages.find((p) => p.kind === P.PAGE_KNOBS && p.name === name);

  /* ---- the ADSR actually groups now ------------------------------------ */
  {
    const { plan, metaIndex } = planOf("obxd");
    const pg = pageNamed(plan, "Main");
    const gs = V.resolveViz({ keys: pg.keys, metaIndex }).groups || [];
    if (!gs.some((g) => g.kind === "envelope"))
      fail("obxd Main still has no envelope group: " + pg.keys.join(","));
    if (!gs.some((g) => g.kind === "filter"))
      fail("obxd Main LOST its filter group to the realignment: " + pg.keys.join(","));
    console.log("  ok  obxd Main draws its envelope, and keeps its filter");
  }

  /* ---- ROW TWO is preferred, which is what keeps cutoff on knob 1 ------ *
   *
   * minijv puts cutoff/resonance first and its ADSR at slots 2..5. A
   * nearest-fit rule moves the ADSR UP to slots 0..3 and pushes macro_cutoff
   * to knob 5 -- on a module whose first two knobs are the filter. Preferring
   * row two moves the envelope DOWN instead and leaves the head of the page
   * alone. */
  {
    const { plan } = planOf("minijv");
    const pg = pageNamed(plan, "Main");
    if (pg.keys[0] !== "macro_cutoff")
      fail("minijv Main no longer starts with macro_cutoff: " + pg.keys.join(","));
    if (pg.keys.slice(4).join(",") !== "macro_attack,macro_decay,macro_sustain,macro_release")
      fail("minijv ADSR is not on row two: " + pg.keys.join(","));
    console.log("  ok  a straddling envelope moves DOWN, leaving knob 1 alone");
  }

  /* ---- an envelope already inside a row is NOT moved -------------------- *
   *
   * "Always put the envelope on row two" is tempting and wrong: 29 envelopes
   * in the fleet already sit inside row one and draw correctly, and many are
   * on pages that exist FOR that envelope, where row one is exactly right and
   * row two would leave the top half empty. An always-rule makes 29 pages
   * worse to fix 24. */
  for (const [id, name] of [["obxd", "Filter Env"], ["obxd", "Amp Env"], ["hera", "Envelope"]]) {
    const { plan, metaIndex } = planOf(id);
    const pg = pageNamed(plan, name);
    if (!pg) continue;
    if (pg.keys[0] === null || pg.keys[0] === undefined) continue;
    const gs = V.resolveViz({ keys: pg.keys, metaIndex }).groups || [];
    const env = gs.find((g) => g.kind === "envelope");
    if (!env) fail(id + " " + name + " lost its envelope");
    if (env.slotStart !== 0)
      fail(id + " " + name + ": an envelope that already sat at slot 0 was moved to " +
           env.slotStart + " -- a dedicated envelope page belongs on row one");
  }
  console.log("  ok  an envelope already drawing in row one is left alone");

  /* ---- no page gains or loses a key ------------------------------------ *
   *
   * The move is a permutation WITHIN a page. If it could spill, a full page
   * would push a knob onto an orphan page holding one control. */
  {
    for (const mod of fx.modules) {
      const plan = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      for (const pg of plan.pages) {
        if (pg.kind !== P.PAGE_KNOBS || !pg.keys) continue;
        if (pg.keys.length > 8)
          fail(mod.id + " page \"" + pg.name + "\" has " + pg.keys.length + " keys");
        if (new Set(pg.keys.filter(Boolean)).size !== pg.keys.filter(Boolean).length)
          fail(mod.id + " page \"" + pg.name + "\" gained a duplicate key");
      }
    }
    console.log("  ok  every page still holds at most 8 distinct keys");
  }

  /* ---- the fleet count, as a floor -------------------------------------- */
  {
    let realigned = 0;
    for (const mod of fx.modules) {
      const plan = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      realigned += (plan.realigned || []).length;
    }
    if (realigned < 20)
      fail("only " + realigned + " pages realigned; measured 24 when this landed, so " +
           "the pass has been narrowed or switched off");
    console.log("  ok  " + realigned + " fleet pages realigned into a drawable layout");
  }

  /* ---- an envelope with NO ATTACK must not hang the renderer ----------- *
   *
   * surge declares twelve LFO pages carrying hold/sustain/release and no
   * attack at all. drawPartialEnv computed its rise from val.attack
   * unconditionally, so with no attack role that is undefined, peakX is NaN,
   * and the NaN reaches line()`s for(;;) whose equality break is never
   * satisfied -- a HANG, the same one docs/PARAM_PAGES.md records for
   * a partial GRID_GEOM freezing the shadow_ui tick.
   *
   * It was unreachable until alignment made those pages drawable: a latent
   * renderer bug EXPOSED by this change, not caused by it. Asserted here
   * because a hang produces no failing assertion of its own -- the suite just
   * stops, which is exactly how it presented.
   *
   * The geometry is checked for NaN rather than for a pixel count, because NaN
   * is the thing that hangs and a count would pass on a wrong-but-finite
   * shape. */
  {
    const D = await import("./src/shared/param_pages/viz_draw.mjs");
    const idx = M.buildMetaIndex({
      hierarchy: { levels: { root: { knobs: ["h", "s", "r"] } } },
      chainParams: [
        { key: "h", type: "float", min: 0, max: 1 },
        { key: "s", type: "float", min: 0, max: 1 },
        { key: "r", type: "float", min: 0, max: 1 },
      ],
    });
    const seg = [];
    const rec = (a, b, c, d) => seg.push([a, b, c, d]);
    const ctx = { fillRect: () => {}, drawLine: rec, line: rec, setPixel: () => {}, pixel: () => {} };
    D.drawEnvelope(ctx, { x: 0, y: 0, w: 64, h: 22 },
                   { hold: "h", sustain: "s", release: "r" },
                   { h: "0.5", s: "0.5", r: "0.5" }, idx);
    if (seg.length === 0) fail("an attack-less envelope drew nothing at all");
    for (const p of seg)
      for (const v of p)
        if (!Number.isFinite(v))
          fail("an attack-less envelope produced a non-finite coordinate (" + v + "). " +
               "That is not a wrong picture, it is the infinite loop in line().");
    console.log("  ok  an envelope with no attack draws finite geometry");
  }

  console.log("PASS: a group one slot from drawable is nudged into a row");
});
'
