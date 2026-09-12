#!/usr/bin/env bash
# A burst of writes to a condition key buys ONE plan, not one plan per write.
#
# WHY THIS EXISTS. `replanIfCondition` used to run a complete `planPages`
# synchronously on every write to a condition key. An encoder sweep is a burst
# of CCs -- a continuous cell emits hundreds per turn -- so turning a knob that
# gates other params re-planned the entire module once per detent, before
# anything was drawn once.
#
# WHAT THIS TEST ACTUALLY MEASURES, because the two numbers are different and
# the difference is the whole reason to read this comment.
#
# REPORTED ON DEVICE with DR32, whose send-effect page gates its cells on the
# send's mode: 26 condition evaluations per planning pass, and 2912 evaluations
# inside a single 10.6 ms tick -- 112 complete planning passes. That report is
# what prompted the change and it is NOT reproduced here or anywhere else. The
# write path cannot produce it: `onKnobTurn` already throttles its setParam and
# its re-plan to one per SETPARAM_THROTTLE_MS (20 ms) per key, so a sweep buys
# at most one or two passes per ~23 ms tick however many detents arrive.
#
# MEASURED HERE, by reverting `replanIfCondition` to call `replanNow()`: the
# 112-detent burst below cost 4 evaluations -- TWO passes, not 112. So this
# test pins a real property (the cost of a burst does not grow with the burst)
# and the device's 2912 remains unexplained: whatever produced it is somewhere
# this test does not look -- warmCurrentPage's 8 reads, flushDueWrites over
# many pending keys, a repeated load()/reloadIfChanged, or a counter counting
# per-condition-per-level rather than per-pass.
#
# DO NOT read a green run here as the DR32 hang being fixed.
#
# THE OBSERVABLE IS PASSES, NOT READS. The `visible` hook fires once per
# condition per pass, so counting its calls counts passes. A read count would
# also fall if a cache were added underneath, which would leave all 112 passes
# in place -- the thing being fixed here is how often the planner RUNS.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null 2>&1 || { echo "FAIL: node required"; exit 1; }

node --input-type=module -e '
import { createController, LAYOUT_MOVY } from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "  ok  " : "FAIL ") + " — " + m); if (!c) fail++; };

/* dr32-shaped: a mode param gating a set of cells on the same level. */
const PARAMS = [
  { key: "send1_mode", name: "Mode", type: "enum", options: ["delay", "reverb"] },
  { key: "send1_time", name: "Time", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "send1_fb",   name: "FB",   type: "float", min: 0, max: 1, step: 0.01 },
];
const HIER = { modes: null, levels: { root: {
  label: "Send 1",
  knobs: ["send1_mode", "send1_time", "send1_fb"],
  params: [
    { key: "send1_mode" },
    { key: "send1_time", visible_if: { param: "send1_mode", equals: "delay" } },
    { key: "send1_fb",   visible_if: { param: "send1_mode", equals: "delay" } },
  ],
} } };

let evals = 0;
let mode = "delay";
let clock = 1000;
const ctl = createController({
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(PARAMS);
    if (b === "send1_mode") return mode;
    if (b.indexOf(":") >= 0) return "";
    return "0.5";
  },
  setParam: (k, v) => { if (String(k).endsWith("send1_mode")) mode = String(v); },
  announce: () => {},
  now: () => clock,
});
ctl.setLayout(LAYOUT_MOVY);
/* ⚠ `visible` is a LOAD option, not a constructor one — `replanIfCondition`
 * reads it off `s.lastLoadOpts`. Passed to createController it is silently
 * ignored, the hook never fires, and every count below is 0 === 0. The control
 * above exists because that is exactly what happened. */
ctl.load({
  prefix: "synth",
  /* One call per condition per planning pass — the pass counter. */
  visible: () => { evals += 1; return true; },
});

/* Settle whatever the load left owed, so the counts below start from zero. */
clock += 20; ctl.tick();

/* The mode knob, driven exactly as the hardware drives it. */
const modeSlot = (() => {
  for (let i = 0; i < 8; i++) if (ctl.keyAt && ctl.keyAt(i) === "send1_mode") return i;
  return 0;
})();

/* ---- CONTROL: the hook is wired and a turn really does re-plan ---------- */
{
  evals = 0;
  clock += 20; ctl.onKnobTurn(modeSlot, 1, clock);
  clock += 20; ctl.tick();
  ok(evals > 0, "control: a turn of the gate knob costs a planning pass (" + evals + " evaluations)");
}
const ONE_PASS = evals;

/* ---- the burst --------------------------------------------------------- */
{
  evals = 0;
  /* An encoder sweep: a burst of detents inside ONE tick. This codebase already
   * has the law written down — a continuous cell emits 255x2 CCs per sweep — so
   * 112 is not a stress figure, it is Tuesday. */
  for (let i = 0; i < 112; i++) ctl.onKnobTurn(modeSlot, i % 2 ? 1 : -1, clock);
  const duringBurst = evals;
  ok(duringBurst === 0,
     "⭐ 112 writes inside one tick plan NOTHING while the burst is arriving (" + duringBurst + ")");

  clock += 20; ctl.tick();
  ok(evals === ONE_PASS,
     "⭐⭐ the whole burst costs ONE pass (" + evals + " evaluations, one pass = " + ONE_PASS + ")");
}

/* ---- and the cost does not grow with the burst -------------------------- */
{
  /* THE property, stated as one: 1 detent and 112 detents cost the same. Before
   * this change the 112-detent burst cost 2 passes where a single turn cost 1
   * — the write throttle had already capped it, which is why the pre-change
   * number is 2 and not 112 (see the header). A burst that costs strictly more
   * than a single turn is the regression this guards.
   * (No apostrophes in here: the whole script is one single-quoted shell arg.) */
  clock += 20; ctl.tick();
  evals = 0;
  clock += 20; ctl.onKnobTurn(modeSlot, 1, clock);
  clock += 20; ctl.tick();
  const one = evals;

  clock += 20; ctl.tick();
  evals = 0;
  for (let i = 0; i < 112; i++) ctl.onKnobTurn(modeSlot, i % 2 ? 1 : -1, clock);
  clock += 20; ctl.tick();
  ok(evals === one,
     "112 detents cost exactly what 1 detent costs (" + evals + " vs " + one + ")");
}

/* ---- it must still actually happen, and before anything is drawn -------- */
{
  clock += 20; ctl.tick();
  evals = 0;
  clock += 20; ctl.onKnobTurn(modeSlot, 1, clock);
  ok(evals === 0, "a pending plan is not run by the write itself");
  clock += 20; ctl.tick();
  ok(evals === ONE_PASS, "⭐ and the tick DOES run it — a deferred plan that never happens is worse");
}

/* ---- a turn of a NON-gate knob buys nothing ---------------------------- */
{
  const timeSlot = (() => {
    for (let i = 0; i < 8; i++) if (ctl.keyAt && ctl.keyAt(i) === "send1_time") return i;
    return -1;
  })();
  if (timeSlot < 0) {
    ok(false, "control: send1_time is not on a knob — this case would test nothing");
  } else {
    evals = 0;
    for (let i = 0; i < 20; i++) { clock += 20; ctl.onKnobTurn(timeSlot, 1, clock); }
    clock += 20; ctl.tick();
    ok(evals === 0,
       "a turn of a knob no condition names plans nothing (" + evals + ")");
  }
}

/* ---- a settled tick is free ------------------------------------------- */
{
  evals = 0;
  for (let i = 0; i < 5; i++) { clock += 20; ctl.tick(); }
  ok(evals === 0, "five settled ticks plan nothing (" + evals + ")");
}

console.log(fail === 0 ? "PASS: a burst of gate writes costs one plan" : "FAIL (" + fail + ")");
process.exit(fail === 0 ? 0 : 1);
'
