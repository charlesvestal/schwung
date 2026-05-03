#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then
  echo "node is required to run this test" >&2
  exit 1
fi

node -e '
import("./src/shared/knob_engine.mjs").then((m) => {
  const { knobInit, knobTick, KNOB_TYPE_FLOAT, KNOB_TYPE_INT, KNOB_TYPE_ENUM } = m;

  // Float: first tick has no prior — divisor=1 path, step=0.01
  let st = knobInit(0.5);
  let v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 1, 1000);
  if (Math.abs(v - 0.51) > 1e-6) { console.log("FAIL float first tick:", v); process.exit(1); }

  // Float: fast tick (<50ms gap) → divisor 4
  v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 1, 1010);
  // 0.51 + 0.01/4 = 0.5125
  if (Math.abs(v - 0.5125) > 1e-6) { console.log("FAIL float fast:", v); process.exit(1); }

  // Float clamps at max
  st = knobInit(0.99);
  v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 5, 1000);
  if (v !== 1) { console.log("FAIL clamp max:", v); process.exit(1); }

  // Int: slow turn (>150ms gap) → divisor 16, accum until threshold
  st = knobInit(0);
  st.lastTickMs = 800;
  for (let i = 0; i < 15; i++) knobTick(st, { type: KNOB_TYPE_INT, min: 0, max: 100, step: 1 }, 1, 1000 + i * 200);
  if (st.value !== 0) { console.log("FAIL int slow accum (15 ticks):", st.value); process.exit(1); }
  knobTick(st, { type: KNOB_TYPE_INT, min: 0, max: 100, step: 1 }, 1, 1000 + 15 * 200);
  if (st.value !== 1) { console.log("FAIL int slow accum (16 ticks):", st.value); process.exit(1); }

  // Enum: 47 options spread over ~800 ticks → ~17 ticks per option
  st = knobInit(0);
  st.lastTickMs = 990;
  const enumCfg = { type: KNOB_TYPE_ENUM, min: 0, max: 46, step: 1, enumCount: 47 };
  // fast turns (10ms apart) → 800/47 ≈ 17 (clamped to [2,40])
  for (let i = 0; i < 16; i++) knobTick(st, enumCfg, 1, 1000 + i * 10);
  if (st.value !== 0) { console.log("FAIL enum 16 fast ticks:", st.value); process.exit(1); }
  knobTick(st, enumCfg, 1, 1000 + 16 * 10);
  if (st.value !== 1) { console.log("FAIL enum 17 fast ticks:", st.value); process.exit(1); }

  // Same-ms ticks should NOT use cold-start divisor=1 (regression: was bypassing acceleration)
  st = knobInit(0.5);
  knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.1 }, 1, 1000); // first → divisor 1, +0.1
  v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.1 }, 1, 1000); // same ms → fast bucket, +0.025
  if (Math.abs(v - 0.625) > 1e-6) { console.log("FAIL same-ms fast bucket:", v); process.exit(1); }

  // enumCount=0 should not advance (was: ran unbounded)
  st = knobInit(0);
  knobTick(st, { type: KNOB_TYPE_ENUM, min: 0, max: 0, step: 1, enumCount: 0 }, 1, 1000);
  knobTick(st, { type: KNOB_TYPE_ENUM, min: 0, max: 0, step: 1, enumCount: 0 }, 1, 1010);
  knobTick(st, { type: KNOB_TYPE_ENUM, min: 0, max: 0, step: 1, enumCount: 0 }, 1, 1020);
  if (st.value !== 0) { console.log("FAIL enum count=0 advance:", st.value); process.exit(1); }

  // Staleness reset: long gap should behave like cold start (divisor=1)
  // First tick at t=1000 advances by step (cold start path).
  // Second tick at t=10_000 (gap 9 seconds) should be treated as cold start again, not divisor=16.
  st = knobInit(0.5);
  knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 1, 1000); // → 0.51
  v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 1, 10000); // gap=9000 > 2000 → cold start, +0.01
  if (Math.abs(v - 0.52) > 1e-6) { console.log("FAIL staleness reset:", v); process.exit(1); }

  // Int batched delta: delta=8 with divisor=4 should emit 2 steps
  st = knobInit(0);
  st.lastTickMs = 800; // seed so first loop tick has delta=200ms → divisor=16... actually want fast turn here
  st.lastTickMs = 990; // 10ms ago → delta=10ms < 50ms → divisor=4
  knobTick(st, { type: KNOB_TYPE_INT, min: 0, max: 100, step: 1 }, 8, 1000);
  if (st.value !== 2) { console.log("FAIL int batched delta=8:", st.value); process.exit(1); }
  // Now delta=10 with divisor=4 should emit 2 steps and keep remainder=2
  st = knobInit(0);
  st.lastTickMs = 990;
  knobTick(st, { type: KNOB_TYPE_INT, min: 0, max: 100, step: 1 }, 10, 1000);
  if (st.value !== 2 || st.tickAccum !== 2) {
      console.log("FAIL int batched delta=10:", st.value, "accum:", st.tickAccum); process.exit(1);
  }

  // Enum batched delta: delta=34 with enumDivisor=17 should emit 2 steps
  st = knobInit(0);
  st.lastTickMs = 990;
  knobTick(st, { type: KNOB_TYPE_ENUM, min: 0, max: 46, step: 1, enumCount: 47 }, 34, 1000);
  if (st.value !== 2) { console.log("FAIL enum batched delta=34:", st.value); process.exit(1); }

  console.log("PASS knob_engine");
}).catch((e) => { console.log("FAIL import:", e); process.exit(1); });
'
