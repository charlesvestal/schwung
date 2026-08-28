#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Levels that declare CHILDREN — minijv's "Edit Parts" is the only one in the
# fleet, and it was broken twice over.
#
# A child level lists TEMPLATE keys: minijv's part_selector lists `partlevel`,
# but the DSP serves `sram_part_<n>_partlevel` and gates on that prefix.
#
#  (a) the planner built knob pages straight from those templates, so two pages
#      of cells looked completely alive — chain_params gives them labels and
#      int 0..127 ranges — and did nothing at all when turned. No error, no
#      blank, no signal. Worse than the level being unreachable.
#
#  (b) the PAGE_CHILD page has no renderer, so it hands off to the list editor
#      — into a level that was itself broken there, because the hand-off never
#      set hierEditorChildCount. loadHierarchyLevel gates its child selector on
#      that, so you got eleven rows of unprefixed template keys instead of
#      "Part 1..8".
#
# Reported from the device as "mini jv - edit parts doesnt do anything".

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

fail() { echo "FAIL: $1" >&2; exit 1; }

# ---- (b) the hand-off carries the child count -------------------------------
file="src/shadow/shadow_ui.js"
blk=$(awk '/^function enterHierarchyEditorFromParamPages\(/,/^}/' "$file")
[ -n "$blk" ] || fail "enterHierarchyEditorFromParamPages is gone"
# Matched on the ASSIGNMENT, not on how the count is derived: it went from
# `levelDef.child_count` to childLevelCount(levelDef) when shadow_ui.js was
# migrated onto child_key.mjs, and pinning the old expression made a strictly
# broader implementation fail. What must hold is that the hand-off sets it,
# and sets it before the level loads.
command grep -qE "hierEditorChildCount = (levelDef\.child_count|childLevelCount\(levelDef\))" <<<"$blk" || \
  fail "the grid hand-off does not set hierEditorChildCount — a child level lands as a flat list of unprefixed keys"
command grep -q "hierEditorChildLabel = levelDef.child_label" <<<"$blk" || \
  fail "the grid hand-off does not set hierEditorChildLabel"
# It must be set BEFORE the level is loaded, or the gate has already run.
cnt=$(command grep -nE "hierEditorChildCount = (levelDef\.child_count|childLevelCount\(levelDef\))" <<<"$blk" | head -n 1 | cut -d: -f1)
ldl=$(command grep -n "loadHierarchyLevel()" <<<"$blk" | tail -n 1 | cut -d: -f1)
[ -n "$cnt" ] && [ -n "$ldl" ] && [ "$cnt" -lt "$ldl" ] || \
  fail "the child count is set AFTER loadHierarchyLevel — the selector gate has already been evaluated"
echo "  ok  the grid hand-off carries child_count/child_label, before the level loads"

# ---- (a) the level's knob pages resolve to CONCRETE keys --------------------
node -e '
Promise.all([
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/child_key.mjs"),
  import("./src/shared/param_pages/page_controller.mjs"),
]).then(async ([P, M, K, C]) => {
  const say = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const j = JSON.parse(require("fs").readFileSync("tests/fixtures/module-contracts.json", "utf8"));

  let checked = 0;
  for (const mod of j.modules) {
    let h = mod.ui_hierarchy, cp = mod.chain_params;
    if (typeof h === "string") { try { h = JSON.parse(h); } catch { continue; } }
    if (typeof cp === "string") { try { cp = JSON.parse(cp); } catch { cp = null; } }
    if (!h || !h.levels) continue;
    const childLevels = Object.keys(h.levels).filter((k) => K.hasChildren(h.levels[k]));
    if (!childLevels.length) continue;
    checked++;

    const ix = M.buildMetaIndex({ hierarchy: h, chainParams: cp });
    /* A module with `modes` plans one mode at a time, so a child level living
       in the second mode is absent from the first mode`s plan and that is
       correct. "Reachable" means reachable in SOME mode. */
    const modeList = (Array.isArray(h.modes) && h.modes.length) ? h.modes : [undefined];
    const pages = modeList.flatMap(
      (md) => P.planPages({ hierarchy: h, chainParams: cp, metaIndex: ix, mode: md }).pages);

    for (const lvl of childLevels) {
      const sel = pages.filter((p) => p.level === lvl && p.kind === P.PAGE_ITEMS && p.childOf);
      if (!sel.length) say(mod.id + "/" + lvl + " has no selector page — the level is unreachable");
      /* The knob pages are BACK, and each must carry its level or its keys
         cannot be resolved and the cells go back to addressing templates. */
      const knobs = pages.filter((p) => p.level === lvl && p.kind === P.PAGE_KNOBS);
      if (!knobs.length) say(mod.id + "/" + lvl + " has no parameter pages at all");
      for (const kp of knobs)
        if (!kp.childLevel)
          say(mod.id + "/" + lvl + " knob page " + JSON.stringify(kp.name) +
              " carries no level -- its keys would go to the wire as templates");
    }
  }
  if (!checked) say("no module in the fixture declares a child level — this test asserts nothing");

  /* ---- end to end: choosing a child re-keys the level ------------------ */
  const m = j.modules.find((x) => x.id === "minijv");
  let h = m.ui_hierarchy, cp = m.chain_params;
  if (typeof h === "string") h = JSON.parse(h);
  if (typeof cp === "string") cp = JSON.parse(cp);
  const reads = [], writes = [];
  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(h);
      if (b === "chain_params") return JSON.stringify(cp);
      /* Parts exist ONLY in performance mode, and since the modes gate the
         walk the controller has to be told which one we are in -- answered
         the way the module answers it, capitalised, to exercise the seeding
         match as well (the level is called "performance"). */
      if (b === "mode") return "Performance";
      reads.push(b); return "10";
    },
    setParam: (k) => writes.push(String(k).replace(/^[^:]+:/, "")),
    announce: () => {},
  });
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 10; i++) ctl.tick();

  const at = ctl.pages.findIndex((p) => p.childOf);
  if (at < 0) say("minijv has no child selector");
  ctl.goToPage(at);
  ctl.onClick(0);                       /* enter the list */
  ctl.onJog(1); ctl.onJog(1);           /* highlight the third */
  writes.length = 0;
  ctl.onClick(0);                       /* commit */

  if (writes.length)
    say("choosing a child WROTE " + JSON.stringify(writes) + " — it is a local selection, " +
        "there is no param behind it");
  if (!(ctl.page && ctl.page.level === "part_selector" && ctl.page.kind === P.PAGE_KNOBS))
    say("after choosing a part we are on " + JSON.stringify(ctl.page && ctl.page.name) +
        " — the choice must land on the parameters it just re-keyed");

  reads.length = 0;
  for (let i = 0; i < 12; i++) ctl.tick();
  const bare = reads.filter((k) => /^part(level|pan)$/.test(k));
  if (bare.length)
    say("still reading TEMPLATE keys after choosing a child: " + JSON.stringify(bare));
  if (!reads.some((k) => k.startsWith("sram_part_2_")))
    say("reads after choosing Part 3 are " + JSON.stringify([...new Set(reads)].slice(0, 4)) +
        " — expected sram_part_2_*");

  /* And a turn must WRITE the resolved key, not the template. */
  writes.length = 0;
  ctl.onKnobTurn(0, 1, 1000);
  ctl.flushWrites && ctl.flushWrites();
  for (let i = 0; i < 6; i++) ctl.tick();
  if (writes.length && !writes.some((k) => k.startsWith("sram_part_2_")))
    say("a knob turn wrote " + JSON.stringify(writes) + " — not the resolved child key");

  console.log("  ok  " + checked + " module(s) with child levels: a selector, and parameter pages that carry it");
  /* ---- the header must say WHICH CHILD -------------------------------
   *
   * The planned name is the page identity and cannot know which child is
   * selected: minijv plans "Edit Parts - 2", where the 2 is the second page
   * OF THE LEVEL. A user who just chose a part reads that as the part -- the
   * numbers collide by coincidence -- and at 57px it truncated to
   * "EDIT PARTS -" and showed no number at all.
   */
  {
    const shown = String(ctl.pageLabel());
    if (!/^Part 3\b/.test(shown))
      say("after choosing Part 3 the header reads " + JSON.stringify(shown) +
          " -- it must name the child");
    if (/Edit Parts/.test(shown))
      say("the header still shows the level name and its continuation index: " +
          JSON.stringify(shown));
    /* The planned name is UNTOUCHED -- other machinery is keyed by it. */
    if (!/^Edit Parts/.test(String(ctl.page.name)))
      say("the planned page name changed to " + JSON.stringify(ctl.page.name) +
          " -- that is the page IDENTITY");
    /* And it must FIT the ~50px the right side gets once the title claims its
       floor, which is what truncated the old one. */
    const F = await import("./src/shared/param_pages/font4x5.mjs");
    const w = F.fontWidth4x5(shown.toUpperCase());
    if (w > 50)
      say("the header label " + JSON.stringify(shown) + " is " + w + "px, over the " +
          "~50px the right side gets -- it will truncate");
  }

  console.log("  ok  choosing a child writes nothing, lands on its parameters, and re-keys reads and writes");
  console.log("  ok  the header names the child, fits, and leaves the page identity alone");
});
'
# The label has to reach the RENDERER, not merely exist on the controller.
# Dropping `pageLabel:` from the render options leaves every unit assertion
# green while the screen goes back to "EDIT PARTS -".
command grep -q "pageLabel: pageLabel()," src/shared/param_pages/page_controller.mjs || {
  echo "FAIL: the render options no longer carry pageLabel" >&2; exit 1; }
command grep -q "o.pageLabel" src/shared/param_pages/render_page_movy.mjs || {
  echo "FAIL: renderPageMovy ignores pageLabel and draws page.name" >&2; exit 1; }
echo "  ok  the label reaches the renderer"

echo "PASS: child levels are selectable and address concrete keys"
