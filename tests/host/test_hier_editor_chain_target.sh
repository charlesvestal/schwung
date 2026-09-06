#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE LIST EDITOR ASKS THE CHAIN TARGET, BECAUSE A BOOLEAN CANNOT NAME THREE
# CHAINS.
#
# `hierEditorIsMasterFx` is a two-way, and the shape
#
#     hierEditorIsMasterFx ? getMasterFxX(hierEditorMasterFxSlot)
#                          : getComponentX(hierEditorSlot, hierEditorComponent)
#
# was written at SEVEN sites while there were two chains. A slot BUS is a third,
# and it falls into the else-arm of every one of them -- where slotChainTarget's
# key rule answers null for "bus1:fx2", `chain_params` comes back `[]`, and an
# empty chain_params is precisely what makes the editor invent a
# `float 0..1 step 0.01` knob for every parameter and write 0.058750 into an
# enum. It does not recover on its own: the draw path's "re-fetch if empty"
# calls the same broken accessor.
#
# It lands on the screen-reader users specifically -- paramPagesEnabled() is
# false only when the screen reader is on, so the LIST path IS that path.
#
# So the branch is resolved ONCE, in hierEditorChainAt(), and the sites ask the
# target. This test pins that in two ways, and the second is the one that
# survives an EIGHTH site being added:
#
#   1. per site: each named function reaches the shared accessor
#   2. globally: the four accessors this replaced are never again handed the
#      editor's own state. A new site can only be written by spelling one of
#      them, so it fails here on the day it is written rather than on the day
#      somebody loads a module on a bus.
#
# Read out of source: these are dispatch decisions in a 25k-line file full of
# host globals, and the lift trick cannot isolate a switch case.

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };
const ok = (m) => console.log("  ok  " + m);

const ui = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const decomment = (s) => s.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
const bare = decomment(ui);
const fn = (name) => {
  const at = bare.indexOf("\nfunction " + name + "(");
  if (at < 0) { fail(name + "() is gone"); return ""; }
  const end = bare.indexOf("\n}\n", at);
  return bare.slice(at, end + 2);
};

/* ---- 1. the resolver exists, and it has THREE arms --------------------- */
{
  const at = fn("hierEditorChainAt");
  if (at.indexOf("MASTER_CHAIN_TARGET") < 0)
    fail("hierEditorChainAt has no Master FX arm");
  else if (at.indexOf("parseBusComponentKey") < 0 || at.indexOf("busChainTarget") < 0)
    fail("hierEditorChainAt has no BUS arm -- a bus insert resolves to " +
         "slotChainTarget, whose key rule answers null for \"bus1:fx2\"");
  else if (at.indexOf("slotChainTarget") < 0)
    fail("hierEditorChainAt has no slot-chain arm");
  else ok("hierEditorChainAt names all three chains");

  for (const [name, callee] of [["hierEditorChainParamsNow", "chainTargetChainParams"],
                                ["hierEditorHierarchyNow", "chainTargetHierarchy"]]) {
    const body = fn(name);
    if (body.indexOf("hierEditorChainAt()") < 0 || body.indexOf(callee) < 0)
      fail(name + "() does not resolve through hierEditorChainAt() + " + callee);
    else ok(name + "() reads through the resolved target");
  }
}

/* ---- 2. every site that used the two-way now asks the target ------------ */
const sites = [
  /* name,                          needs chain_params, needs ui_hierarchy */
  ["changeHierPreset",              1, 1],
  ["refreshHierarchyChainParams",   1, 0],
  ["serviceHierEditorContractSettle", 1, 1],
  ["drawHierarchyEditor",           1, 1],
  /* handleSelect holds TWO of the seven -- the preset-edit-mode entry and the
     dynamic-item commit -- so it is asked for two chain_params reads. */
  ["handleSelect",                  2, 1],
];
for (const [name, wantParams, wantHier] of sites) {
  const body = fn(name);
  const p = (body.match(/hierEditorChainParamsNow\(\)/g) || []).length;
  const h = (body.match(/hierEditorHierarchyNow\(\)/g) || []).length;
  if (p < wantParams)
    fail(name + "() reaches hierEditorChainParamsNow() " + p + " times, " +
         "expected at least " + wantParams + " -- a bus insert there gets []");
  else if (h < wantHier)
    fail(name + "() reaches hierEditorHierarchyNow() " + h + " times, " +
         "expected at least " + wantHier);
  else ok(name + "() asks the chain target");
}

/* ---- 3. AND NO SITE MAY SPELL THE OLD ACCESSORS AGAIN ------------------- */
/* This is the half that catches an EIGHTH site. Each of these four takes the
   editor`s own state as its argument, so a new two-way can only be written by
   naming one of them here. Their INDEX-taking and key-taking forms are still
   used elsewhere (the diagram, the widget warm) with somebody else`s state --
   only the editor`s state is forbidden. */
const forbidden = [
  ["getMasterFxChainParams(hierEditorMasterFxSlot)", "Master FX chain_params"],
  ["getMasterFxHierarchy(hierEditorMasterFxSlot)", "Master FX ui_hierarchy"],
  ["getComponentChainParams(hierEditorSlot, hierEditorComponent)", "slot chain_params"],
  ["getComponentHierarchy(hierEditorSlot, hierEditorComponent)", "slot ui_hierarchy"],
];
for (const [needle, what] of forbidden) {
  const n = bare.split(needle).length - 1;
  if (n > 0)
    fail(n + " site(s) still read " + what + " straight from the editor state " +
         "(" + needle + ") -- that is the two-way, and a bus insert takes the " +
         "wrong arm of it");
  else ok("nothing reads " + what + " from the editor state directly");
}

/* ---- 4. the SWAP row is not a no-op on a bus --------------------------- */
{
  const sel = fn("handleSelect");
  const at = sel.indexOf("SWAP_MODULE_ACTION");
  if (at < 0) fail("handleSelect no longer handles SWAP_MODULE_ACTION");
  else {
    const body = sel.slice(at);
    /* The GUARD has to be the parse, not merely near it: `else if (false)`
       mentions both names and dispatches nothing. So the variable the parse
       binds is read out and required to be what the arm branches on AND what
       the picker is aimed with. */
    const bound = /const\s+(\w+)\s*=\s*BusModel\.parseBusComponentKey\(hierEditorComponent\)/
      .exec(body);
    if (!bound)
      fail("the Swap row does not parse the component key as a bus key -- " +
           "slotChainComponentIndex answers -1 for \"bus1:fx2\", so the row " +
           "answers a click by doing nothing");
    else {
      const v = bound[1];
      const armed = new RegExp("else if \\(" + v + "\\)").test(body);
      const aimed = new RegExp("openBusModuleSelectAt\\(" + v + "\\.").test(body);
      if (!armed) fail("the Swap row`s bus arm does not branch on the parsed " +
                       "key (" + v + ") -- it can be dispatched on anything");
      else if (!aimed) fail("the bus picker is not aimed with the parsed key");
      else ok("Swap opens the bus picker on a bus insert");
    }
  }
  const open = fn("openBusModuleSelectAt");
  if (open.indexOf("busChainBus") < 0 || open.indexOf("busChainPos") < 0)
    fail("openBusModuleSelectAt does not aim the bus cursor");
  else if (open.indexOf("findIndex") < 0)
    fail("openBusModuleSelectAt assumes the cursor row equals the position " +
         "index -- they differ the moment a hole sits ahead of it");
  else ok("the picker is aimed at the position that was being edited");
}

/* ---- 5. Back GOES and Back SAYS resolve through one helper ------------- */
{
  const dest = fn("hierEditorReturnDestination");
  if (dest.indexOf("hierEditorReturnView") < 0 || dest.indexOf("VIEWS.MASTER_FX") < 0)
    fail("hierEditorReturnDestination does not resolve the explicit view first");
  else ok("one resolver decides where Back goes");

  const exitFn = fn("exitHierarchyEditor");
  if (exitFn.indexOf("hierEditorReturnDestination()") < 0)
    fail("exitHierarchyEditor does not use the shared resolver -- it can send " +
         "you somewhere the announcement does not name");
  else ok("the exit uses it");

  const name = fn("hierEditorReturnDestinationName");
  if (name.indexOf("VIEWS.BUS_CHAIN") < 0)
    fail("the Back announcement cannot name the bus chain -- it said " +
         "\"Chain Editor\" while landing on the bus diagram");
  else ok("the announcement names the bus chain");

  const back = fn("handleBack");
  if (back.indexOf("hierEditorReturnDestinationName()") < 0)
    fail("Back out of the list editor does not announce the resolved " +
         "destination");
  else ok("Back announces where it actually went");
  if (back.indexOf("wasMasterFx ? \"Master FX\" : \"Chain Editor\"") >= 0)
    fail("a hand-rolled two-way announcement is back in handleBack");
}

if (failures) process.exit(1);
console.log("PASS: the list editor resolves its chain once — three chains, " +
            "seven sites, one target");
'
