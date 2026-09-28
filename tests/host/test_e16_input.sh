#!/usr/bin/env bash
# The E16's relative encoding is the same two's complement relative_cc.h reads,
# and that header exists because chain_midi.c decoded only +/-1 and lost every
# fast turn (#402). A JS copy that repeats that bug would be invisible: the
# knobs would simply feel slow.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { decode } from "./src/shared/e16_input.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };

eq("cw one",   decode([0xB0, 1, 0x01]), { type: "turn", enc: 0, ticks: 1 });
eq("cw eight", decode([0xB0, 1, 0x08]), { type: "turn", enc: 0, ticks: 8 });
eq("ccw one",  decode([0xB0, 1, 0x7F]), { type: "turn", enc: 0, ticks: -1 });
eq("ccw eight",decode([0xB0, 1, 0x78]), { type: "turn", enc: 0, ticks: -8 });
eq("enc 16",   decode([0xB0, 16, 0x01]),{ type: "turn", enc: 15, ticks: 1 });
eq("centre is no movement", decode([0xB0, 1, 0x40]), null);

eq("push",    decode([0x90, 0, 0x7F]), { type: "push", enc: 0 });
eq("release", decode([0x80, 0, 0x00]), { type: "release", enc: 0 });
eq("note-on velocity 0 is a release",
   decode([0x90, 3, 0x00]), { type: "release", enc: 3 });
eq("shift down", decode([0x90, 16, 0x7F]), { type: "shift", down: true });
eq("shift up",   decode([0x80, 16, 0x00]), { type: "shift", down: false });

eq("wrong channel ignored", decode([0xB1, 1, 0x01]), null);
eq("cc 0 ignored",          decode([0xB0, 0, 0x01]), null);
eq("cc 17 ignored",         decode([0xB0, 17, 0x01]), null);
eq("note 17 ignored",       decode([0x90, 17, 0x7F]), null);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
