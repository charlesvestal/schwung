#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A contract that changes UNDER an unchanged component must be re-read once it
# has SETTLED — not the instant the selection is written.
#
# Reported from hardware, on schwung-airwindows:
#
#   "I've got kCyberCity and it shows head, a delay, a lev. And when I changed
#    to kosmos then I see rege, posi, dry (the kCyberCity controls)."
#   "If I exit the module and come back to it, it shows the right ones."
#
# Strictly one behind, and eventually correct — which places the fault in the
# TIMING of the re-read, not in whether one happens:
#
#   1. writing the selection updates the module's selected index synchronously,
#      so `plugin_name` is immediately right, but only SCHEDULES the plugin
#      load on a worker thread 300 ms later (clap_fx.cpp:806-822).
#   2. `chain_params` is generated from `cached_param_names`, rewritten only at
#      the END of that load (clap_fx.cpp:651). For the whole debounce it
#      reports the plugin still loaded — the PREVIOUS one.
#   3. our re-read fires milliseconds after the write, so it wins the race and
#      caches the previous effect's labels. Leaving and re-entering reads long
#      after the debounce, which is why that heals it.
#
# The fix must not depend on the module volunteering `is_loading`: nothing in
# the fleet is obliged to implement it, and correctness that rests on a flag
# third-party modules must remember to set is not correctness. The fingerprint
# already hashes chain_params CONTENT, so "has it changed" is answerable here.
#
# Cost matters as much as correctness. A param read is ~2.8 ms against a
# 1.68 ms whole-page render, and the effect list is 519 long: a fast jog must
# cost O(1) contract reads, not one per detent.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the contract-settle tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
]).then(([C, D]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const tickFor = (ctl, n) => { for (let i = 0; i < n; i++) ctl.tick(); };
  /* The device debounce, from clap_fx.cpp PLUGIN_LOAD_DEBOUNCE_MS. */
  const DEBOUNCE_MS = 300;
  /* Walk the clock forward in small steps while ticking, the way real time
     passes: a settle that only worked when the clock jumped in one bound
     would not be a settle. */
  const settle = (ctl, dev, ms) => {
    for (let t = 0; t < ms; t += 10) { dev.advance(10); ctl.tick(); }
  };
  /* Comfortably past the deadline and every one of its bounded retries. */
  const SETTLED_MS = C.CONTRACT_SETTLE_MS * (C.CONTRACT_SETTLE_RETRIES + 2);

  /* The three effects from the report, with their real Airwindows param
   * names and their real (differing) param counts. */
  /* The names are the ones the hardware report actually types, not expansions
     invented here: the repo ships no Airwindows parameter table (only
     airwindows_category_map.inc), so the full spellings are not verifiable
     from source and are not claimed. What the cases below assert is that the
     contract CHANGES and the grid follows it, which does not depend on the
     spelling being right. The captured six-param contract in the fixture is
     the one real contract in play. */
  const FX = {
    kCyberCity: ["rege", "posi", "dry"],
    kosmos:     ["kos1", "kos2", "kos3", "kos4"],
    Ensemble:   ["head", "a delay", "a lev"],
  };
  const contractFor = (name) => FX[name].map((n, i) => ({
    key: `param_${i}`, name: n, type: "float", min: 0, max: 1,
  }));

  const grid = (ctl) => (ctl.pages || []).find((p) => p.kind === "knobs");
  const labels = (ctl) => {
    const p = grid(ctl);
    if (!p) return "<no knobs page>";
    const mi = ctl.state.metaIndex;
    /* Only the cells a real effect declares; the hierarchy always names eight
     * knobs, so the tail is legitimately undeclared. */
    return p.keys.slice(0, 4).map((k) => mi.getOrGuess(k).label).join(",");
  };
  const want = (name) => FX[name].concat(["param 3"]).slice(0, 4).join(",");
  /* Whatever effect the CAPTURE was taken with. Read from the fixture rather
     than written out, so a re-capture does not turn every case below into a
     spurious failure. */
  const captured = (dev) => {
    const cp = JSON.parse(dev.getParam("fx1:chain_params"));
    return cp.map((p) => p.name).concat(["param 3"]).slice(0, 4).join(",");
  };

  const openGrid = (dev) => {
    const ctl = C.createController({ ...dev, now: dev.now });
    ctl.load({ slot: 0, component: "fx1", prefix: "fx1" });
    return ctl;
  };

  /* ---- 0. the fixture is the real airwindows contract ------------------- */
  {
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    const g = grid(ctl);
    if (!g) fail("clap planned no knobs page");
    if (g.keys[0] !== "param_0")
      fail("fixture changed: knob 1 is no longer param_0, got " + g.keys[0]);
    const cp = JSON.parse(dev.getParam("fx1:chain_params"));
    if (!cp.length || cp[0].type !== "float")
      fail("fixture changed: clap chain_params is no longer float-typed, " +
           "so metaUnsettled() may now fire and this bug would not reproduce");
    /* The whole diagnosis rests on this: nothing in the contract is an enum,
     * so the existing placeholder-driven re-resolve can never fire for it. */
    const before = dev.readsFor("chain_params");
    tickFor(ctl, C.META_RETRY_INTERVAL_TICKS * 2 + 2);
    if (dev.readsFor("chain_params") !== before)
      fail("maybeResettle re-read a float-only contract; the premise of this " +
           "test (that nothing else re-reads) no longer holds");
  }

  /* ---- 1. THE BUG: three consecutive selections, none one-behind -------- */
  {
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    if (labels(ctl) !== captured(dev))
      fail("expected the captured fixture contract at load, got " + labels(ctl));

    /* One step is passed by luck — a stale read that happens to be right
     * once. Three consecutive selections is the assertion. */
    const order = ["kosmos", "Ensemble", "kCyberCity"];
    for (const name of order) {
      dev.stagePlugin(contractFor(name), { debounceMs: DEBOUNCE_MS });
      ctl.selectionChanged();
      settle(ctl, dev, SETTLED_MS);
      if (labels(ctl) !== want(name))
        fail(`THE BUG: after selecting ${name} the grid shows ` +
             `"${labels(ctl)}", want "${want(name)}"`);
    }
  }

  /* ---- 2. a contract that is still degenerate is not an answer ---------- */
  {
    /* While the plugin pointer is NULL the module answers "[]"
     * (clap_fx.cpp:914-916). Taking that as the settled contract would blank
     * every label — so it must keep waiting, not latch. */
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    /* The degenerate window must outlast TWO probes, or the two-agree rule
       absorbs it on its own and the guard is never asked anything. A real
       module load blocks the callback for ~673 ms (docs/), so a window this
       wide is not a contrivance. */
    dev.stagePlugin(contractFor("kosmos"),
                    { debounceMs: C.CONTRACT_SETTLE_MS * 2 + 200, during: [] });
    ctl.selectionChanged();
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) !== want("kosmos"))
      fail("an empty mid-load contract was taken as settled: " + labels(ctl));
  }

  /* ---- 3. a fast jog costs O(1) contract reads, not one per detent ------ */
  {
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    const before = dev.readsFor("chain_params");
    /* 40 detents inside one settle window, as a spin down a 519-effect list. */
    for (let i = 0; i < 40; i++) {
      ctl.selectionChanged();
      dev.advance(30);          /* a brisk 33 detents a second */
      ctl.tick();
    }
    dev.stagePlugin(contractFor("kosmos"), { debounceMs: DEBOUNCE_MS });
    settle(ctl, dev, SETTLED_MS);
    const spent = dev.readsFor("chain_params") - before;
    if (spent > 4)
      fail(`a 40-detent jog cost ${spent} contract reads; the deadline is not ` +
           "being re-armed per detent");
    if (labels(ctl) !== want("kosmos"))
      fail("the settle after a fast jog did not land: " + labels(ctl));
  }

  /* ---- 4. the browse is not yanked out from under the user ------------- */
  {
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    const pages = ctl.pages.map((p) => p.name);
    const presetIdx = ctl.pages.findIndex((p) => p.kind === "preset");
    if (presetIdx < 0) fail("clap planned no preset page");
    ctl.goToPage(presetIdx);
    const wasOn = ctl.pages[ctl.state.pageIndex].name;
    dev.stagePlugin(contractFor("kosmos"), { debounceMs: DEBOUNCE_MS });
    ctl.selectionChanged();
    settle(ctl, dev, SETTLED_MS);
    if (ctl.pages[ctl.state.pageIndex].name !== wasOn)
      fail("the settle moved the user off " + wasOn + " to " +
           ctl.pages[ctl.state.pageIndex].name);
    if (ctl.pages.map((p) => p.name).join() !== pages.join())
      fail("the settle changed the page set: " + ctl.pages.map((p) => p.name).join());
  }

  /* ---- 5. a knob being TURNED is not rebuilt underneath ----------------- */
  {
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    const start = labels(ctl);
    dev.stagePlugin(contractFor("kosmos"), { debounceMs: DEBOUNCE_MS });
    ctl.selectionChanged();
    ctl.onKnobTouch(0, true);
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) !== start)
      fail("a held knob was rebuilt underneath the hand: " + labels(ctl));
    ctl.onKnobTouch(0, false);
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) !== want("kosmos"))
      fail("the deferred settle never ran after the knob was released: " + labels(ctl));
  }

  /* ---- 6. granny must not regress: retain-on-failed-read still holds ---- */
  {
    const dev = D.createFakeDevice({ id: "granny" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    const keys = (ctl.pages[0] || {}).keys || [];
    if (keys[0] !== "position") fail("granny knob 1 should be position, got " + keys[0]);

    /* Same component, contract read fails: keep the plan we had. */
    dev.failParam("ui_hierarchy", 1);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    if (((ctl.pages[0] || {}).keys || [])[0] !== "position")
      fail("a failed re-read stopped retaining the granny plan: " +
           JSON.stringify((ctl.pages[0] || {}).keys));
    if (!ctl.contractUnresolved)
      fail("a failed ui_hierarchy read is no longer recorded as unresolved");
  }

  /* ---- 7. a FAILED chain_params read must not blank every label --------- */
  {
    /* The sibling of the granny defect, one key over. ui_hierarchy answers,
     * chain_params times out; parsing null as "declares nothing" rebuilt the
     * metaIndex from the hierarchy alone and every knob lost its name, for
     * the rest of the session, because nothing re-reads. */
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    if (labels(ctl) !== captured(dev)) fail("bad start: " + labels(ctl));

    dev.failParam("chain_params", 1);
    ctl.load({ slot: 0, component: "fx1", prefix: "fx1" });
    if (/^param 0/.test(labels(ctl)))
      fail("THE BUG: a failed chain_params read blanked the labels: " + labels(ctl));
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) !== captured(dev))
      fail("a failed chain_params read did not recover: " + labels(ctl));
  }


  /* ---- 8. is_loading is asked ONCE by a module that does not serve it ---- */
  {
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    const before = dev.readsFor("is_loading");
    for (let i = 0; i < 3; i++) {
      dev.stagePlugin(contractFor("kosmos"), { debounceMs: DEBOUNCE_MS });
      ctl.selectionChanged();
      settle(ctl, dev, SETTLED_MS);
    }
    const asked = dev.readsFor("is_loading") - before;
    if (asked > 1)
      fail(`is_loading was asked ${asked} times by a module that answers ""; ` +
           "the support latch is not holding");
  }

  /* ---- 9. a module that DOES serve it is not probed while it says 1 ----- */
  {
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    const ctl = openGrid(dev);            /* serves is_loading by default */
    dev.setLoading(true);
    dev.stagePlugin(contractFor("kosmos"), { debounceMs: DEBOUNCE_MS });
    ctl.selectionChanged();
    const before = dev.readsFor("chain_params");
    /* While the module says it is loading, probing the contract is pointless
       and must cost nothing but the cheap flag read. */
    settle(ctl, dev, C.CONTRACT_SETTLE_MS * 2);
    if (dev.readsFor("chain_params") !== before)
      fail("a module reporting is_loading=1 was still probed for its " +
           "contract " + (dev.readsFor("chain_params") - before) + " times");
    /* ...and the moment it reports ready, the ordinary settle takes over. */
    dev.setLoading(false);
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) !== want("kosmos"))
      fail("an is_loading-serving module did not settle: " + labels(ctl));
  }

  /* ---- 10. a load landing AFTER the first probe is still picked up ------ */
  {
    /* The trap in comparing the first reading against the PRE-WRITE
       fingerprint: an intermediate contract differs from it, so that
       comparison calls it settled and stops — the original bug, one layer up.
       Both agreeing readings have to post-date the deadline. */
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    dev.stagePlugin(contractFor("kosmos"), {
      debounceMs: C.CONTRACT_SETTLE_MS + 200,
      during: contractFor("Ensemble"),     /* real, declared, NOT degenerate */
    });
    ctl.selectionChanged();
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) === want("Ensemble"))
      fail("THE TRAP: settled on the intermediate contract " + labels(ctl));
    if (labels(ctl) !== want("kosmos"))
      fail("did not reach the final contract: " + labels(ctl));
  }


  /* ---- 11. the agreement is not carried across selections -------------- */
  {
    /* A completed settle leaves the last reading fingerprint equal to the
       contract now on screen. If that survives into the NEXT selection, the
       first probe of that selection agrees with it immediately and settles on
       the contract being replaced — the original bug, laundered through the
       confirmation. The agreement has to start empty on every arm. */
    const dev = D.createFakeDevice({ id: "clap", prefix: "fx1" });
    dev.setServesIsLoading(false);
    const ctl = openGrid(dev);
    const SLOW = C.CONTRACT_SETTLE_MS + 200;   /* outlasts the first probe */

    dev.stagePlugin(contractFor("Ensemble"), { debounceMs: SLOW });
    ctl.selectionChanged();
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) !== want("Ensemble"))
      fail("first slow selection did not settle: " + labels(ctl));

    dev.stagePlugin(contractFor("kosmos"), { debounceMs: SLOW });
    ctl.selectionChanged();
    settle(ctl, dev, SETTLED_MS);
    if (labels(ctl) === want("Ensemble"))
      fail("THE BUG: the second selection settled on the contract it was " +
           "replacing, " + labels(ctl));
    if (labels(ctl) !== want("kosmos"))
      fail("second slow selection did not settle: " + labels(ctl));
  }

  /* ---- 12. the is_loading verdict is per COMPONENT, not per session ----- */
  {
    /* Latching "this module does not implement it" forever would mean the
       first component you happen to open decides it for every module you open
       afterwards. */
    const serves = { fx1: false, fx2: true };
    const asked = { fx1: 0, fx2: 0 };
    const contract = JSON.stringify([
      { key: "param_0", name: "P", type: "float", min: 0, max: 1 },
    ]);
    const hierarchy = JSON.stringify({
      modes: null,
      levels: { root: { knobs: ["param_0"], params: ["param_0"] } },
    });
    let clock = 0;
    const io = {
      getParam(key) {
        const [pfx, bare] = [key.split(":")[0], key.split(":").slice(1).join(":")];
        if (bare === "ui_hierarchy") return hierarchy;
        if (bare === "chain_params") return contract;
        if (bare === "is_loading") { asked[pfx]++; return serves[pfx] ? "0" : ""; }
        return "0.5";
      },
      setParam() {}, announce() {}, now: () => clock,
    };
    const ctl = C.createController(io);
    ctl.load({ slot: 0, component: "fx1", prefix: "fx1" });
    for (let i = 0; i < 4; i++) {
      ctl.selectionChanged();
      for (let t = 0; t < C.CONTRACT_SETTLE_MS * 4; t += 10) { clock += 10; ctl.tick(); }
    }
    if (asked.fx1 > 1)
      fail("is_loading asked " + asked.fx1 + " times on a component that answers empty");

    ctl.load({ slot: 0, component: "fx2", prefix: "fx2" });
    ctl.selectionChanged();
    for (let t = 0; t < C.CONTRACT_SETTLE_MS * 4; t += 10) { clock += 10; ctl.tick(); }
    if (asked.fx2 === 0)
      fail("THE BUG: a component that DOES implement is_loading was never " +
           "asked, because the previous one latched unsupported");
  }

  console.log("PASS: contract settle");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
