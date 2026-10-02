import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

/* A canvas PAGE can ask for the host keyboard (ctx.openTextEntry) and gets the
 * answer through its onTextEntry hook. Runs the REAL canvasPageHook and
 * openCanvasPageTextEntry against mocked surroundings. */
const src = readFileSync(new URL('../../src/shadow/shadow_ui.js', import.meta.url), 'utf8');
function fn(name) {
  const start = src.indexOf('function ' + name + '(');
  assert.ok(start >= 0, name);
  const end = src.indexOf('\n}', start);
  return src.slice(start, end + 2);
}
function harness(ov) {
  const c = {
    console, Date, String, Object,
    SCREEN_WIDTH: 128, SCREEN_HEIGHT: 64, needsRedraw: false,
    canvasPageDisabled: {}, canvasPageOverlays: { 'p.js|': ov },
    canvasPageDrawer: () => null, resolveCardScriptPath: () => 'p.js',
    canvasPageState: () => c.state, state: {},
    getComponentParamPrefix: () => 'synth', writes: [],
    getSlotParam: () => '', setSlotParam: (s, k, v) => { c.writes.push([k, v]); return true; },
    isShiftHeld: () => false, debugLog() {}, announce() {},
    kb: null, isTextEntryActive: () => !!c.kb,
    openTextEntry: (o) => { c.kb = o; },
    toolModules: [{ id: 'waveform-editor' }], scanForToolModules: () => c.toolModules,
    started: null, startInteractiveTool: (t, p) => { c.started = [t.id, p]; },
    gridOpen: true, paramPagesActive: () => c.gridOpen, exitParamPages: () => { c.gridOpen = false; },
  };
  vm.createContext(c);
  vm.runInContext('const CANVAS_PAGE_CLOSE = { close: true };\nlet pendingCanvasToolOpen = null;\n' +
    fn('canvasPageHook') + fn('openCanvasPageTextEntry') + fn('serviceCanvasToolOpen') +
    '\nglobalThis.__pending = () => pendingCanvasToolOpen;', c);
  c.hook = (h, p) => vm.runInContext(`canvasPageHook(1, 'synth', { script: 'p.js', key: 'browse' }, ${JSON.stringify(h)}, ${JSON.stringify(p || {})})`, c);
  return c;
}

test('the keyboard opens after the hook returns, and the answer reaches onTextEntry', () => {
  const seen = [];
  const ov = {
    onMidi(ctx) { ctx.openTextEntry({ title: 'Search', initial: 'ab' }); seen.push(['midi-open', !!globalThis.__kb]); },
    onTextEntry(ctx, p) { seen.push(['text', p.text, p.cancelled]); ctx.setParam('search_query', p.text); },
  };
  const c = harness(ov);
  c.hook('onMidi', { data: [176, 3, 127] });
  assert.ok(c.kb, 'keyboard opened');
  assert.equal(c.kb.title, 'Search'); assert.equal(c.kb.initialText, 'ab');
  const kb = c.kb; c.kb = null;
  kb.onConfirm('hello');
  assert.deepEqual(seen.slice(1), [['text', 'hello', false]]);
  assert.deepEqual(c.writes, [['synth:search_query', 'hello']], 'the answer ctx can set params');
});

test('cancelling still answers, with cancelled=true and no text', () => {
  const got = [];
  const c = harness({ onMidi(ctx) { ctx.openTextEntry({ title: 'X' }); },
                      onTextEntry(ctx, p) { got.push([p.text, p.cancelled]); } });
  c.hook('onMidi');
  const kb = c.kb; c.kb = null; kb.onCancel();
  assert.deepEqual(got, [[null, true]]);
});

test('a second request while the keyboard is up is refused', () => {
  let second;
  const c = harness({ onMidi(ctx) { ctx.openTextEntry({ title: 'A' }); second = ctx.openTextEntry({ title: 'B' }); } });
  c.hook('onMidi');
  assert.equal(second, false); assert.equal(c.kb.title, 'A');
  const d = harness({ onMidi(ctx) { second = ctx.openTextEntry({ title: 'C' }); } });
  d.kb = { title: 'already up' };
  d.hook('onMidi');
  assert.equal(second, false); assert.equal(d.kb.title, 'already up');
});

test('openFileInTool QUEUES; the tick leaves the grid and starts the tool', () => {
  let ok;
  const c = harness({ onMidi(ctx) { ok = ctx.openFileInTool('/data/x.wav', 'waveform-editor'); } });
  c.hook('onMidi');
  assert.equal(ok, true);
  assert.equal(c.started, null, 'nothing starts inside the hook');
  vm.runInContext('serviceCanvasToolOpen()', c);
  assert.deepEqual(c.started, ['waveform-editor', '/data/x.wav']);
  assert.equal(c.gridOpen, false, 'the grid is exited first');
  vm.runInContext('serviceCanvasToolOpen()', c);
  assert.deepEqual(c.started, ['waveform-editor', '/data/x.wav'], 'and only once');
});

test('openFileInTool refuses a tool that is not installed', () => {
  let ok;
  const c = harness({ onMidi(ctx) { ok = ctx.openFileInTool('/x.wav', 'nope'); } });
  c.hook('onMidi');
  assert.equal(ok, false); assert.equal(c.__pending(), null);
});
