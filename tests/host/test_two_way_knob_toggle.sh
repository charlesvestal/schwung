#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A BOXED two-way toggles on a detent whichever way it went, once per flick.
# A SWITCH does not: clockwise is on, anticlockwise is off.
#
# THE SPLIT IS THE WIDGET, NOT THE SEMANTICS, and that is the whole of it:
#
#   Mix/Reverb   drawn as an enum SQUARE — a boxed value. Mix and Reverb sit in
#                the same box in the same place, so the cell shows a STATE and
#                names no direction. A direction-absolute knob there is dead
#                half the time and there is nothing on screen to learn it from.
#                Reported from the device: "if there are only two, why not let
#                it wrap otherwise you have to know which way is off and which
#                way is on, in which case you need some knowledge you dont
#                have."
#
#   Off/On       drawn as a SWITCH — a track with a knob at one end of it. The
#                form itself names the direction, the same one every physical
#                switch has. Toggling it breaks that promise: a clockwise turn
#                on an already-on switch turned it OFF. Also reported from the
#                device: "if it's on it should stay on when turning it on."
#
# So the turn partition must equal the DRAW partition — detectSwitch emits
# VIZ_SWITCH for exactly isBooleanMeta && !isTrigger, and knobStep's switch
# branch guards on exactly that pair. Section 6 pins them equal, because a
# drift means a control makes a promise with its shape that the knob does not
# keep. The split is in knobStep alone: both are still clicked and dived the
# same, and isTwoWayMeta still answers true for both.
#
# WRAPPING ALONE IS NOT THE ANSWER, which is what most of this file is about.
# With two values, "wrap" and "toggle on every detent" are identical, and one
# flick of an encoder is a dozen detents — so a flick would land on whichever
# value the detent count happened to be even or odd about. The LATCH is what
# makes the gesture legible, and it is a latch rather than a rate limit: the
# stamp is the last DETENT, so the clock runs on STILLNESS. That distinction
# shipped wrong once already on the trigger and was reported from hardware.
#
# The gap VALUE is deliberately not pinned. The tests assert "clearly inside"
# and "clearly outside" so the constant can be retuned without breaking them,
# while a broken latch still fails.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
Promise.all([
  import("./src/shared/knob_engine.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
]).then(([K, V]) => {
  let failures = 0;
  const fail = (m) => { console.error("FAIL: " + m); failures++; };

  const OFF_ON  = { type: "enum", options: ["Off", "On"] };
  const CHOICE  = { type: "enum", options: ["Mix", "Reverb"] };
  const ON_OFF  = { type: "enum", options: ["On", "Off"] };   /* declared backwards */
  const INTBOOL = { type: "int", min: 0, max: 1 };
  const THREE   = { type: "enum", options: ["A", "B", "C"] };
  const TRIGGER = { type: "enum", options: ["—", "Rnd!"], access: "write" };

  /* One detent, from a cold state, at t. */
  const tap = (meta, from, dir, t) => {
    const st = K.knobInit(from);
    K.knobStep(st, meta, dir, t);
    return st.value;
  };

  /* ---- 1a. a CHOICE: either direction reaches the other value ---------- */
  for (const [name, meta] of [["Mix/Reverb", CHOICE]]) {
    for (const from of [0, 1]) {
      const want = from === 0 ? 1 : 0;
      for (const dir of [1, -1]) {
        const got = tap(meta, from, dir, 1000);
        if (got !== want)
          fail(name + " at " + from + " turned " + (dir > 0 ? "up" : "down") + " gave " + got +
               ", expected " + want + " — a two-way choice with a dead direction is dead half " +
               "the time, and the cell shows a state, not a direction");
      }
    }
  }

  /* ---- 1b. a BOOLEAN: clockwise is ON, and STAYS on -------------------- */
  for (const [name, meta] of [["Off/On", OFF_ON], ["int 0..1", INTBOOL], ["On/Off", ON_OFF]]) {
    const on = name === "On/Off" ? 0 : 1;   /* the ON value, by its WORD */
    const off = 1 - on;
    for (const from of [0, 1]) {
      const up = tap(meta, from, 1, 1000);
      if (up !== on)
        fail(name + " at " + from + " turned clockwise gave " + up + ", expected " + on +
             " — clockwise means ON, and on an already-on switch that is a no-op, not a flip");
      const down = tap(meta, from, -1, 1000);
      if (down !== off)
        fail(name + " at " + from + " turned anticlockwise gave " + down + ", expected " + off +
             " — anticlockwise means OFF");
    }
  }

  /* ...and it is IDEMPOTENT across a whole flick, with no latch to rely on:
   * a dozen clockwise detents all say the same thing. */
  {
    const st = K.knobInit(1);
    for (let t = 1000; t <= 3000; t += 30) K.knobStep(st, OFF_ON, 1, t);
    if (st.value !== 1)
      fail("a 2-second clockwise spin on an ON switch left it at " + st.value +
           " — a switch write is idempotent and must not depend on a gesture latch");
    for (let t = 4000; t <= 6000; t += 30) K.knobStep(st, OFF_ON, -1, t);
    if (st.value !== 0) fail("a 2-second anticlockwise spin did not leave the switch OFF");
  }

  /* ---- 2. ONE FLICK IS ONE FLIP (a choice only — a switch has no flip) -- */
  for (const [name, meta] of [["Mix/Reverb", CHOICE]]) {
    const st = K.knobInit(0);
    let t = 1000;
    K.knobStep(st, meta, 1, t);
    if (st.value !== 1) fail(name + ": the first detent of a flick did not flip it");
    /* Two seconds of detents 30ms apart. Under a plain wrap this lands on
     * whichever parity the count has; under a RATE LIMIT of any plausible
     * size it flips several times. */
    for (t = 1030; t <= 3000; t += 30) K.knobStep(st, meta, 1, t);
    if (st.value !== 1)
      fail(name + ": a 2-second spin left it at " + st.value + " — one flick must be one flip, " +
           "so this is a wrap or a rate limit rather than a gesture latch");
  }

  /* ---- 3. the clock runs on STILLNESS, not on elapsed time -------------- */
  {
    const st = K.knobInit(0);
    let t = 1000;
    K.knobStep(st, CHOICE, 1, t);                       /* flip */
    for (t = 1030; t <= 3000; t += 30) K.knobStep(st, CHOICE, 1, t);
    /* Still latched at t=3000 even though 2s have passed since the FLIP —
     * because the knob never stopped. Now let it stop. */
    K.knobStep(st, CHOICE, 1, 5000);
    if (st.value !== 0)
      fail("the knob went still for 2s and the next detent did not flip it — the stamp must be " +
           "the last DETENT, written before the early return");
    /* And a detent clearly INSIDE the window is still swallowed. */
    K.knobStep(st, CHOICE, 1, 5100);
    if (st.value !== 0) fail("a detent 100ms after a flip was not swallowed");
  }

  /* ---- 4. what is NOT a two-way ----------------------------------------- */
  {
    /* Three options keep the sweep: a long list wants a gate, not a toggle. */
    const st = K.knobInit(0);
    K.knobStep(st, THREE, 1, 1000);
    if (st.value === 1)
      fail("a three-option enum flipped on ONE detent — it should still be gated, and it " +
           "should never toggle");
    for (let t = 1030; t <= 1200; t += 30) K.knobStep(st, THREE, 1, t);
    if (st.value !== 1) fail("a three-option enum did not advance across a gated sweep");
    /* ...and it CLAMPS at the top rather than wrapping round to A. Wrapping a
     * 47-model list would make the end of it unreachable by feel. */
    for (let t = 1230; t <= 3000; t += 30) K.knobStep(st, THREE, 1, t);
    if (st.value !== 2) fail("a three-option enum did not clamp at its last option");

    /* A TRIGGER is a two-option enum on the wire. Toggling it would write
     * "do nothing" on every other flick — for euclidrum that is the write that
     * destroys a kit. */
    const tv = tap(TRIGGER, 1, 1, 1000);
    if (tv === 0)
      fail("a trigger was toggled back to its IDLE spelling — a two-way rule must never " +
           "reach access:write");
  }

  /* ---- 5. the gap matches the trigger latch, by NUMBER ------------------ */
  {
    const fs = require("fs");
    const pc = fs.readFileSync("src/shared/param_pages/page_controller.mjs", "utf8");
    const m = pc.match(/TRIGGER_KNOB_GESTURE_GAP_MS\s*=\s*(\d+)/);
    if (!m) fail("could not find TRIGGER_KNOB_GESTURE_GAP_MS in page_controller.mjs");
    else if (Number(m[1]) !== K.TWO_WAY_GESTURE_GAP_MS)
      fail("the two-way latch is " + K.TWO_WAY_GESTURE_GAP_MS + "ms and the trigger latch is " +
           m[1] + "ms — they are the same rule (one flick is one gesture) and a user cannot " +
           "learn two different flick lengths for two controls that look alike");
  }

  /* ---- 6. the TURN partition equals the DRAW partition ------------------ */
  {
    /* This is the load-bearing claim. A control that wears a track must be the
     * control the knob treats as having a direction, and nothing else may be.
     * Asserted over the whole fleet rather than a handful of literals. */
    const fs = require("fs");
    const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
    /* Turning a meta twice from the SAME start, one detent each way, at times
     * far enough apart that no latch is in play. Direction-absolute lands on
     * two different values; a toggle lands on the same one both times. */
    const isDirectional = (meta) => {
      const up = tap(meta, 0, 1, 1000);
      const dn = tap(meta, 0, -1, 1000);
      return up !== dn;
    };
    let checked = 0, drawn = 0;
    for (const m of fx.modules) {
      for (const p of (m.chain_params || [])) {
        const meta = { ...p, kind: p.kind || p.type };
        const draws = V.isBooleanMeta(meta) && !(meta.writeOnly || meta.access === "write");
        if (!draws && !(Array.isArray(meta.options) && meta.options.length === 2)) continue;
        checked++;
        if (draws) drawn++;
        const turns = isDirectional(meta);
        if (draws !== turns)
          fail(m.id + "." + p.key + (draws
            ? " is DRAWN as a switch but TOGGLES — the track points a direction the knob ignores"
            : " is drawn as a boxed value but turns direction-absolute — nothing on screen says which way"));
      }
    }
    if (drawn < 40)
      fail("only " + drawn + " switch-drawn params found across " + checked +
           " two-value params — the fixture is not exercising this");
  }

  if (failures) process.exit(1);
  console.log("PASS: what is DRAWN as a switch is clockwise-on, a boxed two-way toggles either " +
              "way once per flick, and neither a longer enum nor a trigger is touched");
}).catch((e) => { console.error("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
