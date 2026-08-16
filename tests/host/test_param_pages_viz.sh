#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Graphics resolution + drawing (src/shared/param_pages/viz.mjs, viz_draw.mjs).
#
# Three things, matching the guardrails in
# docs/plans/2026-08-16-next-sessions.md "Session A":
#
#  1. A FLEET DETECTOR SNAPSHOT, checked in at
#     tests/fixtures/snapshots/param_pages_viz.txt — one line per module per
#     fired group. Changing a detector heuristic shows up as a reviewable
#     diff here instead of a surprise on a device. Regenerate deliberately
#     with UPDATE_SNAPSHOTS=1.
#  2. FALSE-POSITIVE traps: vocabulary alone (adjacent role-name matches) is
#     not enough corroboration — a chorus LFO's shape sitting next to a
#     delay's rate/depth must NOT read as one LFO group, and a crossover
#     frequency or Q must not read as an EQ gain.
#  3. Draw-call count against a generous SANITY ceiling, not a tight budget.
#     An earlier version capped it at the plain grid's bar=120/dial=700 and
#     the curve renderers were degraded (a coarse staircase instead of real
#     pixels) to fit under it — a number invented here, never measured on a
#     device, that made schwung-movy's actual envelope/filter/lfo curves
#     (real Bresenham lines, ported verbatim — see viz_draw.mjs) look wrong.
#     Visual correctness against the ported reference wins. The ceiling below
#     is ~1.6x the worst measured today (bar 306, dial 518) — loose enough
#     that it does not fight the real geometry, tight enough to catch an
#     actual regression (an infinite point list, a detector grouping the
#     whole page). If a real device chokes on the real numbers, THAT is the
#     fact to design a tighter budget around — see
#     docs/plans/2026-08-16-next-sessions.md Session C.
#
# viz is opt-in (renderPage only draws what o.viz gives it), so the existing
# param_pages.txt / render budget snapshot are untouched by this file.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param-page viz tests" >&2
  exit 1
fi

UPDATE="${UPDATE_SNAPSHOTS:-}" node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./tools/param-pages/cases.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
  import("node:fs"),
]).then(([H, C, P, M, R, V, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));
  const SNAPSHOT = "tests/fixtures/snapshots/param_pages_viz.txt";

  /* ---- 1. fleet detector snapshot --------------------------------------- */
  const lines = [];
  const SANITY = { bar: 500, dial: 850 };
  const worst = { bar: 0, dial: 0 };
  const clipped = [], missing = new Set(), overSanity = [];

  for (const mod of fx.modules) {
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    for (let i = 0; i < pages.length; i++) {
      const page = pages[i];
      if (page.kind !== P.PAGE_KNOBS) continue;
      const { groups } = V.resolveViz({ keys: page.keys, metaIndex });
      for (const g of groups) {
        lines.push(`${mod.id}\t${page.name}\t${g.kind}\t${g.source}\t${g.keys.join(",")}`);
      }
      if (groups.length === 0) continue;

      const values = {};
      for (const k of page.keys) values[k] = C.fakeValue(k, metaIndex.getOrGuess(k));
      for (const layout of [R.LAYOUT_BAR, R.LAYOUT_DIAL]) {
        const fb = H.createFramebuffer();
        let calls = 0;
        const counting = {
          fillRect: (...a) => { calls++; return fb.fillRect(...a); },
          print: (...a) => { calls++; return fb.print(...a); },
          textWidth: fb.textWidth,
        };
        R.renderPage(counting, {
          page, metaIndex, values, title: "T1 > " + mod.id.toUpperCase(),
          pageIndex: i, pageCount: pages.length, viz: groups, layout,
        });
        if (calls > worst[layout]) worst[layout] = calls;
        if (calls > SANITY[layout]) overSanity.push(`${mod.id}#${i}/${layout}=${calls}`);
        if (fb.clipped() > 0) clipped.push(`${mod.id}#${i}/${layout}`);
        for (const g2 of fb.missingGlyphs) missing.add(g2);
      }
    }
  }

  lines.sort();
  const text = lines.join("\n") + "\n";

  if (process.env.UPDATE) {
    fs.writeFileSync(SNAPSHOT, text);
    console.log("wrote " + SNAPSHOT + " (" + lines.length + " lines)");
  } else {
    if (!fs.existsSync(SNAPSHOT)) fail(SNAPSHOT + " is missing — run with UPDATE_SNAPSHOTS=1 to create it");
    const want = fs.readFileSync(SNAPSHOT, "utf8");
    if (want !== text) {
      fail("fleet viz detection changed — review the diff, then UPDATE_SNAPSHOTS=1 to accept it\n" +
           "(" + lines.length + " groups now fire; snapshot has " + want.trim().split("\n").filter(Boolean).length + ")");
    }
  }
  if (lines.length < 200) fail("only " + lines.length + " groups fired across the fleet — detection is not covering it");

  /* ---- 1b. draw-call budget, viz-enabled pages --------------------------- */
  if (overSanity.length) fail("viz pages blew the sanity ceiling (likely a real regression, not a tuning issue): " + overSanity.slice(0, 6).join(", "));
  if (worst.bar < 20 || worst.dial < 50) fail("the viz draw-call counter is not measuring anything");
  if (clipped.length) fail("viz content drawn outside the display: " + clipped.slice(0, 6).join(", "));
  if (missing.size) fail("viz drew characters the device font cannot draw: " + [...missing].join(", "));

  /* ---- 2. false-positive traps ------------------------------------------- */
  {
    /* osirus ships a chorus LFO shape sitting right next to a delay''s rate
     * and depth. Role vocabulary alone (shape/rate/depth all present, all
     * adjacent) reads exactly like one LFO — it is not one, and firing here
     * was a real bug caught while building this detector. */
    const mod = fx.modules.find((m) => m.id === "osirus");
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const { groups } = V.resolveViz({
      keys: ["chorus_lfo_shape", "delay_rate_rev_decay", "delay_depth", "delay_lfo_shape", "delay_color", "delay_clock"],
      metaIndex,
    });
    const badLfo = groups.find((g) => g.kind === "lfo" && g.keys.includes("chorus_lfo_shape") && g.keys.includes("delay_rate_rev_decay"));
    if (badLfo) fail("a chorus shape and a delay rate/depth were grouped as one LFO — stem corroboration regressed");
  }
  {
    /* A crossover frequency or a Q named with "gain" nowhere in it must not
     * pass isGainRange — and even if a name matched, a positive-only range
     * (a real crossover/Q shape) must be rejected. */
    if (V.isGainRange({ min: 20, max: 20000 })) fail("isGainRange accepted a crossover-frequency-shaped range");
    if (V.isGainRange({ min: 0, max: 36 })) fail("isGainRange accepted a positive-only (Q-shaped) range");
    if (!V.isGainRange({ min: -12, max: 12 })) fail("isGainRange rejected a textbook symmetric gain range");
    if (!V.isGainRange({ min: -6, max: 15 })) fail("isGainRange rejected a plausible asymmetric gain range (fleet has -6..+18 etc)");
  }
  {
    /* Two envelopes elsewhere on the SAME page, one prefixed, must not merge
     * into a single detected group just because attack/decay/sustain/release
     * all exist somewhere in the pool. */
    const metaIndex = M.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "amp_attack", name: "Amp Attack", type: "float", min: 0, max: 1 },
      { key: "amp_decay", name: "Amp Decay", type: "float", min: 0, max: 1 },
      { key: "filter_sustain", name: "Filter Sustain", type: "float", min: 0, max: 1 },
      { key: "filter_release", name: "Filter Release", type: "float", min: 0, max: 1 },
    ]});
    const { groups } = V.resolveViz({ keys: ["amp_attack", "amp_decay", "filter_sustain", "filter_release"], metaIndex });
    if (groups.some((g) => g.kind === "envelope"))
      fail("an amp envelope and a filter envelope were merged into one group across a stem mismatch");
  }

  /* ---- 3. precedence: declared beats detected, viz:false suppresses ----- */
  {
    const metaIndex = M.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "attack", name: "Attack", type: "float", min: 0, max: 1, viz: { group: "g1", role: "attack" } },
      { key: "decay", name: "Decay", type: "float", min: 0, max: 1, viz: { group: "g1", role: "decay" } },
      { key: "sustain", name: "Sustain", type: "float", min: 0, max: 1, viz: false },
      { key: "release", name: "Release", type: "float", min: 0, max: 1 },
    ]});
    const { groups } = V.resolveViz({ keys: ["attack", "decay", "sustain", "release"], metaIndex });
    const g = groups.find((g2) => g2.group === "g1");
    if (!g) fail("a declared viz group did not resolve");
    if (g.source !== "declared") fail("a declared group reported source " + g.source);
    if (g.keys.includes("sustain")) fail("viz:false was not honoured — sustain was pulled into a group anyway");
    if (groups.some((g2) => g2.keys.includes("release") && g2.kind === "envelope"))
      fail("release should stand alone (only 2 of 4 declared, no group claims it) but got grouped");
  }
  {
    /* A host override can correct a detector, but only for a key the module
     * did not already claim. */
    const metaIndex = M.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "weird_knob", name: "Weird", type: "float", min: 0, max: 1 },
    ]});
    const overrides = (key) => (key === "weird_knob" ? { kind: "fader" } : null);
    const { groups } = V.resolveViz({ keys: ["weird_knob"], metaIndex, overrides });
    if (!groups.length || groups[0].kind !== "fader" || groups[0].source !== "override")
      fail("a host override did not resolve to a fader group");
  }

  console.log("PASS: param-page viz — " + lines.length + " groups across the fleet, " +
              "worst draw calls bar=" + worst.bar + "/dial=" + worst.dial +
              ", false-positive traps hold");
});
' || exit 1
