import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { loadUiModule } from '../../src/shared/ui_module_load.mjs';
import { createControlHost } from '../../src/shared/control_host.mjs';
import { createCCMap } from '../../src/shared/cc_map.mjs';

const read = path => readFileSync(new URL('../../' + path, import.meta.url), 'utf8');
const shadow = read('src/shadow/shadow_ui.js');
const chain = read('src/modules/chain/ui.js');
const names = ['init', 'tick', 'onMidiMessageInternal', 'onMidiMessageExternal', 'onUnload', 'onResume'];
function fn(source, name) {
    const start = source.indexOf('function ' + name + '(');
    assert.ok(start >= 0, name);
    const end = source.indexOf('\n}', start);
    assert.ok(end > start, name);
    return source.slice(start, end + 2);
}
function exports() { return Object.fromEntries(names.map(name => [name, () => name])); }

for (const outcome of ['success', 'false', 'throw']) {
    test('callback globals restored on ' + outcome, () => {
        const parent = exports();
        const child = exports();
        const g = { ...parent, chain_ui: { parent: true } };
        const before = { ...g };
        const load = () => {
            Object.assign(g, child, { chain_ui: { child: true } });
            if (outcome === 'throw') throw new Error('partial evaluation');
            return outcome === 'success';
        };
        if (outcome === 'throw') assert.throws(() => loadUiModule(load, 'ui.js', g), /partial evaluation/);
        else {
            const result = loadUiModule(load, 'ui.js', g);
            if (outcome === 'false') assert.equal(result, null);
            else {
                assert.deepEqual(result.callbacks, child);
                assert.equal(result.chainUi.child, true);
            }
        }
        assert.deepEqual(g, before);
    });
}

test('omitted callbacks cannot inherit host callbacks or an earlier module', () => {
    const g = exports();
    loadUiModule(() => { Object.assign(g, exports()); return true; }, 'first.js', g);
    const result = loadUiModule(() => true, 'empty.js', g);
    assert.ok(names.every(name => result.callbacks[name] === null));
    assert.equal(result.chainUi, null);
});

test('nested loads and absent optional exports are restored exactly', () => {
    const g = { tick: () => 'host' };
    const before = { ...g };
    loadUiModule(() => {
        g.tick = () => 'outer';
        g.onUnload = () => {};
        const outer = { ...g };
        loadUiModule(() => { g.tick = () => 'inner'; return true; }, 'inner.js', g);
        assert.deepEqual(g, outer);
        return true;
    }, 'outer.js', g);
    assert.deepEqual(g, before);
});

/* Execute the production entry points, not copies of their routing. Only the
 * native loader/parameter I/O are faked. This catches a caller that forgets the
 * helper, and the CC test crosses load -> host dispatch -> learn -> set write. */
function harness() {
    const c = {
        console: { log() {} }, init() {}, tick() {}, onMidiMessageInternal() {},
        debugLog() {}, coRunUiActive: () => false,
        getModuleUiPath: () => 'ui.js', getChainUiPath: () => 'ui_chain.js', getComponentUiPath: () => 'ui.js',
        setupModuleParamShims() {}, clearModuleParamShims() { c.cleared++; }, cleared: 0,
        loadedModuleUi: null, loadedModuleSlot: -1, loadedModuleComponent: '', moduleUiLoadError: false,
        sourceUiLoadError: false, overtakeModuleCallbacks: null, externalSurfaceMode: 0,
        externalSurfaceMidi() { c.surfaceCalls++; }, surfaceCalls: 0,
        dispatchCanvasMidi: () => false, needsRedraw: false,
        paramShimsInstalled: false,
    };
    c.loadUiModule = (load, path) => loadUiModule(load, path, c);
    c.shadow_load_ui_module = c.host_load_ui_module = () => {
        Object.assign(c, exports());
        return true;
    };
    const files = {};
    const params = {
        'synth:chain_params': JSON.stringify([{ key: 'cutoff', name: 'Cutoff', type: 'float', min: 0, max: 1 }]),
        'synth:ui_hierarchy': '', 'synth:cutoff': '0.5',
    };
    const host = createControlHost({
        fs: { exists: p => p in files, read: p => files[p], write: (p, text) => { files[p] = text; }, ensureDir() {} },
        stateDir: () => '/set', getParam: (slot, key) => params[key],
        setParam: (slot, key, value) => { params[key] = value; return true; },
        chainShape: () => ({ slots: [{ synth: 'synth' }] }),
    });
    c.ccMap = createCCMap({ controls: host.controls, edit: host.edit, targets: host.targets, learn: host.learn });
    vm.createContext(c);
    for (const name of ['loadModuleUi', 'unloadModuleUi', 'loadOvertakeModule', 'startInteractiveTool',
                        'hideToolOvertake', 'suspendOvertakeMode',
                        'resumeOvertakeModule', 'exitToolOvertake', 'invokeModuleOnResume', 'runToolCallback']) {
        vm.runInContext(fn(shadow, name), c);
    }
    for (const name of ['loadSourceUi', 'loadComponentUi']) vm.runInContext(fn(chain, name), c);
    vm.runInContext(shadow.slice(shadow.indexOf('globalThis.onMidiMessageExternal = function(data) {')), c);
    return { c, host, params, files };
}

for (const style of ['globals', 'chain_ui']) {
    test('CC Learn and parameter control survive component open/close: ' + style, () => {
        const { c, host, params, files } = harness();
        const router = c.onMidiMessageExternal;
        c.shadow_load_ui_module = () => {
            Object.assign(c, exports());
            if (style === 'chain_ui') c.chain_ui = { init() {}, tick() {} };
            return true;
        };
        assert.equal(c.loadModuleUi(0, 'synth', 'synth'), true);
        c.unloadModuleUi();
        c.ccMap.beginLearn();
        host.observeWrite(0, 'synth:cutoff', '0.5');
        c.onMidiMessageExternal([0xb0, 1, 64]);
        assert.ok(files['/set/controls.json'], 'CC Learn saves a binding after visiting a component UI');
        assert.equal(JSON.parse(files['/set/controls.json']).cc[0].cc, 1);
        c.onMidiMessageExternal([0xb0, 1, 127]);
        c.ccMap.tick();
        assert.equal(Number(params['synth:cutoff']), 1);
        assert.equal(c.onMidiMessageExternal, router);
    });
}

for (const entry of ['loadModuleUi', 'loadComponentUi', 'loadSourceUi']) {
    for (const outcome of ['false', 'throw']) {
        test(entry + ' restores host after a partial ' + outcome, () => {
            const { c } = harness();
            const before = names.map(name => c[name]);
            c.shadow_load_ui_module = c.host_load_ui_module = () => {
                Object.assign(c, exports());
                if (outcome === 'throw') throw new Error('bad UI');
                return false;
            };
            const result = entry === 'loadModuleUi' ? c[entry](0, 'synth', 'synth') : c[entry]('synth');
            assert.ok(!result);
            assert.deepEqual(names.map(name => c[name]), before);
        });
    }
}

test('legacy Chain loaders retain child exports without recursive host fallbacks', () => {
    const { c } = harness();
    const router = c.onMidiMessageExternal;
    c.host_load_ui_module = () => { c.init = () => {}; return true; };
    assert.equal(c.loadComponentUi('synth').onMidiMessageExternal, null);
    c.host_load_ui_module = () => { c.chain_ui = exports(); return true; };
    assert.equal(typeof c.loadSourceUi('source').onMidiMessageExternal, 'function');
    assert.equal(c.onMidiMessageExternal, router);
});

function toolHarness() {
    const h = harness(), c = h.c;
    Object.assign(c, {
        VIEWS: { OVERTAKE_MODULE: 1, TOOL_RESULT: 2, SLOTS: 3 }, view: 0, activeSlotStateDir: '/set',
        setView: v => { c.view = v; }, deferFeedbackModalForOvertake() {}, autosaveAllSlots() {},
        saveMasterFxChainConfig() {}, saveChainConfigToDir() {}, activateLedQueue() {}, deactivateLedQueue() {},
        unloadOvertakeDsp() {}, loadOvertakeDsp() {},
        suspendedOvertakes: {}, overtakeModuleLoaded: false, overtakeModulePath: '', overtakeModuleId: '',
        overtakeModuleCaps: null, overtakeExitPending: false, hostShiftHeld: false, hostVolumeKnobTouched: false,
        overtakeSuspendKeepsJs: false, overtakeSuspendSelfManaged: false, overtakePassthroughCCs: [],
        overtakeInitPending: false, overtakeInitTicks: 0, ledClearIndex: 0, lastLaunchedTool: null,
        toolActiveTool: null, toolSelectedFile: '', toolOvertakeActive: false, toolNonOvertake: false,
        toolHiddenFile: '', toolHiddenModulePath: '', toolResultMessage: '', toolResultSuccess: false,
        corunTeardown() {}, enterToolsMenu() {}, invokeModuleOnUnload() {},
        NUM_KNOBS: 8, overtakeKnobDelta: Array(8).fill(0), overtakeJogDelta: 0,
        ledQueueNotes: {}, ledQueueCCs: {}, currentSlot0DspPath: '', lastSuspendedToolId: '',
        repairSwallowedShiftRelease() {}, flushLedQueue() {}, paramShimsInstalled: false,
    });
    return h;
}

for (const reconnect of [false, true]) {
    for (const outcome of ['success', 'false', 'throw']) {
        test((reconnect ? 'tool reconnect' : 'fresh tool') + ': ' + outcome, () => {
            const { c } = toolHarness();
            const router = c.onMidiMessageExternal;
            const received = [];
            c.shadow_load_ui_module = () => {
                Object.assign(c, exports(), { onMidiMessageExternal: function(data) {
                    assert.equal(this, undefined, 'preserve the native host plain-call convention');
                    received.push([...data]);
                } });
                if (outcome === 'throw') throw new Error('bad tool');
                return outcome === 'success';
            };
            if (reconnect) {
                c.toolHiddenFile = 'sample.wav'; c.toolHiddenModulePath = '/modules/tool/ui.js'; c.overtakeModuleLoaded = true;
                c.startInteractiveTool({ id: 'tool', path: '/tool' }, '');
            } else c.loadOvertakeModule({ id: 'tool', uiPath: '/tool/ui.js' }, false);
            assert.equal(c.onMidiMessageExternal, router);
            if (outcome === 'success') {
                c.onMidiMessageExternal([0xb0, 1, 64]);
                c.onMidiMessageExternal([0xf0, 0x01, 0x02]);
                assert.deepEqual(received, [[0xb0, 1, 64], [0xf0, 0x01, 0x02]]);
                assert.equal(c.surfaceCalls, 0, 'active tool retains raw MIDI ownership');
            } else {
                c.onMidiMessageExternal([0xb0, 1, 64]);
                assert.equal(received.length, 0);
                assert.equal(c.surfaceCalls, 1);
            }
        });
    }
}

for (const transition of ['hideToolOvertake', 'exitToolOvertake', 'suspendOvertakeMode']) {
    test('external MIDI ownership follows ' + transition + ' and resume', () => {
        const { c } = toolHarness();
        const received = [];
        let resumes = 0;
        c.shadow_load_ui_module = () => {
            c.tick = () => {};
            c.onMidiMessageExternal = data => received.push([...data]);
            c.onResume = () => { resumes++; };
            return true;
        };
        c.loadOvertakeModule({ id: 'tool', uiPath: '/modules/tool/ui.js', capabilities: { suspend_keeps_js: true } }, false);
        c.onMidiMessageExternal([0xb0, 1, 10]);
        c[transition]();
        c.onMidiMessageExternal([0xb0, 1, 20]);
        assert.deepEqual(received, [[0xb0, 1, 10]]);
        assert.equal(c.surfaceCalls, 1, 'host regains MIDI after leaving the tool');
        if (transition === 'suspendOvertakeMode') {
            assert.equal(c.resumeOvertakeModule('tool'), true);
            c.onMidiMessageExternal([0xb0, 1, 30]);
            assert.deepEqual(received.at(-1), [0xb0, 1, 30]);
            assert.equal(resumes, 1);
        }
    });
}

test('external tool callback uses its own param shims during co-run', () => {
    const { c } = toolHarness();
    c.paramShimsInstalled = true;
    c.originalHostGetParam = () => 'tool';
    c.originalHostSetParam = () => {};
    const chainGet = () => 'chain';
    c.host_module_get_param = chainGet;
    c.host_module_set_param = () => {};
    let read;
    c.overtakeModuleCallbacks = { onMidiMessageExternal() { read = c.host_module_get_param(); } };
    c.onMidiMessageExternal([0xb0, 1, 64]);
    assert.equal(read, 'tool');
    assert.equal(c.host_module_get_param, chainGet);
});
