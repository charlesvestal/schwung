import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const root=new URL('../../',import.meta.url);
const read=p=>readFileSync(new URL(p,root),'utf8');
const after=read('src/shadow/shadow_ui.js');
function fn(source,name){
  const start=source.indexOf('function '+name+'(');assert.ok(start>=0,name);
  // Upstream top-level functions close on column zero.
  const end=source.indexOf('\n}',start);assert.ok(end>start,name);
  return source.slice(start,end+2);
}
function harness(source=after){
  const log={tool:[],grid:[],writes:[],macro:[],touch:[],ticks:[],escape:0};
  const VIEWS={PARAM_PAGES:1,OVERTAKE_MODULE:2,CHAIN_EDIT:3,COMPONENT_EDIT:4,CANVAS:5,ENUM_PICKER:6};
  const c={console:{log(){}},Date,VIEWS,view:VIEWS.OVERTAKE_MODULE,coRunView:VIEWS.PARAM_PAGES,
    coRunChainEditSlot:1,corunOverlayId:null,corunOverlayRootView:-1,closed:0,keepMask:0,active:true,needsRedraw:false,
    CORUN_GRP_KNOBS:1,CORUN_GRP_JOG:2,CORUN_GRP_BACK:4,CORUN_GRP_TOUCH:8,
    KNOB_CC_START:71,KNOB_CC_END:78,NUM_KNOBS:8,MoveMainKnob:14,MoveMainButton:3,MoveBack:51,
    MoveKnob1Touch:0,MoveKnob8Touch:7,MidiNoteOn:144,MidiNoteOff:128,MidiCC:176,
    VOLUME_TOUCH_NOTE:8,OVERTAKE_MIDI_LOG:false,hostShiftHeld:false,hostMuteHeld:false,hostVolumeKnobTouched:false,
    splashActive:false,overtakeModuleLoaded:true,overtakeInitPending:false,overtakeSuspendKeepsJs:true,
    overtakeSuspendSelfManaged:false,toolOvertakeActive:true,overtakeKnobDelta:Array(8).fill(0),overtakeJogDelta:0,
    knobTouched:Array(8).fill(false),pendingHierKnobIndex:-1,pendingKnobIndex:-1,knobCardKnob:-1,
    textEntry:false,warning:false,feedback:false,
    coRunCedes:g=>!(c.keepMask&g),
    scenesHandleArmedDelete:()=>false,scenesHandleEditKey:()=>false,scenesHandleMidi:()=>false,
    isTextEntryActive:()=>c.textEntry,handleTextEntryMidi:()=>true,
    feedbackGateActive:()=>c.feedback,feedbackGateInput:()=>true,
    dispatchCanvasMidi:()=>false,maybeDismissWarningFromInput:()=>c.warning,
    debugLog(){},announce(){},
    exitToolOvertake:()=>log.escape++,exitOvertakeMode:()=>log.escape++,
    runToolCallback:f=>f(),overtakeModuleCallbacks:{onMidiMessageInternal:d=>log.tool.push([...d])},
    adjustKnobAndShow:()=>false,handleKnobTurn:(i,d)=>log.macro.push([i,d]),
    handleJog:()=>log.macro.push('jog'),handleSelect:()=>log.macro.push('click'),
    handleBack:()=>{c.view=VIEWS.CHAIN_EDIT;},
    shadow_corun_end:()=>{c.coRunChainEditSlot=-1;},
    shadow_corun_close:()=>{c.closed++;c.corunOverlayId=null;c.corunOverlayRootView=-1;},
    paramPagesActive:()=>c.active,
    isHardwarePadPress:d=>(d[0]&240)===144&&d[1]>=68&&d[1]<=99&&d[2]>0,
    _midiCount:0,_midiWindowStart:0,_knobTurnCount:0,controllerIo:null,currentChrome:null,currentSlot:1,
    shiftIsHeld:()=>c.hostShiftHeld,isLoggingEnabled:()=>false,traced:(_n,f)=>f(),
    exitParamPages:()=>{c.active=false;},
    ctx:{VIEWS,setView:v=>{c.view=v;},isMuteHeld:()=>c.hostMuteHeld},
    tickComponentWidgets:()=>log.ticks.push(['widgets',c.view]),
    tickParamPages:()=>log.ticks.push(['page',c.view]),
    endComponentWidgetVisit:()=>log.ticks.push(['end',c.view]),
    decodeDelta:n=>n<64?n:n-128,
  };
  // Controller boundary is a fake parameter store; MIDI decoding and page
  // dispatch below are the REAL upstream functions, not a replacement router.
  c.controller={state:{touched:-1},pickerOpen:false,
    onKnobTurn:(i,d)=>log.writes.push({slot:1,key:'synth:param'+i,delta:d}),
    onKnobTouch:(i,down)=>{c.controller.state.touched=down?i:-1;log.touch.push([i,down]);},
    onJog:d=>log.grid.push(['page',d]),openPicker:()=>{c.controller.pickerOpen=true;},
    closePicker:()=>{c.controller.pickerOpen=false;},setReveal(){},
  };
  vm.createContext(c);
  const pageInput=read('src/shared/param_pages/page_input.mjs');
  vm.runInContext(pageInput.replace(/^import .*;$/gm,'').replace(/^export /gm,''),c);
  vm.runInContext(fn(read('src/shadow/shadow_ui_param_pages.mjs'),'handleParamPagesMidi'),c);
  for(const name of ['coRunUiActive','coRunWants','runCoRunChainEdit'])vm.runInContext(fn(source,name),c);
  if(source.includes('function dispatchCoRunParamPagesMidi'))vm.runInContext(fn(source,'dispatchCoRunParamPagesMidi'),c);
  const start=source.indexOf('globalThis.onMidiMessageInternal = function(data) {');
  const end=source.indexOf('globalThis.onMidiMessageExternal',start);
  assert.ok(start>=0&&end>start);vm.runInContext(source.slice(start,end),c);
  return {c,log,send:d=>c.onMidiMessageInternal(d)};
}

test('co-run knob writes the visible parameter once instead of a slot macro',()=>{
  const h=harness();h.send([176,71,1]);
  assert.deepEqual(h.log.writes,[{slot:1,key:'synth:param0',delta:1}]);
  assert.equal(h.log.macro.length,0);assert.equal(h.log.tool.length,0);assert.equal(h.c.view,h.c.VIEWS.OVERTAKE_MODULE);
});
test('eight knobs, negative deltas and MIDI channels reach controller, not macro fallback',()=>{
  const h=harness();for(let i=0;i<8;i++){h.send([179,71+i,1]);h.send([179,71+i,127]);}
  assert.equal(h.log.writes.length,16);assert.equal(h.log.writes[15].delta,-1);assert.equal(h.log.macro.length,0);
});
test('touch down, velocity-zero and note-off releases reach grid once, and still reach the tool',()=>{
  const h=harness();const ev=[[144,0,100],[144,0,0],[144,1,100],[128,1,0]];ev.forEach(h.send);
  assert.deepEqual(h.log.touch,[[0,true],[0,false],[1,true],[1,false]]);
  // The co-run touch path has always forwarded both edges to the tool.
  assert.deepEqual(h.log.tool,ev);
  assert.equal(h.c.knobTouched[0],false);assert.equal(h.c.knobTouched[1],false);
});
test('a touch held into the grid is released in host bookkeeping by the grid',()=>{
  const h=harness();h.c.knobTouched[2]=true;h.send([128,2,0]);assert.equal(h.c.knobTouched[2],false);
});
test('Back at the root of an overlay whose root IS the grid closes the overlay',()=>{
  // global_settings: CORUN_ENTRIES enters the grid, so its root view is PARAM_PAGES.
  const h=harness();h.c.corunOverlayId='global_settings';h.c.corunOverlayRootView=h.c.VIEWS.PARAM_PAGES;
  h.c.keepMask=h.c.CORUN_GRP_BACK;  // an overlay handles what the tool KEEPS
  h.send([176,51,127]);
  assert.equal(h.c.closed,1);assert.equal(h.c.corunOverlayId,null);assert.equal(h.log.tool.length,0);
});
test('Back in a grid BELOW an overlay root still navigates the grid',()=>{
  const h=harness();h.c.corunOverlayId='master_fx';h.c.corunOverlayRootView=h.c.VIEWS.CHAIN_EDIT;
  h.c.keepMask=h.c.CORUN_GRP_BACK;
  h.send([176,51,127]);
  assert.equal(h.c.closed,0);assert.equal(h.c.coRunView,h.c.VIEWS.CHAIN_EDIT);assert.equal(h.c.corunOverlayId,'master_fx');
});
test('pad/step notes and transport remain tool-owned',()=>{
  const h=harness();const events=[[144,68,100],[128,68,0],[144,72,100],[144,72,0],[144,16,100],[128,16,0],[176,85,127]];
  events.forEach(h.send);assert.deepEqual(h.log.tool,events);assert.equal(h.log.writes.length,0);
});
test('tool-owned knobs are not intercepted, overlay ownership uses existing inverse rule',()=>{
  const h=harness();h.c.keepMask=1;h.send([176,71,1]);assert.equal(h.c.overtakeKnobDelta[0],1);assert.equal(h.log.writes.length,0);
  h.c.corunOverlayId='test';h.send([176,71,1]);assert.equal(h.log.writes.length,1);
});
test('jog and Back navigate picker, then grid to chain, then exit co-run',()=>{
  const h=harness();h.send([176,14,1]);assert.deepEqual(h.log.grid,[['page',1]]);
  h.send([176,3,127]);assert.equal(h.c.controller.pickerOpen,true);
  h.send([176,51,127]);assert.equal(h.c.controller.pickerOpen,false);assert.equal(h.c.coRunView,h.c.VIEWS.PARAM_PAGES);
  h.send([176,51,127]);assert.equal(h.c.coRunView,h.c.VIEWS.CHAIN_EDIT);assert.equal(h.c.view,h.c.VIEWS.OVERTAKE_MODULE);
  h.send([176,51,127]);assert.equal(h.c.coRunChainEditSlot,-1);assert.equal(h.log.tool.length,0);
});
test('host escape outranks grid, and text entry/warnings/feedback retain priority',()=>{
  const h=harness();h.c.hostShiftHeld=true;h.c.hostVolumeKnobTouched=true;h.send([176,3,127]);assert.equal(h.log.escape,1);assert.equal(h.c.controller.pickerOpen,false);
  for(const flag of ['textEntry','warning','feedback']){const a=harness();a.c[flag]=true;a.send([176,71,1]);assert.equal(a.log.writes.length,0);assert.equal(a.log.macro.length,0);}
});
test('ordinary non-co-run parameter page still reaches existing handler',()=>{
  const h=harness();h.c.view=h.c.VIEWS.PARAM_PAGES;h.c.coRunChainEditSlot=-1;h.send([176,71,1]);assert.equal(h.log.writes.length,1);
});

test('grid ticks and widgets use visible co-run view, and stop after returning to chain',()=>{
  const start=after.indexOf('    if (view === VIEWS.OVERTAKE_MODULE && coRunUiActive() &&\n        coRunView === VIEWS.PARAM_PAGES)');
  assert.ok(start>=0);
  const end=after.indexOf('    /* The debounced',start);assert.ok(end>start);
  const block=after.slice(start,end);
  const h=harness();vm.runInContext(block,h.c);
  assert.deepEqual(h.log.ticks,[['widgets',1],['page',1]]);assert.equal(h.c.view,2);
  h.log.ticks.length=0;h.c.coRunView=3;vm.runInContext(block,h.c);assert.deepEqual(h.log.ticks,[['end',2]]);
  h.log.ticks.length=0;h.c.view=1;vm.runInContext(block,h.c);assert.deepEqual(h.log.ticks,[['widgets',1],['page',1]]);
});

test('view ownership is restored even when a parameter handler throws',()=>{
  const h=harness();h.c.controller.onKnobTurn=()=>{throw new Error('probe');};
  assert.throws(()=>h.send([176,71,1]),/probe/);assert.equal(h.c.view,2);assert.equal(h.c.coRunView,1);
  assert.equal(h.log.macro.length,0);assert.equal(h.log.tool.length,0);
});

test('controller open intent is routed to existing enum editor in co-run context',()=>{
  const h=harness();const opened=[];
  h.c.controller.state.touched=0;
  h.c.controller.onClick=()=>true;
  h.c.controller.takePending=()=>({action:'open',key:'mode',options:['A','B'],index:0});
  h.c.clearParamPagesTouch=()=>{h.c.controller.state.touched=-1;};
  h.c.ctx.openEnumPicker=args=>{opened.push(args);h.c.view=h.c.VIEWS.ENUM_PICKER;};
  h.send([176,3,127]);assert.equal(opened.length,1);assert.equal(h.c.coRunView,6);assert.equal(h.c.view,2);
});
