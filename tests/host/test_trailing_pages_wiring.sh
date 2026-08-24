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
    grab("componentTrailingMenus"),
    "return {",
    "  run: (slot, key, prefix) => componentTrailingMenus(slot, key, prefix),",
    "  setRecord: (r) => { userRecord = r; },",
    "  setLiveBlob: (b) => { liveBlob = b; },",
    "  setChainConfigs: (c) => { chainConfigs = c; },",
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

const moduleActions = actions(r.clean[1]);
if (moduleActions.join(",") !== "swap_module,remove_module")
    fail("Module page must offer exactly Swap Module then Remove Module, got " + moduleActions.join(","));

console.log("  ok  componentTrailingMenus(): [] when empty; (none)/Load+SaveAs-only when no " +
            "record; Save+Delete when a record exists; the real presetRowValue() decides the " +
            "* (imported from current_preset.mjs, not re-typed)");
' "$UI"

# ============================================================================
# 6. BEHAVIOUR: runComponentActionFromGrid — all six actions dispatch.
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
    grab("runComponentActionFromGrid"),
    "const seen = {};",
    "for (const action of [\"up_load\",\"up_save\",\"up_save_as\",\"up_delete\",\"swap_module\",\"remove_module\"]) {",
    "  calls = [];",
    "  runComponentActionFromGrid(1, \"synth\", action);",
    "  seen[action] = calls.slice();",
    "}",
    "return { seen, setRecordCalls };",
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
console.log("  ok  runComponentActionFromGrid(): all six actions (up_load, up_save, " +
            "up_save_as, up_delete, swap_module, remove_module) reach their real handlers");

// remove_module must clear the record BEFORE the removal write, not after —
// only remove_module calls setUserPresetRecord in this harness (the other
// five reach it only through onUserPresetSaved/Loaded/Deleted, which are
// stubbed to markers above, not to the real setter), so ONE call across all
// six iterations, with a null record, is exactly what a correct clear looks
// like. Evidence gathered above and asserted here, not left to sit unread.
const wantSetRecordCalls = [[1, "synth", null]];
if (JSON.stringify(r.setRecordCalls) !== JSON.stringify(wantSetRecordCalls)) {
    fail("remove_module must clear the grid record with setUserPresetRecord(slot, prefix, null) " +
         "before the removal write -- expected " + JSON.stringify(wantSetRecordCalls) +
         ", got " + JSON.stringify(r.setRecordCalls));
}
console.log("  ok  remove_module clears the User Preset record (setUserPresetRecord(1, \"synth\", null))");
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
