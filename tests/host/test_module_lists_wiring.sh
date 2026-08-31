#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Module lists, where they live in shadow_ui.js and the shared picker chrome --
# the parts no unit test over module_lists.mjs can reach.
#
# Half of this is source pins and half of it RUNS the code: the picker filter
# is a set of functions that close over a handful of named things, so they can
# be lifted out with new Function and driven, which is the difference between
# pinning the text of a rule and pinning what it does. The cursor rule in
# particular cannot be pinned as text -- the wrong version and the right one
# differ only in what they compute.
#
# The pins that are load-bearing beyond their own line:
#
#  - the value rule lives in drawChainPicker, not in a caller. Both pickers
#    draw through it, and a mark only one of them draws is the same bug one
#    layer down -- which is why the loaded-module star is already there.
#  - the module_lists action must RETURN before the componentModalFromGrid
#    bookkeeping at the end of runComponentActionFromGrid. That flag is for
#    hand-offs converging on CHAIN_EDIT; this one never goes there, and a flag
#    left raised fires on somebody elses arrival later.
#  - shadow_ui.js must not restate MENU_LIST_X. The picker and the lists
#    screens are one click apart and must share one rectangle, which is what
#    chain_editor_chrome.mjs exists for.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import * as ModuleLists from "./src/shared/module_lists.mjs";

const ui = readFileSync("src/shadow/shadow_ui.js", "utf8");
const chrome = readFileSync("src/shared/chain_editor_chrome.mjs", "utf8");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => { console.error("ok: " + m); };
const eq = (a, b, m) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok(m);
  else fail(m + " -- got " + JSON.stringify(a) + " want " + JSON.stringify(b));
};

function bodyOf(name, src) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone -- this test is anchored on it"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return src.slice(at, end + 2);
}
/* Lift a top-level function out of shadow_ui.js and hand it its dependencies
   as parameters. The file cannot be imported -- it is a device UI module full
   of host globals -- but a function that closes over a few named things can be
   RUN. */
function lift(name, deps) {
  const b = bodyOf(name, ui);
  return b === null ? null : new Function(...deps, b + "\nreturn " + name + ";");
}
/* Some of these ASSIGN to module-level state. Passing those in as parameters
   would make the assignment invisible, so they are declared as locals in the
   same scope and copied back into `st` after every call. */
function liftStateful(name, deps, mutables) {
  const b = bodyOf(name, ui);
  if (b === null) return null;
  const decl = mutables.map((m) => "let " + m + " = st." + m + ";").join("\n");
  const sync = mutables.map((m) => "st." + m + " = " + m + ";").join(" ");
  return new Function("st", ...deps,
    decl + "\n" + b + "\nvar __f = " + name + ";" +
    "\nreturn function(...a){ try { return __f(...a); } finally { " + sync + " } };");
}

/* ---- 1. the Module page row ------------------------------------------- */
const menu = bodyOf("moduleMenuEntries", ui) || "";
if (/"Add to List"/.test(menu)) ok("the Module page offers Add to List");
else fail("the Module page has no Add to List row");
if (/action: "module_lists"/.test(menu)) ok("the row carries the module_lists action");
else fail("the Add to List row has no module_lists action");

/* Order: Help (when present), then Add to List, then the destructive pair. */
const order = ["Module Help", "Add to List", "Swap Module", "Remove Module"]
  .map(s => menu.indexOf(s));
if (order.every((v, i) => v >= 0 && (i === 0 || v > order[i - 1]))) {
  ok("the rows are in order: Help, Add to List, Swap, Remove");
} else {
  fail("Module page row order changed -- the non-destructive rows must lead");
}

/* The row is UNCONDITIONAL: unlike Module Help it is not inside the
   getModuleHelpChildren guard. */
const helpGuard = menu.indexOf("getModuleHelpChildren");
const addRow = menu.indexOf("Add to List");
const guardClose = menu.indexOf("}", helpGuard);
if (helpGuard >= 0 && addRow > guardClose) {
  ok("Add to List is outside the help.json guard, so every module gets it");
} else {
  fail("Add to List looks gated on help.json -- it must be unconditional");
}

/* ---- 2. the action returns before the CHAIN_EDIT bookkeeping ---------- */
const act = bodyOf("runComponentActionFromGrid", ui) || "";
const caseAt = act.indexOf("case \"module_lists\"");
const bookAt = act.indexOf("gridActionOpenedSomething");
if (caseAt < 0) fail("runComponentActionFromGrid has no module_lists case");
else if (bookAt < 0) fail("gridActionOpenedSomething is gone -- test anchored on it");
else if (/return true;/.test(act.slice(caseAt, bookAt))) {
  ok("the module_lists case returns before the CHAIN_EDIT bookkeeping");
} else {
  fail("the module_lists case falls through to componentModalFromGrid -- the flag would fire on an unrelated CHAIN_EDIT arrival");
}

/* ---- 3. the three views exist and are drawn -------------------------- */
for (const v of ["MODULE_LISTS", "MODULE_LISTS_EDIT", "MODULE_LISTS_ACTIONS"]) {
  if (new RegExp("case VIEWS\\." + v + ":\\s+draw").test(ui)) ok(v + " has a draw case");
  else fail(v + " is not drawn -- a view with no draw case is a blank screen");
}

/* ---- 4. the lists screens draw through the SHARED rect --------------- */
if (/export function drawListScreen\(/.test(chrome)) ok("drawListScreen lives in the shared chrome");
else fail("drawListScreen is not in chain_editor_chrome.mjs -- the lists screens must share the picker rect");
if (/MENU_LIST_X/.test(ui)) fail("shadow_ui.js restates the list rect -- draw through drawListScreen instead");
else ok("shadow_ui.js does not restate the list rect");

/* ---- 5. the shared chrome draws an entry value ----------------------- */
/* Behaviour, not text: drawChainPicker now DELEGATES to drawListScreen, so
   the value rule can no longer be recognised by the shape of one expression.
   Run it and read the rows drawPageChromeList was handed. */
{
  const mod = await import("./src/shared/chain_editor_chrome.mjs");
  const seen = [];
  /* A ctx that records nothing but keeps the draw from throwing. print is what
     drawPageChromeList paints rows with, so the row text is observable there.
     Rather than parse pixels, spy at the entry: drawListScreen maps entries to
     { name, value }, and every one of them reaches print. */
  const ctx = {
    fillRect: () => {},
    textWidth: (s) => String(s).length * 5,
    print: (x, y, s) => { seen.push(String(s)); },
  };
  mod.drawChainPicker(ctx, {
    headerLeft: "S1 > Synth",
    entries: [
      { id: "", name: "None" },
      { id: "braids", name: "Braids" },
      { id: "__list_filter__", name: "List", value: "Favorites" },
    ],
    index: 0,
    currentId: "braids",
  });
  const joined = seen.join("|");
  if (/Favorites/.test(joined)) ok("drawChainPicker prints an entry own value");
  else fail("drawChainPicker ignores item.value -- the filter row would render blank");
  if (/\*/.test(joined)) ok("...and still marks the loaded module with a star");
  else fail("drawChainPicker lost the loaded-module mark");
}

/* ---- 6. the filter helpers, RUN -------------------------------------- */
const PICKER_FILTER_ID = (ui.match(/const PICKER_FILTER_ID = "([^"]+)"/) || [])[1];
if (PICKER_FILTER_ID) ok("the filter row has an id: " + PICKER_FILTER_ID);
else fail("PICKER_FILTER_ID is missing");

/* One state used by every behavioural check below. dx7 and braids are synths
   in Live; tape is not, and gate is in an FX-only list. */
const listsState = {
  version: 1,
  lists: [
    { name: "Favorites", modules: ["braids"] },
    { name: "Live", modules: ["braids", "dx7"] },
    { name: "Pedals", modules: ["gate"] },
  ],
};
const ensure = () => {};

const pickerRealIds = lift("pickerRealIds", [])();
const pickerEligibleLists = lift("pickerEligibleLists",
  ["moduleListsEnsureLoaded", "moduleListsState", "ModuleLists", "pickerRealIds"])(
    ensure, listsState, ModuleLists, pickerRealIds);
const pickerApplyFilter = lift("pickerApplyFilter",
  ["moduleListsEnsureLoaded", "moduleListsState", "ModuleLists", "pickerRealIds"])(
    ensure, listsState, ModuleLists, pickerRealIds);
const pickerFirstSelectableIndex = lift("pickerFirstSelectableIndex",
  ["PICKER_FILTER_ID"])(PICKER_FILTER_ID);

const SYNTH_SCAN = () => ([
  { id: "", name: "None" },
  { id: "braids", name: "Braids" },
  { id: "dx7", name: "DX7" },
  { id: "sf2", name: "SF2" },
  { id: "__get_more__", name: "[Get more...]" },
]);

eq(pickerRealIds(SYNTH_SCAN()), ["braids", "dx7", "sf2"],
   "the module ids are the real ones -- None (empty id) and the __ rows are not modules");

/* Criterion 4: a list with no installed module of this type is never offered. */
eq(pickerEligibleLists(SYNTH_SCAN()), ["Favorites", "Live"],
   "Pedals is not offered to a synth picker -- no member of it is installed here");

/* Criterion 5: the synthetic rows survive every filter, asserted by NAME. */
{
  const kept = pickerApplyFilter(
    SYNTH_SCAN().concat([{ id: "__move_left__", name: "  Move Left" },
                         { id: "__move_right__", name: "  Move Right" }]),
    "Favorites").map(m => m.name);
  eq(kept, ["None", "Braids", "[Get more...]", "  Move Left", "  Move Right"],
     "None, the moves and Get more all survive a filter; only non-members go");
}
eq(pickerApplyFilter(SYNTH_SCAN(), null).map(m => m.id),
   ["", "braids", "dx7", "sf2", "__get_more__"],
   "the All filter (null) is the identity");
eq(pickerApplyFilter(SYNTH_SCAN(), "Ghost").map(m => m.id),
   ["", "braids", "dx7", "sf2", "__get_more__"],
   "a list that no longer exists shows everything rather than an empty screen");

/* The cursor rule, on its own. */
eq(pickerFirstSelectableIndex([
     { id: PICKER_FILTER_ID }, { id: "__move_left__" }, { id: "__move_right__" },
     { id: "" }, { id: "braids" }]), 3,
   "the first selectable row is past the filter row AND past both moves");

/* ---- 7. enterComponentSelect, RUN ------------------------------------ */
/* Criteria 2, 3, 6, 7 and 8 are all about what this function BUILDS, so it is
   driven rather than grepped. Everything it closes over is injected. */
const CFG = () => ({ midiFx: [], synth: { module: "braids", params: {} },
                     fx: [{ module: "reverb", params: {} },
                          { module: "delay", params: {} }] });

function makeEnter(st, opts) {
  const o = opts || {};
  const scan = o.scan || SYNTH_SCAN;
  return liftStateful("enterComponentSelect",
    ["slotChainComponents", "isChainModuleKey", "scanModulesForType",
     "getChainComponentModule", "chainConfigs", "chainMoveEntries",
     "pickerEligibleLists", "pickerApplyFilter", "pickerFirstSelectableIndex",
     "PICKER_FILTER_ID", "setView", "VIEWS", "announce"],
    ["availableModules", "selectedModuleIndex", "selectedSlot",
     "selectedChainComponent", "componentSelectFilter", "needsRedraw"])(
    st,
    () => [{ key: "synth", label: "Synth" }],
    () => true,
    scan,
    () => (o.loaded === undefined ? { module: "braids" } : o.loaded),
    [CFG()],
    () => (o.moves || []),
    pickerEligibleLists, pickerApplyFilter, pickerFirstSelectableIndex,
    PICKER_FILTER_ID,
    () => {}, { COMPONENT_SELECT: "cs" },
    (m) => { st.announced = String(m); });
}
function freshState(filter) {
  return { availableModules: [], selectedModuleIndex: 0, selectedSlot: 0,
           selectedChainComponent: 0, componentSelectFilter: filter || null,
           needsRedraw: false, announced: "" };
}

/* Criterion 2: row 0 reads List / All. */
{
  const st = freshState(null);
  makeEnter(st)(0, 0);
  eq(st.availableModules[0],
     { id: PICKER_FILTER_ID, name: "List", value: "All", clickVerb: "LIST" },
     "row 0 is the filter row, reading List / All, and naming its own CLK verb");
}
/* Criterion 2 again, with a filter set: the value is the list name. */
{
  const st = freshState("Live");
  makeEnter(st)(0, 0);
  eq(st.availableModules[0].value, "Live", "row 0 shows the active list as its value");
  eq(st.availableModules.map(m => m.id),
     [PICKER_FILTER_ID, "", "braids", "dx7", "__get_more__"],
     "the rows below are filtered to the list, synthetics kept");
}
/* Criterion 8: the cursor opens on the loaded module... */
{
  const st = freshState(null);
  makeEnter(st)(0, 0);
  eq(st.availableModules[st.selectedModuleIndex].id, "braids",
     "the cursor opens on the loaded module");
}
/* ...on the first real row for an EMPTY position, never the filter row... */
{
  const st = freshState(null);
  makeEnter(st, { loaded: null })(0, 0);
  eq(st.selectedModuleIndex, 1, "an empty position opens on None, not on the filter row");
}
/* ...and never on a move row when the filter HIDES the loaded module. This is
   the case the obvious arithmetic gets wrong: the moves are spliced under the
   loaded module, so with it filtered away they are no longer at the top. */
{
  const st = freshState("Pedals");
  const moves = [{ id: "__move_left__", name: "  Move Left" },
                 { id: "__move_right__", name: "  Move Right" }];
  /* Pedals is eligible for THIS scan (gate is installed), so the filter
     stands, and it hides braids -- the loaded module. */
  const scan = () => ([{ id: "", name: "None" },
                       { id: "braids", name: "Braids" },
                       { id: "gate", name: "Gate" },
                       { id: "__get_more__", name: "[Get more...]" }]);
  makeEnter(st, { moves, scan })(0, 0);
  const at = st.availableModules[st.selectedModuleIndex];
  if (at && at.id !== PICKER_FILTER_ID &&
      at.id !== "__move_left__" && at.id !== "__move_right__") {
    ok("with the loaded module filtered away the cursor lands on a real row, not a move");
  } else {
    fail("the cursor opened on " + (at ? at.id : "nothing") +
         " -- it must never rest on the filter row or a move row");
  }
}
/* Criterion 7: a stored filter that matches nothing here falls back to All AND
   announces it. */
{
  const st = freshState("Pedals");   /* FX-only, against a synth scan */
  makeEnter(st)(0, 0);
  eq(st.componentSelectFilter, null, "an ineligible stored filter falls back to All");
  eq(st.availableModules[0].value, "All", "...and row 0 says All");
  if (/reset to All/i.test(st.announced)) ok("...and the fallback is ANNOUNCED: " + st.announced);
  else fail("the fallback to All was silent -- announced: " + st.announced);
  eq(st.availableModules.map(m => m.id).slice(1),
     ["", "braids", "dx7", "sf2", "__get_more__"], "...showing everything again");
}
/* Criterion 6: the filter is session state, so a second picker keeps it. */
{
  const st = freshState("Live");
  makeEnter(st)(0, 0);
  eq(st.componentSelectFilter, "Live", "an eligible filter survives entering a picker");
  makeEnter(st)(0, 0);
  eq(st.availableModules[0].value, "Live", "...and is still applied on the NEXT picker");
}

/* Criterion 3: the click cycles All -> each eligible list -> All, in place. */
{
  const eligible = pickerEligibleLists(SYNTH_SCAN());
  const seq = [];
  let f = null;
  for (let i = 0; i < 4; i++) { f = ModuleLists.nextFilter(f, eligible); seq.push(f); }
  eq(seq, ["Favorites", "Live", null, "Favorites"], "the cycle is All -> each eligible list -> All");
}
/* ...and the click handler stays ON the filter row so a second click cycles,
   and re-enters rather than re-filtering in place. */
{
  const apply = bodyOf("applyComponentSelection", ui) || "";
  const at = apply.indexOf("PICKER_FILTER_ID");
  const branch = at < 0 ? "" : apply.slice(at, apply.indexOf("return;", at));
  if (at < 0) fail("applyComponentSelection has no filter-row branch");
  else {
    if (/nextFilter/.test(branch)) ok("the click cycles through ModuleLists.nextFilter");
    else fail("the click does not use nextFilter -- the cycle order lives in the model");
    if (/enterComponentSelect\(/.test(branch)) ok("...and re-enters, so entry and click cannot drift");
    else fail("the click re-filters in place -- entry and click would drift");
    if (/selectedModuleIndex = 0/.test(branch)) ok("...leaving the cursor on the filter row so a second click cycles again");
    else fail("the cursor leaves the filter row after a click -- the row could not be cycled twice");
    if (/scanModulesForType\(/.test(branch)) ok("...and the eligible set comes from a FRESH scan, not the filtered rows");
    else fail("the eligible set is computed from the already-filtered rows -- the cycle would shrink every step");
  }
}
/* The filter row must not be announced as a load. */
{
  const sel = ui.slice(ui.indexOf("case VIEWS.COMPONENT_SELECT:", ui.indexOf("function handleSelect")));
  const head = sel.slice(0, sel.indexOf("applyComponentSelection()"));
  if (/PICKER_FILTER_ID/.test(head)) ok("handleSelect does not announce Loading for the filter row");
  else fail("handleSelect announces Loading for the filter row, which loads nothing");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
