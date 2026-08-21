#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A contract read that FAILED must never produce a page plan.
#
# Reported from hardware: "in granny when I select a sample, it reorders the
# knobs and puts the sample file in knob position 1 - until I go into the
# waveform position editor and back."
#
# The chain:
#   1. picking a sample writes synth:sample_path, and granny loads the WAV
#      SYNCHRONOUSLY inside set_param, on the SPI callback thread that also
#      serves param requests. The write times out at 100 ms.
#   2. closing the file browser re-enters the knob grid in the SAME JS handler,
#      milliseconds later, and controller.load() reads synth:ui_hierarchy while
#      the channel is still busy. The read returns null.
#   3. null parsed to nothing, which looked exactly like "this module declares
#      no hierarchy", so the planner paginated chain_params 8 at a time - and
#      granny declares sample_path FIRST, so the sample file landed on knob 1
#      and shifted every other knob along.
#   4. it latched: the cached failure was re-planned from, and metaSettled was
#      set because the chain_params-derived metadata looked complete, so the
#      contract was never read again. Only a full teardown healed it.
#
# The distinction the fix rests on is already on the wire:
#   null  the binding could not claim the param channel  -> retry, know nothing
#   ""    an unserved but reachable key, zeroed buffer   -> genuinely absent
#
# Both halves below are required. The first alone would be passed by a fix that
# simply never plans anything.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the contract-read-failure tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
]).then(([C, D]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const keys1 = (ctl) => ((ctl.pages[0] || {}).keys || []);
  /* Enough ticks to cover CONTRACT_RETRY_INTERVAL_TICKS a few times over. */
  const RECOVER_TICKS = C.CONTRACT_RETRY_INTERVAL_TICKS * 3 + 2;
  const tickFor = (ctl, n) => { for (let i = 0; i < n; i++) ctl.tick(); };

  /* ---- 0. the fixture still reproduces the reported bug ----------------- */
  {
    const dev = D.createFakeDevice({ id: "granny" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    if (keys1(ctl)[0] !== "position")
      fail("granny knob 1 should be position, got " + keys1(ctl)[0]);
    const cp = JSON.parse(dev.getParam("synth:chain_params"));
    if (cp[0].key !== "sample_path")
      fail("granny fixture changed: chain_params[0] is no longer sample_path, " +
           "so a chain_params fallback would no longer misplace it");
  }

  /* ---- 1. a failed contract read plans NOTHING -------------------------- */
  {
    const dev = D.createFakeDevice({ id: "granny" });
    dev.failParam("ui_hierarchy", 1);
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });

    if (keys1(ctl)[0] === "sample_path")
      fail("THE BUG: a failed ui_hierarchy read put sample_path on knob 1 - " +
           JSON.stringify(keys1(ctl)));
    if (ctl.pages.length !== 0)
      fail("a failed contract read produced a page plan: " + JSON.stringify(keys1(ctl)));
    if (!ctl.contractUnresolved)
      fail("a null ui_hierarchy read was not recorded as unresolved");

    /* ...and it must not have burned a second doomed round trip asking for
     * chain_params it could not use. */
    if (dev.readsFor("chain_params") !== 0)
      fail("chain_params was read despite the hierarchy read failing");

    /* ---- 2. ...and it RECOVERS, on its own, from the read cursor -------- */
    tickFor(ctl, RECOVER_TICKS);
    if (ctl.contractUnresolved) fail("the contract never re-resolved");
    if (keys1(ctl)[0] !== "position")
      fail("after recovery granny knob 1 is " + keys1(ctl)[0] + ", expected position");
    if (ctl.page.name !== "Main")
      fail("after recovery the page is named " + ctl.page.name + ", expected Main");
  }

  /* ---- 3. a re-entry that fails KEEPS the plan it already had ----------- */
  {
    /* The reported sequence: the grid is already up for this component, the
     * user dives into the sample browser and comes back, and the load() on the
     * way back gets nothing. The plan on screen is still this component plan -
     * stale by at most one retry interval, and right. */
    const dev = D.createFakeDevice({ id: "granny" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    const before = keys1(ctl).join();

    dev.failParam("ui_hierarchy", 1);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    if (!ctl.contractUnresolved) fail("re-entry: the failed read was not recorded");
    if (keys1(ctl).join() !== before)
      fail("re-entry: the page set changed under a failed read, from [" + before +
           "] to [" + keys1(ctl).join() + "]");

    tickFor(ctl, RECOVER_TICKS);
    if (ctl.contractUnresolved) fail("re-entry: the contract never re-resolved");
    if (keys1(ctl).join() !== before) fail("re-entry: recovery changed the page set");
  }

  /* ---- 4. a DIFFERENT component is not shown the old one knobs ---------- */
  {
    const dev = D.createFakeDevice({ id: "granny" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    if (!ctl.pages.length) fail("granny planned no pages");

    /* The fake device serves one prefix, so fx1 reads answer null - which is
     * exactly the shape of an unservable request. */
    ctl.load({ slot: 0, component: "fx1", prefix: "fx1" });
    if (!ctl.contractUnresolved) fail("an unservable fx1 contract was not recorded as unresolved");
    if (ctl.pages.length !== 0)
      fail("a different component under a failed read kept the previous page set: " +
           JSON.stringify(keys1(ctl)));
  }

  /* ---- 5. the latch is gone -------------------------------------------- */
  {
    /* A single failed read used to be permanent: the wrong plan was re-planned
     * from the cached failure, and metaSettled stopped the contract ever being
     * re-read. Hold the read down for longer than one retry interval to prove
     * the recovery is a real loop and not one lucky attempt. */
    const dev = D.createFakeDevice({ id: "granny" });
    dev.failParam("ui_hierarchy", 4);
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    tickFor(ctl, C.CONTRACT_RETRY_INTERVAL_TICKS * 8 + 2);
    if (ctl.contractUnresolved)
      fail("a contract that failed 4 times never re-resolved");
    if (keys1(ctl)[0] !== "position")
      fail("after 4 failed reads knob 1 is " + keys1(ctl)[0] + ", expected position");
  }

  /* ---- 6. the retry is BOUNDED, and the recovery probe is SLOW --------- *
   *
   * Two rates, deliberately. The FAST retry (CONTRACT_RETRY_INTERVAL_TICKS)
   * is bounded by CONTRACT_RETRY_LIMIT: a component whose channel never
   * answers must not cost a read every interval for the rest of the session,
   * and must not hold the screen forever.
   *
   * After that budget it keeps probing at CONTRACT_RECOVER_INTERVAL_TICKS —
   * ~20x slower — because giving up the SCREEN is not giving up the
   * COMPONENT. Those were the same act until 2026-08, which is why loading
   * tablor drew a blank chain that only a navigate-away-and-back fixed. The
   * budget below allows for that slow probe; it does not allow for the fast
   * loop coming back, which is what a failed probe used to do by handing out
   * a fresh budget.
   */
  {
    const dev = D.createFakeDevice({ id: "granny" });
    dev.failParam("ui_hierarchy", 1e9);
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    dev.resetCounters();
    const ticks = C.CONTRACT_RETRY_INTERVAL_TICKS * (C.CONTRACT_RETRY_LIMIT + 20);
    tickFor(ctl, ticks);
    const n = dev.readsFor("ui_hierarchy");
    const probes = Math.ceil(ticks / C.CONTRACT_RECOVER_INTERVAL_TICKS);
    if (n > C.CONTRACT_RETRY_LIMIT + probes)
      fail("unbounded contract retries: " + n + " reads, limit is " +
           C.CONTRACT_RETRY_LIMIT + " + " + probes + " slow probe(s)");
    if (n <= C.CONTRACT_RETRY_LIMIT)
      fail("the slow recovery probe never ran (" + n + " reads) — an unreadable " +
           "component would stay blank for the whole session");
    if (n < 2) fail("the contract retry loop is not running at all (" + n + " reads)");
  }

  /* ---- 7. genuine absence still paginates chain_params ------------------ */
  {
    /* Four fleet modules publish chain_params and no hierarchy at all. They
     * answer "" - an unserved but reachable key - and that fallback is not
     * what was broken. Do not fix a timeout by breaking it. */
    for (const id of ["branchage", "belt-in", "po32-drum", "smack-in"]) {
      const dev = D.createFakeDevice({ id });
      if (dev.getParam("synth:ui_hierarchy") !== "")
        fail(id + ": the fake device must answer \"\" for a genuinely absent hierarchy");
      dev.resetCounters();
      const ctl = C.createController(dev);
      ctl.load({ slot: 0, component: "synth", prefix: "synth" });
      if (ctl.contractUnresolved) fail(id + ": genuine absence was treated as a failed read");
      if (!ctl.pages.length) fail(id + ": the chain_params pagination fallback stopped working");
      if (ctl.page.name !== "Params") fail(id + ": fallback page 1 is named " + ctl.page.name);
      const cp = JSON.parse(dev.getParam("synth:chain_params"));
      if (keys1(ctl)[0] !== cp[0].key)
        fail(id + ": the fallback no longer paginates in declaration order");
    }
  }

  /* ---- 8. no new per-frame reads on the healthy path -------------------- */
  {
    /* The read budget is the hard constraint here: a param round trip is
     * ~2.8 ms against a 1.68 ms whole-page render. A resolved contract must
     * cost the same one read per tick it always did. */
    const dev = D.createFakeDevice({ id: "granny" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    dev.resetCounters();
    const N = 60;
    tickFor(ctl, N);
    if (dev.reads.length > N)
      fail("a resolved contract cost " + dev.reads.length + " reads over " + N +
           " ticks - more than one per tick");
    if (dev.readsFor("ui_hierarchy") !== 0)
      fail("a resolved contract is being re-read every tick");
  }

  console.log("PASS: a failed contract read plans nothing, recovers on its own, " +
              "and genuine absence still paginates chain_params");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
