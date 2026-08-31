#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A two-state control NAMED FOR AN ACTION is a button.
#
# `access: "write"` is the right way to declare a trigger and almost nothing in
# the fleet does it: 58 controls across 14 modules are momentary actions
# declared as ordinary booleans. forge alone has 13 -- Rnd Kit, Copy A>B,
# Swap A/B -- each drawing a latching switch for something you press once.
#
# The tell needs BOTH halves. A verb alone is not enough (`rnd_pitch_amt` is
# how much to randomise) and two states alone is not enough (Mono, Sync and
# Bypass are real switches). Every exclusion below came from a false positive
# found while sweeping the fleet, and each is asserted so it cannot come back.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node is required" >&2; exit 1; fi

node -e '
import("./src/shared/param_pages/param_meta.mjs").then((M) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const meta = (p) => M.buildMetaIndex({ chainParams: [p] }).getOrGuess(p.key);
  const inferred = (p) => !!meta(p).trigger_inferred;
  const E = (key, name, options, extra) => Object.assign({ key, name, type: "enum", options }, extra || {});

  /* ---- inferred: a verb with two states ------------------------------- */
  for (const p of [E("rnd_kit", "Rnd Kit", ["Off", "On"]),
                   E("copy_a_b", "Copy A>B", ["Off", "On"]),
                   E("swap_ab", "Swap A/B", ["Off", "On"]),
                   { key: "rnd_preset", name: "Rnd Preset", type: "int", min: 0, max: 1 },
                   { key: "rnd_pan", name: "Rnd Pan", type: "float", min: 0, max: 1, step: 1 }])
    if (!inferred(p)) fail(p.key + " is a two-state action and should be inferred a trigger");

  /* ---- inferred: the module spelled the momentary out ------------------ */
  if (!inferred(E("capture", "Capture Now", ["idle", "trigger"])))
    fail("idle/trigger is a module declaring a momentary in the only words it had");

  /* ---- NOT inferred: a mode is a state -------------------------------- */
  /* ducker: "Trigger" here NAMES a mode; it is not an act. */
  if (inferred(E("mode", "Mode", ["Trigger", "Gate"])))
    fail("a param called mode is a state, whatever its options are called");
  for (const p of [E("trigger_mode", "Trigger", ["Free", "Retrig"]),
                   E("retrigger", "Retrigger", ["Off", "On"]),
                   E("lfo_sync", "Sync", ["Off", "On"]),
                   E("fenv_hard_reset", "Hard Reset", ["Off", "On"])])
    if (inferred(p)) fail(p.key + " is a setting, not a gesture");

  /* ---- NOT inferred: a quantity that merely has a verb in its name ----- */
  for (const p of [{ key: "rnd_pitch_amt", name: "Rnd Pitch Amt", type: "float", min: 0, max: 1 },
                   { key: "global_rnd_seed", name: "Seed", type: "int", min: 0, max: 65535 },
                   { key: "random_rate", name: "Rnd Rate", type: "float", min: 0, max: 1 }])
    if (inferred(p)) fail(p.key + " is an amount, not a button");

  /* ---- NOT inferred: a verb with MORE than two states ----------------- */
  if (inferred(E("rnd_octave_range", "Rnd Range", ["+1", "-1", "+2", "-2"])))
    fail("a multi-way choice is a list, not a button");

  /* ---- a declaration always wins over the guess ----------------------- */
  const declaredState = meta(E("rnd_kit", "Rnd Kit", ["Off", "On"], { access: "readwrite" }));
  if (declaredState.writeOnly)
    fail("an explicit access must override the heuristic");

  console.log("PASS: two-state actions infer a trigger; modes, quantities and "
            + "multi-way choices do not, and an explicit access wins");
});
'
