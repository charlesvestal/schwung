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
command grep -q "hierEditorChildCount = levelDef.child_count" <<<"$blk" || \
  fail "the grid hand-off does not set hierEditorChildCount — a child level lands as a flat list of unprefixed keys"
command grep -q "hierEditorChildLabel = levelDef.child_label" <<<"$blk" || \
  fail "the grid hand-off does not set hierEditorChildLabel"
# It must be set BEFORE the level is loaded, or the gate has already run.
cnt=$(command grep -n "hierEditorChildCount = levelDef.child_count" <<<"$blk" | head -n 1 | cut -d: -f1)
ldl=$(command grep -n "loadHierarchyLevel()" <<<"$blk" | tail -n 1 | cut -d: -f1)
[ -n "$cnt" ] && [ -n "$ldl" ] && [ "$cnt" -lt "$ldl" ] || \
  fail "the child count is set AFTER loadHierarchyLevel — the selector gate has already been evaluated"
echo "  ok  the grid hand-off carries child_count/child_label, before the level loads"

# ---- (a) no knob pages built from template keys -----------------------------
node -e '
Promise.all([
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/child_key.mjs"),
]).then(([P, M, K]) => {
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
    const pages = P.planPages({ hierarchy: h, chainParams: cp, metaIndex: ix }).pages;

    for (const lvl of childLevels) {
      const knobPages = pages.filter((p) => p.level === lvl && p.kind === P.PAGE_KNOBS);
      if (knobPages.length)
        say(mod.id + "/" + lvl + " planned " + knobPages.length + " knob page(s) from " +
            "TEMPLATE keys (" + knobPages[0].keys.slice(0, 3).join(",") + ") — those cells " +
            "read and write keys the DSP does not serve, and look alive doing it");
      const childPages = pages.filter((p) => p.level === lvl && p.kind === P.PAGE_CHILD);
      if (!childPages.length)
        say(mod.id + "/" + lvl + " has no child page at all — the level is unreachable");
    }
  }
  if (!checked) say("no module in the fixture declares a child level — this test is asserting nothing");

  /* And the resolver the real fix will need still produces what the DSP wants. */
  const lvl = { child_prefix: "sram_part_", child_count: 8 };
  const got = K.resolveChildKey(lvl, 0, "partlevel");
  if (got !== "sram_part_0_partlevel")
    say("resolveChildKey produced " + JSON.stringify(got) + ", not the sram_part_<n>_<key> " +
        "form jv880_plugin.cpp gates on");

  console.log("  ok  " + checked + " module(s) with child levels: a child page, and no template knob pages");
  console.log("  ok  resolveChildKey still yields the concrete key the DSP serves");
});
'
echo "PASS: child levels hand off cleanly instead of drawing dead knobs"
