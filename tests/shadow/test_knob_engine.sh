#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# knob_engine, after the two knob models were unified into one.
#
# This file used to pin the OLD engine's time-based divisor curve: 0.01 on the
# first detent, then step/4 on a fast turn, step/16 on a slow one, ten detents
# per enum option, and an accumulator whose remainder was observable as
# state.tickAccum. All of that was a second implementation living alongside
# param_pages/movy_knob.mjs, and the two disagreed badly enough that a knob
# behaved differently depending on which screen you touched it from -- 79,917
# detents to cross a 20..20000 param in the overlay against 200 on the grid.
#
# knob_engine is now an adapter over the single model. The behaviour pinned
# below is deliberately expressed as PROPERTIES rather than as the old exact
# numbers, because the exact numbers were the thing that turned out to be
# arbitrary. What has to stay true is that the config shape these older call
# sites pass still reaches the model intact -- including the option STRINGS,
# whose loss would turn every Off/On switch into a four-detent control.

if ! command -v node >/dev/null 2>&1; then
  echo "node is required to run this test" >&2
  exit 1
fi

node -e '
import("./src/shared/knob_engine.mjs").then((m) => {
  const { knobInit, knobStep, perDetentStep,
          KNOB_TYPE_FLOAT, KNOB_TYPE_INT, KNOB_TYPE_ENUM } = m;
  const fail = (msg) => { console.log("FAIL " + msg); process.exit(1); };

  /* ---- ONE entry point ------------------------------------------------
   *
   * Not "two implementations that agree" -- that was an intermediate state
   * and it is still two places to look. knobStep is the only way to move a
   * knob, and the config-shaped knobTick / knobConfigFromMeta pair that the
   * older call sites used is gone rather than aliased.
   *
   * An alias would keep this test green while leaving a second name for the
   * next reader to wire something up to, which is how the two models drifted
   * apart in the first place. */
  for (const gone of ["knobTick", "knobConfigFromMeta", "movyKnobTick", "movyKnobInit"]) {
    if (m[gone] !== undefined)
      fail("knob_engine still exports `" + gone + "` -- one model means one " +
           "entry point, not an alias for the old one");
  }
  if (typeof knobStep !== "function") fail("knobStep is not exported");

  const CASES = [
    ["float 0..1 step .01",  { type: "float", min: 0,  max: 1,     step: 0.01  }],
    ["float 0..1 step .001", { type: "float", min: 0,  max: 1,     step: 0.001 }],
    ["float 20..20000",      { type: "float", min: 20, max: 20000, step: 1     }],
    ["int 0..127",           { type: "int",   min: 0,  max: 127,   step: 1     }],
    ["int 0..8",             { type: "int",   min: 0,  max: 8,     step: 1     }],
    ["int 0..20000",         { type: "int",   min: 0,  max: 20000, step: 1     }],
    ["switch Off/On",        { type: "enum",  min: 0,  max: 1, step: 1, options: ["Off", "On"] }],
    ["enum LP/HP",           { type: "enum",  min: 0,  max: 1, step: 1, options: ["LP", "HP"] }],
    ["enum 47",              { type: "enum",  min: 0,  max: 46, step: 1,
                               options: Array.from({ length: 47 }, (_, i) => "o" + i) }],
  ];
  const target = (p) => p.type === "enum" ? p.options.length - 1 : p.max;
  const sweep = (p, fine) => {
    const st = knobInit(p.min);
    let t = 0, n = 0;
    while (st.value < target(p) && n < 400000) { t += 10; knobStep(st, p, 1, t, fine); n++; }
    return n;
  };

  /* ---- a SWITCH must survive the config round trip -------------------
   *
   * The model tells a two-state boolean from an ordinary two-option enum by
   * the option STRINGS, and flips it on one detent. A config carrying only a
   * count would make every switch a four-detent control, which reads on the
   * device as a knob that does nothing. */
  {
    const sw = { type: "enum", min: 0, max: 1, step: 1, options: ["Off", "On"] };
    const st = knobInit(0);
    knobStep(st, sw, 1, 1000);
    if (st.value !== 1) fail("a switch did not flip on ONE detent: " + st.value);
  }
  /* ...and a config built by hand, with only a count, must NOT be mistaken
   * for a switch -- the names are unknown, so the ordinary gate applies. */
  {
    const st = knobInit(0);
    knobStep(st, { type: KNOB_TYPE_ENUM, min: 0, max: 1, step: 1 }, 1, 1000);
    if (st.value !== 0)
      fail("a count-only enum config flipped on one detent -- it was guessed to be a switch");
  }

  /* ---- shift-fine: a tenth of the step, and the GATE IS LIFTED --------
   *
   * Asserted as the two clauses of the rule rather than as "fine is slower",
   * because "slower" is false for a gated control and this test asserted it
   * wrongly twice: lifting the four-detent gate on an enum makes shift FASTER
   * while still being the precise gesture (one click, one option, instead of
   * four clicks per option and a partial turn doing nothing).
   *
   * Both clauses were broken before the unification: fine was float-only in
   * the adapter, so shift did nothing at all on an int or an enum, and in the
   * model a 1-unit int step times 0.1 rounded back to zero. */
  {
    /* Clause 2, everywhere: the FIRST detent under shift always moves. */
    for (const [name, p] of CASES) {
      const st = knobInit(p.min);
      knobStep(st, p, 1, 1000, true);
      if (st.value === p.min)
        fail(name + ": the first detent under shift moved nothing -- the gate " +
             "is still in the way, or the step rounded back to zero");
    }
    /* Clause 1, floats: a tenth, so strictly finer. */
    for (const [name, p] of CASES.filter(([, q]) => q.type === "float")) {
      const coarse = sweep(p, false), fine = sweep(p, true);
      if (fine <= coarse)
        fail(name + ": shift-fine on a float must be finer, got " + fine +
             " detents against a coarse " + coarse);
    }
    /* Clause 1, ints: a tenth of the coarse step, never below one whole unit.
     * Expected value comes from the model\u0027s own perDetentStep, not from a
     * recorded number, so the rule and the assertion cannot drift. */
    for (const [name, p] of CASES.filter(([, q]) => q.type === "int")) {
      const want = Math.max(1, Math.round(perDetentStep(p) * 0.1));
      const st = knobInit(p.min);
      knobStep(st, p, 1, 1000, true);
      if (st.value - p.min !== want)
        fail(name + ": one shift detent moved " + (st.value - p.min) +
             ", expected a tenth of the coarse step floored at 1, i.e. " + want);
    }
    /* Clause 1, enums: exactly one option. */
    for (const [name, p] of CASES.filter(([, q]) => q.type === "enum")) {
      const st = knobInit(0);
      knobStep(st, p, 1, 1000, true);
      if (st.value !== 1)
        fail(name + ": one shift detent moved " + st.value + " options, expected 1");
    }
  }

  /* ---- an int must be movable AT ALL under shift ---------------------
   *
   * The model shipped with fine = coarse * 0.1, which for a 1-unit int step
   * is 0.1 and rounds straight back to where it started: 400,000 detents
   * without crossing 0..127. */
  {
    const st = knobInit(0);
    let t = 0, moved = false;
    for (let i = 0; i < 50 && !moved; i++) {
      t += 10;
      knobStep(st, { type: "int", min: 0, max: 127, step: 1 }, 1, t, true);
      moved = st.value > 0;
    }
    if (!moved) fail("an int could not be moved with shift held in 50 detents");
  }

  /* ---- degenerate configs must not run away -------------------------- */
  {
    const st = knobInit(0);
    for (let i = 0; i < 3; i++)
      knobStep(st, { type: KNOB_TYPE_ENUM, min: 0, max: 0, step: 1, options: [] }, 1, 1000 + i * 10);
    if (st.value !== 0) fail("an enum with no options advanced: " + st.value);
  }
  {
    const st = knobInit(0.99);
    const v = knobStep(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 5, 1000);
    if (v !== 1) fail("float did not clamp at max: " + v);
  }
  {
    const st = knobInit(0.5);
    const before = st.value;
    if (knobStep(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 0, 1000) !== before)
      fail("a zero delta moved the value");
    if (knobStep(null, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 1, 1000) === undefined)
      fail("a null state threw instead of returning a value");
  }

  console.log("PASS knob_engine — one model, one entry point");
}).catch((e) => { console.log("FAIL import:", e); process.exit(1); });
'
