#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Coming back from an editor must land on the page you left, even when the
# pages are not back yet.
#
# Reported from the device: choosing a sample in granny returned you to page 1
# instead of the page the sample knob was on.
#
# The restore looked through controller.pages ONCE, at the moment of re-entry,
# and gave up if the page was not there. Coming out of granny it is not:
# granny loads the WAV synchronously inside set_param, on the SPI thread that
# also serves param requests, so the contract read straight after a selection
# times out -- and planPages correctly refuses to invent pages from a failed
# read. Empty list, nothing matched, page 1.
#
# So the request is REMEMBERED and re-applied when the pages arrive.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/page_controller.mjs").then((C) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* Two levels, so there is more than one page to be wrong about. */
  const HIER = JSON.stringify({ modes: null, levels: {
    root:  { label: "Main",   knobs: ["a"], params: [{ key: "a" }, { level: "more", label: "More" }] },
    more:  { label: "More",   knobs: ["b"], params: [{ key: "b" }] },
  }});
  const CP = JSON.stringify([
    { key: "a", name: "A", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "b", name: "B", type: "float", min: 0, max: 1, step: 0.01 },
  ]);

  /* answering=false models the module blocking the param channel while it
     loads a sample -- the read does not complete, which is NOT the same as
     the module declaring nothing. */
  let answering = false;
  const ctl = C.createController({
    getParam: (k) => {
      if (!answering) return null;
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      return "0.5";
    },
    setParam: () => {}, announce: () => {},
  });

  /* Enter while the module is busy: no pages exist yet. */
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 4; i++) ctl.tick();
  if (ctl.pages.length) fail("fixture: pages planned from a failed read");

  ctl.restorePage("More");

  /* The module finishes; the pages arrive. */
  answering = true;
  for (let i = 0; i < 40; i++) ctl.tick();
  if (!ctl.pages.length) fail("fixture: pages never arrived");

  const landed = ctl.page && ctl.page.name;
  if (landed !== "More")
    fail("landed on " + JSON.stringify(landed) + " -- the restore was dropped when " +
         "the pages were not ready, which is the granny bug");

  /*
   * NOT asserted here: that the request is one-shot, and that a request for a
   * page which never appears is abandoned once the contract settles. Both are
   * implemented (applyPendingRestore clears s.restoreName on success, and on
   * metaSettled when it cannot be satisfied) and BOTH ARE UNOBSERVABLE through
   * the public API: they only differ on a SECOND replan, and the controller
   * performs at most one re-resolution -- maybeResettle is gated by
   * triedReresolve, so no amount of ticking produces another.
   *
   * Mutating either clause therefore survives this test. That is stated rather
   * than papered over with assertions that cannot fail: two such checks were
   * written first, passed against the mutations, and were deleted.
   */

  console.log("  ok  a restore asked for before the pages exist is honoured when they arrive");
  console.log("PASS: the page you left survives a module that was still loading");
});
'

# The call site must actually ASK. The controller can be perfect and the
# feature still absent — this is a unit test of the controller, so the wiring
# is pinned at the source.
pp="src/shadow/shadow_ui_param_pages.mjs"
if ! command grep -q "controller.restorePage(restorePageName)" "$pp"; then
  echo "FAIL: enterParamPages no longer asks the controller to restore the page" >&2
  exit 1
fi
echo "  ok  enterParamPages hands the page name to the controller"
