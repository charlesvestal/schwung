#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Rendering tests for the param-page grid.
#
# Two halves:
#
#  1. INVARIANTS swept over every grid page of all 76 fleet modules, in both
#     layouts. These catch the bugs that are invisible in code review and
#     obvious on hardware — most importantly a character the device font cannot
#     draw, which renders as *nothing* on the OLED. Five modules ship such
#     strings today (forge "Copy A>B", sfz's cents unit, euclidrum's "—" enum
#     option), so the renderer folds them to ASCII.
#
#  2. SNAPSHOTS of representative pages, stored as half-block art in
#     tests/fixtures/snapshots/param_pages.txt so a layout change shows up as a
#     readable diff rather than a number. Regenerate deliberately with:
#
#         UPDATE_SNAPSHOTS=1 bash tests/host/test_param_pages_render.sh
#
# The renderer draws through the device's own font atlas, so these are
# pixel-accurate, not an approximation. See tools/param-pages/harness.mjs.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param-page render tests" >&2
  exit 1
fi

UPDATE="${UPDATE_SNAPSHOTS:-}" node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./tools/param-pages/cases.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page.mjs"),
  import("./src/shared/param_pages/page_nav.mjs"),
  import("node:fs"),
]).then(([H, C, P, M, R, N, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));

  /* ---- 1. fleet sweep: both layouts, every grid page ------------------- */
  const missing = new Set();
  let rendered = 0;
  const blank = [], spill = [], wide = [];

  for (const mod of fx.modules) {
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    for (let i = 0; i < pages.length; i++) {
      const page = pages[i];
      if (page.kind !== P.PAGE_KNOBS) continue;
      if (page.keys.length > 8) wide.push(mod.id + "#" + i + " (" + page.keys.length + " keys)");

      const values = {};
      for (const k of page.keys) values[k] = C.fakeValue(k, metaIndex.getOrGuess(k));

      for (const layout of [R.LAYOUT_BAR, R.LAYOUT_DIAL]) {
        const fb = H.createFramebuffer();
        R.renderPage(H.drawContext(fb), {
          page, metaIndex, values,
          title: "T1 > " + mod.id.toUpperCase(),
          pageIndex: i, pageCount: pages.length, layout,
        });
        rendered++;
        for (const g of fb.missingGlyphs) missing.add(g);
        if (fb.countLit() < 50) blank.push(mod.id + "#" + i + "/" + layout);
        /* Anything the framebuffer had to clip is content the OLED would
         * silently drop — row 63 is legal, row 64 is a layout bug. */
        if (fb.clipped() > 0) spill.push(mod.id + "#" + i + "/" + layout + " (" + fb.clipped() + "px)");
      }
    }
  }

  if (rendered < 1000) fail("only " + rendered + " renders — the sweep is not covering the fleet");
  if (missing.size) {
    fail("characters the device font cannot draw reached the screen: " +
         [...missing].map((c) => JSON.stringify(c) + " U+" + c.codePointAt(0).toString(16)).join(", ") +
         " — add them to ASCII_FOLD in render_page.mjs");
  }
  if (blank.length) fail("near-blank pages: " + blank.slice(0, 6).join(", "));
  if (spill.length) fail("content drawn outside the 128x64 display: " + spill.slice(0, 6).join(", "));
  if (wide.length) fail("a page carried more than 8 keys: " + wide.slice(0, 6).join(", "));

  /* ---- 1b. draw-call budget --------------------------------------------- */
  {
    /* Every pixel crosses a QuickJS->C binding on device, so the number of draw
     * calls per frame is the cost that matters, not wall time here. Measured
     * over the fleet: bar median 52 / max 72, dial median 290 / max 472 — the
     * dial is dearer because a circle outline is emitted pixel by pixel. Both
     * are affordable when the shadow UI only redraws on change; these ceilings
     * exist so a future change cannot quietly make it ten times worse. */
    const BUDGET = { bar: 120, dial: 700 };
    const worst = { bar: 0, dial: 0 };
    for (const mod of fx.modules) {
      const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      for (let i = 0; i < pages.length; i++) {
        const page = pages[i];
        if (page.kind !== P.PAGE_KNOBS) continue;
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
            pageIndex: i, pageCount: pages.length, layout,
          });
          if (calls > worst[layout]) worst[layout] = calls;
          if (calls > BUDGET[layout]) {
            fail(mod.id + "#" + i + " (" + layout + ") took " + calls +
                 " draw calls, budget " + BUDGET[layout]);
          }
        }
      }
    }
    if (worst.bar < 20 || worst.dial < 50) fail("the draw-call counter is not measuring anything");
  }

  /* ---- 2. the fold itself ---------------------------------------------- */
  {
    const cases = [["Copy A→B", "Copy A>B"], ["Swap A↔B", "Swap A<>B"], ["¢", "c"], ["—", "-"], ["plain", "plain"]];
    for (const [input, want] of cases) {
      const got = R.asciiFold(input);
      if (got !== want) fail(`asciiFold(${JSON.stringify(input)}) = ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
    }
    /* Unmapped and unprintable becomes a visible "?" rather than nothing. */
    if (R.asciiFold("☠") !== "?") fail("an unmapped character should fold to \"?\"");
  }

  /* ---- 3. label shortening keeps the distinguishing part ---------------- */
  {
    const fb = H.createFramebuffer();
    const ctx = H.drawContext(fb);
    const W = 30;   /* a 32 px cell less its gutters */
    const short = (s) => R.shortenLabel(ctx, s, W);
    if (fb.textWidth(short("Filter Env")) > W) fail("\"Filter Env\" did not fit");
    if (!/Env$/.test(short("Filter Env"))) fail("two-word shortening dropped the distinguishing word: " + short("Filter Env"));
    if (short("Cutoff") !== "Cutof") fail("short single words should truncate, got " + short("Cutoff"));
    if (short("Gain") !== "Gain") fail("a label that already fits must not change");
    /* Head words reduce toward their initials while the last word is kept as
     * long as it fits: "Low Freq Osc" -> "LFOsc", not "LFO". */
    if (short("Low Freq Osc") !== "LFOsc") fail("multi-word shortening changed, got " + short("Low Freq Osc"));
    /* A numeral is an index, not a word to initialise: "Osc 1 Octave" must not
     * collapse to "O1O" — surge has 100+ params named this way. */
    if (short("Osc 1 Octave") !== "O1Oct") fail("a numeric word was abbreviated away, got " + short("Osc 1 Octave"));
    if (!/1/.test(short("Osc 1 Unison Voices"))) fail("an index must survive shortening");
    /* Page names hold back the " - N" suffix so you can still tell which page. */
    const pn = R.fitPageName(ctx, "Oscillator 1 - 2", 86);
    if (!/ - 2$/.test(pn)) fail("a page number was truncated away: " + pn);
    if (!/1/.test(pn)) fail("a page name lost its index: " + pn);
  }

  /* ---- 3b. cells are disambiguated against each other ------------------ */
  {
    const fb = H.createFramebuffer();
    const ctx = H.drawContext(fb);
    /* euclidrum declares all eight lane switches as name:"Enabled" and only the
     * key says which lane. Eight cells reading "Enabl" is a broken screen, not
     * an abbreviation. */
    const mod = fx.modules.find((m) => m.id === "euclidrum");
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const page = pages.find((p) => p.kind === P.PAGE_KNOBS);
    const labels = R.pageCellLabels(ctx, page.keys, metaIndex, 30);
    if (new Set(labels).size !== labels.length) fail("euclidrum cells still collide: " + labels.join(","));
    if (!labels.every((l) => /\d$/.test(l))) fail("lane labels should carry their index: " + labels.join(","));

    /* And nowhere in the fleet may two cells on one page read the same. */
    const bad = [];
    for (const m2 of fx.modules) {
      const pp = P.planPages({ hierarchy: m2.ui_hierarchy, chainParams: m2.chain_params }).pages;
      const ix = M.buildMetaIndex({ hierarchy: m2.ui_hierarchy, chainParams: m2.chain_params });
      for (const pg of pp) {
        if (pg.kind !== P.PAGE_KNOBS) continue;
        const L = R.pageCellLabels(ctx, pg.keys, ix, 30).filter(Boolean);
        if (L.length !== new Set(L).size) bad.push(m2.id + "/" + pg.name + ": " + L.join(","));
      }
    }
    if (bad.length) fail(bad.length + " pages have duplicate cell labels, e.g. " + bad[0]);
  }

  /* ---- 3c. an unused knob position is marked, not blank ----------------- */
  {
    /* A three-control page leaving five cells empty reads as a failed render
     * unless the empty positions say something. */
    const mod = fx.modules.find((m) => m.id === "branchage");
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const sparse = pages.find((p) => p.kind === P.PAGE_KNOBS && p.keys.length > 0 && p.keys.length <= 4);
    if (!sparse) fail("expected a sparse page in branchage");

    const values = {};
    for (const k of sparse.keys) values[k] = C.fakeValue(k, metaIndex.getOrGuess(k));
    const withMarks = H.createFramebuffer();
    R.renderPage(H.drawContext(withMarks), {
      page: sparse, metaIndex, values, title: "T", pageIndex: 0, pageCount: 1,
    });
    const full = H.createFramebuffer();
    R.renderPage(H.drawContext(full), {
      page: { ...sparse, keys: sparse.keys.slice(0, sparse.keys.length) },
      metaIndex, values, title: "T", pageIndex: 0, pageCount: 1,
    });
    /* The bottom row holds no params on this page, so any ink there is the
     * empty-cell marker. */
    let bottomInk = 0;
    for (let y = 40; y < 64; y++) for (let x = 0; x < 128; x++) bottomInk += withMarks.pixels[y * 128 + x];
    if (bottomInk === 0) fail("empty knob positions are completely blank — the page looks broken");
    if (bottomInk > 400) fail("empty-cell markers are too loud: " + bottomInk + " lit pixels");
  }

  /* ---- 3d. a modulated param is marked ---------------------------------- */
  {
    const mod = fx.modules.find((m) => m.id === "obxd");
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const page = pages.find((p) => p.kind === P.PAGE_KNOBS);
    const values = {};
    for (const k of page.keys) values[k] = "0.5";

    const plain = H.createFramebuffer();
    R.renderPage(H.drawContext(plain), { page, metaIndex, values, title: "T", pageIndex: 0, pageCount: 1 });
    const marked = H.createFramebuffer();
    R.renderPage(H.drawContext(marked), {
      page, metaIndex, values, title: "T", pageIndex: 0, pageCount: 1,
      modulated: (k) => k === page.keys[0],
    });
    if (marked.countLit() <= plain.countLit()) {
      fail("a modulated param is drawn identically to an unmodulated one — the list editor marks these with ~");
    }
  }

  /* ---- 3e. the section picker ------------------------------------------- */
  {
    const mod = fx.modules.find((m) => m.id === "minijv");
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const groups = N.groupIndex(pages);
    if (groups.length > 25) fail("minijv should fold to under 25 sections, got " + groups.length);
    /* "Tone 1" (the level page) and "Tone1/…" (its children) differ only by a
     * space and must not list as two sections. */
    const names = groups.map((g) => String(g.name).replace(/\s+/g, "").toLowerCase());
    if (names.length !== new Set(names).size) fail("sections contain near-duplicate names: " + groups.map((g) => g.name).join(","));

    const fb = H.createFramebuffer();
    R.renderPicker(H.drawContext(fb), { entries: groups, index: 6, title: "Sections" });
    if (fb.clipped() > 0) fail("the picker drew outside the display");
    if (fb.missingGlyphs.size) fail("the picker used characters the font cannot draw");
    if (fb.countLit() < 100) fail("the picker drew almost nothing");

    /* Selecting the last entry must still render (scroll clamp). */
    const fb2 = H.createFramebuffer();
    R.renderPicker(H.drawContext(fb2), { entries: groups, index: groups.length - 1, title: "Sections" });
    if (fb2.clipped() > 0) fail("the picker clipped at the end of the list");
  }

  /* ---- 3e2. the page rule is a map, not a row of ticks ------------------ */
  {
    /* Measured off the rendered pixels, because this is a geometry problem and
     * every previous attempt at it was wrong in a way only pixels show. */
    const readRule = (fb, y) => {
      const runs = [];
      let run = null;
      for (let x = 0; x < 128; x++) {
        const lit = fb.pixels[y * 128 + x];
        if (lit && !run) run = { start: x, len: 1 };
        else if (lit) run.len++;
        else if (run) { runs.push(run); run = null; }
      }
      if (run) runs.push(run);
      return runs;
    };

    for (const [id, pageIdx] of [["minijv", 20], ["surge", 10], ["osirus", 5], ["obxd", 3]]) {
      const mod = fx.modules.find((m) => m.id === id);
      const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const groups = pages.map((p) => (p.level == null ? p.kind : p.level));
      const page = pages[pageIdx];
      const fb = H.createFramebuffer();
      R.renderPage(H.drawContext(fb), {
        page, metaIndex, values: {}, title: "T", pageIndex: pageIdx,
        pageCount: pages.length, pageGroups: groups,
      });

      const runs = readRule(fb, 8);
      if (!runs.length) fail(id + ": the page rule drew nothing");

      /* It spans the full width — no margin at either end. */
      if (runs[0].start !== 0) fail(id + ": the page rule starts at x=" + runs[0].start + ", not 0");
      const last = runs[runs.length - 1];
      if (last.start + last.len !== 128) fail(id + ": the page rule stops at " + (last.start + last.len) + ", not 128");

      /* Any gap that IS drawn is exactly one pixel — a separator, never a
       * spacer. (A dense module drops separators entirely and draws one solid
       * bar; that is the trade, not a bug.) */
      for (let i = 1; i < runs.length; i++) {
        const gap = runs[i].start - (runs[i - 1].start + runs[i - 1].len);
        if (gap !== 1) fail(id + ": found a " + gap + "px gap in the page rule; separators must be 1px");
      }

      /* Every module gets a real per-page ruler rather than the position-marker
       * fallback: separators are dropped before pages are, because a visible
       * page matters more than a visible separator. The marker fallback draws
       * one 8px block on an otherwise 1px rule, which is what this rejects. */
      /* The fallback draws a fixed 8px block regardless of page count; a real
       * ruler draws ONE PAGE, so its marker is about w/pageCount wide. At 17
       * pages that is legitimately ~8px, so compare against the page width
       * rather than against a constant. */
      let tallW = 0;
      for (let x = 0; x < 128; x++) if (fb.pixels[10 * 128 + x]) tallW++;
      const perPage = Math.ceil(128 / pages.length);
      if (tallW > perPage + 1) {
        fail(id + " (" + pages.length + " pages): position marker is " + tallW +
             "px but one page is only " + perPage + "px — it fell back to the marker");
      }
    }

    /* The current page is drawn taller, or there is no "you are here". */
    {
      const mod = fx.modules.find((m) => m.id === "obxd");
      const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const groups = pages.map((p) => (p.level == null ? p.kind : p.level));
      const fb = H.createFramebuffer();
      R.renderPage(H.drawContext(fb), {
        page: pages[3], metaIndex, values: {}, title: "T", pageIndex: 3,
        pageCount: pages.length, pageGroups: groups,
      });
      let tall = 0;
      for (let x = 0; x < 128; x++) if (fb.pixels[10 * 128 + x]) tall++;
      if (tall === 0) fail("no page in the rule is drawn taller — there is no position indicator");
      if (tall > 40) fail("too much of the page rule is drawn tall: " + tall + "px");
    }
  }

  /* ---- 3f. the first-run hint ------------------------------------------ */
  {
    const fb = H.createFramebuffer();
    const ctx = H.drawContext(fb);
    const lines = ["Jog: page", "Shift+Jog: section", "Click: section list",
                   "Hold knob: name", "Shift: show values"];
    /* Longer lines clip silently, which would ship a hint nobody can read. */
    for (const l of lines) {
      if (ctx.textWidth(l) > 114) fail("hint line does not fit the panel: " + JSON.stringify(l));
    }
    R.renderHint(ctx, { lines, title: "Param Pages" });
    if (fb.clipped() > 0) fail("the hint drew outside the display");
    if (fb.missingGlyphs.size) fail("the hint used characters the font cannot draw");
    if (fb.countLit() < 100) fail("the hint drew almost nothing");
  }

  /* ---- 4. the renderer touches nothing outside its rect ----------------- */
  {
    const mod = fx.modules.find((m) => m.id === "obxd");
    const metaIndex = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const { pages } = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const page = pages.find((p) => p.kind === P.PAGE_KNOBS);
    const fb = H.createFramebuffer();
    /* Half-height region: a tool drawing the grid under its own header. */
    R.renderPage(H.drawContext(fb), {
      page, metaIndex, values: {}, title: "T", pageIndex: 0, pageCount: 1,
      rect: { x: 0, y: 0, w: 128, h: 32 },
    });
    let below = 0;
    for (let y = 32; y < 64; y++) for (let x = 0; x < 128; x++) below += fb.pixels[y * 128 + x];
    if (below > 0) fail("renderPage drew " + below + " pixels outside its rect — a tool cannot share the screen with it");
  }

  /* ---- 5. purity: same inputs, same pixels ------------------------------ */
  {
    const one = C.renderCases().text;
    const two = C.renderCases().text;
    if (one !== two) fail("rendering is not deterministic");
  }

  /* ---- 6. snapshots ------------------------------------------------------ */
  {
    const { text } = C.renderCases();
    if (process.env.UPDATE) {
      fs.writeFileSync(C.SNAPSHOT, text);
      console.log("UPDATED " + C.SNAPSHOT);
    } else {
      if (!fs.existsSync(C.SNAPSHOT)) fail("no snapshot file — run with UPDATE_SNAPSHOTS=1");
      const want = fs.readFileSync(C.SNAPSHOT, "utf8");
      if (want !== text) {
        const a = want.split("\n"), b = text.split("\n");
        let firstDiff = -1;
        for (let i = 0; i < Math.max(a.length, b.length); i++) if (a[i] !== b[i]) { firstDiff = i; break; }
        let header = "";
        for (let i = firstDiff; i >= 0; i--) if (b[i] && b[i].startsWith("### ")) { header = b[i]; break; }
        fail("rendering changed at line " + (firstDiff + 1) + (header ? "  [" + header.slice(4) + "]" : "") +
             "\n  expected: " + JSON.stringify((a[firstDiff] || "").slice(0, 70)) +
             "\n  actual:   " + JSON.stringify((b[firstDiff] || "").slice(0, 70)) +
             "\n  If this change is intended: UPDATE_SNAPSHOTS=1 bash tests/host/test_param_pages_render.sh");
      }
    }
  }

  console.log("PASS: param-page rendering — " + rendered + " page renders, no undrawable characters, " +
              "nothing off-screen, snapshots match");
});
'
