#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A TWO-OPTION ENUM MAY ASK TO BE STEPPED RATHER THAN TOGGLED.
#
# The toggle is the default and stays the default: a boxed two-way shows a
# state and not a direction, so a direction-absolute turn has a dead half for
# anyone who cannot see which way is on. A host that DOES know the pair is
# ordered -- a note mode, a pad layout -- can say so per param.
#
# Both paths are asserted. The default one is the one that matters.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the turn:absolute test" >&2
  exit 1
fi

node --input-type=module -e '
import { knobInit, knobStep } from "./src/shared/knob_engine.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const choice = { type: "enum", kind: "enum", options: ["Chromatic", "In Key"], min: 0, max: 1, step: 1 };
const declared = { ...choice, turn: "absolute" };

/* DEFAULT: each gesture flips, whichever way it went. */
let st = knobInit(0);
let v = 0;
for (let i = 0; i < 4; i++) v = knobStep(st, choice, 1, 1000 + i * 1000);
if (v !== 0) bad("the default stopped toggling: four clockwise gestures landed on " + v);

/* DECLARED: direction decides, and a continued turn stays put. */
st = knobInit(0);
for (let i = 0; i < 4; i++) v = knobStep(st, declared, 1, 1000 + i * 1000);
if (v !== 1) bad("turn:absolute clockwise landed on " + v + ", want 1");
for (let i = 0; i < 4; i++) v = knobStep(st, declared, -1, 9000 + i * 1000);
if (v !== 0) bad("turn:absolute counter-clockwise landed on " + v + ", want 0");
/* ...including inside one fast flick, where the toggle latch would swallow it. */
st = knobInit(1);
v = knobStep(st, declared, -1, 1000);
if (v !== 0) bad("turn:absolute ignored a counter-clockwise detent from 1");

/* A TRIGGER stays out: turn:absolute must not make a write-only pair steppable. */
const trig = { ...declared, writeOnly: true };
st = knobInit(0);
v = knobStep(st, trig, 1, 1000);
if (v === 1 && knobStep(st, trig, 1, 5000) === 1)
    bad("a trigger took the turn:absolute rule");

/* A SWITCH IS UNAFFECTED: it was already direction-absolute. */
const sw = { type: "enum", kind: "enum", options: ["Off", "On"], min: 0, max: 1, step: 1 };
st = knobInit(0);
v = knobStep(st, sw, 1, 1000);
if (v !== 1) bad("an Off/On switch stopped being direction-absolute");

if (fail) process.exit(1);
console.log("PASS: turn:absolute steps by direction, and the default still toggles");
'
