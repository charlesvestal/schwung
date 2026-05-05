// Repro for the param-shim teardown bug ("Bug D"): when an overtake module
// is parked via suspend_keeps_js, opening a chain-component editor calls
// setupModuleParamShims/clearModuleParamShims, which mutate the SAME global
// names the parked module's tick relies on. The parked tick must keep talking
// to its own (overtake-DSP) shims regardless.
//
// This test extracts the relevant function bodies from src/shadow/shadow_ui.js
// and exercises them in a Node vm sandbox. Stubs are minimal — the goal is to
// observe parked-tick reads after chain-edit mutates globals.
//
// RED today, GREEN after the snapshot/swap/restore fix.

import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { strict as assert } from 'node:assert';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../..');
const source = readFileSync(path.join(repoRoot, 'src/shadow/shadow_ui.js'), 'utf8');

// --- extraction helpers -----------------------------------------------------
function extractFn(name) {
    const re = new RegExp(`(^|\\n)function\\s+${name}\\s*\\(`);
    const m = re.exec(source);
    if (!m) throw new Error(`function ${name} not found`);
    const start = m.index + (m[1] ? 1 : 0);
    let i = source.indexOf('{', start);
    if (i < 0) throw new Error(`opening brace not found for ${name}`);
    let depth = 1, pos = i + 1;
    while (depth > 0 && pos < source.length) {
        const c = source[pos++];
        if (c === '{') depth++;
        else if (c === '}') depth--;
    }
    return source.slice(start, pos);
}

// The parked-tick code lives in an anonymous `{ ... }` block placed right
// after the leading "/* Background tick ... */" comment inside globalThis.tick.
// Find the marker, walk past the closing `*/`, then take the next `{ ... }`.
function extractParkedTickBlock() {
    const marker = "/* Background tick for JS-suspended overtake modules.";
    const idx = source.indexOf(marker);
    if (idx < 0) throw new Error("parked-tick marker not found");
    const commentEnd = source.indexOf('*/', idx);
    if (commentEnd < 0) throw new Error("parked-tick comment end not found");
    const open = source.indexOf('{', commentEnd + 2);
    if (open < 0) throw new Error("parked-tick block opener not found");
    let depth = 1, pos = open + 1;
    while (depth > 0 && pos < source.length) {
        const c = source[pos++];
        if (c === '{') depth++;
        else if (c === '}') depth--;
    }
    return source.slice(open, pos);
}

// --- sandbox ---------------------------------------------------------------
const sandbox = {
    console,
    debugLog: () => {},
    activateLedQueue: () => {},
    deactivateLedQueue: () => {},
    flushLedQueue: () => {},
    invokeModuleOnUnload: () => {},
    setView: () => {},
    VIEWS: { SLOTS: 0, OVERTAKE_MODULE: 1 },
    needsRedraw: false,
    NUM_KNOBS: 8,
    overtakeKnobDelta: new Array(8).fill(0),
    overtakeJogDelta: 0,
    overtakeInitPending: false,
    overtakeExitPending: false,
    overtakeModuleLoaded: false,
    overtakeModuleId: "",
    overtakeModulePath: "",
    overtakeModuleCallbacks: null,
    overtakeSuspendKeepsJs: false,
    overtakePassthroughCCs: [],
    suspendedOvertakes: {},
    lastSuspendedToolId: "",
    currentSlot0DspPath: "",
    ledQueueNotes: {},
    ledQueueCCs: {},
    ledClearIndex: 0,
    getSlotParam: (slot, key) => `chain[${slot}:${key}]`,
    setSlotParam: () => {},
    getComponentParamPrefix: (k) => k === "midiFx" ? "midi_fx1" : k,
    shadow_set_param: () => {},
    shadow_set_param_timeout: () => {},
    shadow_get_param: () => "",
    shadow_set_overtake_mode: () => {},
    shadow_set_suspend_overtake: () => {},
    shadow_set_skip_led_clear: () => {},
    shadow_request_exit: () => {},
    unloadModuleUi: () => {},
    enterComponentSelect: () => {},
    CHAIN_COMPONENTS: [{ key: "midiFx" }, { key: "synth" }],
    scanForToolModules: () => [],
    toolModules: [],
    startInteractiveTool: () => {},
    move_midi_internal_send: () => {},
    clear_screen: () => {},
    print: () => {},
    draw_rect: () => {},
    fill_rect: () => {},
    draw_line: () => {},
    draw_image: () => {},
    host_track_event: undefined,
};
sandbox.globalThis = sandbox;
vm.createContext(sandbox);

// --- inject extracted bodies ------------------------------------------------
for (const name of ['setupModuleParamShims', 'clearModuleParamShims',
                    'suspendOvertakeMode', 'resumeOvertakeModule']) {
    vm.runInContext(extractFn(name), sandbox);
}
vm.runInContext(`globalThis.runParkedTicks = function() ${extractParkedTickBlock()};`, sandbox);

// --- helpers to set up an active overtake module ---------------------------
// Mirrors what loadOvertakeModule does for a tool: install the overtake-DSP
// shim and mark the module as the active overtake.
function installOvertakeShim(tag) {
    vm.runInContext(`
        globalThis.host_module_get_param = function(key) { return "${tag}:" + key; };
        globalThis.host_module_set_param = function(key, value) { globalThis._sets["${tag}"] = { key: key, value: value }; };
        globalThis.host_module_set_param_blocking = globalThis.host_module_set_param;
    `, sandbox);
}
function setActive(id, dspPath, ticker) {
    sandbox.overtakeModuleId = id;
    sandbox.overtakeModulePath = `/modules/${id}`;
    sandbox.overtakeModuleLoaded = true;
    sandbox.overtakeSuspendKeepsJs = true;
    sandbox.overtakeModuleCallbacks = { tick: ticker, init: null, onMidiMessageInternal: null, onUnload: null };
    sandbox.currentSlot0DspPath = dspPath;
}

vm.runInContext(`globalThis._sets = {};`, sandbox);

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

// ---- Scenario 1: single parked module survives chain-edit shim churn ----
console.log("Scenario 1: single parked module survives chain-edit");
{
    sandbox.suspendedOvertakes = {};
    let parkedReads = [];
    const tickFn = () => {
        // What the real tb3po tick does: read DSP state via host_module_get_param.
        parkedReads.push(globalThis.host_module_get_param("a.position"));
    };
    // Bind tickFn into the sandbox so it sees sandbox's globalThis
    vm.runInContext(`globalThis._tickFn = function() { _tickReads.push(globalThis.host_module_get_param("a.position")); };`, sandbox);
    vm.runInContext(`globalThis._tickReads = [];`, sandbox);

    installOvertakeShim("tb3po-DSP");
    setActive("tb3po", "/modules/tb3po/dsp.so",
              vm.runInContext(`globalThis._tickFn`, sandbox));

    // Park
    vm.runInContext(`suspendOvertakeMode();`, sandbox);
    check("tb3po is parked", !!sandbox.suspendedOvertakes["tb3po"]);

    // Pre-mutation tick — should read tb3po's overtake DSP value
    vm.runInContext(`runParkedTicks();`, sandbox);
    let reads = sandbox._tickReads;
    check("parked tick (pre-chain-edit) reads tb3po-DSP",
          reads[reads.length - 1] === "tb3po-DSP:a.position",
          `got ${JSON.stringify(reads[reads.length - 1])}`);

    // Simulate user opening chain editor for slot 2 / midiFx → overwrites globals
    vm.runInContext(`setupModuleParamShims(2, "midiFx");`, sandbox);
    vm.runInContext(`runParkedTicks();`, sandbox);
    reads = sandbox._tickReads;
    check("parked tick reads tb3po-DSP even while chain shims installed",
          reads[reads.length - 1] === "tb3po-DSP:a.position",
          `got ${JSON.stringify(reads[reads.length - 1])}`);

    // Simulate user backing out → globals deleted
    vm.runInContext(`clearModuleParamShims();`, sandbox);
    vm.runInContext(`runParkedTicks();`, sandbox);
    reads = sandbox._tickReads;
    check("parked tick still reads tb3po-DSP after chain shims cleared",
          reads[reads.length - 1] === "tb3po-DSP:a.position",
          `got ${JSON.stringify(reads[reads.length - 1])}`);

    // Resume — globals must be restored so foreground tb3po sees them again
    vm.runInContext(`resumeOvertakeModule("tb3po");`, sandbox);
    const v = vm.runInContext(`globalThis.host_module_get_param("a.position")`, sandbox);
    check("after resume, host_module_get_param routes to tb3po-DSP",
          v === "tb3po-DSP:a.position",
          `got ${JSON.stringify(v)}`);
}

// ---- Scenario 2: two parked modules stay isolated -----------------------
console.log("Scenario 2: two parked modules stay isolated");
{
    sandbox.suspendedOvertakes = {};
    vm.runInContext(`globalThis._readsA = []; globalThis._readsB = [];
                     globalThis._tickA = function(){ _readsA.push(globalThis.host_module_get_param("k")); };
                     globalThis._tickB = function(){ _readsB.push(globalThis.host_module_get_param("k")); };`, sandbox);

    // Park module A
    installOvertakeShim("A-DSP");
    setActive("modA", "/modules/modA/dsp.so", vm.runInContext(`globalThis._tickA`, sandbox));
    vm.runInContext(`suspendOvertakeMode();`, sandbox);

    // Park module B
    installOvertakeShim("B-DSP");
    setActive("modB", "/modules/modB/dsp.so", vm.runInContext(`globalThis._tickB`, sandbox));
    vm.runInContext(`suspendOvertakeMode();`, sandbox);

    // Mutate globals via chain-edit
    vm.runInContext(`setupModuleParamShims(0, "synth");`, sandbox);
    vm.runInContext(`runParkedTicks();`, sandbox);

    const a = sandbox._readsA[sandbox._readsA.length - 1];
    const b = sandbox._readsB[sandbox._readsB.length - 1];
    check("parked module A reads its own DSP", a === "A-DSP:k", `got ${JSON.stringify(a)}`);
    check("parked module B reads its own DSP", b === "B-DSP:k", `got ${JSON.stringify(b)}`);
}

// --- exit -------------------------------------------------------------------
if (failures.length) {
    console.log(`\nFAIL: ${failures.length} assertion(s) failed`);
    process.exit(1);
}
console.log("\nPASS");
