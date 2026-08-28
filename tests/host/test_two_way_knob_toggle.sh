#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A control with exactly TWO values TOGGLES on a detent, whichever way it went,
# once per flick.
#
# There were three spellings of one control and two of them had a dead
# direction:
#
#   Off/On (or int 0..1)  direction-ABSOLUTE: right meant On, left meant Off,
#                         so at Off a left turn did nothing, forever
#   Mix/Reverb            fell to the enum branch and CLAMPED behind a
#                         four-detent gate, so at Mix a left turn did nothing,
#                         forever, and a right turn took four detents
#
# Reported from the device: "if there are only two, why not let it wrap
# otherwise you have to know which way is off and which way is on, in which
# case you need some knowledge you dont have." There is no way to acquire it —
# the cell shows a STATE, not a direction.
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
]).then(([K]) => {
  let failures = 0;
  const fail = (m) => { console.error("FAIL: " + m); failures++; };

  const OFF_ON  = { type: "enum", options: ["Off", "On"] };
  const CHOICE  = { type: "enum", options: ["Mix", "Reverb"] };
  const INTBOOL = { type: "int", min: 0, max: 1 };
  const THREE   = { type: "enum", options: ["A", "B", "C"] };
  const TRIGGER = { type: "enum", options: ["—", "Rnd!"], access: "write" };

  /* One detent, from a cold state, at t. */
  const tap = (meta, from, dir, t) => {
    const st = K.knobInit(from);
    K.knobStep(st, meta, dir, t);
    return st.value;
  };

  /* ---- 1. either direction reaches the other value --------------------- */
  for (const [name, meta] of [["Off/On", OFF_ON], ["Mix/Reverb", CHOICE], ["int 0..1", INTBOOL]]) {
    for (const from of [0, 1]) {
      const want = from === 0 ? 1 : 0;
      for (const dir of [1, -1]) {
        const got = tap(meta, from, dir, 1000);
        if (got !== want)
          fail(name + " at " + from + " turned " + (dir > 0 ? "up" : "down") + " gave " + got +
               ", expected " + want + " — a two-way with a dead direction is dead half the " +
               "time, and the cell shows a state, not a direction");
      }
    }
  }

  /* ---- 2. ONE FLICK IS ONE FLIP ---------------------------------------- */
  for (const [name, meta] of [["Off/On", OFF_ON], ["Mix/Reverb", CHOICE], ["int 0..1", INTBOOL]]) {
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
    K.knobStep(st, OFF_ON, 1, t);                       /* flip */
    for (t = 1030; t <= 3000; t += 30) K.knobStep(st, OFF_ON, 1, t);
    /* Still latched at t=3000 even though 2s have passed since the FLIP —
     * because the knob never stopped. Now let it stop. */
    K.knobStep(st, OFF_ON, 1, 5000);
    if (st.value !== 0)
      fail("the knob went still for 2s and the next detent did not flip it — the stamp must be " +
           "the last DETENT, written before the early return");
    /* And a detent clearly INSIDE the window is still swallowed. */
    K.knobStep(st, OFF_ON, 1, 5100);
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

  if (failures) process.exit(1);
  console.log("PASS: a two-value control toggles either way, once per flick, and neither a " +
              "longer enum nor a trigger is touched");
}).catch((e) => { console.error("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
