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
  const { planPages, pageSlotKeys, PAGE_KNOBS, PAGE_PRESET, PAGE_ITEMS } = m;
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const plan = (id) => {
    const mod = fx.modules.find((x) => x.id === id);
    if (!mod) fail("fixture has no module \"" + id + "\"");
    return { mod, ...planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params }) };
  };
  /*
   * Every key a page gives you a way to change -- not only knob cells.
   *
   * A preset browser IS the control for its list_param, and an items list IS
   * the control for its select_param; both are richer than a knob, which is
   * why the planner now drops a selector from knobs[] even when the module
   * authored it there. Counting only p.keys modelled the OLD rule and would
   * report those keys as unreachable when they are in fact the whole subject
   * of their own page.
   */
  const keysOf = (pages) => {
    const s = new Set();
    for (const p of pages) {
      for (const k of (p.keys || [])) s.add(k);
      if (p.listParam) s.add(p.listParam);
      if (p.selectParam) s.add(p.selectParam);
    }
    return s;
  };

  if (fx.modules.length < 70) fail("fixture shrank: " + fx.modules.length + " modules");

  /* ---- 1. every module plans, and every declared key lands on a page ---- */
  let totalPages = 0;
  const uncovered = [];
  const dupNames = [];
  const deliberatelyExcluded = new Set();
  for (const mod of fx.modules) {
    const h = mod.ui_hierarchy;
    /*
     * A module declaring `modes` plans ONE mode at a time -- the inactive
     * mode`s levels are not orphans and are no longer swept in (see
     * test_param_pages_mode_gating.sh). So coverage is the union over modes:
     * "every declared key is reachable in SOME mode", which is the invariant
     * that was always meant. Asserting it against a single plan would have
     * demanded minijv show its performance tree while in patch mode, i.e. the
     * bug. Every other module has exactly one plan and is unaffected.
     */
    const modeList = Array.isArray(h && h.modes) && h.modes.length ? h.modes : [undefined];
    const plans = modeList.map((md) => planPages({ hierarchy: h, chainParams: mod.chain_params, mode: md }));
    const r = { pages: plans.flatMap((p) => p.pages), warnings: plans.flatMap((p) => p.warnings || []) };
    totalPages += plans[0].pages.length;

    /* Page names are the only way a user tells 47 pages apart. Per PLAN, not
     * over the union -- two modes may each legitimately own a page called
     * "Presets", and they are never on screen together. */
    for (const p of plans) {
      const names = p.pages.filter((pg) => pg.kind === PAGE_KNOBS).map((pg) => pg.name);
      if (names.length !== new Set(names).size) dupNames.push(mod.id);
    }

    /* Declared = every editable key any level lists, on a knob or not.
     *
     * child_prefix levels are excluded: their keys are synthesised per instance
     * at runtime (prefix + index + key), not enumerable here.
     *
     * Two further exclusions are DELIBERATE, and both are narrow. A key the
     * author put on knobs[] is always honoured; only keys we pull in from
     * params[] are filtered:
     *   - selector params (list/count/name/items/select/mode) drive their own
     *     page kind, and browsing 2427 presets by encoder is what the preset
     *     page exists to avoid
     *   - `ui_`-prefixed keys are module UI state, not musical parameters
     * Both remain reachable in the list editor, which renders params[] verbatim.
     * The count is asserted below so the filter cannot quietly widen. */
    const authoredAnywhere = new Set();
    for (const lvl of Object.values((h && h.levels) || {})) {
      if (!lvl || typeof lvl !== "object") continue;
      for (const k of (lvl.knobs || [])) {
        const kk = typeof k === "string" ? k : (k && k.key);
        if (kk) authoredAnywhere.add(kk);
      }
    }
    const selectors = new Set();
    for (const lvl of Object.values((h && h.levels) || {})) {
      if (!lvl || typeof lvl !== "object") continue;
      for (const f of ["list_param", "count_param", "name_param", "items_param", "select_param"]) {
        if (lvl[f]) selectors.add(lvl[f]);
      }
    }
    if (h && h.mode_param) selectors.add(h.mode_param);
    const excludedOnPurpose = (k) =>
      !authoredAnywhere.has(k) && (selectors.has(k) || /^ui_/.test(k));

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
        if (kk && !excludedOnPurpose(kk)) declared.add(kk);
        if (kk && excludedOnPurpose(kk)) deliberatelyExcluded.add(mod.id + ":" + kk);
      }
    }
    const reachable = keysOf(r.pages);
    const missing = [...declared].filter((k) => !reachable.has(k));
    if (missing.length) uncovered.push(mod.id + " (" + missing.length + "): " + missing.slice(0, 4).join(","));
  }
  if (uncovered.length) fail("declared keys unreachable from any page:\n  " + uncovered.join("\n  "));
  /* The deliberate exclusions must stay a handful. If this grows, the filter is
   * swallowing musical parameters and the 28% regression is back by the side
   * door. */
  if (deliberatelyExcluded.size > 25) {
    fail("the grid is excluding " + deliberatelyExcluded.size + " declared keys, which is too many to be selectors and UI state: " +
         [...deliberatelyExcluded].slice(0, 8).join(", "));
  }
  if (deliberatelyExcluded.size === 0) fail("the exclusion filter matched nothing — it is not wired up");
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
    /* Patch is minijv`s first mode and so the default plan; the part selector
       lives in the OTHER one, which is the whole point of the mode split. */
    const { pages } = plan("minijv");
    const perf = planPages({ hierarchy: fx.modules.find((x) => x.id === "minijv").ui_hierarchy,
                             chainParams: fx.modules.find((x) => x.id === "minijv").chain_params,
                             mode: "performance" }).pages;
    /* The mode selector is an ITEMS page now -- same gesture, same machinery.
       PAGE_ITEMS was a kind nothing ever drew. It leads BOTH modes: it is the
       only way back to the other one. */
    for (const [label, set] of [["patch", pages], ["performance", perf]]) {
      if (!(set[0].kind === PAGE_ITEMS && set[0].modeSelect))
        fail("minijv " + label + " must open on the mode select, got " + set[0].kind);
      if (!set.some((p) => p.kind === PAGE_PRESET)) fail("minijv " + label + " has no preset page");
    }
    if (!perf.some((p) => p.kind === PAGE_ITEMS && p.childOf))
      fail("minijv lost its child_prefix part selector");
    if (pages.some((p) => p.kind === PAGE_ITEMS && p.childOf))
      fail("minijv shows its part selector in PATCH mode");
    if (pages.filter((p) => p.kind === PAGE_ITEMS).length < 3) fail("minijv lost items_param pages");
    if (pages.length < 60) fail("minijv patch collapsed to " + pages.length + " pages (expected ~70)");
    /* Performance is the small tree; if it ever approaches patch`s size the
       two have been merged again. */
    if (perf.length > 20) fail("minijv performance planned " + perf.length + " pages — trees merged?");

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

  /* ---- 11. a FAILED contract read must not produce a page plan ---------- */
  {
    /*
     * The wire has three answers for `<prefix>:ui_hierarchy` and only two of
     * them are the same thing:
     *
     *   JSON      the module declares this hierarchy
     *   ""        the module declares NO hierarchy      -> paginate chain_params
     *   null      the READ FAILED (param channel busy)  -> we know nothing
     *
     * The first two both parse to something the planner can act on; a failed
     * read parses to nothing, which is indistinguishable from absence by the
     * time it gets here — the fleet capture itself records `ui_hierarchy: null`
     * for the four modules that genuinely have none. So the caller, the only
     * one that saw the wire, passes `unresolved`.
     *
     * Collapsing the two is the granny bug: pick a sample, granny loads the WAV
     * synchronously on the param-serving thread, the ui_hierarchy read times
     * out at 100 ms, and the planner happily paginates chain_params instead —
     * whose first entry is `sample_path`, so the sample file lands on knob 1
     * and every other knob shifts.
     */
    const g = fx.modules.find((x) => x.id === "granny");
    if (!g) fail("fixture has no module \"granny\"");
    if (g.chain_params[0].key !== "sample_path")
      fail("granny fixture changed: chain_params[0] is no longer sample_path, " +
           "so this test no longer reproduces the reported bug");

    const good = planPages({ hierarchy: g.ui_hierarchy, chainParams: g.chain_params });
    if (good.pages[0].keys[0] !== "position")
      fail("granny page 1 knob 1 should be position, got " + good.pages[0].keys[0]);
    if (good.unresolved) fail("a good contract was reported unresolved");

    /* What the bug looked like: chain_params paginated as though the module had
     * declared no hierarchy at all. Pinned so the symptom stays legible. */
    const asAbsent = planPages({ hierarchy: null, chainParams: g.chain_params });
    if (asAbsent.pages[0].keys[0] !== "sample_path")
      fail("the granny chain_params fallback no longer starts with sample_path — " +
           "this test no longer describes the reported symptom");

    /* ...and that is exactly what an unresolved read must NOT produce. */
    for (const h of [null, g.ui_hierarchy]) {
      const r = planPages({ hierarchy: h, chainParams: g.chain_params, unresolved: true });
      if (!r.unresolved) fail("planPages did not report unresolved back to the caller");
      if (r.pages.length !== 0)
        fail("an unresolved contract produced " + r.pages.length + " page(s): " +
             JSON.stringify((r.pages[0] || {}).keys));
      if ((r.pages[0] || { keys: [] }).keys[0] === "sample_path")
        fail("an unresolved contract put sample_path on knob 1");
    }

    /* ...and the fallback that legitimately exists must be untouched: four
     * fleet modules publish chain_params and no hierarchy at all. */
    for (const id of ["branchage", "belt-in", "po32-drum", "smack-in"]) {
      const m = fx.modules.find((x) => x.id === id);
      if (!m) continue;
      if (m.ui_hierarchy) fail(id + " grew a ui_hierarchy; pick another no-hierarchy module");
      const r = planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params });
      if (r.unresolved) fail(id + ": genuine absence was reported as unresolved");
      if (!r.pages.length) fail(id + ": chain_params pagination fallback stopped working");
      if (r.pages[0].name !== "Params") fail(id + ": fallback page 1 is named " + r.pages[0].name);
      if (r.pages[0].keys[0] !== m.chain_params[0].key)
        fail(id + ": fallback no longer paginates in declaration order");
    }
  }

  /* ---- a SELECTOR key never takes a knob, even when authored -----------
   *
   * Reported from the device: "why is preset a knob on impressive chords?"
   * Its root level declares preset_index as list_param AND lists it first in
   * knobs[], so it got a browser page and knob 1. knobs[] there is
   * byte-identical to params[] -- the author listed everything rather than
   * curating -- and the knob could not work anyway: preset_index is declared
   * int 0..500 against 52 presets.
   *
   * Fleet-wide only two levels do this (impressive-chords, breakbeat) and both
   * are the uncurated case, so dropping it costs nothing and no module loses a
   * control: the browser page built from the same list_param is still there.
   */
  {
    for (const id of ["impressive-chords", "breakbeat"]) {
      const m = fx.modules.find((x) => x.id === id);
      if (!m) fail(id + " is missing from the fixture");
      const r = planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params });
      const lv = (m.ui_hierarchy.levels || {}).root || {};
      const sel = lv.list_param;
      if (!sel) fail(id + " root no longer declares list_param; pick another module");
      if (!(lv.knobs || []).includes(sel))
        fail(id + " no longer authors " + sel + " as a knob; this case is gone from the fleet");
      for (const pg of r.pages) {
        if (pg.kind !== "knobs") continue;
        if ((pg.keys || []).includes(sel))
          fail(id + ": selector " + sel + " still took a knob cell on page " + pg.name);
      }
      /* The control is not lost -- it has its own page, from the same key. */
      if (!r.pages.some((pg) => pg.kind === "preset" && pg.listParam === sel))
        fail(id + ": dropping " + sel + " from the knobs also lost its browser page");
    }
  }

  console.log("PASS: param-page planner — " + fx.modules.length + " modules, " + totalPages +
              " pages, every declared key reachable, no duplicate page names");
});
'
