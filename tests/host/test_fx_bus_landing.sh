#!/usr/bin/env bash
#
# WHERE THE CURSOR LANDS when an FX bus editor opens.
#
# Reported twice from hardware as "opening MFX jumps to Send A", the second time
# with a module loaded -- which is what ruled out the empty-chain explanation and
# found the real cause.
#
# enterFxBus works the landing out, and its LAST statement is
# enterMasterFxSettings(), which loads the chain and then assigned
# `selectedMasterFxComponent = 0` unconditionally. So the answer was computed and
# thrown away one call later. Row 0 used to BE FX 1, so the hardcoded 0 was
# invisible until the row grew a door at its head.
#
# THE FIRST VERSION OF THIS TEST COULD NOT HAVE CAUGHT THAT: it drove enterFxBus
# with enterMasterFxSettings stubbed to a no-op, so it asserted a value the real
# code overwrites. It tests the RESOLVER now, and pins separately that the mjs
# calls it, does not assign a literal, and does so AFTER the load -- before the
# load there are no module rows and the resolver falls through to its last
# resort, which is the same bug wearing a different number.
set -euo pipefail

cd "$(dirname "$0")/../.."
UI="src/shadow/shadow_ui.js"
MFX="src/shadow/shadow_ui_master_fx.mjs"
[ -f "$UI" ] && [ -f "$MFX" ] || { echo "FAIL: missing sources"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.argv[1], "utf8");
const mfx = readFileSync(process.argv[2], "utf8");
let bad = 0;
const fail = (m) => { console.log("FAIL: " + m); bad = 1; };
const ok = (m) => console.log("  ok  " + m);
const grab = (n) => {
  const m = src.match(new RegExp("^function " + n + "\\([^]*?^}", "m"));
  if (!m) { fail("could not lift " + n + "()"); process.exit(1); }
  return m[0];
};

/* Rows as the device builds them. Row and FX position deliberately differ --
   that gap is the whole reason masterFxRowOf exists. */
const FULL = [
  { kind: "sendbus", key: "sendbus1" }, { kind: "sendbus", key: "sendbus2" },
  { kind: "module", key: "fx1", index: 0 }, { kind: "module", key: "fx2", index: 1 },
  { kind: "add", key: "add_fx" }, { kind: "settings", key: "settings" },
];
const EMPTY = [
  { kind: "sendbus", key: "sendbus1" }, { kind: "sendbus", key: "sendbus2" },
  { kind: "add", key: "add_fx" }, { kind: "settings", key: "settings" },
];

function resolve(rows, remembered) {
  const body = [
    "let currentFxBusIndex = 0;",
    "const lastFxBusComponent = []; lastFxBusComponent[0] = " + JSON.stringify(remembered) + ";",
    "const ROWS = " + JSON.stringify(rows) + ";",
    "function masterFxChainComponents() { return ROWS; }",
    "function isBusDoor(k) { return k === \"sendbus\" || k === \"busback\"; }",
    grab("masterFxPositionOf"), grab("masterFxRowOf"), grab("resolveFxBusLanding"),
    "const at = resolveFxBusLanding();",
    "return { at, key: (ROWS[at] || {}).key };",
  ].join("\n");
  return new Function(body)();
}

/* Nothing remembered: the first MODULE, not the head of the row. */
if (resolve(FULL, undefined).key !== "fx1") {
  fail("a fresh entry landed on " + resolve(FULL, undefined).key + ", expected fx1");
}
ok("a fresh entry lands on the first module");

if (resolve(FULL, 3).key !== "fx2") {
  fail("a remembered selection landed on " + resolve(FULL, 3).key + ", expected fx2");
}
ok("a remembered selection is restored");

/* NEVER a head door, even when that is where the cursor was left. */
for (const [at, what] of [[0, "Send A"], [1, "Send B"]]) {
  if (resolve(FULL, at).key !== "fx1") {
    fail("a cursor left on " + what + " landed there again");
  }
}
ok("a cursor left on a head door falls back to the first module");

/* AN EMPTY CHAIN LANDS ON THE `+`, not on row 0 -- row 0 is Send A, so falling
   back to 0 is the same bug in its quietest form. */
const e = resolve(EMPTY, undefined);
if (e.key !== "add_fx") {
  fail("an empty chain landed on " + JSON.stringify(e.key) + ", expected the `+` "
       + "box. It is reached as the first NON-DOOR row; falling back to 0 lands "
       + "on Send A, which is the original bug in its quietest form");
}
if (resolve(EMPTY, 0).key !== "add_fx") fail("an empty chain honoured a door memory");
ok("an empty chain lands on the `+` box");

for (const bogus of [99, -3, null, "fx1"]) {
  if (resolve(FULL, bogus).key !== "fx1") {
    fail("a stale memory " + JSON.stringify(bogus) + " did not fall back");
  }
}
ok("a stale or out-of-range memory falls back");

/* ---- the WIRING, which the resolver alone cannot show ------------------ */
const body = mfx.match(/export function enterMasterFxSettings\(\)[^]*?^}/m);
if (!body) fail("could not find enterMasterFxSettings in shadow_ui_master_fx.mjs");
else {
  const b = body[0];
  if (/selectedMasterFxComponent\s*=\s*0\s*;/.test(b)) {
    fail("enterMasterFxSettings assigns selectedMasterFxComponent = 0. It is the "
         + "LAST call of enterFxBus, so it throws away the landing that was just "
         + "worked out -- and row 0 is the Send A box.");
  }
  if (!/resolveFxBusLanding/.test(b)) {
    fail("enterMasterFxSettings does not call resolveFxBusLanding()");
  }
  /* ORDER: the component list is derived from the chain, so resolving before
     the load sees no modules and falls through to the last resort. */
  const load = b.indexOf("loadMasterFxChainConfig()");
  const res = b.indexOf("resolveFxBusLanding");
  if (load < 0 || res < 0 || res < load) {
    fail("the landing is resolved BEFORE loadMasterFxChainConfig() -- there are "
         + "no module rows yet, so it can only fall through");
  }
  /* The announcement has to name the box actually landed on. */
  if (/getMasterFxSlotModule\(0\)/.test(b)) {
    fail("the entry announcement still reads position 0 rather than the box "
         + "landed on -- it will speak a different module from the one selected");
  }
  ok("enterMasterFxSettings resolves after the load and announces what it landed on");
}

if (bad) process.exit(1);
console.log("PASS");
' "$UI" "$MFX"
