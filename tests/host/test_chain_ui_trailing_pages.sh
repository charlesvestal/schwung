#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The two trailing pages ("My Presets" / "Module") reach a module that draws
# its OWN param pages.
#
# Every component the shadow UI paginates gets them, because enterParamPages
# hands componentParamPagesIo to the controller and the controller appends
# whatever io.trailingMenus returns (test_trailing_pages_wiring.sh pins that).
# A module shipping ui_chain.js builds its own controller, so it got neither —
# a drum machine with a pad-select editor could not save a preset while a synth
# on the stock editor could. The pages are not a property of who drew the grid.
#
# Behaviour over structure, in this suite's house style: setupModuleParamShims
# and clearModuleParamShims are LIFTED out of shadow_ui.js with `new Function`
# and actually RUN against stubs, because a grep for the binding can pass while
# the code underneath never executes. Each lift ends on a marker that only
# appears if the pasted body ran.

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "  ok  $*"; }

UI="src/shadow/shadow_ui.js"
API="docs/API.md"

[ -f "$UI" ] || fail "missing $UI"
command -v node >/dev/null 2>&1 || fail "node is required"

# ---------------------------------------------------------------------------
# 1. BEHAVIOUR: the bindings install, carry slot+component, and clear.
# ---------------------------------------------------------------------------

node - "$UI" <<'NODE' || fail "chain-UI trailing bindings do not behave"
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");

function lift(name) {
    const i = src.indexOf(`function ${name}(`);
    if (i < 0) throw new Error(`${name} not found`);
    /* brace-match the declaration */
    let d = 0, started = false, end = i;
    for (let p = i; p < src.length; p++) {
        const c = src[p];
        if (c === "{") { d++; started = true; }
        else if (c === "}") { d--; if (started && d === 0) { end = p + 1; break; } }
    }
    return src.slice(i, end);
}

const calls = [];
const g = {};
/* Only what the two lifted functions actually touch. If either grows a new
 * dependency this throws, which is the point: the test breaks loudly rather
 * than measuring a stub. */
const env = {
    globalThis: g,
    paramShimsInstalled: false,
    originalHostGetParam: undefined,
    originalHostSetParam: undefined,
    getComponentParamPrefix: (k) => (k === "synth" ? "synth" : k),
    getSlotParam: () => "",
    setSlotParam: () => true,
    slotChainComponentIndex: () => 0,
    unloadModuleUi: () => {},
    enterComponentSelect: () => {},
    componentTrailingMenus: (slot, comp, prefix) => {
        calls.push(["menus", slot, comp, prefix]);
        return [{ name: "My Presets", entries: [{ label: "Preset" }] },
                { name: "Module", entries: [{ label: "Swap Module" }] }];
    },
    runComponentActionFromGrid: (slot, comp, action) => {
        calls.push(["action", slot, comp, action]);
        return action === "up_load";      /* pretend Load opened a screen */
    },
    scanForToolModules: () => [],
    toolModules: [],
};

const names = Object.keys(env);
const body = lift("setupModuleParamShims") + "\n" + lift("clearModuleParamShims") +
             "\nsetupModuleParamShims(2, 'synth');\nreturn 'ran';";
const marker = new Function(...names, body)(...names.map((n) => env[n]));
if (marker !== "ran") throw new Error("lifted body did not execute");

if (typeof g.shadow_component_trailing_menus !== "function")
    throw new Error("shadow_component_trailing_menus was not installed");
if (typeof g.shadow_component_run_action !== "function")
    throw new Error("shadow_component_run_action was not installed");

const menus = g.shadow_component_trailing_menus();
if (!Array.isArray(menus) || menus.length !== 2)
    throw new Error("trailing menus did not come through");
if (menus[0].name !== "My Presets" || menus[1].name !== "Module")
    throw new Error("wrong pages: " + menus.map((m) => m.name).join(","));
/* Bound with the slot and component already applied — the module never sees
 * them, exactly as host_swap_module works. */
const m = calls.find((c) => c[0] === "menus");
if (m[1] !== 2 || m[2] !== "synth")
    throw new Error("menus not bound to the slot/component: " + JSON.stringify(m));

if (g.shadow_component_run_action("up_save") !== false)
    throw new Error("an action that opens nothing must report false");
if (g.shadow_component_run_action("up_load") !== true)
    throw new Error("an action that opens a screen must report true");
if (g.shadow_component_run_action("") !== false)
    throw new Error("an empty action must be inert");
const a = calls.find((c) => c[0] === "action");
if (a[1] !== 2 || a[2] !== "synth")
    throw new Error("action not bound to the slot/component: " + JSON.stringify(a));

/* And they must not outlive the module that owned them. */
const clear = new Function(...names, lift("clearModuleParamShims") +
                           "\nclearModuleParamShims();\nreturn 'ran';");
if (clear(...names.map((n) => env[n])) !== "ran")
    throw new Error("clear body did not execute");
if (g.shadow_component_trailing_menus !== undefined ||
    g.shadow_component_run_action !== undefined)
    throw new Error("bindings survived clearModuleParamShims");
NODE
pass "bindings install, carry slot+component, report screen-opening, and clear"

# ---------------------------------------------------------------------------
# 2. The host's own path is untouched — it still goes through the one helper.
# ---------------------------------------------------------------------------

grep -q "trailingMenus: () => componentTrailingMenus(slotIndex, componentKey, prefix)" "$UI" \
    || fail "componentParamPagesIo no longer supplies trailingMenus"
pass "the shadow UI's own trailing-menu wiring is unchanged"

# ---------------------------------------------------------------------------
# 3. Documented, with the guard a module needs on an older host.
# ---------------------------------------------------------------------------

grep -q "shadow_component_trailing_menus" "$API" || fail "$API does not document the binding"
grep -q "shadow_component_run_action"     "$API" || fail "$API does not document the action call"
grep -q "PAGE_MENU"                       "$API" || fail "$API does not say the pages arrive as PAGE_MENU"
pass "docs/API.md documents both calls, the guard, and the page kind"

# ---------------------------------------------------------------------------
# 4. BEHAVIOUR: the hand-off bookkeeping is right for BOTH grids.
#
#    This is the bug hardware found: runComponentActionFromGrid decided "did
#    this action open a screen?" by testing view !== PARAM_PAGES, which is
#    only true-for-the-right-reason when the caller WAS the host's param
#    pages. From a module-owned grid (COMPONENT_EDIT) every action, Save As
#    included, looked like a hand-off, armed a return to a grid the module
#    cannot host, and the device spun on synth:ui_hierarchy reads.
# ---------------------------------------------------------------------------

node - "$UI" <<'NODE' || fail "hand-off bookkeeping is wrong for a module-owned grid"
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
const lift = (name) => {
    const i = src.indexOf(`function ${name}(`);
    if (i < 0) throw new Error(`${name} not found`);
    let d = 0, started = false;
    for (let p = i; p < src.length; p++) {
        if (src[p] === "{") { d++; started = true; }
        else if (src[p] === "}") { d--; if (started && d === 0) return src.slice(i, p + 1); }
    }
    throw new Error(`${name}: unbalanced`);
};
const run = (startView, action) => {
    const body = `
        let view = ${startView};
        const VIEWS = { PARAM_PAGES: 0, COMPONENT_EDIT: 5, CHAIN_EDIT: 3, GLOBAL_SETTINGS: 7, MODULE_LISTS: 9 };
        function gridActionOpenedSomething(...c) { return c.some(Boolean); }
        let componentModalFromGrid = false, componentGridReturnSlot = -1, componentGridReturnKey = "";
        let componentGridReturnModuleUi = false, componentGridReturnEnter = true;
        let componentHelpReturnSlot = -1, componentHelpReturnKey = "", componentHelpReturnModuleUi = false;
        let moduleListsSlot = -1, moduleListsKey = "", moduleListsModuleId = "", moduleListsMemberIndex = 0;
        let moduleListsReturnModuleUi = false, moduleListsEditIndex = 0, moduleListsActionIndex = 0;
        let moduleListsTarget = "", moduleListsConfirmDelete = false, moduleListsPendingName = null;
        let helpNavStack = [], helpDetailScrollState = null, needsRedraw = false;
        let moduleListsCorrupt = false;
        function debugLog() {}
        const chainConfigs = { 1: { synth: { module: "9w9" } } };
        function createEmptyChainConfig() { return {}; }
        function getChainComponentModule(cfg, k) { return cfg && cfg[k]; }
        function getComponentParamPrefix(k) { return k; }
        function getUserPresetRecord() { return { name: "Kit A" }; }
        function setUserPresetRecord() {}
        function setView(v) { view = v; }
        function announce() {}
        function exitParamPages() {}
        function paramPagesExitMenu() {}
        function getSlotStateWithRetry() { return "{}"; }
        function overwriteUserPreset() { return true; }
        function onUserPresetSaved() {}
        /* the ones that hand off move the view; the ones that stay put do not */
        function enterPresetBrowser() { view = 6; }
        function enterPresetSaveAs() { /* keyboard overlay: view untouched */ }
        function enterPresetDeleteConfirm() { view = 8; }
        function enterComponentSelect() { view = 4; }
        function applyChainComponentPick() { view = VIEWS.CHAIN_EDIT; }
        function slotChainComponentIndex() { return 0; }
        function getModuleHelpChildren() { return [{ title: "t" }]; }
        function getModuleDisplayName() { return "9W9"; }
        function moduleListsLoad() {}
        function moduleListsRowLabel() { return ""; }
        ${lift("runComponentActionFromGrid")}
        const ret = runComponentActionFromGrid(1, "synth", ${JSON.stringify(action)});
        return { ret, armed: componentModalFromGrid, moduleUi: componentGridReturnModuleUi,
                 helpModuleUi: componentHelpReturnModuleUi, listsModuleUi: moduleListsReturnModuleUi, view };
    `;
    return new Function(body)();
};

/* From the MODULE grid (COMPONENT_EDIT): */
let r = run(5, "up_save_as");
if (r.armed) throw new Error("Save As from a module grid must NOT arm a hand-off (it is a keyboard overlay)");
r = run(5, "up_save");
if (r.armed) throw new Error("Save from a module grid must NOT arm a hand-off");
r = run(5, "up_load");
if (!r.armed) throw new Error("Load from a module grid must arm the hand-off");
if (r.moduleUi !== true) throw new Error("...and must record that it came from a MODULE grid");
r = run(5, "module_help");
if (r.helpModuleUi !== true) throw new Error("Module Help from a module grid must record the module origin");
r = run(5, "module_lists");
if (r.listsModuleUi !== true) throw new Error("Add to List from a module grid must record the module origin");

/* From the HOST grid (PARAM_PAGES) nothing changes: */
r = run(0, "up_save_as");
if (r.armed) throw new Error("host grid: Save As must not arm (unchanged)");
r = run(0, "up_load");
if (!r.armed) throw new Error("host grid: Load must arm (unchanged)");
if (r.moduleUi !== false) throw new Error("host grid: origin must be recorded as the host, not a module");
NODE
pass "Save As from a module grid stays put; Load/Help/Lists record the module origin; host grid unchanged"

node - "$UI" <<'NODE' || fail "the return does not go back through the module"
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
const lift = (name) => {
    const i = src.indexOf(`function ${name}(`);
    let d = 0, started = false;
    for (let p = i; p < src.length; p++) {
        if (src[p] === "{") { d++; started = true; }
        else if (src[p] === "}") { d--; if (started && d === 0) return src.slice(i, p + 1); }
    }
    throw new Error(name);
};
const scenario = (name, setup) => {
    const body = `
        let calls = [];
        let view = 3;
        const VIEWS = { CHAIN_EDIT: 3, COMPONENT_EDIT: 5 };
        let selectedSlot = 1, needsRedraw = false;
        let componentModalFromGrid = true, componentGridReturnSlot = 1, componentGridReturnKey = "synth";
        let componentGridReturnModuleUi = false, componentGridReturnEnter = false, componentGridReturnModule = "";
        let componentHelpReturnSlot = 1, componentHelpReturnKey = "synth", componentHelpReturnModuleUi = false;
        let helpNavStack = [], helpDetailScrollState = null;
        let moduleListsSlot = 1, moduleListsKey = "synth", moduleListsModuleId = "9w9", moduleListsReturnModuleUi = false;
        let moduleListsEditIndex = 0, moduleListsActionIndex = 0, moduleListsTarget = "", moduleListsConfirmDelete = false, moduleListsPendingName = null;
        let chainConfigs = { 1: { synth: { module: "9w9" } } };
        function isTextEntryActive() { return false; }
        function getChainComponentModule(cfg, k) { return cfg && cfg[k]; }
        function getComponentParamPrefix(k) { return k; }
        function componentParamPagesIo() { return {}; }
        function paramPagesChromeFor() { return {}; }
        function setView(v) { view = v; }
        function enterParamPages(slot, key, prefix, page) { calls.push("host:" + page); }
        function unloadModuleUi() { calls.push("unload"); }
        function enterComponentEditFallback(slot, key) { calls.push("module:" + key); }
        function restoreModuleUiPage(name, enter) { calls.push("restore:" + name + ":" + enter); }
        function openComponentEditor(slot, key, mfx) { calls.push("door:" + key + ":" + mfx); }
        ${setup}
        ${lift(name)}
        const fired = ${name}();
        return { fired, calls, gridFlag: componentGridReturnModuleUi, helpFlag: componentHelpReturnModuleUi, listsFlag: moduleListsReturnModuleUi };
    `;
    return new Function(body)();
};

/* grid return */
let r = scenario("maybeReturnToComponentGrid", "componentGridReturnModuleUi = true;");
if (!r.calls.includes("module:synth")) throw new Error("module origin must return through the module: " + r.calls);
if (r.calls.some(c => c.startsWith("host:"))) throw new Error("module origin must NOT re-enter through enterParamPages: " + r.calls);
if (!r.calls.includes("unload")) throw new Error("the stale module UI must be unloaded before reloading");
if (r.gridFlag !== false) throw new Error("the flag must be consumed");
if (!r.calls.includes("restore:My Presets:false")) throw new Error("must land the reloaded module on My Presets with the saved disposition: " + r.calls);
r = scenario("maybeReturnToComponentGrid", "");
if (!r.calls.includes("host:My Presets")) throw new Error("host origin must still land on My Presets: " + r.calls);
if (r.calls.some(c => c.startsWith("module:"))) throw new Error("host origin must not touch the module path");
r = scenario("maybeReturnToComponentGrid", "componentGridReturnModuleUi = true; chainConfigs = { 1: {} };");
if (r.fired) throw new Error("a removed module must not be re-entered");
if (!r.calls.includes("unload")) throw new Error("...but its stale UI must be unloaded");

/* a completed SWAP from the module grid: the position now holds a DIFFERENT
 * module. Neither door assumption survives that -- the new module may have a
 * hierarchy (stock editor) and may still be loading. Old UI out, then the one
 * door every editor opens with, page 1, nothing restored. Hardware: 9W9 ->
 * 303 landed on a fallback with nothing to draw. */
r = scenario("maybeReturnToComponentGrid",
             'componentGridReturnModuleUi = true; componentGridReturnModule = "9w9"; chainConfigs = { 1: { synth: { module: "303" } } };');
if (!r.fired) throw new Error("swap: must fire");
if (!r.calls.includes("unload")) throw new Error("swap: the old module UI must be unloaded: " + r.calls);
if (!r.calls.includes("door:synth:-1")) throw new Error("swap: must open the NEW module through openComponentEditor: " + r.calls);
if (r.calls.some(c => c.startsWith("module:") || c.startsWith("host:") || c.startsWith("restore:")))
    throw new Error("swap: must not re-enter the old grid's door or restore its page: " + r.calls);
/* ...and the same swap from the STOCK grid into a module-UI module (303 -> 9W9):
 * enterParamPages would be a contract read nobody answers. */
r = scenario("maybeReturnToComponentGrid",
             'componentGridReturnModule = "303";');
if (!r.calls.includes("door:synth:-1") || r.calls.some(c => c.startsWith("host:")))
    throw new Error("swap from stock grid: must go through the door, not enterParamPages: " + r.calls);

/* help return */
r = scenario("maybeReturnToComponentHelp", "componentHelpReturnModuleUi = true;");
if (!r.calls.includes("module:synth") || r.calls.some(c => c.startsWith("host:")))
    throw new Error("help: module origin must return through the module: " + r.calls);
if (!r.calls.includes("restore:Module:true")) throw new Error("help: must land on the Module page, entered: " + r.calls);
r = scenario("maybeReturnToComponentHelp", "");
if (!r.calls.includes("host:Module")) throw new Error("help: host origin unchanged: " + r.calls);

/* lists return */
r = scenario("exitModuleLists", "moduleListsReturnModuleUi = true;");
if (!r.calls.includes("module:synth") || r.calls.some(c => c.startsWith("host:")))
    throw new Error("lists: module origin must return through the module: " + r.calls);
if (!r.calls.includes("restore:Module:true")) throw new Error("lists: must land on the Module page, entered: " + r.calls);
r = scenario("exitModuleLists", "");
if (!r.calls.includes("host:Module")) throw new Error("lists: host origin unchanged: " + r.calls);
NODE
pass "every hand-off returns through the module when it came from one, lands on the page it left from, and through enterParamPages when it did not"

node - "$UI" <<'NODE' || fail "restoreModuleUiPage is not gated"
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
const i = src.indexOf("function restoreModuleUiPage(");
if (i < 0) throw new Error("restoreModuleUiPage missing");
const fn = src.slice(i, src.indexOf("\n}\n", i) + 3);
const run = (view, hook) => new Function(`
    const VIEWS = { COMPONENT_EDIT: 5 };
    let view = ${view};
    let got = null;
    const loadedModuleUi = ${hook ? "{ restorePage: (n, o) => { got = [n, o.enter]; } }" : "{}"};
    ${fn}
    restoreModuleUiPage("Module", true);
    return got;
`)();
const a = run(5, true);
if (!a || a[0] !== "Module" || a[1] !== true) throw new Error("must hand the name and disposition to the module: " + JSON.stringify(a));
if (run(3, true) !== null) throw new Error("must not fire from another view");
if (run(5, false) !== null) throw new Error("must be inert for a chain UI that declares no hook");
NODE
pass "the reloaded module is told which page to land on, only while it is on screen"

node - "$UI" <<'NODE' || fail "the module is not told its presets changed"
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
const i = src.indexOf("function notifyModuleUiPresetsChanged(");
if (i < 0) throw new Error("notifyModuleUiPresetsChanged missing");
const fn = src.slice(i, src.indexOf("\n}\n", i) + 3);
const run = (view, hook) => new Function(`
    const VIEWS = { COMPONENT_EDIT: 5 };
    let view = ${view};
    let hits = 0;
    const loadedModuleUi = ${hook ? "{ onPresetsChanged: () => { hits++; } }" : "{}"};
    ${fn}
    notifyModuleUiPresetsChanged();
    return hits;
`)();
if (run(5, true) !== 1) throw new Error("must call the hook when the module UI is on screen");
if (run(3, true) !== 0) throw new Error("must not call it from another view");
if (run(5, false) !== 0) throw new Error("must be inert for a chain UI that declares no hook");
if (!src.match(/function recordUserPresetFromDevice[^]*?notifyModuleUiPresetsChanged\(\);/))
    throw new Error("recordUserPresetFromDevice must notify (covers Save and Load)");
NODE
pass "the module is told when a preset is saved or loaded, only while it is on screen"

echo "OK: chain-UI trailing pages"
