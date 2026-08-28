#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A boolean draws as a switch however its author spelled it.
#
# detectSwitch used to require an ENUM with two Off/On options. An `int` with
# min 0 and max 1 is the same control and never qualified, so it drew as a
# NUMBER -- the one widget that tells you nothing, because "1" does not say
# what the other state is or that there are only two. That is 61 parameters
# across 11 modules in the fleet: obxd declares 25 of them, dexed 8, and it is
# how ambiotica spells its Tempo Sync.
#
# The trap on the other side is float 0..1. That is a mix or an amount and the
# single most common continuous range in the fleet -- widening far enough to
# catch it would collapse hundreds of real dials into on/off. It is asserted
# explicitly for that reason.
#
# The picture and the FEEL must agree: knob_engine turns exactly what viz draws
# as a switch, on the same predicate rather than on a parallel copy of the
# rule. A control drawn as a switch but turned like a list (or the reverse) is
# the failure this shared predicate exists to prevent.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/viz.mjs"),
  import("./src/shared/knob_engine.mjs"),
  import("node:fs"),
]).then(([V, K, fs]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* ---- the rule ------------------------------------------------------- */
  const CASES = [
    ["enum Off/On",   { type: "enum", options: ["Off", "On"] },        true ],
    ["enum 0/1",      { type: "enum", options: ["0", "1"] },           true ],
    ["int 0..1",      { type: "int",  min: 0, max: 1 },                true ],
    ["float 0..1",    { type: "float", min: 0, max: 1 },               false],
    ["int 0..127",    { type: "int",  min: 0, max: 127 },              false],
    ["int 0..8",      { type: "int",  min: 0, max: 8 },                false],
    ["int 0..0",      { type: "int",  min: 0, max: 0 },                false],
    ["int 1..2",      { type: "int",  min: 1, max: 2 },                false],
    ["enum LP/HP",    { type: "enum", options: ["LP", "HP"] },         false],
    ["enum 3 opts",   { type: "enum", options: ["A", "B", "C"] },      false],
    ["nothing",       null,                                           false],
  ];
  for (const [name, meta, want] of CASES) {
    if (V.isBooleanMeta(meta) !== want)
      fail(name + ": isBooleanMeta returned " + V.isBooleanMeta(meta) + ", expected " + want);
  }
  console.log("  ok  an int 0..1 is a boolean; a float 0..1 is a dial");

  /* ---- the feel follows the picture ------------------------------------ */
  for (const [name, meta, isSwitch] of CASES) {
    if (!meta) continue;
    const st = K.knobInit(0);
    K.knobStep(st, { min: 0, max: 1, step: 1, ...meta }, 1, 1000);
    const flipped = st.value === 1;
    if (isSwitch && !flipped)
      fail(name + " draws as a switch but did not flip on ONE detent (got " + st.value + ")");
  }
  /* ...and a switch reaches the other value on ONE detent rather than
   * accumulating, which is what an int would have done on the numeric path.
   *
   * It no longer follows the DIRECTION turned: with two values there is
   * nowhere to go but the other one, so either direction toggles and one
   * flick is one flip. That rule and its latch live in
   * tests/host/test_two_way_knob_toggle.sh; what matters HERE is only that
   * what is drawn as a switch does not fall through to the numeric path. */
  {
    const st = K.knobInit(1);
    K.knobStep(st, { type: "int", min: 0, max: 1 }, -1, 1000);
    if (st.value !== 0) fail("an int boolean did not move on one detent");
  }
  console.log("  ok  what is drawn as a switch is turned as a switch");

  /* ---- the fleet ------------------------------------------------------- */
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  let intBools = 0, mods = new Set();
  for (const m of fx.modules) {
    for (const p of (m.chain_params || [])) {
      if (p.type === "int" && p.min === 0 && p.max === 1 && !p.options) {
        intBools++; mods.add(m.id);
        if (!V.isBooleanMeta(p))
          fail(m.id + "." + p.key + " is an int 0..1 and was not recognised as a boolean");
      }
    }
  }
  if (intBools < 40)
    fail("only " + intBools + " int-boolean params found in the fleet — the fixture " +
         "changed shape, so this test is no longer measuring what it claims");
  console.log("  ok  " + intBools + " fleet params across " + mods.size +
              " modules draw as switches instead of numbers");

  /* A float 0..1 must NOT have been swept up. This is the expensive mistake:
   * mix, amount, drive and depth are all this shape. */
  let floats = 0;
  for (const m of fx.modules) {
    for (const p of (m.chain_params || [])) {
      if (p.type === "float" && p.min === 0 && p.max === 1) {
        floats++;
        if (V.isBooleanMeta(p))
          fail(m.id + "." + p.key + " is a float 0..1 and was flattened into a switch");
      }
    }
  }
  console.log("  ok  " + floats + " float 0..1 params stayed dials");

  console.log("PASS: a boolean draws as a switch however it is spelled");
});
'
