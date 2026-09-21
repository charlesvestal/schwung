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
  const visible = (cond) => !cond || !cond.param ? true
         : (String(cond.param).endsWith("engine") ? String(cond.equals) === engine : true);

  const reads = [];
  const io = { getParam: serve(hierarchy, reads), setParam: () => {}, visible };

  const c = C.createController(io);
  c.load({ slot: 0, component: "synth", prefix: "synth", visible });
  for (let i = 0; i < 5; i++) c.tick();

  const names = () => c.state.pages.map((p) => p.name).join(" ");
  if (!/Drum/.test(names()) || /Cym/.test(names()))
    fail("the first plan should show Drum and not Cym, got: " + names());
  console.log("  ok  a gate with no cell still decides the first page set");

  /* ---- 1 + 2: the gate is read off-page, and a change re-plans ---------- */
  reads.length = 0;
  for (let i = 0; i < 40; i++) c.tick();
  if (reads.indexOf("engine") < 0)
    fail("the gate key was never read: " + [...new Set(reads)].join(","));
  console.log("  ok  a gate key with no cell joins the page read rotation");

  engine = "1";
  let swapped = false;
  for (let i = 0; i < 60 && !swapped; i++) { c.tick(); swapped = /Cym/.test(names()); }
  if (!swapped) fail("the page set never followed the gate, still: " + names());
  if (/Drum/.test(names())) fail("the drum level should be gone, got: " + names());
  console.log("  ok  a change to the gate re-plans the page set");

  /* ---- 3: a gate that IS a cell is not read twice ----------------------- */
  const h2 = JSON.parse(JSON.stringify(hierarchy));
  h2.levels.mix.knobs = ["engine", "vol"];
  engine = "0";
  const reads2 = [];
  const c2 = C.createController({ getParam: serve(h2, reads2), setParam: () => {}, visible });
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

  /* ---- 5: a gate is not an unreachable param ---------------------------- */
  const { findings } = V.validateContract({ id: "t", hierarchy, chainParams });
  const un = findings.filter((f) => f.rule === "unreachable-params");
  if (un.some((f) => /engine/.test(f.message)))
    fail("a gate key was reported as unreachable: " + un.map((f) => f.message).join(" | "));
  console.log("  ok  a gate key is not reported as unreachable");

  console.log("PASS: visible_if gate keys — a gate with no cell is read, re-plans the page set, is capped by the shared constant, and is not called unreachable");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
