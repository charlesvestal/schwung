#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A knob must be able to CROSS its parameter.
#
# Reported from the device: knobs "super slow in the overlay" on the Master FX
# chain. The overlay (the hierarchy list editor) turns through knob_engine,
# which stepped by the module's DECLARED step divided by 4-16. The knob grid
# turns through movy_knob, which normalizes to 1% of the param's own range.
# Same parameter, two engines, wildly different:
#
#   float 20..20000 step 1    overlay 79,917 detents    grid 200
#
# A declared `step` is a statement about PRECISION, not about sweep. Taking it
# literally makes any module that declares a fine step on a wide range
# untouchable from the overlay.
#
# Pinned here as a sweep BUDGET rather than as a step size: the number that
# matters is how much wrist it takes to cross the thing, and that is what was
# wrong. Expressed against the Movy grid's own cost for the same param, so the
# two engines cannot drift apart again without failing.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/knob_engine.mjs"),
  import("./src/shared/param_pages/movy_knob.mjs"),
]).then(([E, V]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* Detents to cross the whole range at a fast turn (10ms apart, the fast
   * band of the divisor curve). */
  const sweep = (m) => {
    const cfg = { type: m.type, min: m.min, max: m.max, step: m.step };
    const st = E.knobInit(m.min);
    let t = 0, n = 0;
    while (st.value < m.max && n < 500000) { t += 10; E.knobTick(st, cfg, 1, t); n++; }
    return n;
  };
  /* What the same param costs on the knob grid. */
  const gridSweep = (m) => Math.ceil((m.max - m.min) / (V.perDetentStep(m) / V.detentsPerStep(m)));

  const CASES = [
    ["float 0..1 step .01",    { type: "float", min: 0,   max: 1,     step: 0.01  }],
    ["float 0..1 step .001",   { type: "float", min: 0,   max: 1,     step: 0.001 }],
    ["float 20..20000 step 1", { type: "float", min: 20,  max: 20000, step: 1     }],
    ["float -12..12 step .01", { type: "float", min: -12, max: 12,    step: 0.01  }],
    ["float 0..100 step .5",   { type: "float", min: 0,   max: 100,   step: 0.5   }],
  ];

  /* Within this multiple of the grid, the engine is a relative rather than a
   * stranger. The two acceleration curves genuinely differ, so this is not an
   * equality -- but 400x, which is what shipped, is not a difference in feel,
   * it is a different feature. */
  const TOLERANCE = 3;
  for (const [name, m] of CASES) {
    const o = sweep(m), g = gridSweep(m);
    if (o > g * TOLERANCE)
      fail(name + " takes " + o + " detents in the overlay against " + g +
           " on the grid -- more than " + TOLERANCE + "x, which is not a " +
           "difference in feel");
  }

  /* The floor must not COARSEN a param that already declares a coarse step.
   * A replacement rather than a floor would have quietly retuned most of the
   * fleet, whose params are 0..1 with a 0.01 step -- exactly 1% already. */
  for (const [name, m] of CASES) {
    const declared = m.step;
    if (E.floatStep({ ...m }) < declared)
      fail(name + " had its declared step REDUCED to " + E.floatStep({ ...m }) +
           " -- the normalisation is a floor, never a replacement");
  }
  {
    const coarse = { type: "float", min: 0, max: 1, step: 0.25 };
    if (E.floatStep(coarse) !== 0.25)
      fail("a param declaring a step coarser than 1% did not keep it: " +
           E.floatStep(coarse));
  }

  /* Fine control must survive. The slow end of the divisor curve is what you
   * reach for to place a value, and it has to stay meaningfully finer than
   * the fast end or the floor has eaten the precision entirely. */
  {
    const m = { type: "float", min: 20, max: 20000, step: 1 };
    const one = (gapMs) => {
      const st = E.knobInit(1000);
      let t = 1000;
      /* Two ticks: the first is the cold-start "click" (divisor 1). */
      E.knobTick(st, m, 1, t += gapMs);
      const before = st.value;
      E.knobTick(st, m, 1, t += gapMs);
      return st.value - before;
    };
    const slow = one(400), fast = one(10);
    if (!(slow < fast / 3))
      fail("a deliberate turn moves " + slow + " against " + fast + " for a fast " +
           "one -- fine control is gone");
  }

  /* Shift-held FINE must remain meaningfully finer than coarse.
   *
   * The floor broke this the first time: the caller pre-divided the declared
   * step by 10 and the floor put it straight back, so fine and coarse came
   * out 1.2x apart. It is now an argument of the engine itself, and pinned here as
   * well as in test_param_pages_controller.sh, because the bug was in the
   * interaction between the two and either file alone can look correct. */
  {
    const m = { type: "float", min: 20, max: 20000, step: 1 };
    const coarse = E.floatStep(m), fine = E.floatStep(m, true);
    if (!(fine <= coarse / 5))
      fail("fine adjust is " + fine + " against a coarse " + coarse +
           " -- the range floor has eaten it");
  }

  /* Degenerate ranges must not produce NaN or Infinity into a param write. */
  for (const bad of [{ type: "float", min: 0, max: 0, step: 0.01 },
                     { type: "float", min: 5, max: 1, step: 0.01 },
                     { type: "float", min: 0, max: 1, step: 0 }]) {
    const v = E.floatStep(bad);
    if (!isFinite(v) || v <= 0)
      fail("a degenerate range produced a step of " + v);
  }

  console.log("  ok  every float case within " + TOLERANCE + "x of the grid:");
  for (const [name, m] of CASES)
    console.log("      " + name.padEnd(24) + String(sweep(m)).padStart(6) +
                " overlay /" + String(gridSweep(m)).padStart(5) + " grid");
  console.log("  ok  a coarser declared step is kept; the floor never reduces one");
  console.log("  ok  fine control survives: slow end of the curve, and shift-fine");
  console.log("PASS: a knob can cross its parameter from the overlay");
});
'
