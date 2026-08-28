#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A module declaring `modes` gets ONE mode's pages, not both.
#
# minijv is the only module in the fleet that declares them. The planner
# re-roots the walk to the active mode (page_plan.mjs), and then the orphan
# sweep at the end used to haul the inactive tree straight back in -- its
# comment names minijv's own performance/perf_main/part_selector, because it
# was written for them back when the Mode page was unreachable and those
# levels really were orphans. Measured before the fix: 76 pages in BOTH modes,
# differing only in which level got named "Main". In Performance you jogged
# past 69 patch pages to reach the 6 performance ones; in Patch, "Edit Parts"
# sat at position 72 editing a performance you were not in.
#
# The sweep still has to work. A level reachable from NO mode root is a true
# orphan and must still be emitted -- that is the case the sweep exists for.
# Only "reachable from a mode root that is not the active one" is excluded.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param-page planner tests" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/page_plan.mjs").then(async (m) => {
  const fs = await import("node:fs");
  const { planPages } = m;
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  const mod = fx.modules.find((x) => x.id === "minijv");
  if (!mod) fail("fixture has no minijv");
  const H = mod.ui_hierarchy;
  if (!Array.isArray(H.modes) || H.modes.length !== 2)
    fail("minijv no longer declares two modes; this test has no subject");

  const planFor = (mode) => planPages({ hierarchy: H, chainParams: mod.chain_params, mode });
  const patch = planFor("patch");
  const perf = planFor("performance");

  /* ---- 1. the two page sets are disjoint apart from the Mode page ---- */
  const levelsOf = (r) => new Set(r.pages.map((p) => p.level).filter((x) => x));
  const P = levelsOf(patch), F = levelsOf(perf);
  const shared = [...P].filter((k) => F.has(k));
  if (shared.length) fail("levels reachable in BOTH modes: " + shared.join(", "));

  /* Named, not just counted: a plan that lost the performance tree entirely
     would also produce an empty intersection too.

     patch_main / perf_main are deliberately absent from BOTH lists: each is a
     `children` alias re-listing the knobs of the level above it, and the walk
     dedupes an identical key list rather than drawing the same page twice. */
  for (const need of ["patch", "patch_common", "effects", "tone1", "save_slot", "expansions"])
    if (!P.has(need)) fail("patch mode lost level " + need);
  for (const need of ["performance", "part_selector", "load_expansion"])
    if (!F.has(need)) fail("performance mode lost level " + need);
  for (const bad of ["performance", "part_selector", "load_expansion"])
    if (P.has(bad)) fail("patch mode still shows " + bad);
  for (const bad of ["patch", "tone1", "effects", "save_slot"])
    if (F.has(bad)) fail("performance mode still shows " + bad);

  /* The Mode page belongs to no level and must survive in both. */
  const modePage = (r) => r.pages.find((p) => p.modeSelect);
  if (!modePage(patch) || !modePage(perf)) fail("the Mode page vanished");

  /* ---- 2. performance mode is not buried behind the patch tree ---- */
  if (perf.pages.length >= patch.pages.length)
    fail("performance plans " + perf.pages.length + " pages vs patch " +
         patch.pages.length + " -- the trees are still merged");

  /* ---- 3. a TRUE orphan is still swept in ----
     The sweep is the only thing that reaches a level no edge names, and
     minijv has none, so synthesise one rather than trusting the fleet. */
  const withOrphan = JSON.parse(JSON.stringify(H));
  withOrphan.levels.stray = { label: "Stray", knobs: ["octave_transpose"],
                              params: [{ key: "octave_transpose", label: "Octave" }] };
  const strayed = planPages({ hierarchy: withOrphan, chainParams: mod.chain_params, mode: "patch" });
  if (!strayed.pages.some((p) => p.level === "stray"))
    fail("a level reachable from NO mode root was dropped -- the sweep is over-tightened");

  /* ---- 4. an unknown mode still falls back to the first, and still gates ---- */
  const bogus = planFor("no-such-mode");
  if (bogus.pages.length !== patch.pages.length)
    fail("an unknown mode did not fall back to the first mode");

  console.log("PASS: mode gating — patch " + patch.pages.length + " pages, performance " +
              perf.pages.length + ", disjoint levels, true orphans still swept");
});
'
