#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A hierarchy `modes` selector, on the grid.
#
# PAGE_MODES was planned by the planner and rendered by NOBODY, so minijv's
# Mode page ejected to the list editor. The consequence went further than one
# screen: performance mode was unreachable from the grid, and parts only exist
# in performance mode -- so "edit parts does nothing" was partly this.
#
# It is an items page now: a list, a cursor, a current marker, click to choose.
# The same machinery the child selector uses, and for the same reason -- a
# second page kind means a second jog handler and a second renderer, and the
# two module pickers drifted apart exactly that way once already.
#
# Two things make a mode different from an ordinary items page, and both are
# pinned below: it writes a real param, and choosing RE-ROOTS the hierarchy so
# every page after it is a different page.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/page_controller.mjs"),
]).then(([P, C]) => {
  const say = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const j = JSON.parse(require("fs").readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const m = j.modules.find((x) => x.id === "minijv");
  let h = m.ui_hierarchy, cp = m.chain_params;
  if (typeof h === "string") h = JSON.parse(h);
  if (typeof cp === "string") cp = JSON.parse(cp);

  /* ---- there is no page kind the grid cannot draw --------------------- */
  const owned = new Set(["knobs", "menu", "preset", "items"]);
  const planned = P.planPages({ hierarchy: h, chainParams: cp }).pages;
  for (const p of planned)
    if (!owned.has(p.kind))
      say("minijv still plans a " + p.kind + " page, which nothing draws — it will eject");

  const sel = planned.find((p) => p.modeSelect);
  if (!sel) say("no mode selector page");
  if (sel.kind !== P.PAGE_ITEMS) say("the mode selector is a " + sel.kind + ", not an items page");
  if (!sel.selectParam) say("the mode selector writes no param");

  /*
   * The rows read as words, not as level ids.
   *
   * Levels are lowercase, so the rows drew "patch" / "performance" against a
   * fleet where every other list is title-case. A module that declares the
   * mode in chain_params spells the same two words properly there, and that
   * is what gets shown -- borrowed ONLY where the option matches the mode
   * name case-aside, because derivedLabels is also the VALUE commitItem
   * carries, so anything else would be a silent relabelling.
   *
   * Driven off a synthetic contract, not minijv`s: the plugin`s GENERATED
   * chain_params has no `mode` entry at all today (only its module.json
   * mirror does, which the shadow UI never reads), so minijv is the
   * nothing-to-borrow case and proves only the fallback.
   */
  {
    const withOpts = cp.concat([{ key: "mode", name: "Mode", type: "enum",
                                  options: ["Patch", "Performance"] }]);
    const rows = P.planPages({ hierarchy: h, chainParams: withOpts })
      .pages.find((p) => p.modeSelect).derivedLabels;
    if (!rows.includes("Performance"))
      say("declared enum options were not adopted as the mode rows: " + JSON.stringify(rows));
    for (const l of rows)
      if (!h.modes.some((mo) => String(mo).toLowerCase() === String(l).toLowerCase()))
        say("mode row " + JSON.stringify(l) + " is not a declared mode case-aside");
    /* A relabelling is refused rather than adopted. */
    const renamed = cp.concat([{ key: "mode", name: "Mode", type: "enum",
                                 options: ["Single", "Multi"] }]);
    const rows2 = P.planPages({ hierarchy: h, chainParams: renamed })
      .pages.find((p) => p.modeSelect).derivedLabels;
    if (rows2.join() !== h.modes.join())
      say("options that are not the mode names were adopted anyway: " + JSON.stringify(rows2));
  }
  /* Nothing to borrow: the rows are the mode names, and nothing throws. */
  if (sel.derivedLabels.join() !== h.modes.join())
    say("with no declared options the rows should be the mode names, got " +
        JSON.stringify(sel.derivedLabels));

  /* ---- choosing a mode writes it AND re-roots the hierarchy ----------- */
  let modeVal = "Patch";
  const writes = [];
  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(h);
      if (b === "chain_params") return JSON.stringify(cp);
      if (b === "mode") return modeVal;
      return "10";
    },
    setParam: (k, v) => {
      const b = String(k).replace(/^[^:]+:/, "");
      writes.push(b + "=" + v);
      if (b === "mode") modeVal = v;
    },
    announce: () => {},
  });
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 10; i++) ctl.tick();

  /*
   * The ROOT is the first page that has a level -- the mode page itself has
   * none. Testing "does the level list CONTAIN performance" is not enough:
   * the patch-rooted walk reaches that level too, so the containment holds
   * with or without a re-plan and two mutations survived on it.
   */
  const rootOf = (pages) => (pages.filter((p) => p.level)[0] || {}).level;
  const before = rootOf(ctl.pages);
  if (before !== "patch") say("did not start rooted at the first mode, got " + before);

  const at = ctl.pages.findIndex((p) => p.modeSelect);
  ctl.goToPage(at);
  ctl.onClick(0);                 /* enter */
  ctl.onJog(1);                   /* highlight performance */
  writes.length = 0;
  ctl.onClick(0);                 /* choose */

  if (!writes.some((w) => w.startsWith("mode=")))
    say("choosing a mode wrote " + JSON.stringify(writes) + " — the module is never told");

  const after = rootOf(ctl.pages);
  if (after !== "performance")
    say("after choosing performance the pages are still rooted at " + JSON.stringify(after) +
        " — the hierarchy was not re-planned");
  /* The parts level is what performance mode unlocks, and patch mode must not
     have it AT ALL. This used to read "reachable earlier in performance than
     in patch", because the orphan sweep put the whole performance tree on the
     end of the patch plan too -- the parts page was at 72 of 76, editing a
     performance you were not in. See test_param_pages_mode_gating.sh. */
  const partsAt = ctl.pages.findIndex((p) => p.childOf);
  if (partsAt < 0) say("performance mode does not reach the part selector");
  const patchPlan = P.planPages({ hierarchy: h, chainParams: cp, mode: "patch" }).pages;
  if (patchPlan.some((p) => p.childOf))
    say("patch mode still plans the part selector — the root did not actually move");

  /*
   * NOT asserted: that the current-mode marker follows the module rather than
   * what we asked for. tickItems reads it back and matches case-insensitively
   * ("Performance" against "performance"), but `current` is not reachable
   * through the public API, so a test for it would only be able to assert
   * itself. Said here rather than written as a check that cannot fail.
   */

  /* ---- entering a module SEEDS the mode from the module ----------------
   *
   * No caller passes `mode` on entry, so the planner fell back to modes[0]:
   * a MiniJV sitting in Performance planned its patch tree until you found
   * this page and picked Performance by hand. It went unnoticed because both
   * trees were planned regardless until mode gating landed.
   *
   * The module answers "Performance"; the level is called "performance". The
   * seeding match is case-insensitive for exactly that reason, so answering
   * it the way the module really does is the test, not a convenience. */
  for (const [answer, want] of [["Performance", "performance"], ["Patch", "patch"],
                                ["1", "performance"], ["", "patch"]]) {
    const c2 = C.createController({
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return JSON.stringify(h);
        if (b === "chain_params") return JSON.stringify(cp);
        if (b === "mode") return answer;
        return "10";
      },
      setParam: () => {}, announce: () => {},
    });
    c2.load({ slot: 0, component: "synth" });
    for (let i = 0; i < 10; i++) c2.tick();
    const got = rootOf(c2.pages);
    if (got !== want)
      say("a module answering mode=" + JSON.stringify(answer) + " was planned at root " +
          JSON.stringify(got) + ", expected " + want);
  }

  /* A FAILED read is not an answer. It must not seed modes[0] as though the
     module had said so -- leaving it unset is what makes the next reload ask
     again. Observable as: the root is still the fallback, but nothing latched,
     so a later successful read moves it. */
  {
    let answer = null;
    let clock = 0;
    const c3 = C.createController({
      now: () => clock,
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return JSON.stringify(h);
        if (b === "chain_params") return JSON.stringify(cp);
        if (b === "mode") return answer;
        return "10";
      },
      setParam: () => {}, announce: () => {},
    });
    c3.load({ slot: 0, component: "synth" });
    for (let i = 0; i < 10; i++) c3.tick();
    if (rootOf(c3.pages) !== "patch") say("a failed mode read should fall back to the first mode");
    answer = "Performance";
    /* The re-ask is on the contract settle deadline, which is wall-clock. */
    clock += C.CONTRACT_SETTLE_MS * 4;
    for (let i = 0; i < 40; i++) { clock += 20; c3.tick(); }
    if (rootOf(c3.pages) !== "performance")
      say("a failed mode read LATCHED — the later answer never took effect");
  }

  console.log("  ok  every page minijv plans is one the grid draws");
  console.log("  ok  the mode selector is an items page and writes its param");
  console.log("  ok  choosing a mode re-roots the hierarchy and reaches the parts level");
  console.log("PASS: modes are selectable on the grid");
});
'
