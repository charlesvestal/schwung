import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

/* A knob released while the shadow UI is off screen never reaches it, so the
 * touch set must not survive leaving the screen -- or the next card for that
 * knob is "held, no deadline" and sticks over the editor. Runs the REAL
 * reconcile against mocked host reads. */
const src=readFileSync(new URL('../../src/shadow/shadow_ui.js',import.meta.url),'utf8');
function fn(name){
  const start=src.indexOf('function '+name+'(');assert.ok(start>=0,name);
  const end=src.indexOf('\n}',start);assert.ok(end>start,name);
  return src.slice(start,end+2);
}
function harness(){
  const VIEWS={CHAIN_EDIT:1,OVERTAKE_MODULE:2};
  const c={VIEWS,view:VIEWS.CHAIN_EDIT,mode:1,corun:false,closed:0,
    knobTouched:Array(8).fill(false),drained:0,
    pendingHierKnobIndex:-1,pendingHierKnobDelta:0,
    shadow_get_display_mode:()=>c.mode,
    coRunUiActive:()=>c.corun,
    knobCardClose:()=>{c.closed++;c.closedAfterDrain=c.drained;},
    processPendingHierKnob:()=>{c.drained++;c.pendingHierKnobDelta=0;}};
  vm.createContext(c);
  vm.runInContext('let lastShadowUiOnScreen = null;\n'+fn('shadowUiOnScreen')+fn('reconcileDisplayModeExit'),c);
  return {c,tick:()=>vm.runInContext('reconcileDisplayModeExit()',c)};
}

test('dismissing with a knob held closes the card AND forgets the touch',()=>{
  const h=harness();h.tick();
  h.c.knobTouched[0]=true;          // finger still on knob 1 at the dismiss
  h.c.mode=0;h.tick();
  assert.equal(h.c.closed,1);
  assert.deepEqual(h.c.knobTouched,Array(8).fill(false));
});
test('leaving drops a HELD knob: it is what re-raised the card every tick',()=>{
  // Measured on hardware: with the card closed, processPendingHierKnob raised
  // it again on the next tick because pendingHierKnobIndex was still 0.
  const h=harness();h.tick();
  h.c.pendingHierKnobIndex=0;h.c.pendingHierKnobDelta=0;
  h.c.mode=0;h.tick();
  assert.equal(h.c.pendingHierKnobIndex,-1);assert.equal(h.c.drained,0,'nothing to drain');
});
test('a pending turn is drained BEFORE the card is closed, as the release does',()=>{
  const h=harness();h.tick();
  h.c.pendingHierKnobIndex=3;h.c.pendingHierKnobDelta=2;
  h.c.mode=0;h.tick();
  assert.equal(h.c.drained,1);assert.equal(h.c.closedAfterDrain,1,'close after drain');
  assert.equal(h.c.pendingHierKnobIndex,-1);assert.equal(h.c.pendingHierKnobDelta,0);
});
test('a co-run exit drops the pending turn instead of draining it under the wrong view',()=>{
  const h=harness();h.c.view=h.c.VIEWS.OVERTAKE_MODULE;h.c.corun=true;h.tick();
  h.c.pendingHierKnobIndex=1;h.c.pendingHierKnobDelta=1;
  // corun stays "active" in the mock only to prove the guard; the real exit
  // test below covers the transition itself.
  h.c.mode=0;h.tick();
  assert.equal(h.c.drained,0);assert.equal(h.c.pendingHierKnobIndex,-1);
});
test('coming back is not an exit, and staying put does nothing',()=>{
  const h=harness();h.tick();h.c.mode=0;h.tick();h.c.closed=0;
  h.c.knobTouched[2]=true;h.c.mode=1;h.tick();h.tick();
  assert.equal(h.c.closed,0);assert.equal(h.c.knobTouched[2],true);
});
test('a co-run editor closing is an exit even though display_mode stays 1',()=>{
  const h=harness();h.c.view=h.c.VIEWS.OVERTAKE_MODULE;h.c.corun=true;h.tick();
  h.c.knobTouched[4]=true;h.c.corun=false;h.tick();   // tool ended co-run
  assert.equal(h.c.closed,1);assert.equal(h.c.knobTouched[4],false);
});
test('an overtake tool alone on screen is not the shadow UI; the first tick is not a transition',()=>{
  const h=harness();h.c.view=h.c.VIEWS.OVERTAKE_MODULE;h.tick();h.tick();
  assert.equal(h.c.closed,0);
  const g=harness();g.c.mode=0;g.tick();assert.equal(g.c.closed,0);
});
test('the reconcile runs every tick, not inside the 4 Hz feedback-guard block',()=>{
  const t=src.indexOf('globalThis.tick = function');
  const call=src.indexOf('reconcileDisplayModeExit();',t);
  const block=src.indexOf('if (++_feedbackHoldTickCounter >= FEEDBACK_HOLD_CHECK_INTERVAL)',t);
  assert.ok(call>t&&block>t);assert.ok(call<block,'call must precede the throttled block');
  assert.equal(src.indexOf('reconcileDisplayModeExit();',call+1),-1,'exactly one call site');
});
