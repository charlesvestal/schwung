#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Navigation over a planned PageSet (src/shared/param_pages/page_nav.mjs).
#
# The native shadow UI and any tool driving the same grid must move around it
# identically, so the rules live in one pure module and are pinned here.
#
# The pressure these tests encode: minijv plans to 76 pages and surge to 51.
# Linear jog is not navigation at that size, and 52 of the fleet's grid pages
# are one- or two-control continuations that a coarse gesture should skip.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param-page navigation tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_nav.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("node:fs"),
]).then(([N, P, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const plan = (id) => {
    const m = fx.modules.find((x) => x.id === id);
    if (!m) fail("fixture has no module \"" + id + "\"");
    return P.planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params }).pages;
  };

  /* ---- 1. stepping clamps rather than wraps ----------------------------- */
  {
    const pages = plan("obxd");
    if (N.step(pages, 0, -1) !== 0) fail("stepping back from the first page must stay put, not wrap to the end");
    const last = pages.length - 1;
    if (N.step(pages, last, 1) !== last) fail("stepping past the last page must stay put");
    if (N.step(pages, 3, 1) !== 4) fail("an ordinary step should advance by one");
    if (N.step([], 0, 1) !== 0) fail("an empty page set must not throw");
  }

  /* ---- 2. level-step skips continuation pages --------------------------- */
  {
    const pages = plan("minijv");
    /* Find a level that actually spans two pages. */
    let i = pages.findIndex((p, k) => k > 0 && / - 2$/.test(p.name));
    if (i < 0) fail("minijv should have a continuation page to skip");
    const base = i - 1;
    if (N.step(pages, base, 1) !== base + 1) fail("a fine step should land ON the continuation page");
    const coarse = N.stepLevel(pages, base, 1);
    if (coarse === base + 1) fail("a level step should skip past the continuation page");
    if (pages[coarse].level === pages[base].level) fail("a level step should land on a different level");
  }

  /* ---- 3. stepping back lands on a level FIRST page --------------------- */
  {
    const pages = plan("minijv");
    const i = pages.findIndex((p, k) => k > 0 && / - 2$/.test(p.name));
    /* From the page after a two-page level, going back should land on that
     * level page 1, not its continuation. */
    const back = N.stepLevel(pages, i + 1, -1);
    if (/ - \d+$/.test(pages[back].name)) {
      fail("stepping back landed mid-level on \"" + pages[back].name + "\"");
    }
  }

  /* ---- 4. level-stepping traverses the whole set and terminates ---------- */
  {
    for (const id of ["minijv", "surge", "obxd", "sf2", "branchage"]) {
      const pages = plan(id);
      let i = 0, guard = 0;
      const seen = new Set([0]);
      for (;;) {
        const next = N.stepLevel(pages, i, 1);
        if (next === i) break;
        if (seen.has(next) && next < i) fail(id + ": level stepping went backwards");
        seen.add(next); i = next;
        if (++guard > 500) fail(id + ": level stepping did not terminate");
      }
      if (i !== pages.length - 1) fail(id + ": level stepping stopped at " + i + " of " + (pages.length - 1));
    }
  }

  /* ---- 5. the jump index is per level, not per page --------------------- */
  {
    const pages = plan("minijv");
    const jx = N.jumpIndex(pages);
    if (jx.length >= pages.length) fail("the jump index should be shorter than the page list");
    if (jx.some((e) => / - \d+$/.test(e.name))) fail("the jump index should not list continuation pages: " +
      jx.filter((e) => / - \d+$/.test(e.name)).map((e) => e.name).join(", "));
    for (const e of jx) {
      if (pages[e.index] === undefined) fail("jump entry points outside the page list");
    }
    const multi = jx.find((e) => e.pages > 1);
    if (!multi) fail("minijv has multi-page levels; the index should say so");
  }

  /* ---- 6. grouping folds the four tone subtrees ------------------------- */
  {
    const pages = plan("minijv");
    const groups = N.groupIndex(pages);
    const jx = N.jumpIndex(pages);
    if (groups.length >= jx.length) fail("grouping did not reduce the index");
    if (groups.length > 25) fail("minijv should fold to well under 25 groups, got " + groups.length);
    for (const g of groups) if (pages[g.index] === undefined) fail("group points outside the page list");
  }

  /* ---- 7. rebuild keeps the user where they were ------------------------ */
  {
    const pages = plan("obxd");
    const idx = pages.findIndex((p) => p.name === "Filter");
    if (idx < 0) fail("obxd should have a Filter page");

    /* A module finishing its load republishes a bigger tree: every index moves. */
    const grown = [{ kind: "knobs", name: "Brand New", level: "new", keys: ["x"] }, ...pages];
    const landed = N.reanchor(pages, idx, grown);
    if (grown[landed].name !== "Filter") fail("reanchor lost the page by name, landed on " + grown[landed].name);

    /* The page disappears entirely — fall back to the same level, then a grid. */
    const without = pages.filter((p) => p.name !== "Filter");
    const fallback = N.reanchor(pages, idx, without);
    if (without[fallback].kind !== "knobs") fail("reanchor should fall back to a grid page");

    if (N.reanchor(pages, idx, []) !== 0) fail("reanchor into an empty set must be 0");
  }

  /* ---- 8. the landing page skips a leading preset/mode screen ----------- */
  {
    const jv = plan("minijv");
    if (jv[0].kind !== "modes") fail("minijv should open with a mode page");
    if (jv[N.firstGrid(jv)].kind !== "knobs") fail("firstGrid should find a grid page");
    const only = [{ kind: "preset", name: "Presets", level: "root" }];
    if (N.firstGrid(only) !== 0) fail("with no grid page, firstGrid should be 0");
  }

  console.log("PASS: param-page navigation — clamping, level skip, jump index, grouping, rebuild reanchor");
});
'
