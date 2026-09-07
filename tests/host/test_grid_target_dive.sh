#!/usr/bin/env bash
#
# CLICKING "Targ" ON A ROUTE PAGE OPENS THE PICKER.
#
# The Targ cell is the one divable thing on the synthesised slot / Master FX
# contracts, and openParamEditorFromGrid recognises it by matching the full grid
# key against a regex. When the slot keys were renamed lfoN: -> modN: that regex
# was left behind, so the door drew, took the click, and did nothing at all --
# no error, no log line, no visible difference from a cell that is merely busy.
# Reported from the device after everything else had gone green.
#
# NOTHING ELSE COULD SEE IT. The count-drift pin greps for two-route literals,
# and this pattern lives INSIDE A REGEX LITERAL -- a known blind spot for the
# grep-based pins in this directory. So this test EVALUATES the matcher against
# real keys rather than reading the source for a shape.
set -euo pipefail

cd "$(dirname "$0")/../.."
UI="src/shadow/shadow_ui.js"
[ -f "$UI" ] || { echo "FAIL: missing $UI"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.argv[1], "utf8");
let bad = 0;
const fail = (m) => { console.log("FAIL: " + m); bad = 1; };
const ok = (m) => console.log("  ok  " + m);

/* Lift the two regexes out of openParamEditorFromGrid and run them. They are
   read from the source, never restated here -- a copy would pass while the
   shipped one rotted, which is the whole failure being pinned. */
const fn = src.match(/^function openParamEditorFromGrid\([^]*?^}/m);
if (!fn) { console.log("FAIL: could not lift openParamEditorFromGrid"); process.exit(1); }
const body = fn[0];

const grab = (label, re) => {
  const m = body.match(re);
  if (!m) { fail("could not find the " + label + " target matcher"); return null; }
  /* m[1] is the literal INCLUDING its slashes -- strip them, or new RegExp
     treats the leading slash as a character to match and nothing ever does. */
  const body_ = m[1].slice(1, -1);
  console.log("      (" + label + " matcher: /" + body_ + "/)");
  return new RegExp(body_);
};
const master = grab("Master FX",
  /\?\s*(\/\^master_settings:master_fx:[^/]+\/)\.exec/);
const slot = grab("slot",
  /:\s*(\/\^slot:[^/]+\/)\.exec/);

if (slot) {
  /* EVERY route, not just the first -- a regex frozen at [12] passes on mod1
     and mod2 and silently refuses the other six. */
  for (let n = 1; n <= 8; n++) {
    const key = "slot:mod" + n + ":target";
    const m = slot.exec(key);
    if (!m) fail(key + " does not open the picker -- the Targ cell is dead on that page");
    else if (m[1] !== String(n)) fail(key + " captured route " + m[1] + ", expected " + n);
  }
  if (!bad) ok("all eight slot routes open the target picker, each with its own index");

  /* And it must not match things that are not a route target. */
  for (const no of ["slot:mod9:target", "slot:mod1:depth", "slot:volume",
                    "slot:lfo1:target", "slot:mod0:target"]) {
    if (slot.exec(no)) fail(slot + " also matches " + no + ", which is not a slot route target");
  }
  ok("it matches nothing else -- not mod9, not depth, not the legacy spelling");
}

if (master) {
  /* Master FX keeps the lfoN: stem: its params are parsed in the SHIM by a
     literal strncmp on "lfo1:"/"lfo2:". If this ever becomes modN:, every
     Master FX LFO control writes a key nothing reads. */
  for (let n = 1; n <= 2; n++) {
    const key = "master_settings:master_fx:lfo" + n + ":target";
    if (!master.exec(key)) fail(key + " does not open the picker");
  }
  if (master.exec("master_settings:master_fx:mod1:target")) {
    fail("the Master FX matcher accepts modN: -- the shim only parses lfoN:");
  }
  if (!bad) ok("Master FX opens on lfoN: and refuses modN:");
}

if (bad) process.exit(1);
console.log("PASS: the Targ cell opens the target picker on every route page");
' "$UI"
