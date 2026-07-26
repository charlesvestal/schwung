#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Golden tests for the param-page planner (src/shared/param_pages/page_plan.mjs)
# against a real 76-module fleet capture (tests/fixtures/module-contracts.json).
#
# The planner turns a module's declared ui_hierarchy + chain_params into an
# ordered list of knob pages. The invariants below are the ones that decide
# whether a knob-page UI is an improvement on the list editor or a regression;
# see docs/plans/2026-07-26-param-pages-audit.md.
#
# The load-bearing one is COVERAGE. knobs[] is the author's chosen eight, not
# their parameter set: fleet-wide, 879 keys across 57 modules are listed in
# params[] and sit on no knob. A planner that renders knobs[] only would hide
# 28% of the fleet's declared parameters relative to the list editor we ship.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param-page planner tests" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/page_plan.mjs").then(async (m) => {
  const fs = await import("node:fs");
  const { planPages, pageSlotKeys, PAGE_KNOBS, PAGE_PRESET, PAGE_MODES, PAGE_CHILD, PAGE_ITEMS } = m;
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const plan = (id) => {
    const mod = fx.modules.find((x) => x.id === id);
    if (!mod) fail("fixture has no module \"" + id + "\"");
    return { mod, ...planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params }) };
  };
  const keysOf = (pages) => {
    const s = new Set();
    for (const p of pages) for (const k of (p.keys || [])) s.add(k);
    return s;
  };

  if (fx.modules.length < 70) fail("fixture shrank: " + fx.modules.length + " modules");

  /* ---- 1. every module plans, and every declared key lands on a page ---- */
  let totalPages = 0;
  const uncovered = [];
  const dupNames = [];
  for (const mod of fx.modules) {
    const h = mod.ui_hierarchy;
    const r = planPages({ hierarchy: h, chainParams: mod.chain_params });
    totalPages += r.pages.length;

    /* Page names are the only way a user tells 47 pages apart. */
    const names = r.pages.filter((p) => p.kind === PAGE_KNOBS).map((p) => p.name);
    if (names.length !== new Set(names).size) dupNames.push(mod.id);

    /* Declared = every editable key any level lists, on a knob or not.
     * child_prefix levels are excluded: their keys are synthesised per
     * instance at runtime (prefix + index + key), not enumerable here. */
    const declared = new Set();
    for (const lvl of Object.values((h && h.levels) || {})) {
      if (!lvl || typeof lvl !== "object" || lvl.child_prefix) continue;
      for (const k of (lvl.knobs || [])) {
        const kk = typeof k === "string" ? k : (k && k.key);
        if (kk) declared.add(kk);
      }
      for (const p of (lvl.params || [])) {
        if (p && p.level) continue;
        const kk = typeof p === "string" ? p : (p && p.key);
        if (kk) declared.add(kk);
      }
    }
    const reachable = keysOf(r.pages);
    const missing = [...declared].filter((k) => !reachable.has(k));
    if (missing.length) uncovered.push(mod.id + " (" + missing.length + "): " + missing.slice(0, 4).join(","));
  }
  if (uncovered.length) fail("declared keys unreachable from any page:\n  " + uncovered.join("\n  "));
  if (dupNames.length) fail("duplicate knob-page names within: " + dupNames.join(", "));
  if (totalPages < 500) fail("fleet page count collapsed to " + totalPages + " (expected ~600)");

  /* ---- 2. overflow pages carry params[]-only keys (the §1 regression) ---- */
  {
    /* sf2 declares 2 knobs but 6 editable params: without overflow the knob
     * page shows Octave + Gain and hides the reverb/chorus controls. */
    const { pages } = plan("sf2");
    const k = keysOf(pages);
    for (const need of ["octave_transpose", "gain", "reverb_on", "reverb_level", "chorus_on", "chorus_level"]) {
      if (!k.has(need)) fail("sf2 overflow lost \"" + need + "\"");
    }
    const grid = pages.filter((p) => p.kind === PAGE_KNOBS);
    if (!grid.some((p) => p.authored === false)) fail("sf2 has no overflow page (authored:false)");
  }

  /* ---- 3. a deduped alias level still contributes its extra params ------ */
  {
    /* genera publishes a `children` alias re-listing root knobs, plus two
     * params of its own. Suppressing the whole level drops scale/gen_mode. */
    const k = keysOf(plan("genera").pages);
    for (const need of ["scale", "gen_mode"]) {
      if (!k.has(need)) fail("genera alias-level dedupe dropped \"" + need + "\"");
    }
  }

  /* ---- 4. minijv: the fleet in one module ------------------------------- */
  {
    const { pages } = plan("minijv");
    if (pages[0].kind !== PAGE_MODES) fail("minijv must open on the mode select, got " + pages[0].kind);
    if (!pages.some((p) => p.kind === PAGE_PRESET)) fail("minijv has no preset page");
    if (!pages.some((p) => p.kind === PAGE_CHILD)) fail("minijv lost its child_prefix part selector");
    if (pages.filter((p) => p.kind === PAGE_ITEMS).length < 3) fail("minijv lost items_param pages");
    if (pages.length < 60) fail("minijv collapsed to " + pages.length + " pages (expected ~76)");

    /* Four near-identical tone subtrees: without parent prefixes the user gets
     * four pages called "Filter" and no way to tell them apart. */
    const filters = pages.filter((p) => p.kind === PAGE_KNOBS && /Filter/.test(p.name));
    if (filters.length < 4) fail("minijv should have >=4 Filter pages, got " + filters.length);
    if (new Set(filters.map((p) => p.name)).size !== filters.length) {
      fail("minijv Filter pages are not uniquely named: " + filters.map((p) => p.name).join(", "));
    }
  }

  /* ---- 5. preset page precedes the level it decorates (decision A) ------ */
  {
    /* obxd/root is simultaneously an 8-knob page and a 128-preset browser. */
    const { pages } = plan("obxd");
    const iPreset = pages.findIndex((p) => p.kind === PAGE_PRESET);
    const iKnobs = pages.findIndex((p) => p.kind === PAGE_KNOBS);
    if (iPreset < 0) fail("obxd has no preset page");
    if (!(iPreset < iKnobs)) fail("obxd preset page must precede its knob pages");
  }

  /* ---- 6. no hierarchy at all → paginate chain_params ------------------- */
  {
    const { pages, warnings } = plan("branchage");
    if (pages.length === 0) fail("branchage (no ui_hierarchy) planned zero pages");
    if (!warnings.some((w) => /no ui_hierarchy/.test(w))) fail("branchage should warn about the missing hierarchy");
    const k = keysOf(pages);
    if (k.size < 20) fail("branchage fallback reached only " + k.size + " params (expected 27)");
  }

  /* ---- 7. a level with more knobs than one page continues ---------------- */
  {
    /* breakbeat/root declares 17 knobs → 3 pages. */
    const { pages } = plan("breakbeat");
    const grid = pages.filter((p) => p.kind === PAGE_KNOBS);
    if (!grid.some((p) => / - 2$/.test(p.name))) fail("breakbeat 17-knob level did not continue onto a second page");
    if (grid.some((p) => (p.keys || []).length > 8)) fail("a page exceeded 8 knobs");
  }

  /* ---- 8. slot mapping is stable and bounded ---------------------------- */
  {
    const { pages } = plan("obxd");
    const grid = pages.find((p) => p.kind === PAGE_KNOBS);
    const slots = pageSlotKeys(grid);
    if (slots.length !== 8) fail("pageSlotKeys must return 8 slots, got " + slots.length);
    if (slots[0] !== grid.keys[0]) fail("knob 1 does not map to the page first key");
    if (pageSlotKeys({ kind: PAGE_PRESET }).some((s) => s !== null)) fail("non-grid page must map to no knobs");
  }

  /* ---- 9. purity: planning twice yields an identical plan --------------- */
  {
    const a = plan("surge"), b = plan("surge");
    if (a.fingerprint !== b.fingerprint) fail("fingerprint is not deterministic");
    if (JSON.stringify(a.pages) !== JSON.stringify(b.pages)) fail("planner is not deterministic");
    /* A mode change must produce a different fingerprint (rebuild trigger). */
    const jv = fx.modules.find((x) => x.id === "minijv");
    const p1 = planPages({ hierarchy: jv.ui_hierarchy, chainParams: jv.chain_params, mode: "patch" });
    const p2 = planPages({ hierarchy: jv.ui_hierarchy, chainParams: jv.chain_params, mode: "performance" });
    if (p1.fingerprint === p2.fingerprint) fail("mode change did not change the fingerprint");
    if (JSON.stringify(p1.pages) === JSON.stringify(p2.pages)) fail("minijv modes plan identical page sets");
  }

  /* ---- 10. visible_if is honoured when the caller supplies an evaluator -- */
  {
    const mod = fx.modules.find((x) => x.id === "mrsample");
    const all = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const none = planPages({
      hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params,
      visible: () => false,
    });
    const kAll = keysOf(all.pages), kNone = keysOf(none.pages);
    if (!kAll.has("loop_start")) fail("mrsample loop_start should be visible by default (fail-open)");
    if (kNone.has("loop_start")) fail("visible:false did not hide a visible_if param");
    if (kNone.size >= kAll.size) fail("visible_if evaluator had no effect");
  }

  console.log("PASS: param-page planner — " + fx.modules.length + " modules, " + totalPages +
              " pages, every declared key reachable, no duplicate page names");
});
'
