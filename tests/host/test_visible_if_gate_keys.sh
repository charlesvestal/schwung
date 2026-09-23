#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A visible_if GATE THAT IS NOT A CELL still moves the page set.
#
# `visible_if` hides a level whose condition is false, and the re-plan that
# brings it back is driven by the condition's value CHANGING -- which the
# controller only notices for keys it READS. It read the page's own cells and
# nothing else, so a gate the user cannot turn was a gate that never moved:
# the page set was decided once, at entry, and frozen.
#
# That is not a corner. A module whose mode lives OUTSIDE the grid is the
# normal case for a multi-engine instrument -- a drum machine where the pad you
# hit selects the voice, and a cymbal wants different pages from a drum. Such a
# module could declare a perfectly correct visible_if and watch it do nothing,
# with no error anywhere. Reported from the device on schwung-urchin: every
# page for every voice.
#
# Pinned here:
#   1. a gate key with no cell joins the page's read rotation
#   2. when its value changes, the page set is re-planned
#   3. a gate key that IS a cell is not read twice
#   4. the cap is the declared-extras cap, not a second number
#   5. validate_contract does not call a gate "unreachable"

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/validate_contract.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
]).then(([C, V, VIZ]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* Two gated levels and one ungated one, the urchin shape: the gate is
     derived by the module from which pad was hit, so it is on no page. */
  const hierarchy = {
    levels: {
      root: { name: "R", knobs: [], params: [
        { level: "mix",  label: "Mix" },
        { level: "drum", label: "Drum" },
        { level: "cym",  label: "Cym" },
      ] },
      mix:  { name: "Mix",  knobs: ["vol", "pan"] },
      drum: { name: "Drum", knobs: ["pitch"], visible_if: { param: "engine", equals: "0" } },
      cym:  { name: "Cym",  knobs: ["size"],  visible_if: { param: "engine", equals: "1" } },
    },
  };
  const chainParams = [
    { key: "engine", name: "Engine", type: "int", min: 0, max: 1, default: 0 },
    { key: "vol",   name: "Vol",   type: "int", min: 0, max: 100, default: 50 },
    { key: "pan",   name: "Pan",   type: "int", min: -100, max: 100, default: 0 },
    { key: "pitch", name: "Pitch", type: "int", min: 0, max: 100, default: 50 },
    { key: "size",  name: "Size",  type: "int", min: 0, max: 100, default: 50 },
  ];

  let engine = "0";
  const values = { vol: "50", pan: "0", pitch: "50", size: "50" };
  /* The controller READS its contract off the device, so the fake serves it. */
  const serve = (hier, reads) => (k) => {
    const bare = k.indexOf(":") >= 0 ? k.slice(k.indexOf(":") + 1) : k;
    if (bare === "ui_hierarchy") return JSON.stringify(hier);
    if (bare === "chain_params") return JSON.stringify(chainParams);
    if (reads) reads.push(bare);
    if (bare === "engine") return engine;
    return values[bare] !== undefined ? values[bare] : null;
  };
  /*
   * THE EVALUATOR CONSULTS THE CONTROLLER\x27S CACHE FIRST, then falls back to a
   * read — paramPagesCachedValue() then getSlotParamCached() in shadow_ui.js.
   * Modelled faithfully here, and it is the whole point of the test: a fake
   * that reads the device variable directly would follow the gate whether or
   * not the controller ever read it, and would pass on main.
   */
  const live = { c: null };
  const visible = (cond) => {
    if (!cond || !cond.param) return true;
    if (!String(cond.param).endsWith("engine")) return true;
    const cached = live.c && live.c.state && live.c.state.values
                 ? live.c.state.values.engine : undefined;
    const v = cached !== undefined ? cached : engine;   /* cache, then read */
    return String(cond.equals) === String(v);
  };

  const reads = [];
  const io = { getParam: serve(hierarchy, reads), setParam: () => {}, visible };
  engine = "0";

  const c = C.createController(io);
  live.c = c;
  c.load({ slot: 0, component: "synth", prefix: "synth", visible });
  for (let i = 0; i < 5; i++) c.tick();

  const names = () => c.state.pages.map((p) => p.name).join(" ");
  if (!/Drum/.test(names()) || /Cym/.test(names()))
    fail("the first plan should show Drum and not Cym, got: " + names());
  console.log("  ok  a gate with no cell still decides the first page set");

  /* ---- 1: IDLE COSTS NOTHING ------------------------------------------
     A gate whose value only moves because of a write from THIS grid already
     re-plans through the write path, and the planner reads whatever else it
     needs on demand. Polling every condition key would spend stops refreshing
     values that were already right — echidna-fx has ~72 of them behind one
     Cat knob — and slow the knobs sharing the rotation for nothing. */
  reads.length = 0;
  for (let i = 0; i < 40; i++) c.tick();
  if (reads.indexOf("engine") >= 0)
    fail("an idle grid polled the gate — it should be an EVENT, not a poll");
  console.log("  ok  an idle grid does not poll gate keys");

  /* ---- 2b: A PAD PRESS DOES NOT WAIT FOR ITS TURN --------------- */
  /* Once a pass is a fifth of a second on a full page — fine for a refresh,
     far too slow for "I hit a hat, show me the hat pages", which is the whole
     gesture an off-page gate serves. The vouch prioritises the next stop. */
  {
    /* A FULL page, deliberately. With two cells the ordinary rotation comes
       round every four ticks and would pass this on its own — the whole point
       is the page where waiting for your turn is slow. */
    const big = ["k1","k2","k3","k4","k5","k6","k7","k8"];
    const h3 = JSON.parse(JSON.stringify(hierarchy));
    h3.focus_press_param = "live_press";
    h3.levels.mix.knobs = big;
    const cp3 = chainParams.concat(big.map((k) => (
      { key: k, name: k, type: "int", min: 0, max: 100, default: 50 })));
    for (const k of big) values[k] = "50";
    engine = "0";
    const reads3 = [];
    const serve3 = (k) => {
      const bare = k.indexOf(":") >= 0 ? k.slice(k.indexOf(":") + 1) : k;
      reads3.push(bare);
      if (bare === "ui_hierarchy") return JSON.stringify(h3);
      if (bare === "chain_params") return JSON.stringify(cp3);
      if (bare === "engine") return engine;
      return values[bare] !== undefined ? values[bare] : null;
    };
    const c3 = C.createController({ getParam: serve3, setParam: () => {}, visible });
    live.c = c3;
    c3.load({ slot: 0, component: "synth", prefix: "synth", visible });
    for (let i = 0; i < 60; i++) c3.tick();
    if (!/Drum/.test(c3.state.pages.map((p) => p.name).join(" ")))
      fail("setup: expected the drum pages");

    /* Measure the READ, not the page swap. The swap can also come from a
       periodic re-plan, whose evaluator falls back to a device read on a cache
       miss — so timing the swap would pass with or without the priority stop.
       Ticks-until-the-gate-is-read isolates exactly the thing being claimed. */
    engine = "1";
    const t0 = reads3.length;
    if (!c3.vouchLivePress()) fail("the vouch was not taken — focus_press_param not seen");
    let ticks = 0, sawGate = false;
    while (ticks < 40 && !sawGate) {
      c3.tick(); ticks++;
      sawGate = reads3.slice(t0).indexOf("engine") >= 0;
    }
    if (!sawGate) fail("the gate was never read after the press");
    /* The read lands on one tick and the re-plan it triggers on the next. */
    c3.tick();
    const names3 = () => c3.state.pages.map((p) => p.name).join(" ");
    if (!/Cym/.test(names3())) fail("the pages did not follow the press: " + names3());
    if (/Drum/.test(names3())) fail("the drum pages should be gone: " + names3());
    console.log("  ok  the pages follow a pad press (gate read in " + ticks + " tick(s))");

    /* ⚠ THE PRIORITY ITSELF IS PINNED AT THE SOURCE, not by timing.
     *
     * A fake device answers instantly, so "how many ticks" here measures the
     * harness, not the rotation: the assertion passed with the priority stop
     * REMOVED, which is exactly the kind of test that reports a feature it is
     * not exercising. On the device the gate would wait its turn — a full
     * rotation, ~200ms on a page of eight — and that is the cost this buys
     * back. Same approach as test_grid_visible_if_context.sh: the ordering is
     * the contract, so the contract is what gets checked. */
    const pc = require("fs").readFileSync("src/shared/param_pages/page_controller.mjs", "utf8");
    if (!/function vouchLivePress\(\)[\s\S]{0,1200}s\.gatesDue = true/.test(pc))
      fail("a live pad press does not mark the gates due");
    const iDue = pc.indexOf("if (s.gatesDue)");
    const iWarm = pc.indexOf("const warm = neighbourPrefetch");
    if (iDue < 0) fail("there is no gates-due stop");
    if (!(iDue < iWarm)) fail("the gates-due stop does not run BEFORE the ordinary rotation stop");
    if (!/s\.gatesDue = false/.test(pc))
      fail("the gates-due flag is never cleared — every tick would be spent on gates");
    console.log("  ok  a press marks the gates due, and that stop is served before the ordinary rotation");

    /* The OTHER outside event: the module moves its own focus. A pad hit with
       no transport running moves focus by the note, not the vouch, so pinning
       only the press would leave that path dead. */
    const iSync = pc.indexOf("function syncChildIndexFromModule");
    const iDue2 = pc.indexOf("s.gatesDue = true", iSync);
    const iEnd = pc.indexOf("\n    }", iSync);
    if (iSync < 0 || iDue2 < 0 || iDue2 > iEnd)
      fail("the module moving its own focus does not mark the gates due");
    console.log("  ok  the module moving its own focus marks the gates due too");
  }

  /* ---- 3: a gate that IS a cell is not read twice ----------------------- */
  const h2 = JSON.parse(JSON.stringify(hierarchy));
  h2.levels.mix.knobs = ["engine", "vol"];
  engine = "0";
  const reads2 = [];
  const c2 = C.createController({ getParam: serve(h2, reads2), setParam: () => {}, visible });
  live.c = c2;
  c2.load({ slot: 0, component: "synth", prefix: "synth", visible });
  for (let i = 0; i < 5; i++) c2.tick();
  reads2.length = 0;
  for (let i = 0; i < 12; i++) c2.tick();
  const perPass = reads2.filter((k) => k === "engine").length;
  if (perPass > 6) fail("a gate that is already a cell was read twice a pass (" + perPass + ")");
  console.log("  ok  a gate that is already a cell is not read twice");

  /* ---- 4: one cap, shared with the declared extras ---------------------- */
  const src = require("fs").readFileSync("src/shared/param_pages/page_controller.mjs", "utf8");
  if (!/MAX_DECLARED_EXTRA_KEYS/.test(src))
    fail("the gate lane invents its own cap instead of reusing the extras cap");
  if (typeof VIZ.MAX_DECLARED_EXTRA_KEYS !== "number")
    fail("MAX_DECLARED_EXTRA_KEYS is not exported as a number");
  console.log("  ok  the gate lane is capped by the SAME constant the declared extras use");

  /* ---- 4b: THE FIRST PLAN IS THE GRID\x27S ----------------------------------
   *
   * controller.load() runs inside enterParamPages, BEFORE setView flips the
   * view to PARAM_PAGES -- so evaluateVisibilityCondition, which asks whether
   * the grid is up to decide whose slot to read, took the list editor\x27s slot
   * (-1 from here), read null and failed open. Every gated level visible, on
   * the one plan the user actually lands on.
   *
   * setView is deliberately NOT moved ahead of the load: it closes the knob
   * card and clears the touch set. Pinned at the source, in the same style as
   * test_grid_visible_if_context.sh, because the ordering is the contract. */
  const pp = require("fs").readFileSync("src/shadow/shadow_ui_param_pages.mjs", "utf8");
  const ui = require("fs").readFileSync("src/shadow/shadow_ui.js", "utf8");
  if (!/export function paramPagesEntering/.test(pp))
    fail("shadow_ui_param_pages does not publish whether the grid is coming up");
  if (!/entering = true;[\s\S]{0,400}controller\.load\(/.test(pp))
    fail("the entering window does not cover controller.load — the first plan still has no context");
  if (!/finally\s*\{\s*entering = false;/.test(pp))
    fail("the entering flag is not cleared in a finally — a throwing load would strand it true");
  if (!/paramPagesEntering\(\)\s*\)\s*&&\s*paramPagesActive\(\)/.test(ui))
    fail("evaluateVisibilityCondition does not take the grid context while the grid is coming up");
  if (!/view === VIEWS\.PARAM_PAGES \|\| paramPagesEntering\(\)/.test(ui))
    fail("the view test was replaced rather than widened — the hierarchy editor would inherit the grid\x27s context");
  console.log("  ok  the first plan resolves gates against the grid, and the list editor keeps its own context");

  /* ---- 4c: A PER-PAD GATE IS READ FOR THE PAD ----------------------------
   *
   * A condition on a child level names the TEMPLATE ("type"), and the
   * evaluator resolves it for the instance on screen ("pad0_type"). The gate
   * lane first shipped reading it through fullKey, which resolves only a key
   * that is a cell on the current page -- and the lane reads exactly the keys
   * that are not. So it asked for `synth:type`, got the chain host\x27s "" for
   * an unknown key, and cached "" under `type`, the slot the evaluator
   * consults FIRST: one pad press turned a correct gate into a wrong one. */
  {
    const hp = { focus_press_param: "live_press", levels: {
      root: { name: "Pads", child_prefix: "pad", child_count: 4, knobs: ["tune", "decay"],
              params: [{ key: "tune" }, { key: "decay" },
                       { key: "size", visible_if: { param: "type", equals: "1" } }] },
    } };
    const cpp = ["type", "tune", "decay", "size", "live_press"].map((k) =>
      ({ key: k, name: k, type: "int", min: 0, max: 100 }));
    for (const i of [0, 1, 2, 3]) for (const k of ["type", "tune", "decay", "size"])
      cpp.push({ key: "pad" + i + "_" + k, name: k, type: "int", min: 0, max: 100 });
    const padType = { pad0_type: "1", pad1_type: "0" };
    let focus = "0";
    const rp = [];
    const sp = (k) => {
      const b = k.slice(k.indexOf(":") + 1);
      if (b === "ui_hierarchy") return JSON.stringify(hp);
      if (b === "chain_params") return JSON.stringify(cpp);
      rp.push(b);
      if (b in padType) return padType[b];
      if (/^pad\d_/.test(b)) return "5";
      return "";              /* what the chain host serves for an unknown key */
    };
    const cpd = C.createController({ getParam: sp, setParam: () => {} });
    cpd.load({ slot: 0, component: "synth", prefix: "synth", visible: () => true });
    for (let i = 0; i < 20; i++) cpd.tick();
    rp.length = 0;
    if (!cpd.vouchLivePress()) fail("setup: the per-pad vouch was not taken");
    for (let i = 0; i < 3; i++) cpd.tick();
    if (rp.indexOf("type") >= 0)
      fail("the gate lane read the bare template `type` -- a per-pad gate must be read for the pad: " + rp.join(","));
    if (rp.indexOf("pad0_type") < 0)
      fail("the gate lane never read the resolved key pad0_type: " + rp.join(","));
    if (cpd.state.values.type !== "1")
      fail("the cached gate is not pad 0\x27s answer: " + JSON.stringify(cpd.state.values.type));
    console.log("  ok  a per-pad gate is read for the pad on screen, not as its bare template");
  }

  /* The NEIGHBOUR LANE note was replaced rather than joined in the first cut
     of this change; it records why that lane is conditional and bounded. */
  if (!/THE NEIGHBOUR LANE/.test(src)) fail("the neighbour-lane note was deleted");

  /* ---- 5: a gate is not an unreachable param ---------------------------- */
  const { findings } = V.validateContract({ id: "t", hierarchy, chainParams });
  const un = findings.filter((f) => f.rule === "unreachable-params");
  if (un.some((f) => /engine/.test(f.message)))
    fail("a gate key was reported as unreachable: " + un.map((f) => f.message).join(" | "));
  console.log("  ok  a gate key is not reported as unreachable");

  console.log("PASS: visible_if gate keys — an off-page gate is read when something OUTSIDE the grid moves (a press, or the module\x27s own focus), never on an idle poll; the first plan resolves against the grid; a gate is not called unreachable");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
