// Repro for the orphaned-exit bug: the Tools shortcut (SHADOW_UI_FLAG_JUMP_TO_TOOLS)
// tears down an active overtake module by calling exitOvertakeMode(), which only
// *arms* the exit (overtakeExitPending = true) and relies on the next
// VIEWS.OVERTAKE_MODULE tick to run completeOvertakeExit(). But the very next
// statement is enterToolsMenu(), so the view leaves OVERTAKE_MODULE and that tick
// never comes. Consequences observed on device:
//
//   1. overtake_mode never drops to 0, so the C-side never replays Move's cached
//      LED snapshot — the LEDs never come back.
//   2. overtakeExitPending stays true forever. The exit branch in the
//      OVERTAKE_MODULE tick is checked BEFORE the init branch, so the next load
//      of ANY overtake module is torn down on its first tick and init() never
//      runs — the module looks like it silently refuses to load.
//
// Device log that produced this test (Performance FX):
//   TOOLS flag: exiting active overtake before menu     <- flag armed, never drained
//   TOOLS SELECT overtake: performance-fx
//   OVERTAKE tick: exit phase                           <- init() never called
//   ... repeats for every subsequent load
//
// This test extracts the real function bodies and the real Tools-shortcut branch
// from src/shadow/shadow_ui.js and runs them in a Node vm sandbox.
//
// RED before the completeOvertakeExit(true) drain, GREEN after.

import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../..');
const source = readFileSync(path.join(repoRoot, 'src/shadow/shadow_ui.js'), 'utf8');

// --- extraction helpers -----------------------------------------------------
function braceMatchFrom(open) {
    let depth = 1, pos = open + 1;
    while (depth > 0 && pos < source.length) {
        const c = source[pos++];
        if (c === '{') depth++;
        else if (c === '}') depth--;
    }
    if (depth !== 0) throw new Error(`unbalanced braces from offset ${open}`);
    return source.slice(open, pos);
}

function extractFn(name) {
    const re = new RegExp(`(^|\\n)function\\s+${name}\\s*\\(`);
    const m = re.exec(source);
    if (!m) throw new Error(`function ${name} not found`);
    const start = m.index + (m[1] ? 1 : 0);
    const open = source.indexOf('{', start);
    if (open < 0) throw new Error(`opening brace not found for ${name}`);
    return source.slice(start, open) + braceMatchFrom(open);
}

// The Tools-shortcut teardown for a plain (non-tool, non-parked) overtake module
// lives in an `else { ... }` block inside globalThis.tick, identified by its
// debugLog marker. Take the block that encloses that marker.
function extractToolsShortcutExitBranch() {
    const marker = '"TOOLS flag: exiting active overtake before menu"';
    const idx = source.indexOf(marker);
    if (idx < 0) throw new Error("tools-shortcut exit marker not found");
    const open = source.lastIndexOf('{', idx);
    if (open < 0) throw new Error("tools-shortcut exit block opener not found");
    return braceMatchFrom(open);
}

// --- sandbox ---------------------------------------------------------------
const calls = { setOvertakeMode: [], requestExit: 0, enterToolsMenu: 0, setView: [] };

const sandbox = {
    console,
    debugLog: () => {},
    // exitOvertakeMode deps
    corunTeardown: () => {},
    autosaveAllSlots: () => {},
    saveMasterFxChainConfig: () => {},
    saveChainConfigToDir: () => {},
    activeSlotStateDir: "/tmp/slots",
    invokeModuleOnUnload: () => {},
    deactivateLedQueue: () => {},
    unloadOvertakeDsp: () => {},
    host_write_file: () => true,
    suspendedOvertakes: {},
    shadow_set_param_timeout: () => {},
    shadow_set_param: () => {},
    NUM_KNOBS: 8,
    overtakeKnobDelta: new Array(8).fill(0),
    overtakeJogDelta: 0,
    // completeOvertakeExit deps
    shadow_set_overtake_mode: (m) => { calls.setOvertakeMode.push(m); },
    shadow_set_skip_led_clear: () => {},
    shadow_request_exit: () => { calls.requestExit++; },
    enterToolsMenu: () => { calls.enterToolsMenu++; },
    setView: (v) => { calls.setView.push(v); },
    VIEWS: { SLOTS: 0, OVERTAKE_MODULE: 1, TOOLS: 2 },
    needsRedraw: false,
    // module state
    overtakeExitPending: false,
    overtakeInitPending: false,
    overtakeModuleLoaded: false,
    overtakeModuleId: "",
    overtakeModulePath: "",
    overtakeModuleCallbacks: null,
    overtakeSuspendKeepsJs: false,
    overtakeSuspendSelfManaged: false,
    overtakePassthroughCCs: [],
    toolOvertakeActive: false,
    toolNonOvertake: false,
    // branch siblings we must not take
    suspendOvertakeMode: () => { throw new Error("suspendOvertakeMode should not run for a plain overtake module"); },
    exitToolOvertake: () => { throw new Error("exitToolOvertake should not run for a plain overtake module"); },
};
sandbox.globalThis = sandbox;
vm.createContext(sandbox);

for (const name of ['exitOvertakeMode', 'completeOvertakeExit']) {
    vm.runInContext(extractFn(name), sandbox);
}
vm.runInContext(`globalThis.runToolsShortcutExit = function() ${extractToolsShortcutExitBranch()};`, sandbox);

// Mirrors the OVERTAKE_MODULE tick dispatch: the exit branch is checked BEFORE
// the init branch, which is the whole reason a stale flag is fatal.
vm.runInContext(`
    globalThis.overtakeModuleTick = function() {
        if (overtakeExitPending) { completeOvertakeExit(); return "exit"; }
        if (overtakeInitPending) { overtakeInitPending = false; return "init"; }
        return "run";
    };
`, sandbox);

function setActiveOvertake(id) {
    sandbox.overtakeModuleId = id;
    sandbox.overtakeModulePath = `/data/UserData/schwung/modules/overtake/${id}`;
    sandbox.overtakeModuleLoaded = true;
    sandbox.overtakeSuspendKeepsJs = false;   // plain overtake, no suspend_keeps_js
    sandbox.toolOvertakeActive = false;       // not an interactive tool
    sandbox.overtakeModuleCallbacks = { init: () => {}, tick: () => {}, onMidiMessageInternal: () => {} };
}

function resetCalls() {
    calls.setOvertakeMode.length = 0;
    calls.setView.length = 0;
    calls.requestExit = 0;
    calls.enterToolsMenu = 0;
}

// --- scenarios --------------------------------------------------------------
const failures = [];
function check(label, cond, detail) {
    if (cond) {
        console.log(`  ok   - ${label}`);
    } else {
        console.log(`  FAIL - ${label}${detail ? ` (${detail})` : ''}`);
        failures.push(label);
    }
}

// ---- Scenario 1: Tools shortcut must not orphan the exit ----
console.log("Scenario 1: Tools shortcut drains the pending exit");
{
    resetCalls();
    setActiveOvertake("performance-fx");

    vm.runInContext(`runToolsShortcutExit();`, sandbox);

    check("exit flag drained before leaving OVERTAKE_MODULE",
          sandbox.overtakeExitPending === false,
          `overtakeExitPending=${sandbox.overtakeExitPending}`);
    check("overtake_mode dropped to 0 (C-side LED restore fires)",
          calls.setOvertakeMode.length === 1 && calls.setOvertakeMode[0] === 0,
          `calls=${JSON.stringify(calls.setOvertakeMode)}`);
    check("did NOT bounce the user out to Move",
          calls.requestExit === 0 && calls.setView.length === 0,
          `requestExit=${calls.requestExit} setView=${JSON.stringify(calls.setView)}`);
}

// ---- Scenario 2: the next load actually reaches init() ----
console.log("Scenario 2: the module loaded after a Tools-shortcut exit reaches init()");
{
    // State left behind by scenario 1, then a fresh load arms init.
    sandbox.overtakeInitPending = true;
    setActiveOvertake("performance-fx");

    const branch = vm.runInContext(`overtakeModuleTick();`, sandbox);
    check("first tick after load takes the init branch, not the exit branch",
          branch === "init",
          `branch=${branch}`);
}

// ---- Scenario 3: a stale flag would still be fatal (guards the tick ordering) ----
console.log("Scenario 3: a stale exit flag still preempts init (why the drain matters)");
{
    sandbox.overtakeExitPending = true;
    sandbox.overtakeInitPending = true;
    setActiveOvertake("performance-fx");

    const branch = vm.runInContext(`overtakeModuleTick();`, sandbox);
    check("exit branch wins over init branch when the flag is stale",
          branch === "exit",
          `branch=${branch}`);
    check("init stayed pending (module never started)",
          sandbox.overtakeInitPending === true);
}

// ---- Scenario 4: the normal Back exit still returns to Move ----
console.log("Scenario 4: skipNavigation does not change the normal Back exit");
{
    resetCalls();
    sandbox.overtakeExitPending = true;
    sandbox.toolOvertakeActive = false;

    vm.runInContext(`completeOvertakeExit();`, sandbox);

    check("returns to the slots view", calls.setView.length === 1 && calls.setView[0] === sandbox.VIEWS.SLOTS,
          `setView=${JSON.stringify(calls.setView)}`);
    check("requests exit back to Move", calls.requestExit === 1, `requestExit=${calls.requestExit}`);
    check("flag cleared", sandbox.overtakeExitPending === false);
}

// ---- Scenario 5: interactive tools still land in the Tools menu ----
console.log("Scenario 5: interactive-tool exit still enters the Tools menu");
{
    resetCalls();
    sandbox.overtakeExitPending = true;
    sandbox.toolOvertakeActive = true;

    vm.runInContext(`completeOvertakeExit();`, sandbox);

    check("entered the Tools menu", calls.enterToolsMenu === 1, `enterToolsMenu=${calls.enterToolsMenu}`);
    check("did not also return to Move", calls.requestExit === 0, `requestExit=${calls.requestExit}`);
    check("toolOvertakeActive cleared", sandbox.toolOvertakeActive === false);
}

// --- exit -------------------------------------------------------------------
if (failures.length) {
    console.log(`\nFAIL: ${failures.length} assertion(s) failed`);
    process.exit(1);
}
console.log("\nPASS");
