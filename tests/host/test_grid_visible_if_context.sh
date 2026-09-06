#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# visible_if on the knob grid read the WRONG slot and failed open.
#
# evaluateVisibilityCondition resolved its condition against hierEditorSlot /
# hierEditorComponent -- the LIST editor's identity, which enterParamPages
# never sets. From the grid that is slot -1: the read answers null, the
# evaluator fails open, and every visible_if condition is true. A send level
# meant to collapse to the armed type's cells showed all twenty, three pages
# deep, with nothing logged. Reported from the device.
#
# Pinned: on PARAM_PAGES the evaluator takes the grid's own slot/component,
# resolving a per-instance key through the grid's child index by level NAME.

fail() { echo "FAIL: $*" >&2; exit 1; }
ui="src/shadow/shadow_ui.js"
pp="src/shadow/shadow_ui_param_pages.mjs"

body=$(sed -n '/^function evaluateVisibilityCondition(condition, levelDef) {/,/^}/p' "$ui")
[ -n "$body" ] || fail "evaluateVisibilityCondition is gone"
echo "$body" | command grep -q 'paramPagesActive()' || fail "evaluateVisibilityCondition never asks whether the grid is up -- it reads the list editor's slot from the grid and fails open"
echo "$body" | command grep -q 'paramPagesSlot()' || fail "on the grid the evaluator does not use the grid's slot"
echo "$body" | command grep -q 'paramPagesComponent()' || fail "on the grid the evaluator does not use the grid's component"
echo "$body" | command grep -q 'paramPagesLevelNameOf(levelDef)' || fail "a per-instance condition cannot find its instance: the level name is not resolved from its definition"
command grep -q '^export function paramPagesLevelNameOf' "$pp" || fail "paramPagesLevelNameOf is not exported by $pp"
command grep -q 'paramPagesLevelNameOf,' "$ui" || fail "shadow_ui.js does not import paramPagesLevelNameOf"
# A re-plan follows every detent of a gating knob; a blocking read per condition froze the OLED.
echo "$body" | command grep -q 'paramPagesCachedValue(' || fail "the grid evaluator does not consult the controller's own values first -- every re-plan pays a blocking read per condition"
# ...and it must ask with the TEMPLATE key. `k` arrives resolved by
# hierChildKeyFor ("pad3_type"); the controller keys its values by what the
# level LISTS ("type"). Asking with the concrete key missed every time, so a
# per-instance condition never hit the cache -- the very case the level-name
# lookup above was added to serve -- and it was SILENT, because a miss still
# answers correctly, just with the blocking read this branch exists to avoid.
echo "$body" | command grep -q 'paramPagesCachedValue(hierGenericKeyFor(' || fail "the grid evaluator asks the value cache with the CONCRETE key -- a per-instance condition can never hit it"
echo "$body" | command grep -q 'getSlotParamCached(' || fail "a cache miss on the grid goes to an uncached blocking read"
if echo "$body" | command grep -q '[^a-zA-Z]getSlotParam(' ; then fail "the grid branch still reads through the uncached getSlotParam"; fi
command grep -q '^export function paramPagesCachedValue' "$pp" || fail "paramPagesCachedValue is not exported by $pp"
echo "  ok  visible_if on the knob grid resolves against the grid's own slot and component"
# ---- and the value cache is keyed by the TEMPLATE key ----------------------
#
# The pin above says the evaluator inverts before it asks. This says WHY: the
# controller's own values -- the cache that branch consults first -- are filed
# under what the level LISTS, while the key the evaluator is handed has already
# been resolved to an instance. So the two dialects really do differ, and a
# lookup with the concrete key really does miss. If the controller ever starts
# filing resolved keys, this fails and the inversion above becomes wrong.
#
# Driven through a real controller rather than grepped, because the keying is a
# runtime fact: nothing in the source says which of the two goes into s.values.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node is required" >&2; exit 1; fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
]).then(([C, P]) => {
  const say = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const j = JSON.parse(require("fs").readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const m = j.modules.find((x) => x.id === "minijv");
  if (!m) say("minijv is not in the contract fixture -- nothing here declares a child level");
  let h = m.ui_hierarchy, cp = m.chain_params;
  if (typeof h === "string") h = JSON.parse(h);
  if (typeof cp === "string") cp = JSON.parse(cp);

  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(h);
      if (b === "chain_params") return JSON.stringify(cp);
      if (b === "mode") return "Performance";
      return "10";
    },
    setParam: () => {},
    announce: () => {},
  });
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 10; i++) ctl.tick();

  const at = ctl.pages.findIndex((p) => p.childOf);
  if (at < 0) say("minijv has no child selector -- the fixture no longer exercises this");
  ctl.goToPage(at);
  ctl.onClick(0);                 /* enter the list */
  ctl.onJog(1); ctl.onJog(1);     /* Part 3 */
  ctl.onClick(0);                 /* commit -- re-keys the level */
  if (!(ctl.page && ctl.page.kind === P.PAGE_KNOBS && ctl.page.childLevel))
    say("choosing a part did not land on a child-level knob page");
  for (let i = 0; i < 24; i++) ctl.tick();

  const vals = ctl.state && ctl.state.values;
  if (!vals) say("the controller exposes no state.values -- paramPagesCachedValue reads that");
  const held = Object.keys(vals);
  if (!held.length) say("the controller cached no values at all after 24 ticks");

  /* The keys on the page ARE the templates; the wire keys are resolved. */
  const tmpl = (ctl.page.keys || []).filter((k) => k && k in vals);
  if (!tmpl.length)
    say("none of the page keys " + JSON.stringify((ctl.page.keys || []).slice(0, 4)) +
        " reached state.values -- this test can no longer tell the dialects apart");
  const concrete = held.filter((k) => /^sram_part_\d+_/.test(k));
  if (concrete.length)
    say("state.values holds RESOLVED keys " + JSON.stringify(concrete.slice(0, 3)) +
        " -- the evaluator must stop inverting, the dialects now agree");
  console.log("  ok  the controller files a child level under its template keys (" +
              JSON.stringify(tmpl.slice(0, 2)) + "), never the resolved ones");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
' || exit 1

echo "PASS: test_grid_visible_if_context"
