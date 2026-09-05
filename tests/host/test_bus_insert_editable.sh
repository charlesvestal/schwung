#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A BUS INSERT'S PARAMETERS ARE REACHABLE, AND THEY WERE NOT.
#
# The bus screens shipped with Click on a populated insert going to the module
# picker unconditionally, no Shift+Click branch, and no BUS_CHAIN case in
# buildKnobContextForKnob -- so a CloudSeed loaded on a bus kept its defaults
# for good and the eight encoders answered null. Every leg of the fix is a
# WIRING fact rather than a pixel, so the snapshot harness cannot see any of
# them; each one below fails silently in a different way if it is removed:
#
#   - the Click branch:      the picker again, and no way in
#   - the entry-gate reader: slotChainTarget answers null for "bus1:fx2", so
#                            the hierarchy read never happens and the gate
#                            holds forever on a "Loading..." screen
#   - the ENTER routing:     enterHierarchyEditorWith would take it, which
#                            fetches chain_params through slotChainTarget --
#                            null key, empty list, and an empty chain_params
#                            list is exactly what makes the grid invent a
#                            float 0..1 knob for every parameter
#   - the return view:       Back ejects into the SLOT chain editor, which is
#                            the "identity lost, params still right" failure
#                            the Master FX routing note already describes
#
# Read out of source rather than executed: these are dispatch decisions inside
# a 25k-line file full of host globals, and the lift trick cannot isolate a
# switch case.

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };
const ok = (m) => console.log("  ok  " + m);

const ui = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const decomment = (s) => s.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
const fn = (name) => {
  const at = ui.indexOf("function " + name + "(");
  if (at < 0) { fail(name + "() is gone"); return ""; }
  const end = ui.indexOf("\n}\n", at);
  return decomment(ui.slice(at, end + 2));
};

/* ---- 1. Click edits, and only the picker is left for an empty box ------- */
{
  const sel = fn("handleSelect");
  const at = sel.indexOf("case VIEWS.BUS_CHAIN:");
  if (at < 0) fail("handleSelect no longer handles VIEWS.BUS_CHAIN");
  else {
    const body = sel.slice(at, sel.indexOf("case VIEWS.", at + 10));
    if (body.indexOf("enterBusComponentEdit()") < 0)
      fail("Click on a bus insert does not reach enterBusComponentEdit() -- " +
           "the position is uneditable again");
    if (body.indexOf("enterBusModuleSelect()") < 0)
      fail("the empty box no longer opens the picker");
    ok("Click edits a loaded bus insert and adds on an empty one");
  }
}

/* ---- 2. Shift+Click is where the swap went ----------------------------- */
{
  const shiftSites = (decomment(ui).match(
    /view === VIEWS\.BUS_CHAIN && !selectingBusModule\) \{\s*enterBusModuleSelect/g) || []).length;
  /* BOTH dispatch sites -- the ordinary one and the co-run one. A branch added
     to one only is a gesture that works everywhere except under a tool. */
  if (shiftSites !== 2)
    fail("Shift+Click reaches the bus picker at " + shiftSites + " dispatch " +
         "sites, expected 2 (the ordinary one and the co-run one)");
  else ok("Shift+Click swaps, at both dispatch sites");
}

/* ---- 3. the entry gate can read a bus insert --------------------------- */
{
  const reader = fn("componentEntryReader");
  if (reader.indexOf("parseBusComponentKey") < 0 || reader.indexOf("busChainTarget") < 0)
    fail("componentEntryReader has no bus branch -- the hierarchy would be " +
         "read through slotChainTarget, whose key rule answers null for a bus " +
         "key, so the gate can only hold");
  else ok("the entry gate reads a bus insert through the bus target");

  const open = fn("openComponentEditor");
  if (open.indexOf("enterBusHierarchyEditorWith") < 0)
    fail("openComponentEditor does not route a bus insert to its own entry -- " +
         "enterHierarchyEditorWith fetches chain_params through slotChainTarget " +
         "and would hand the grid an empty list");
  else ok("ENTER routes a bus insert to enterBusHierarchyEditorWith");
}

/* ---- 4. chain_params come from the BUS target -------------------------- */
{
  const enter = fn("enterBusHierarchyEditorWith");
  if (enter.indexOf("chainTargetChainParams") < 0 || enter.indexOf("busChainTarget") < 0)
    fail("the bus editor does not fetch chain_params through the bus target -- " +
         "an empty list is what invents a float 0..1 knob for every parameter");
  else ok("chain_params come from the bus target");
  if (enter.indexOf("hierEditorReturnView = VIEWS.BUS_CHAIN") < 0)
    fail("the LIST path does not set its return view -- Back would eject into " +
         "the slot chain editor");
  else ok("the list path returns to the bus chain");
  if (enter.indexOf("paramPagesEnabled()") < 0)
    fail("the bus editor does not offer the knob grid at all");
  /* BOTH destinations. paramPagesEnabled() is false whenever the screen reader
     is on, so a grid-only bus insert is uneditable for exactly the users who
     cannot see the diagram behind it. */
  if (enter.indexOf("setView(VIEWS.HIERARCHY_EDITOR)") < 0)
    fail("the bus editor has no LIST path -- a screen-reader session gets " +
         "nothing at all from the click");
  else ok("both destinations are wired");
}

/* ---- 5. Back out of the grid lands on the bus chain -------------------- */
{
  const chrome = fn("paramPagesChromeFor");
  if (chrome.indexOf("parseBusComponentKey") < 0)
    fail("paramPagesChromeFor has no bus branch -- the grid would say S1, read " +
         "the slot chain module-key spelling and send Back to the chain editor");
  else if (chrome.indexOf("returnView: VIEWS.BUS_CHAIN") < 0)
    fail("the bus chrome does not send Back to the bus chain");
  else ok("the grid names the bus and returns to it");
}

/* ---- 6. and the encoders resolve a context ----------------------------- */
{
  const build = fn("buildKnobContextForKnob");
  if (build.indexOf("VIEWS.BUS_CHAIN") < 0)
    fail("buildKnobContextForKnob has no BUS_CHAIN case -- the eight encoders " +
         "answer null, which is not no mapping, it is no feedback either");
  else ok("the encoders resolve a bus context");
  const get = fn("getKnobContext");
  if (get.indexOf("busChainPos") < 0 || get.indexOf("busChainBus") < 0)
    fail("the knob-context cache is not keyed on the bus CURSOR -- it would " +
         "serve one position from another, and Bus 2 from Bus 1");
  else ok("the cache is keyed on both halves of the bus cursor");
}

if (failures) process.exit(1);
console.log("PASS: a bus insert is editable — Click, the entry gate, the two " +
            "editors, Back, and the encoders");
'
