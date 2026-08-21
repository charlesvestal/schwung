#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A component whose contract could not be read must recover BY ITSELF.
#
# Reported from the device: loading tablor drew a blank chain, and "switching
# to another chain and back I saw it". The cause was a module copying 116 files
# (20.4 MB) inside create_instance, on the SPI callback — so the contract
# genuinely could not be read for several seconds, and no retry budget is the
# right answer to that.
#
# What was ours: giving up released the screen AND stopped asking. Those were
# the same act, so the component stayed blank for the rest of the session and
# only a navigate-away-and-back fixed it.
#
# Pinned here: the budget still ends (the screen must not be held forever), the
# grid still refuses to invent pages from a failed read, the slow probe costs
# about one read per interval while it is failing, and the pages come back on
# their own once the channel answers — with no navigation and no host reload.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/page_controller.mjs").then((C) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  let answering = false;
  let reads = 0;
  const HIER = JSON.stringify({ modes: null, levels: {
      root: { label: "S", knobs: ["cutoff"], params: [{ key: "cutoff", label: "Cutoff" }] } } });
  const CP = JSON.stringify([{ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 }]);

  const ctl = C.createController({
    getParam: (k) => {
      reads++;
      if (!answering) return null;          /* the read did not complete */
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      return "0.5";
    },
    setParam: () => {},
    announce: () => {},
  });

  ctl.load({ slot: 0, component: "synth" });
  if (ctl.pages.length) fail("planned pages from a failed read — the whole point is that it must not");

  /* Spend the retry budget. The +3 is because the limit is checked before the
   * increment, so the give-up lands one interval after the last retry. */
  for (let i = 0; i < (C.CONTRACT_RETRY_LIMIT + 3) * C.CONTRACT_RETRY_INTERVAL_TICKS; i++) ctl.tick();

  if (ctl.contractUnresolved)
    fail("still claiming unresolved after the budget — the host would hold the screen forever");
  if (ctl.pages.length)
    fail("pages appeared while the channel was still refusing");

  /* While given-up AND still failing, the probe must be nearly free. */
  const probeStart = reads;
  const PROBE_TICKS = C.CONTRACT_RECOVER_INTERVAL_TICKS * 3;
  for (let i = 0; i < PROBE_TICKS; i++) ctl.tick();
  const probeReads = reads - probeStart;
  /* One reload attempt per interval; a reload reads ui_hierarchy and may read
   * chain_params, so allow a small constant per interval — but nowhere near
   * one per tick, which is what "keep retrying hard" would cost. */
  const intervals = PROBE_TICKS / C.CONTRACT_RECOVER_INTERVAL_TICKS;
  if (probeReads > intervals * 4)
    fail("the give-up probe cost " + probeReads + " reads over " + intervals +
         " intervals — it is supposed to be about one read per interval");
  if (probeReads === 0)
    fail("the give-up probe never ran, so nothing would ever recover");

  /* The module finishes whatever was blocking it. Nothing else happens: no
   * navigation, no reload from the host. It must come back on its own. */
  answering = true;
  for (let i = 0; i < C.CONTRACT_RECOVER_INTERVAL_TICKS * 2 + 10; i++) ctl.tick();
  if (!ctl.pages.length)
    fail("did not recover once the channel answered — this IS the blank-chain bug");
  if (ctl.contractUnresolved) fail("recovered but still flagged unresolved");

  console.log("  ok  budget ends, screen released, no invented pages");
  console.log("  ok  probe costs " + probeReads + " reads over " + intervals + " intervals");
  console.log("  ok  recovers by itself: " + ctl.pages.length + " page(s), no navigation");
  console.log("PASS: an unreadable component recovers without navigating away and back");
});
'
