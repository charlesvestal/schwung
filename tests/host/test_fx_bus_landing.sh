#!/usr/bin/env bash
#
# WHERE enterFxBus PUTS THE CURSOR.
#
# Two bugs met here. The FX buses never remembered where they were left -- the
# slot chain has had lastChainComponent[] all along -- so every entry started at
# the head of the row. And the head of the row is now a DOOR (Send A on the
# master, the way back on a send), so "the start" became a way out of the screen
# you had just opened.
#
# The second half is a sequencing bug and is why it landed on A specifically.
# enterFxBus calls invalidateMasterFxConfig(), whose reload is LAZY -- drawMasterFx
# does it on the next diagram frame -- so asking which row holds FX 1 read an
# EMPTY chain, got -1, and left the selection on row 0. Entering Master FX with a
# full chain landed on Send A. Reported from hardware.
#
# The harness below models that lazy reload: masterFxChainComponents() answers
# nothing until something calls reload(). A version of enterFxBus that does not
# reload therefore fails here exactly as it failed on the device.
set -euo pipefail

cd "$(dirname "$0")/../.."
UI="src/shadow/shadow_ui.js"
[ -f "$UI" ] || { echo "FAIL: cannot find $UI"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.argv[1], "utf8");
let bad = 0;
const fail = (m) => { console.log("FAIL: " + m); bad = 1; };
const ok = (m) => console.log("  ok  " + m);
const grab = (n) => {
  const m = src.match(new RegExp("^function " + n + "\\([^]*?^}", "m"));
  if (!m) { fail("could not lift " + n + "()"); process.exit(1); }
  return m[0];
};

/* The master row as the device builds it: two send doors, two modules, + and
   Settings. Positions and rows deliberately differ, which is the whole reason
   masterFxRowOf exists. */
const ROWS = [
  { kind: "sendbus", key: "sendbus1" }, { kind: "sendbus", key: "sendbus2" },
  { kind: "module", key: "fx1", index: 0 }, { kind: "module", key: "fx2", index: 1 },
  { kind: "add", key: "add_fx" }, { kind: "settings", key: "settings" },
];

function land(remembered) {
  const body = [
    "const FX_BUSES=[{id:\"master\",send:-1,short:\"MFX\",prefix:\"master_fx:\",hasPresets:true,busLevelKeys:[]}];",
    "let currentFxBusIndex=0;",
    "const lastFxBusComponent=[]; lastFxBusComponent[0]=" + JSON.stringify(remembered) + ";",
    "let selectedMasterFxComponent=0, masterFxChainLength=-1;",
    "let inMasterFxSettingsMenu,editingMasterFxSetting,inMasterPresetPicker,",
    "    selectingMasterFxModule,currentMasterPresetName;",
    "let loaded=true, reloads=0;",
    "const ROWS=" + JSON.stringify(ROWS) + ";",
    "function fxBusSwap(i){currentFxBusIndex=i;}",
    "function fxBusIsMaster(){return true;}",
    /* THE LAZY MIRROR, modelled: invalidate empties it and only reload refills. */
    "function invalidateMasterFxConfig(){loaded=false;}",
    "function masterFxChainComponents(){return loaded?ROWS:[];}",
    "function masterFxRowOf(p){const r=masterFxChainComponents();",
    "  for(let i=0;i<r.length;i++) if(r[i].kind===\"module\"&&r[i].index===p) return i; return -1;}",
    "const MASTER_CHAIN_TARGET={reload(){loaded=true;reloads++;}};",
    "function isBusDoor(k){return k===\"sendbus\"||k===\"busback\";}",
    "function fxBusSummary(){return \"\";} function fxBusReturnNow(){return 0;}",
    "let fxBusSummaries=[],fxBusReturns=[],selectedFxBusRow=0;",
    "function enterMasterFxSettings(){}",
    grab("enterFxBus"),
    "enterFxBus(0);",
    "return { sel:selectedMasterFxComponent, reloads,",
    "         key:(masterFxChainComponents()[selectedMasterFxComponent]||{}).key };",
  ].join("\n");
  return new Function(body)();
}

const fresh = land(undefined);
if (fresh.reloads < 1) {
  fail("enterFxBus never reloaded the chain. The mirror it just invalidated is "
       + "refilled LAZILY, so every question it asks about the row reads an empty "
       + "chain -- which is how a full Master FX landed on the Send A box.");
}
if (fresh.key !== "fx1") {
  fail("with nothing remembered it landed on " + JSON.stringify(fresh.key)
       + ", expected fx1 -- the first MODULE, not the head of the row");
}
ok("a fresh entry reloads and lands on the first module");

const kept = land(3);
if (kept.key !== "fx2") {
  fail("a remembered selection landed on " + JSON.stringify(kept.key)
       + ", expected fx2 -- entering a bus must return you to where you were");
}
ok("a remembered selection is restored");

/* NEVER LAND ON A DOOR, even when that is where the cursor was left: it is a way
   out of the screen you just opened, the complaint defaultChainComponent already
   answers for the `+`. */
for (const [at, what] of [[0, "Send A"], [1, "Send B"]]) {
  const d = land(at);
  if (d.key !== "fx1") {
    fail("a cursor left on " + what + " landed there again (" + d.key + ")");
  }
}
ok("a cursor left on a head door falls back to the first module");

/* The row is as long as the chain, so a remembered index can outlive it. */
for (const bogus of [99, -3, null, "fx1"]) {
  const b = land(bogus);
  if (b.key !== "fx1") {
    fail("a stale remembered value " + JSON.stringify(bogus) + " landed on "
         + JSON.stringify(b.key) + " instead of falling back");
  }
}
ok("a stale or out-of-range memory falls back rather than pointing at nothing");

/* And the memory has to be WRITTEN, or every entry is a fresh one. */
if (!/lastFxBusComponent\[currentFxBusIndex\] = selectedMasterFxComponent/.test(src)) {
  fail("nothing records the selection into lastFxBusComponent -- it will always "
       + "be empty, so the restore above can never fire on the device");
}
ok("the jog records the selection per bus");

if (bad) process.exit(1);
console.log("PASS");
' "$UI"
