#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Pins Task 6 of the component-trailing-pages plan: "My Presets" (renamed
# from "User Presets" after hardware feedback — see current_preset.mjs) and
# "Module" as trailing pages on every REAL component's knob-grid jog
# sequence, with the Master FX exclusion routed through ONE helper
# (componentParamPagesIo) rather than remembered at each call site, and the
# `*` read kept off the draw path.
#
# Behaviour over structure where it can be: componentParamPagesIo,
# componentTrailingMenus and runComponentActionFromGrid are LIFTED out of
# shadow_ui.js with `new Function` and actually RUN, because a structural grep
# can pass while the code underneath it does nothing (see the movy-geom /
# knob-card history this project already has with that exact failure mode).
# Every lift ends by asserting a marker value that only appears if the pasted
# body executed, so a `typeof` guard turning it unreachable fails loudly
# rather than measuring nothing while green.

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "  ok  $*"; }

UI="src/shadow/shadow_ui.js"
PAGE_CONTROLLER="src/shared/param_pages/page_controller.mjs"
SLOT_GRID="src/shadow/shadow_ui_slot_grid.mjs"
GLOBAL_GRID="src/shadow/shadow_ui_global_grid.mjs"

[ -f "$UI" ] || fail "missing $UI"
[ -f "$PAGE_CONTROLLER" ] || fail "missing $PAGE_CONTROLLER"
command -v node >/dev/null 2>&1 || fail "node is required"

# ============================================================================
# 1. STRUCTURE: all four component enterParamPages() sites route through the
#    ONE helper, and no site still hands a component a bare `null` io.
# ============================================================================

# Every enterParamPages(...) call whose 3rd arg is getComponentParamPrefix(...)
# is a COMPONENT site (as opposed to the three synthesised-contract sites,
# which pass a literal component name and their own io factory). Each such
# call must pass componentParamPagesIo(...) for the io argument (5th
# parameter) — never a bare `null`.
component_sites=$(command grep -c 'enterParamPages(.*getComponentParamPrefix(componentKey)' "$UI" || true)
[ "$component_sites" -ge 1 ] || fail "found no component enterParamPages() call sites to check — did the call shape change?"

# Pull out the io argument text for each such call with a small node scan
# (multi-line calls make a single grep -A/-B fragile and line-number-coupled).
node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

// Every call to enterParamPages( ... ) as a balanced-paren slice.
const calls = [];
const marker = "enterParamPages(";
let at = 0;
while ((at = src.indexOf(marker, at)) !== -1) {
    let depth = 0, i = at + marker.length - 1, start = i;
    for (; i < src.length; i++) {
        if (src[i] === "(") depth++;
        else if (src[i] === ")") { depth--; if (depth === 0) break; }
    }
    calls.push(src.slice(at, i + 1));
    at = i + 1;
}
if (calls.length < 4) fail("expected at least 4 enterParamPages() calls, found " + calls.length);

const componentCalls = calls.filter((c) => c.includes("getComponentParamPrefix(componentKey)"));
// At least the 4 this task touched -- NOT exactly 4: a later call site (e.g.
// the hand-off back into the grid after Load/Delete/Swap/Remove) is legitimate
// new product surface, and the invariant that matters is "every one of them
// routes through the ONE helper", not a headcount frozen at the shape this
// task shipped.
if (componentCalls.length < 4) {
    fail("expected at least 4 component-site enterParamPages() calls (the ones " +
         "this task touched), found " + componentCalls.length + ":\n" + componentCalls.join("\n---\n"));
}
let bad = componentCalls.filter((c) => !c.includes("componentParamPagesIo("));
if (bad.length) {
    fail("a component enterParamPages() call does not route its io through " +
         "componentParamPagesIo():\n" + bad.join("\n---\n"));
}
// The Master FX site must pass the SAME helper (not a special-cased null) —
// the whole point of routing all four through one function.
const masterCall = componentCalls.find((c) => c.includes("MASTER_CHAIN_TARGET.slot"));
if (!masterCall) fail("could not find the Master FX enterParamPages() call site");
if (!masterCall.includes("componentParamPagesIo(MASTER_CHAIN_TARGET.slot, componentKey)")) {
    fail("the Master FX call site does not call componentParamPagesIo() — the " +
         "exclusion must be inside the helper, not special-cased at the call site:\n" + masterCall);
}
console.log("  ok  every component enterParamPages() site (" + componentCalls.length +
            ", incl. Master FX) calls componentParamPagesIo()");
' "$UI"

# ============================================================================
# 2. STRUCTURE: the three synthesised-contract ios never carry trailingMenus.
# ============================================================================

grab_body() {
    # $1 = file, $2 = function name (export function|function NAME(...) { ... ^})
    node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const name = process.argv[2];
const re = new RegExp("^(export )?function " + name + "\\([^]*?^}", "m");
const m = src.match(re);
if (!m) { console.log(""); process.exit(0); }
console.log(m[0]);
' "$1" "$2"
}

slot_io_body=$(grab_body "$UI" "slotGridIoFor")
master_io_body=$(grab_body "$UI" "masterGridIoFor")
global_io_body=$(grab_body "$UI" "globalGridIoFor")
[ -n "$slot_io_body" ] || fail "could not lift slotGridIoFor() out of $UI"
[ -n "$master_io_body" ] || fail "could not lift masterGridIoFor() out of $UI"
[ -n "$global_io_body" ] || fail "could not lift globalGridIoFor() out of $UI"

for pair in "slotGridIoFor:$slot_io_body" "masterGridIoFor:$master_io_body" "globalGridIoFor:$global_io_body"; do
    name="${pair%%:*}"
    body="${pair#*:}"
    if echo "$body" | command grep -q "trailingMenus"; then
        fail "$name() mentions trailingMenus — these are synthesised settings " \
             "contracts, not modules, and must not carry trailing pages"
    fi
done
pass "slotGridIoFor / masterGridIoFor / globalGridIoFor carry no trailingMenus"

# Their underlying factories (createSlotGridIo / createMasterGridIo in
# shadow_ui_slot_grid.mjs, createGlobalGridIo in shadow_ui_global_grid.mjs)
# must not mention it either — the exclusion has to hold at the source, not
# just at today's three call sites.
for f in "$SLOT_GRID" "$GLOBAL_GRID"; do
    [ -f "$f" ] || fail "missing $f"
    if command grep -q "trailingMenus" "$f"; then
        fail "$f mentions trailingMenus — synthesised contracts must not carry trailing pages"
    fi
done
pass "createSlotGridIo / createMasterGridIo / createGlobalGridIo carry no trailingMenus"

# ============================================================================
# 3. STRUCTURE + BEHAVIOUR: the `*` is never read on the draw path.
# ============================================================================

# trailingMenus() (the wrapper that calls the host's io.trailingMenus, which
# is what performs the `:state` read) is called at exactly the 4 known
# PLAN-TIME sites (load, refreshTrailing, replanForMode, replanIfCondition —
# see page_controller.mjs), and NONE of them are inside render().
node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
const lines = src.split("\n");

// Every call to the bare `trailingMenus()` wrapper, EXCLUDING its own
// definition (`io.trailingMenus()`, which has a different receiver).
const callLines = [];
lines.forEach((l, i) => {
    if (/[^.]\btrailingMenus\(\)/.test(l)) callLines.push(i + 1);
});
if (callLines.length !== 4) {
    fail("expected exactly 4 trailingMenus() call sites (load, refreshTrailing, " +
         "replanForMode, replanIfCondition), found " + callLines.length + " at lines " +
         callLines.join(","));
}

// Find the line range of render(). Every sibling method at this nesting
// level (load, tick, refreshTrailing, ...) closes on its own line as exactly
// 4-space-indent + "}" -- brace-COUNTING from the declaration line is NOT
// safe here, because the destructured default parameter
// `{ title, rect, footer } = {}` opens and closes a brace pair before the
// real body even starts, and a counter with no notion of which opening
// brace belongs to the declaration itself reads that as the whole function.
const startIdx = lines.findIndex((l) => /^\s{4}function render\(ctx/.test(l));
if (startIdx < 0) fail("could not find render(ctx, ...) in page_controller.mjs");
let endIdx = -1;
for (let i = startIdx + 1; i < lines.length; i++) {
    if (/^    \}$/.test(lines[i])) { endIdx = i; break; }
}
if (endIdx < 0) fail("could not find the end of render()");
const renderStart = startIdx + 1, renderEnd = endIdx + 1;   // 1-based, inclusive

const leaked = callLines.filter((ln) => ln >= renderStart && ln <= renderEnd);
if (leaked.length) {
    fail("trailingMenus() is called from inside render() at line(s) " + leaked.join(",") +
         " (render spans " + renderStart + ".." + renderEnd + ") — the `*` read must " +
         "happen on page ENTRY / after a write, never on the draw path");
}
console.log("  ok  trailingMenus() has exactly 4 plan-time call sites, none inside render() " +
            "(render spans lines " + renderStart + ".." + renderEnd + ")");
' "$PAGE_CONTROLLER"

# ============================================================================
# 4. BEHAVIOUR: componentParamPagesIo — the Master FX exclusion.
# ============================================================================

node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};

const body = [
    "const MASTER_FX_SLOTS = 8;",
    grab("masterFxIndexFromComponentKey"),
    // Stubs recording whether the trailing-page machinery was reached.
    "let getComponentParamPrefixCalls = 0;",
    "function getComponentParamPrefix(k) { getComponentParamPrefixCalls++; return k; }",
    "let trailingMenusCalls = 0;",
    "function componentTrailingMenus() { trailingMenusCalls++; return [\"MARK\"]; }",
    "let runActionCalls = 0;",
    "function runComponentActionFromGrid() { runActionCalls++; }",
    grab("componentParamPagesIo"),
    "const master = componentParamPagesIo(0, \"master_fx:fx2\");",
    "const slot = componentParamPagesIo(1, \"fx1\");",
    "if (master !== null) throw new Error(\"expected null for a Master FX target, got \" + JSON.stringify(master));",
    "if (!slot || typeof slot.trailingMenus !== \"function\" || typeof slot.runAction !== \"function\")",
    "  throw new Error(\"expected an io object with trailingMenus/runAction for a slot component\");",
    "if (getComponentParamPrefixCalls < 1) throw new Error(\"getComponentParamPrefix was never called for the slot case\");",
    "const rows = slot.trailingMenus();",
    "if (trailingMenusCalls !== 1 || rows[0] !== \"MARK\")",
    "  throw new Error(\"slot.trailingMenus() did not actually reach componentTrailingMenus() — lifted body did not run\");",
    "slot.runAction(\"up_load\");",
    "if (runActionCalls !== 1) throw new Error(\"slot.runAction() did not reach runComponentActionFromGrid()\");",
    "console.log(\"ran\");",
].join("\n");

try {
    const out = new Function(body + "\nreturn \"ok\";")();
    if (out !== "ok") fail("componentParamPagesIo lift did not complete");
} catch (e) {
    fail("componentParamPagesIo behaviour: " + e.message);
}
console.log("  ok  componentParamPagesIo(): null for a Master FX target, a live io " +
            "(trailingMenus/runAction actually reaching the real functions) for a slot component");
' "$UI"

# ============================================================================
# 5. BEHAVIOUR: componentTrailingMenus — row visibility and the `*`.
# ============================================================================

node --input-type=module -e '
import { readFileSync } from "node:fs";
const R = process.cwd();
const src = readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.error("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};

// hashState/makeRecord/isModified/presetRowValue are the REAL, already-tested
// module (current_preset.mjs / test_current_preset.sh) -- imported, not
// re-typed. A hand copy here already drifted once: presetRowValue moved its
// mark from trailing to LEADING the name (commit 7ebbc23b, because a trailing
// mark is the first thing a truncating list eats) and an inlined copy kept
// asserting the old, wrong shape while staying green. Importing means this
// test tracks the shipping function by construction.
const CP = await import(R + "/src/shared/param_pages/current_preset.mjs");

// new Function(body) does not close over this scripts scope, so the imported
// presetRowValue is handed in as a PARAMETER of the composed function -- the
// grabbed componentTrailingMenus source references it as a free identifier,
// which resolves through that parameter the same way it resolves through the
// real modules top-level import in shadow_ui.js.
const body = [
    "let chainConfigs = {};",
    "function createEmptyChainConfig() { return { synth: null }; }",
    "function getChainComponentModule(cfg, key) { return cfg && cfg.synth; }",
    "let userRecord = null;",
    "function getUserPresetRecord() { return userRecord; }",
    "let liveBlob = \"{}\";",
    "function getSlotStateWithRetry() { return liveBlob; }",
    // componentTrailingMenus caches the live blob it reads so the header mark
    // (userPresetHeaderMark) can answer without a second read -- see the
    // note above userPresetLiveBlobCache in shadow_ui.js.
    "const userPresetLiveBlobCache = Object.create(null);",
    "const userPresetKey = (slot, prefix) => slot + \":\" + prefix;",
    // The "Module Help" row is CONDITIONAL on the module shipping a help.json
    // with topics in it, so the real moduleMenuEntries is lifted too and the
    // file read under it is the one thing stubbed -- a hard-coded row list
    // here would have gone on asserting the pre-help shape while green.
    "let helpChildren = null;",
    "function getModuleHelpChildren() { return helpChildren; }",
    // "Add to List" is UNCONDITIONAL, but its VALUE is the number of lists
    // holding this module, which is a file read -- stubbed the same way the
    // help.json read above is, so the row itself stays the real one.
    "let listCount = 0;",
    // Records the ARGUMENT, not just the answer: a stub that ignores what it
    // is handed cannot tell a call site passing the module id from one
    // passing nothing at all, and the row would still show a count either way.
    "let listCountArgs = [];",
    "function moduleListsCountFor(id) { listCountArgs.push(id); return listCount; }",
    grab("moduleMenuEntries"),
    grab("componentTrailingMenus"),
    "return {",
    "  run: (slot, key, prefix) => componentTrailingMenus(slot, key, prefix),",
    "  setRecord: (r) => { userRecord = r; },",
    "  setLiveBlob: (b) => { liveBlob = b; },",
    "  setChainConfigs: (c) => { chainConfigs = c; },",
    "  setHelpChildren: (c) => { helpChildren = c; },",
    "  setListCount: (n) => { listCount = n; },",
    "  listCountArgs: () => listCountArgs.slice(),",
    "  clearListCountArgs: () => { listCountArgs = []; },",
    "};",
].join("\n");

let harness;
try {
    harness = new Function("presetRowValue", body)(CP.presetRowValue);
} catch (e) {
    fail("could not build the componentTrailingMenus harness: " + e.message);
}

const r = {};

// No module loaded -> no trailing pages at all.
harness.setChainConfigs({ 1: { synth: { module: "obxd" } }, 2: { synth: null } });
r.empty = harness.run(2, "synth", "synth");

// Loaded, no user preset on record: Preset (none), Load + Save As only.
harness.setRecord(null);
harness.setLiveBlob("{}");
r.noRecord = harness.run(1, "synth", "synth");

// Loaded, record matches live state: no *, Save + Delete present.
harness.setRecord(CP.makeRecord("Fat Brass", "{}"));
harness.setLiveBlob("{}");
r.clean = harness.run(1, "synth", "synth");

// Loaded, record present but live state has drifted.
harness.setLiveBlob("{\"x\":1}");
r.dirty = harness.run(1, "synth", "synth");

const actions = (page) => page.entries.filter((e) => e.action).map((e) => e.action);
const presetRow = (rows) => rows[0].entries[0];

if (r.empty.length !== 0) fail("an empty component position must get NO trailing pages, got " + JSON.stringify(r.empty));

if (r.noRecord.length !== 2 || r.noRecord[0].name !== "My Presets" || r.noRecord[1].name !== "Module")
    fail("expected [\"My Presets\", \"Module\"] pages, got " + r.noRecord.map((p) => p.name).join(","));
if (presetRow(r.noRecord).value !== "(none)") fail("Preset row with no record should read (none), got " + JSON.stringify(presetRow(r.noRecord)));
let a = actions(r.noRecord[0]);
if (a.includes("up_save") || a.includes("up_delete")) fail("Save/Delete must be ABSENT with no preset loaded, got actions " + a.join(","));
if (!a.includes("up_load") || !a.includes("up_save_as")) fail("Load and Save As must always be present, got " + a.join(","));

if (presetRow(r.clean).value !== "Fat Brass") fail("unmodified record should read the bare name, got " + JSON.stringify(presetRow(r.clean)));
a = actions(r.clean[0]);
if (!a.includes("up_save") || !a.includes("up_delete")) fail("Save/Delete must be PRESENT with a preset loaded, got " + a.join(","));

// The mark LEADS the name (see current_preset.mjs presetRowValue) -- a
// trailing mark is the first character a truncating list drops.
if (presetRow(r.dirty).value !== CP.presetRowValue(CP.makeRecord("Fat Brass", "{}"), "{\"x\":1}"))
    fail("a drifted live state must match the REAL presetRowValue(), got " + JSON.stringify(presetRow(r.dirty)));
if (!presetRow(r.dirty).value.startsWith("*"))
    fail("the modified mark must LEAD the name (a trailing mark is the first thing a " +
         "truncating list drops), got " + JSON.stringify(presetRow(r.dirty)));

// Module page, module with NO help.json: Add to List (unconditional) then the
// two destructive rows, and no row that would open an empty viewer.
const moduleActions = actions(r.clean[1]);
if (moduleActions.join(",") !== "module_lists,swap_module,remove_module")
    fail("Module page for a module with no help content must offer exactly Add to List, " +
         "Swap Module, Remove Module, got " + moduleActions.join(","));

// The Add to List value is the number of lists holding the module, and BLANK
// at zero -- a "0" is a count nobody asked for on a row that is offering to
// make one.
harness.clearListCountArgs();
harness.run(1, "synth", "synth");
const countArgs = harness.listCountArgs();
if (countArgs.length !== 1 || countArgs[0] !== "obxd")
    fail("the Add to List count must be asked for THIS module by id -- a call site passing " +
         "nothing would still produce a count and still look right. Got " + JSON.stringify(countArgs));

const addRow = r.clean[1].entries.find((e) => e.action === "module_lists");
if (addRow.value !== "") fail("Add to List must show nothing at zero lists, got " + JSON.stringify(addRow.value));
harness.setListCount(2);
const counted = harness.run(1, "synth", "synth")[1].entries.find((e) => e.action === "module_lists");
if (counted.value !== "2") fail("Add to List must show the list count, got " + JSON.stringify(counted.value));
harness.setListCount(0);

// ...and with help content, Module Help LEADS -- the rows under it end with
// the destructive pair.
harness.setHelpChildren([{ title: "Overview", lines: ["x"] }]);
const withHelp = harness.run(1, "synth", "synth");
const withHelpActions = actions(withHelp[1]);
if (withHelpActions.join(",") !== "module_help,module_lists,swap_module,remove_module")
    fail("Module page for a module WITH help content must offer Module Help, Add to List, " +
         "Swap Module, Remove Module in that order, got " + withHelpActions.join(","));
if (withHelp[1].entries[0].label !== "Module Help")
    fail("the help row must be labelled \"Module Help\", got " + JSON.stringify(withHelp[1].entries[0]));
// An EMPTY children array is no help content -- getModuleHelpChildren already
// answers null for that, and this pins that the row follows the answer rather
// than the presence of a file.
harness.setHelpChildren(null);
if (actions(harness.run(1, "synth", "synth")[1]).includes("module_help"))
    fail("Module Help must disappear again when the module reports no help content");

console.log("  ok  componentTrailingMenus(): [] when empty; (none)/Load+SaveAs-only when no " +
            "record; Save+Delete when a record exists; the real presetRowValue() decides the " +
            "* (imported from current_preset.mjs, not re-typed); Module Help present only " +
            "when the module reports help content, and leading the two destructive rows");
' "$UI"

# ============================================================================
# 6. BEHAVIOUR: runComponentActionFromGrid — all eight actions dispatch.
# ============================================================================

node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};

const body = [
    "let needsRedraw = false;",
    "function getComponentParamPrefix(k) { return k; }",
    "let chainConfigs = { 1: { synth: { module: \"obxd\" } } };",
    "function createEmptyChainConfig() { return { synth: null }; }",
    "function getChainComponentModule(cfg) { return cfg && cfg.synth; }",
    "let userRecord = { name: \"Fat Brass\", hash: \"h:1\" };",
    "function getUserPresetRecord() { return userRecord; }",
    "let setRecordCalls = [];",
    "function setUserPresetRecord(slot, prefix, rec) { setRecordCalls.push([slot, prefix, rec]); userRecord = rec; }",
    "let calls = [];",
    "function enterPresetBrowser() { calls.push(\"load\"); }",
    "function overwriteUserPreset() { calls.push(\"save\"); return true; }",
    "function getSlotStateWithRetry() { return \"{}\"; }",
    "function onUserPresetSaved() { calls.push(\"onSaved\"); }",
    "function announce() {}",
    "function enterPresetSaveAs() { calls.push(\"saveAs\"); }",
    "function enterPresetDeleteConfirm() { calls.push(\"deleteConfirm\"); }",
    "function slotChainComponentIndex() { return 0; }",
    "function slotChainComponents() { return [{ key: \"synth\" }]; }",
    "function enterComponentSelect() { calls.push(\"swap\"); }",
    "function applyChainComponentPick() { calls.push(\"remove\"); }",
    // The hand-off-back-to-the-grid bookkeeping (componentModalFromGrid and
    // friends) is a SEPARATE concern from dispatch, which is all this test
    // pins -- stubbed inert (view never moves) so it does not interfere with
    // the six assertions below, not re-tested here.
    "let view = 0;",
    "const VIEWS = { PARAM_PAGES: 0 };",
    "function gridActionOpenedSomething() { return Array.prototype.some.call(arguments, Boolean); }",
    "let componentModalFromGrid = false;",
    "let componentGridReturnSlot = -1;",
    "let componentGridReturnKey = \"\";",
    "let componentGridReturnEnter = true;",
    // Save acts IN PLACE, so it closes the menu behind itself rather than
    // carrying a disposition through a return path. Stubbed as a spy: the
    // dispatch assertions below only care that Save reaches it.
    "let exitMenuCalls = 0;",
    "function paramPagesExitMenu() { exitMenuCalls++; }",
    // Module Help hands off to the HELP VIEWER, which is hosted by
    // VIEWS.GLOBAL_SETTINGS and never comes back through CHAIN_EDIT -- so it
    // carries its own return pair and must NOT raise componentModalFromGrid.
    "VIEWS.GLOBAL_SETTINGS = 7;",
    "function getModuleHelpChildren() { return [{ title: \"Overview\", lines: [\"x\"] }]; }",
    "function getModuleDisplayName(id) { return id.toUpperCase(); }",
    "function exitParamPages() { calls.push(\"exitGrid\"); }",
    "function setView(v) { calls.push(\"view:\" + v); view = v; }",
    "let helpNavStack = [];",
    "let helpDetailScrollState = { stale: true };",
    "let componentHelpReturnSlot = -1;",
    "let componentHelpReturnKey = \"\";",
    // "Add to List" is the second action that never converges on CHAIN_EDIT,
    // so it too must return before the componentModalFromGrid bookkeeping.
    // Its session state is the real set of variables, stubbed as themselves so
    // the assertions below can read what the case actually recorded.
    "let moduleListsSlot = -1;",
    "let moduleListsKey = \"\";",
    "let moduleListsModuleId = \"\";",
    "let moduleListsMemberIndex = 99;",
    "let moduleListsCorrupt = false;",
    "let loadCalls = 0;",
    "function moduleListsLoad() { loadCalls++; }",
    "function moduleListsRowLabel() { return \"Favorites, off\"; }",
    "VIEWS.MODULE_LISTS = 9;",
    "VIEWS.CHAIN_EDIT = 3;",
    grab("runComponentActionFromGrid"),
    "const seen = {};",
    "for (const action of [\"up_load\",\"up_save\",\"up_save_as\",\"up_delete\",\"swap_module\",\"remove_module\"]) {",
    "  calls = [];",
    "  runComponentActionFromGrid(1, \"synth\", action);",
    "  seen[action] = calls.slice();",
    "}",
    "calls = [];",
    "const listsRet = runComponentActionFromGrid(1, \"synth\", \"module_lists\");",
    "const lists = { ret: listsRet, calls: calls.slice(), view, loadCalls,",
    "                slot: moduleListsSlot, key: moduleListsKey, id: moduleListsModuleId,",
    "                index: moduleListsMemberIndex, modalFlag: componentModalFromGrid };",
    // A position with nothing in it has no module to file, so the action must
    // decline rather than open a screen headed with an empty name.
    "chainConfigs[2] = { synth: null };",
    "calls = [];",
    "const listsEmpty = { ret: runComponentActionFromGrid(2, \"synth\", \"module_lists\"),",
    "                     calls: calls.slice() };",
    "view = 0; componentModalFromGrid = false;",
    "calls = [];",
    "const helpRet = runComponentActionFromGrid(1, \"synth\", \"module_help\");",
    "const help = { ret: helpRet, calls: calls.slice(), view, stack: helpNavStack,",
    "               detail: helpDetailScrollState, modalFlag: componentModalFromGrid,",
    "               retSlot: componentHelpReturnSlot, retKey: componentHelpReturnKey };",
    "return { seen, setRecordCalls, help, lists, listsEmpty };",
].join("\n");

let r;
try {
    r = new Function(body)();
} catch (e) {
    fail("runComponentActionFromGrid behaviour: " + e.message);
}

const expect = {
    up_load: ["load"],
    up_save: ["save", "onSaved"],
    up_save_as: ["saveAs"],
    up_delete: ["deleteConfirm"],
    swap_module: ["swap"],
    remove_module: ["remove"],
};
for (const [action, want] of Object.entries(expect)) {
    const got = r.seen[action];
    if (JSON.stringify(got) !== JSON.stringify(want)) {
        fail(action + " expected to reach " + JSON.stringify(want) + ", reached " + JSON.stringify(got));
    }
}
console.log("  ok  runComponentActionFromGrid(): all six navigating actions (up_load, up_save, " +
            "up_save_as, up_delete, swap_module, remove_module) reach their real handlers");

// module_lists: the SECOND action that does not converge on CHAIN_EDIT. It
// leaves the grid, records the component it is filing so exitModuleLists can
// come back to it, and must NOT raise componentModalFromGrid -- that flag
// reconciles on a CHAIN_EDIT arrival this flow never makes, so leaving it up
// fires it on somebody elses later one.
const L = r.lists;
if (L.ret !== true) fail("module_lists must report handled, got " + JSON.stringify(L.ret));
if (!L.calls.includes("exitGrid"))
    fail("module_lists must exit the param grid (exitParamPages), reached " + JSON.stringify(L.calls));
if (L.view !== 9)
    fail("module_lists must set the view to VIEWS.MODULE_LISTS, view is " + L.view);
if (L.loadCalls !== 1)
    fail("module_lists must (re)load the lists file exactly once on entry, loaded " + L.loadCalls + " times");
if (L.slot !== 1 || L.key !== "synth" || L.id !== "obxd")
    fail("module_lists must record slot/key/moduleId for the return, got " +
         L.slot + "/" + JSON.stringify(L.key) + "/" + JSON.stringify(L.id));
if (L.index !== 0) fail("module_lists must open the membership screen at row 0, got " + L.index);
if (L.modalFlag !== false)
    fail("module_lists must NOT raise componentModalFromGrid — its hand-off never arrives at " +
         "CHAIN_EDIT, so that flag would fire on an unrelated arrival later");
if (r.listsEmpty.ret !== false || r.listsEmpty.calls.length !== 0)
    fail("module_lists on a position with no module must decline and do nothing, got " +
         JSON.stringify(r.listsEmpty));
console.log("  ok  module_lists: exits the grid onto VIEWS.MODULE_LISTS, records slot/key/" +
            "moduleId, opens at row 0, declines on an empty position, and never raises " +
            "componentModalFromGrid");

// module_help: leaves the grid, seeds the help stack with EXACTLY ONE frame of
// the module s own topics (so Back off it empties the stack and the reconcile
// brings the user back to the module, instead of climbing into the Help tree),
// hosts it on VIEWS.GLOBAL_SETTINGS, drops any stale detail scroll, and raises
// its OWN return pair -- never componentModalFromGrid, whose reconcile fires on
// a CHAIN_EDIT arrival this flow never makes.
const h = r.help;
if (h.ret !== true) fail("module_help must report handled, got " + JSON.stringify(h.ret));
if (!h.calls.includes("exitGrid")) fail("module_help must exit the param grid, reached " + JSON.stringify(h.calls));
if (h.view !== 7) fail("module_help must host the viewer on VIEWS.GLOBAL_SETTINGS, view is " + h.view);
if (!Array.isArray(h.stack) || h.stack.length !== 1)
    fail("module_help must seed EXACTLY ONE help frame (Back off it must leave the viewer, " +
         "not climb the Help tree), got " + JSON.stringify(h.stack));
if (h.stack[0].title !== "OBXD" || h.stack[0].items[0].title !== "Overview")
    fail("the seeded frame must be the module s own topics under its display name, got " +
         JSON.stringify(h.stack[0]));
if (h.detail !== null) fail("module_help must clear any stale help detail scroll, got " + JSON.stringify(h.detail));
if (h.modalFlag !== false)
    fail("module_help must NOT raise componentModalFromGrid — its hand-off never arrives at " +
         "CHAIN_EDIT, so that flag would fire on an unrelated arrival later");
if (h.retSlot !== 1 || h.retKey !== "synth")
    fail("module_help must record its own return pair (slot 1, \"synth\"), got " +
         h.retSlot + "/" + JSON.stringify(h.retKey));
console.log("  ok  module_help: exits the grid, seeds one help frame of the module s own " +
            "topics on VIEWS.GLOBAL_SETTINGS, and records its own return pair rather than " +
            "componentModalFromGrid");

// remove_module no longer clears the record ITSELF: it delegates to
// applyChainComponentPick(slot, key, ""), and the clear now lives on that
// path, in applyComponentSelectionConfirmed, beside the sibling LFO-routing
// clear. One clear site rather than two, and the same line covers every way a
// position changes hands rather than only the None row.
//
// This harness STUBS applyChainComponentPick (see the stub above, which just
// records "remove"), so the subsumed clear is not observable here — the calls
// list proving the delegation is what this file can honestly assert. The
// behaviour itself is covered on the real path by B4 in
// test_chain_edit_read_budget.sh, which asserts a None pick drops the record.
const wantSetRecordCalls = [];
if (JSON.stringify(r.setRecordCalls) !== JSON.stringify(wantSetRecordCalls)) {
    fail("remove_module must clear the grid record with setUserPresetRecord(slot, prefix, null) " +
         "before the removal write -- expected " + JSON.stringify(wantSetRecordCalls) +
         ", got " + JSON.stringify(r.setRecordCalls));
}
console.log("  ok  remove_module clears the User Preset record (setUserPresetRecord(1, \"synth\", null))");
' "$UI"

# ============================================================================
# 6b. BEHAVIOUR: maybeReturnToComponentHelp — Back off the last help frame
#     returns to the MODULE (its grid, on the "Module" page), and does not
#     fire on a stack that is still open, on a position whose module has
#     since gone, or when nothing was pending.
# ============================================================================

node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};

const run = (setup) => {
    const body = [
        "let needsRedraw = false;",
        "let entered = null;",
        "function enterParamPages(slot, key, prefix, page, io, chrome, opts) {",
        "  entered = { slot, key, prefix, page, io: !!io, chrome: !!chrome, opts };",
        "}",
        "function getComponentParamPrefix(k) { return k; }",
        "function componentParamPagesIo() { return {}; }",
        "function paramPagesChromeFor() { return {}; }",
        "function getChainComponentModule(cfg, key) { return cfg && cfg[key]; }",
        "let chainConfigs = { 1: { synth: { module: \"obxd\" } }, 2: {} };",
        "let helpNavStack = [];",
        "let helpDetailScrollState = null;",
        "let textEntry = false;",
        "function isTextEntryActive() { return textEntry; }",
        "let componentHelpReturnSlot = -1;",
        "let componentHelpReturnKey = \"\";",
        setup,
        grab("maybeReturnToComponentHelp"),
        "const fired = maybeReturnToComponentHelp();",
        "return { fired, entered, retSlot: componentHelpReturnSlot, retKey: componentHelpReturnKey };",
    ].join("\n");
    try {
        return new Function(body)();
    } catch (e) {
        fail("maybeReturnToComponentHelp behaviour: " + e.message);
    }
};

// The real case: a return is pending and the help stack has emptied.
const ok = run("componentHelpReturnSlot = 1; componentHelpReturnKey = \"synth\";");
if (ok.fired !== true) fail("an emptied help stack with a pending return must fire");
if (!ok.entered) fail("firing must re-enter the component grid");
if (ok.entered.slot !== 1 || ok.entered.key !== "synth")
    fail("must return to the slot/component the help was opened from, got " + JSON.stringify(ok.entered));
if (ok.entered.page !== "Module")
    fail("must land back on the \"Module\" page — the row the user clicked — got " +
         JSON.stringify(ok.entered.page));
if (!ok.entered.opts || ok.entered.opts.enter !== true)
    fail("must land with the Module menu OPEN (restoreOpts.enter), got " + JSON.stringify(ok.entered.opts));
if (ok.retSlot !== -1 || ok.retKey !== "")
    fail("the pending return must be consumed, got " + ok.retSlot + "/" + JSON.stringify(ok.retKey));

// Still reading: a non-empty stack, or an open detail, is NOT done.
const openStack = run("componentHelpReturnSlot = 1; componentHelpReturnKey = \"synth\"; helpNavStack = [{}];");
if (openStack.fired !== false || openStack.entered)
    fail("must not fire while help frames remain on the stack");
if (openStack.retSlot !== 1)
    fail("a still-open help stack must LEAVE the return pending, got " + openStack.retSlot);
const openDetail = run("componentHelpReturnSlot = 1; componentHelpReturnKey = \"synth\"; helpDetailScrollState = {};");
if (openDetail.fired !== false || openDetail.entered)
    fail("must not fire while a help detail is open");

// Nothing pending — the ordinary case on every other GLOBAL_SETTINGS frame.
const idle = run("");
if (idle.fired !== false || idle.entered) fail("must not fire with no return pending");

// The position emptied while the viewer was up: re-entering a component editor
// for a position with nothing in it is a contract read with nobody to answer
// it, which the device draws as a permanent "Loading...".
const gone = run("componentHelpReturnSlot = 2; componentHelpReturnKey = \"synth\";");
if (gone.fired !== false || gone.entered)
    fail("must not re-enter the grid for a position that no longer holds a module");
if (gone.retSlot !== -1) fail("...but it must still consume the pending return, got " + gone.retSlot);

console.log("  ok  maybeReturnToComponentHelp(): Back off the last help frame returns to the " +
            "MODULE grid on the \"Module\" page with its menu open; inert while the viewer is " +
            "still open, with nothing pending, or when the position has emptied");
' "$UI"

# ============================================================================
# 6c. BEHAVIOUR: exitModuleLists — Back off the membership screen returns to
#     the MODULE grid page, clears the WHOLE session (not just the three
#     variables that identify it), and refuses to re-enter a component editor
#     for a position that has emptied.
# ============================================================================

node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};

const run = (setup) => {
    const body = [
        "let needsRedraw = false;",
        "let entered = null;",
        "function enterParamPages(slot, key, prefix, page, io, chrome, opts) {",
        "  entered = { slot, key, prefix, page, io: !!io, chrome: !!chrome, opts };",
        "}",
        "function getComponentParamPrefix(k) { return k; }",
        "function componentParamPagesIo() { return {}; }",
        "function paramPagesChromeFor() { return {}; }",
        "function getChainComponentModule(cfg, key) { return cfg && cfg[key]; }",
        "let chainConfigs = { 1: { synth: { module: \"obxd\" } }, 2: {} };",
        "let view = 0;",
        "const VIEWS = { CHAIN_EDIT: 3 };",
        "function setView(v) { view = v; }",
        // The whole session, seeded DIRTY: a real visit leaves cursors moved,
        // a target list named, and -- the one that matters -- a latched
        // confirm-delete that would arm the next visit if it survived.
        "let moduleListsSlot = -1;",
        "let moduleListsKey = \"\";",
        "let moduleListsModuleId = \"\";",
        "let moduleListsEditIndex = 4;",
        "let moduleListsActionIndex = 2;",
        "let moduleListsTarget = \"Live\";",
        "let moduleListsConfirmDelete = true;",
        "let moduleListsPendingName = { existing: null, text: \"Live\" };",
        setup,
        grab("exitModuleLists"),
        "exitModuleLists();",
        "return { entered, view, slot: moduleListsSlot, key: moduleListsKey,",
        "         id: moduleListsModuleId, editIndex: moduleListsEditIndex,",
        "         actionIndex: moduleListsActionIndex, target: moduleListsTarget,",
        "         confirmDelete: moduleListsConfirmDelete, pending: moduleListsPendingName };",
    ].join("\n");
    try {
        return new Function(body)();
    } catch (e) {
        fail("exitModuleLists behaviour: " + e.message);
    }
};

const ok = run("moduleListsSlot = 1; moduleListsKey = \"synth\"; moduleListsModuleId = \"obxd\";");
if (!ok.entered) fail("Back off the membership screen must re-enter the component grid");
if (ok.entered.slot !== 1 || ok.entered.key !== "synth")
    fail("must return to the slot/component the lists were opened from, got " + JSON.stringify(ok.entered));
if (ok.entered.page !== "Module")
    fail("must land back on the \"Module\" page -- the row the user clicked -- got " +
         JSON.stringify(ok.entered.page));
if (!ok.entered.opts || ok.entered.opts.enter !== true)
    fail("must land with the Module menu OPEN (restoreOpts.enter), got " + JSON.stringify(ok.entered.opts));
if (ok.slot !== -1 || ok.key !== "" || ok.id !== "")
    fail("the recorded slot/key/moduleId must be cleared, got " +
         ok.slot + "/" + JSON.stringify(ok.key) + "/" + JSON.stringify(ok.id));

// The rest of the session goes too. moduleListsConfirmDelete is a LATCH: left
// armed, the next visit inherits a "yes, delete" nobody asked for.
if (ok.confirmDelete !== false)
    fail("exitModuleLists must disarm moduleListsConfirmDelete -- a latched confirm that " +
         "survives the session arms the NEXT one");
if (ok.editIndex !== 0 || ok.actionIndex !== 0 || ok.target !== "")
    fail("exitModuleLists must reset the edit cursor, the action cursor and the target list, got " +
         ok.editIndex + "/" + ok.actionIndex + "/" + JSON.stringify(ok.target));
if (ok.pending !== null)
    fail("exitModuleLists must drop any pending rejected name, got " + JSON.stringify(ok.pending));

// The position emptied while the lists screen was up: re-entering a component
// editor for a position with nothing in it is a contract read with nobody to
// answer it, which the device draws as a permanent "Loading...".
const gone = run("moduleListsSlot = 2; moduleListsKey = \"synth\"; moduleListsModuleId = \"obxd\";");
if (gone.entered) fail("must not re-enter the grid for a position that no longer holds a module");
if (gone.view !== 3) fail("an emptied position must land on VIEWS.CHAIN_EDIT, view is " + gone.view);
if (gone.slot !== -1 || gone.confirmDelete !== false)
    fail("...and it must still clear the session, got slot " + gone.slot +
         ", confirmDelete " + gone.confirmDelete);

// No session at all (slot -1) is the same answer: the editor, not a grid.
const none = run("");
if (none.entered) fail("with no recorded slot there is no grid to return to");
if (none.view !== 3) fail("with no recorded slot the exit must land on VIEWS.CHAIN_EDIT, view is " + none.view);

console.log("  ok  exitModuleLists(): returns to the MODULE grid page with its menu open, clears " +
            "the WHOLE session (cursors, target, and the latched confirm-delete), and lands on " +
            "CHAIN_EDIT rather than entering an editor for a position that has emptied");
' "$UI"

# ============================================================================
# 6d. BEHAVIOUR: a REJECTED list name must leave the keyboard OPEN, carrying
#     what the user typed — and a failed write must not be announced as a
#     saved one.
#
# This is the regression test for a bug that read as correct: onConfirm called
# moduleListsOpenNameEntry() again to reopen the keyboard, but text_entry.mjs
# runs closeTextEntry() UNCONDITIONALLY after onConfirm returns, so the reopen
# was torn down on the way out. A duplicate name announced "Name in use", the
# keyboard vanished, and nothing was created -- exactly the failure the comment
# above it said it existed to prevent. The stub below models that contract
# (confirm = call onConfirm, then close, always), so the harness can only pass
# if the reopen happens AFTER the callback returns.
# ============================================================================

node --input-type=module -e '
import { readFileSync } from "node:fs";
const R = process.cwd();
const src = readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.error("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};

// The REAL model, imported rather than re-typed -- the rejection rule under
// test (a case-insensitive duplicate) is its rule, not this files.
const ML = await import(R + "/src/shared/module_lists.mjs");

const body = [
    "let needsRedraw = false;",
    "let saveOk = true;",
    "let saveCalls = 0;",
    "function moduleListsSave() { saveCalls++; return saveOk; }",
    "let spoken = [];",
    "function announce(t) { spoken.push(t); }",
    "let moduleListsTarget = \"\";",
    "let moduleListsPendingName = null;",
    // text_entry.mjs, modelled to its actual contract: open installs the
    // callbacks and raises the flag; SPECIAL_CONFIRM calls onConfirm(buffer)
    // and then closes, whatever the callback did.
    "let entry = null;",
    "function openTextEntry(opts) { entry = opts; }",
    "function isTextEntryActive() { return entry !== null; }",
    "function confirmWith(text) { const e = entry; e.onConfirm(text); entry = null; }",
    grab("moduleListsOpenNameEntry"),
    grab("moduleListsTickPendingName"),
    "return {",
    "  open: (existing, prefill) => moduleListsOpenNameEntry(existing, prefill),",
    "  confirm: confirmWith,",
    "  tick: () => moduleListsTickPendingName(),",
    "  active: () => isTextEntryActive(),",
    "  entry: () => entry,",
    "  spoken: () => spoken,",
    "  clearSpoken: () => { spoken = []; },",
    "  saveCalls: () => saveCalls,",
    "  setSaveOk: (v) => { saveOk = v; },",
    "  target: () => moduleListsTarget,",
    "  pending: () => moduleListsPendingName,",
    "};",
].join("\n");

let h, state;
try {
    state = ML.emptyState();
    ML.createList(state, "Live");
    h = new Function("ModuleLists", "moduleListsState", body)(ML, state);
} catch (e) {
    fail("could not build the moduleListsOpenNameEntry harness: " + e.message);
}

// ---- 1. a rejected NEW name: the keyboard comes back, carrying the text ----
h.open(null);
if (h.entry().initialText !== "") fail("New List must open on an empty buffer, got " + JSON.stringify(h.entry().initialText));
h.confirm("live");
if (h.active())
    fail("the confirm path closes the keyboard unconditionally -- a harness where it is still " +
         "open here is not modelling text_entry.mjs and proves nothing");
if (!h.spoken().includes("Name in use"))
    fail("a duplicate name must say why, said " + JSON.stringify(h.spoken()));
if (h.saveCalls() !== 0) fail("a rejected name must not write the file");
h.tick();
if (!h.active())
    fail("a REJECTED name must leave the keyboard open -- reopening from inside onConfirm cannot " +
         "work, because text_entry.mjs closes it after the callback returns");
if (h.entry().initialText !== "live")
    fail("the reopened keyboard must carry WHAT THE USER TYPED, not a blank buffer or the old " +
         "name -- got " + JSON.stringify(h.entry().initialText));
if (h.entry().title !== "New List")
    fail("the reopened keyboard must still be the same one, got " + JSON.stringify(h.entry().title));
if (h.pending() !== null) fail("servicing the pending name must consume it");

// ---- 2. a rejected RENAME comes back with the typed text, not the old name --
h.clearSpoken();
h.open("Live");
if (h.entry().initialText !== "Live") fail("Rename must open seeded with the current name");
h.confirm("favorites");
h.tick();
if (!h.active()) fail("a rejected rename must leave the keyboard open");
if (h.entry().initialText !== "favorites")
    fail("a rejected rename must come back with the TYPED text, not the old name -- got " +
         JSON.stringify(h.entry().initialText));
if (h.entry().title !== "Rename List") fail("...still the rename keyboard");

// ---- 3. an accepted name writes, and a FAILED write is not a saved one -----
h.clearSpoken();
h.open(null);
h.confirm("Studio");
h.tick();
if (h.active()) fail("an accepted name must not reopen the keyboard");
if (h.saveCalls() !== 1) fail("an accepted name must write once, wrote " + h.saveCalls() + " times");
if (h.spoken().join("|") !== "Created Studio")
    fail("a successful create announces the creation, said " + JSON.stringify(h.spoken()));

h.clearSpoken();
h.setSaveOk(false);
h.open(null);
h.confirm("Rehearsal");
if (h.spoken().length !== 1 || !/save failed/i.test(h.spoken()[0]))
    fail("a FAILED write must not be announced as a persisted change -- the boolean " +
         "moduleListsSave() returns is the only report there is, and discarding it announces " +
         "a save that did not happen. Said " + JSON.stringify(h.spoken()));

console.log("  ok  moduleListsOpenNameEntry(): a rejected name leaves the keyboard OPEN (deferred " +
            "past text_entry.mjs unconditional close) carrying the typed text, and a failed write " +
            "is announced as a failure rather than as a save");
' "$UI"

# The deferral only works if tick() actually services it -- a pending name
# recorded and never re-offered is the same vanished keyboard, one layer down.
command grep -q "moduleListsTickPendingName()" "$UI" || \
    fail "nothing calls moduleListsTickPendingName() -- a rejected name would be recorded and never re-offered"
node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const at = src.indexOf("globalThis.tick = function()");
if (at < 0) { console.log("FAIL: could not find globalThis.tick"); process.exit(1); }
if (src.indexOf("moduleListsTickPendingName();", at) < 0) {
    console.log("FAIL: globalThis.tick() must service moduleListsPendingName -- the reopen " +
                "cannot happen inside onConfirm, so if the tick does not run it, it never runs");
    process.exit(1);
}
console.log("  ok  globalThis.tick() services the pending rejected name");
' "$UI"

# ============================================================================
# 6e. BEHAVIOUR: the per-list actions screen — which rows a list offers, the
#     confirm LATCH, and the save-failure branches.
#
# Three of these are rules that would regress SILENTLY:
#
#  - Favorites offers Clear ALONE. Rename and Delete are absent, not present
#    and refusing: a row that answers a click by doing nothing teaches that
#    the screen is broken. A regression here draws two extra rows that look
#    exactly like working ones until they are clicked.
#  - Back with the confirm up is "No". It must cancel and delete NOTHING --
#    and it must not also leave the screen, because the overlay is what the
#    press was answering.
#  - a Delete or Clear whose write FAILED must not be announced as a saved
#    one. moduleListsSave() returning false is the only report there is; the
#    plan text discarded it, which is the same defect class as
#    move_midi_internal_send returning true on a discarded write.
# ============================================================================

node --input-type=module -e '
import { readFileSync } from "node:fs";
const R = process.cwd();
const src = readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.error("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};

// The REAL model, imported rather than re-typed: whether Favorites is
// protected, and what a delete does to the state, are its rules.
const ML = await import(R + "/src/shared/module_lists.mjs");

const body = [
    "let needsRedraw = false;",
    "let saveOk = true;",
    "let saveCalls = 0;",
    "function moduleListsSave() { saveCalls++; return saveOk; }",
    "let spoken = [];",
    "function announce(t) { spoken.push(t); }",
    "let view = 0;",
    "const VIEWS = { MODULE_LISTS: 1, MODULE_LISTS_EDIT: 2, MODULE_LISTS_ACTIONS: 3 };",
    "function setView(v) { view = v; }",
    "let renameOpened = null;",
    "function moduleListsOpenNameEntry(existing, prefill) { renameOpened = { existing, prefill }; }",
    grab("moduleListsEditRows"),
    grab("moduleListsEditRowLabel"),
    grab("moduleListsClampEditIndex"),
    grab("moduleListsRows"),
    grab("moduleListsRowLabel"),
    grab("moduleListsClampMemberIndex"),
    grab("moduleListsActionRows"),
    grab("moduleListsSelectAction"),
    grab("moduleListsActionsBack"),
    "return {",
    "  actionRows: () => moduleListsActionRows(),",
    "  editRows: () => moduleListsEditRows(),",
    "  rows: () => moduleListsRows(),",
    "  select: () => moduleListsSelectAction(),",
    "  back: () => moduleListsActionsBack(),",
    "  clampIndex: () => moduleListsClampMemberIndex(),",
    "  view: () => view,",
    "  spoken: () => spoken,",
    "  clearSpoken: () => { spoken = []; },",
    "  saveCalls: () => saveCalls,",
    "  setSaveOk: (v) => { saveOk = v; },",
    "  setTarget: (v) => { moduleListsTarget = v; },",
    "  target: () => moduleListsTarget,",
    "  setConfirm: (v) => { moduleListsConfirmDelete = v; },",
    "  confirm: () => moduleListsConfirmDelete,",
    "  setActionIndex: (v) => { moduleListsActionIndex = v; },",
    "  actionIndex: () => moduleListsActionIndex,",
    "  setEditIndex: (v) => { moduleListsEditIndex = v; },",
    "  editIndex: () => moduleListsEditIndex,",
    "  setIndex: (v) => { moduleListsMemberIndex = v; },",
    "  index: () => moduleListsMemberIndex,",
    "  renameOpened: () => renameOpened,",
    "};",
].join("\n");

const build = (state) => {
    try {
        return new Function("ModuleLists", "moduleListsState", "moduleListsModuleId",
                            "moduleListsTarget", "moduleListsConfirmDelete",
                            "moduleListsActionIndex", "moduleListsEditIndex",
                            "moduleListsMemberIndex", body)(
            ML, state, "obxd", "", false, 0, 0, 0);
    } catch (e) {
        fail("could not build the module-lists actions harness: " + e.message);
    }
};

const seed = () => {
    const s = ML.emptyState();
    ML.createList(s, "Live");
    ML.createList(s, "Studio");
    ML.toggleMembership(s, "Favorites", "obxd");
    ML.toggleMembership(s, "Live", "obxd");
    return s;
};

/* ---- 1. Favorites offers Clear ALONE ---------------------------------- */
let h = build(seed());
h.setTarget("Favorites");
let names = h.actionRows().map((r) => r.name);
if (JSON.stringify(names) !== JSON.stringify(["Clear"]))
    fail("Favorites must offer Clear ALONE -- Rename and Delete ABSENT, not present and " +
         "refusing a click. Got " + JSON.stringify(names));
// The case-insensitive half of the same rule: the protection is the models,
// so a list a user typed in lower case is the same protected list.
h.setTarget("favorites");
names = h.actionRows().map((r) => r.name);
if (JSON.stringify(names) !== JSON.stringify(["Clear"]))
    fail("the Favorites protection compares names case-INSENSITIVELY (isProtected), so " +
         "\"favorites\" must offer Clear alone too. Got " + JSON.stringify(names));
h.setTarget("Live");
names = h.actionRows().map((r) => r.name);
if (JSON.stringify(names) !== JSON.stringify(["Rename", "Delete", "Clear"]))
    fail("an ordinary list must offer all three actions, got " + JSON.stringify(names));

/* ---- 2. Delete ARMS a confirm; it does not delete ---------------------- */
h = build(seed());
h.setTarget("Live");
h.setActionIndex(1);              // Delete
h.select();
if (h.confirm() !== true) fail("clicking Delete must raise the confirm, not delete");
if (h.saveCalls() !== 0) fail("arming the confirm must not write the file");
if (h.editRows().length !== 3) fail("arming the confirm must not remove the list");
if (!h.spoken().join("|").includes("Delete Live?"))
    fail("arming the confirm must ASK, said " + JSON.stringify(h.spoken()));

/* ---- 3. Back with the confirm up cancels, and deletes NOTHING ---------- */
h.clearSpoken();
h.back();
if (h.confirm() !== false) fail("Back with the confirm up must disarm it");
if (h.view() !== 0)
    fail("Back is answering the OVERLAY, so it must not also leave the actions screen -- " +
         "view moved to " + h.view());
if (h.editRows().map((r) => r.name).join(",") !== "Favorites,Live,Studio")
    fail("Back with the confirm up must leave the list INTACT, got " +
         JSON.stringify(h.editRows().map((r) => r.name)));
if (h.saveCalls() !== 0) fail("a cancelled delete must not write the file");
if (!/^Cancelled, Live, Delete$/.test(h.spoken().join("|")))
    fail("a cancelled delete must SAY it was cancelled and then where the user is -- announcing " +
         "the bare target repeats the word already in the header, so a cancel reads as a stray " +
         "repeat. Said " + JSON.stringify(h.spoken()));
// And the next click must be the ordinary row action again, not the delete
// the latch was holding.
h.select();
if (h.editRows().length !== 3)
    fail("the click after a cancelled confirm must NOT delete -- the latch was disarmed, " +
         "so the click belongs to the Delete row and re-arms");
if (h.confirm() !== true) fail("...it re-arms the confirm instead");

/* ---- 4. Confirming deletes, writes, and lands on the Edit screen ------- */
h.clearSpoken();
h.select();
if (h.confirm() !== false) fail("confirming must disarm the latch");
if (h.editRows().map((r) => r.name).join(",") !== "Favorites,Studio")
    fail("a confirmed delete must remove the list, got " +
         JSON.stringify(h.editRows().map((r) => r.name)));
if (h.saveCalls() !== 1) fail("a confirmed delete must write once, wrote " + h.saveCalls());
if (h.spoken().join("|") !== "Deleted Live. Edit Lists, Favorites, 1 module")
    fail("a successful delete announces the deletion AND the screen it lands on with the row " +
         "under the resolved cursor -- the cursor has just slid onto a list the user did not " +
         "choose, and announce() is not a queue so it has to be ONE call. Said " +
         JSON.stringify(h.spoken()));
if (h.view() !== 2) fail("a confirmed delete returns to the Edit Lists screen, view " + h.view());
if (h.target() !== "") fail("a confirmed delete clears the target, got " + JSON.stringify(h.target()));

/* ---- 5. A delete that SHRINKS the screens leaves both cursors in range -- */
// The Edit cursor first: it was sitting on the last of three rows, and the
// list it pointed at is the one that just went.
h = build(seed());
h.setTarget("Studio");
h.setEditIndex(2);
h.setActionIndex(1);
h.select();                       // arm
h.select();                       // confirm
if (h.editRows().length !== 2) fail("the delete did not take");
if (h.editIndex() !== 1)
    fail("a delete that shrinks the Edit screen must re-resolve its cursor -- index " +
         h.editIndex() + " against " + h.editRows().length + " rows");
// Then the membership cursor, on the way back out to MODULE_LISTS. Its rows
// are the lists PLUS the two management doors, so a delete shrinks it too.
h.setIndex(4);                    // the last row of the pre-delete screen
h.clampIndex();
if (h.index() !== 3)
    fail("a delete that shrinks the membership screen must leave moduleListsMemberIndex in RANGE " +
         "on the way back -- index " + h.index() + " against " + h.rows().length + " rows");
if (h.rows()[h.index()] === undefined)
    fail("moduleListsMemberIndex must address a row that EXISTS, so the label and the click do not " +
         "read undefined");

/* ---- 6. A FAILED write is not announced as a saved one ----------------- */
h = build(seed());
h.setSaveOk(false);
h.setTarget("Live");
h.setActionIndex(1);
h.select();                       // arm
h.clearSpoken();
h.select();                       // confirm
if (h.spoken().length !== 1 || !/save failed/i.test(h.spoken()[0]))
    fail("a DELETE whose write failed must not be announced as a persisted one -- the boolean " +
         "moduleListsSave() returns is the only report there is. Said " + JSON.stringify(h.spoken()));

h = build(seed());
h.setSaveOk(false);
h.setTarget("Favorites");
h.setActionIndex(0);              // Clear, the only row Favorites offers
h.clearSpoken();
h.select();
if (h.spoken().length !== 1 || !/save failed/i.test(h.spoken()[0]))
    fail("a CLEAR whose write failed must not be announced as a persisted one, said " +
         JSON.stringify(h.spoken()));

/* ---- 7. ...and a successful Clear says so, once ------------------------ */
h = build(seed());
h.setTarget("Favorites");
h.setActionIndex(0);
h.select();
if (h.saveCalls() !== 1) fail("Clear must write once, wrote " + h.saveCalls());
if (h.spoken().join("|") !== "Cleared Favorites")
    fail("a successful clear announces the clear, said " + JSON.stringify(h.spoken()));
if (h.editRows()[0].value !== "0")
    fail("Clear must empty the members -- the Edit row still reads " + h.editRows()[0].value);

/* ---- 8. Rename seeds the keyboard with the CURRENT name ---------------- */
h = build(seed());
h.setTarget("Live");
h.setActionIndex(0);              // Rename
h.select();
const opened = h.renameOpened();
if (!opened) fail("Rename must open the keyboard");
if (opened.existing !== "Live")
    fail("Rename must open seeded with the CURRENT name, got " + JSON.stringify(opened.existing));
if (h.saveCalls() !== 0) fail("opening the rename keyboard must not write anything");

/* ---- 9. Back with NO confirm up: one level out, cursor re-resolved -------- */
// Every Back test above runs with the latch ARMED, so the ordinary branch --
// the clamp, the setView and the announce -- was never executed by anything.
h = build(seed());
h.setTarget("Studio");
h.setEditIndex(9);                // out of range for a 3-row screen
h.setActionIndex(0);
h.clearSpoken();
h.back();
if (h.confirm() !== false) fail("Back with no confirm up must leave the latch disarmed");
if (h.view() !== 2)
    fail("Back with no confirm up must leave the actions screen for Edit Lists, view " + h.view());
if (h.editIndex() !== 2)
    fail("Back must re-resolve the Edit cursor on the way -- it was out of range at 9 and the " +
         "screen has " + h.editRows().length + " rows, got " + h.editIndex());
if (h.spoken().join("|") !== "Edit Lists, Studio, 0 modules")
    fail("Back must name the screen AND the row under the resolved cursor, as every other " +
         "arrival on that screen does. Said " + JSON.stringify(h.spoken()));

console.log("  ok  the per-list actions screen: Favorites offers Clear ALONE (case-insensitively), " +
            "Delete arms a confirm that Back cancels without deleting, a confirmed delete " +
            "re-resolves both shrunken cursors, a failed write is announced as a failure, and " +
            "both Back branches say what changed and where the user landed");
' "$UI"

# The membership cursor is clamped by the BACK case, not by the drawer -- a
# clamp that exists and is never called is the out-of-range row it was written
# to prevent.
node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const at = src.indexOf("case VIEWS.MODULE_LISTS_EDIT:\n            /*");
const seg = at < 0 ? "" : src.slice(at, at + 600);
const clamp = seg.indexOf("moduleListsClampMemberIndex()");
const set = seg.indexOf("setView(VIEWS.MODULE_LISTS)");
if (at < 0 || clamp < 0 || set < 0 || clamp > set) {
    console.log("FAIL: handleBack on MODULE_LISTS_EDIT must call moduleListsClampMemberIndex() BEFORE " +
                "returning to MODULE_LISTS -- a delete on the Edit screen removed a row from the " +
                "screen below, and the label read on arrival is taken at the old index");
    process.exit(1);
}
console.log("  ok  Back off Edit Lists re-resolves the membership cursor before returning");
' "$UI"

# Three rules in the module-lists wiring that a behaviour harness cannot see,
# because they live in switch cases node cannot load.
#
#  - the three views must be drawn from the MAIN render switch, not only from
#    dispatchCoRunDraw(). This exact mistake has already been made once here:
#    PARAM_PAGES landed its case in the co-run dispatcher alone, the wiring
#    test passed because it only asked whether the case existed ANYWHERE, and
#    on device the view was entered, the controller ran, pages were announced,
#    and nothing was ever drawn. See tests/host/test_param_pages_wiring.sh.
#  - the confirm LATCH must be cleared when the actions screen is ENTERED.
#    Inheriting it means the first click on a freshly opened list deletes it.
#  - the jog must be refused while the confirm is up: it is drawn LAST so it
#    is fed FIRST, and a cursor that moves under an overlay changes which row
#    the Back that dismisses it announces.
node -e '
const fs = require("fs");
const s = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const tickAt = s.indexOf("globalThis.tick = function()");
if (tickAt < 0) fail("could not locate globalThis.tick to check the render switch");
for (const view of ["MODULE_LISTS", "MODULE_LISTS_EDIT", "MODULE_LISTS_ACTIONS"]) {
    const re = new RegExp("case VIEWS\\." + view + ":(?![A-Z_])", "g");
    const at = [...s.matchAll(re)].map((m) => m.index);
    const draws = at.filter((i) => /draw[A-Za-z]*\(\)/.test(s.slice(i, i + 120)));
    if (!draws.length) fail(view + " is never drawn");
    if (!draws.some((i) => i > tickAt)) {
        fail(view + " is only drawn from the co-run dispatcher -- the main render switch " +
             "inside globalThis.tick has no case, so entering the view draws NOTHING. That " +
             "is the PARAM_PAGES bug, one view over.");
    }
}

/* The latch is cleared on ENTRY to the actions screen, before the setView. */
{
    /* Two cases open with this line -- the jog and the click. The click is
     * the one that ENTERS the actions screen, so it is the one whose body
     * contains the setView. */
    const cases = [...s.matchAll(/case VIEWS\.MODULE_LISTS_EDIT: \{/g)].map((m) => m.index);
    const at = cases.find((i) => s.slice(i, i + 1400).includes("setView(VIEWS.MODULE_LISTS_ACTIONS)"));
    if (at === undefined) fail("could not find the MODULE_LISTS_EDIT select case");
    const seg = s.slice(at, at + 1400);
    const clear = seg.indexOf("moduleListsConfirmDelete = false");
    const set = seg.indexOf("setView(VIEWS.MODULE_LISTS_ACTIONS)");
    if (clear < 0 || set < 0 || clear > set) {
        fail("clicking a list must DISARM moduleListsConfirmDelete before entering the " +
             "actions screen -- the latch is inherited otherwise, and the first click on a " +
             "freshly opened list deletes it without asking");
    }
}

/* The jog is refused while the confirm is up. */
{
    const at = s.indexOf("case VIEWS.MODULE_LISTS_ACTIONS: {");
    if (at < 0) fail("could not find the MODULE_LISTS_ACTIONS jog case");
    const seg = s.slice(at, at + 700);
    const guard = seg.indexOf("if (moduleListsConfirmDelete) break;");
    const move = seg.indexOf("moduleListsActionIndex = Math.max");
    if (guard < 0 || move < 0 || guard > move) {
        fail("the jog must early-out on moduleListsConfirmDelete BEFORE moving the cursor -- " +
             "the overlay is drawn LAST so it is fed FIRST, and a cursor that moves underneath " +
             "it changes which row the Back that dismisses it announces");
    }
}

console.log("  ok  all three module-lists views are drawn from the MAIN render switch, the " +
            "confirm latch is disarmed on entry, and the jog is refused while it is up");
' "$UI"

# ============================================================================
# 7. BEHAVIOUR: doSavePreset (shadow_ui_presets.mjs) treats a FAILED read
#    (null) as the error and a DECLARED-EMPTY state ("") as writable — the
#    same tri-state rule overwriteUserPreset already applies. enterPresetSaveAs
#    is a new, grid-only entry point onto this same function, so a component
#    that legitimately declares "" state can now reach it for the first time
#    from a path this task added.
# ============================================================================

PRESETS_MJS="src/shadow/shadow_ui_presets.mjs"
[ -f "$PRESETS_MJS" ] || fail "missing $PRESETS_MJS"

node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui_presets.mjs");
    return m[0];
};

const runWith = (stateJson) => {
    const body = [
        "let writeCalls = [];",
        "let errorCalls = 0;",
        "let announceCalls = [];",
        "let onSavedCalls = [];",
        "const ctx = {",
        "  getSlotStateWithRetry: () => " + JSON.stringify(stateJson) + ",",
        "  needsRedraw: false,",
        "  onPresetSaved: (...a) => onSavedCalls.push(a),",
        "};",
        "function showSaveError() { errorCalls++; }",
        "function announce(t) { announceCalls.push(t); }",
        "function presetDir(id) { return \"/presets/\" + id; }",
        "function uniquePresetName(n) { return n; }",
        "function safeFileStem(n) { return n; }",
        "function uniquePresetPath(dir, stem) { return dir + \"/\" + stem + \".json\"; }",
        "const PRESET_VERSION = 1;",
        "let presetPrefix = \"synth\";",
        "let presetModule = \"obxd\";",
        "let presets = [];",
        "let selectedPreset = 0;",
        "function loadPresetList() {}",
        "function host_write_file(path, payload) { writeCalls.push([path, payload]); return true; }",
        grab("doSavePreset"),
        "doSavePreset(1, \"My Preset\");",
        "return { writeCalls, errorCalls, announceCalls, onSavedCalls };",
    ].join("\n");
    try {
        return new Function(body)();
    } catch (e) {
        fail("doSavePreset behaviour (stateJson=" + JSON.stringify(stateJson) + "): " + e.message);
    }
};

const failed = runWith(null);
if (failed.writeCalls.length !== 0) fail("a failed (null) state read must not write a preset file, wrote " + JSON.stringify(failed.writeCalls));
if (failed.errorCalls !== 1) fail("a failed (null) state read must raise exactly one save error, raised " + failed.errorCalls);
if (failed.onSavedCalls.length !== 0) fail("a failed (null) state read must not notify onPresetSaved, notified " + JSON.stringify(failed.onSavedCalls));

// A DECLARED-EMPTY read ("") is a real answer, not an error -- it must be
// written through, exactly like overwriteUserPreset already does.
const empty = runWith("");
if (empty.errorCalls !== 0) fail("a declared-empty (\"\") state must NOT be treated as a save error, raised " + empty.errorCalls);
if (empty.writeCalls.length !== 1) fail("a declared-empty (\"\") state must still write a preset file, wrote " + JSON.stringify(empty.writeCalls));
if (empty.onSavedCalls.length !== 1) fail("a successful save of a declared-empty state must still notify onPresetSaved, notified " + JSON.stringify(empty.onSavedCalls));

console.log("  ok  doSavePreset(): a FAILED (null) state read refuses to save; a DECLARED-EMPTY " +
            "(\"\") state is a real answer and is written through");
' "$PRESETS_MJS"

echo "PASS"
