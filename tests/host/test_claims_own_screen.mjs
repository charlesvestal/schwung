import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

// A module that draws its own screen (ui_chain.js -> COMPONENT_EDIT) gets the
// buttons it claims -- and only while its UI is the one receiving them.
//
// reconcileCcClaim() read the module id from the LIST EDITOR's pair
// (hierEditorSlot / hierEditorComponent) on every claim view but the knob
// grid, and nothing on the way into COMPONENT_EDIT sets that pair. So a
// ui_chain.js module never got its claim (the buttons went to Move), or got
// the claim of whichever module the list editor last showed.
//
// The REAL reconcile runs here, with the real claim tables and focus
// derivation lifted out of shadow_ui.js; only the IPC boundary is stubbed
// (param reads, module metadata, host_claim_ccs).

const root=new URL('../../',import.meta.url);
const source=readFileSync(new URL('src/shadow/shadow_ui.js',root),'utf8');
function fn(name){
  const start=source.indexOf('function '+name+'(');assert.ok(start>=0,name);
  // Upstream top-level functions close on column zero.
  const end=source.indexOf('\n}',start);assert.ok(end>start,name);
  return source.slice(start,end+2);
}
function block(from,to){
  const start=source.indexOf(from);assert.ok(start>=0,from);
  const end=source.indexOf(to,start);assert.ok(end>start,to);
  return source.slice(start,end+to.length);
}

const VIEWS={CHAIN_EDIT:1,PARAM_PAGES:2,HIERARCHY_EDITOR:3,COMPONENT_EDIT:4,
  COMPONENT_PARAMS:5,CANVAS:6,OVERTAKE_MODULE:7};

function harness(){
  const claims=[];
  const params={};               // "slot|key" -> served value (null = read failed)
  const metas={};                // module id -> module.json
  const c={VIEWS,console:{log(){}},
    view:VIEWS.CHAIN_EDIT,coRunView:-1,coRunChainEditSlot:-1,corunOverlayId:null,
    selectedSlot:0,editingComponentKey:'',
    loadedModuleUi:null,loadedModuleSlot:-1,loadedModuleComponent:'',
    hierEditorSlot:-1,hierEditorComponent:'',hierEditorIsMasterFx:false,
    gridActive:false,gridSlot:-1,gridComponent:null,
    BusModel:{parseBusComponentKey:()=>null},
    paramPagesChromeFor:()=>null,          // slot components carry no chrome
    paramPagesActive:()=>c.gridActive,
    paramPagesSlot:()=>c.gridSlot,
    paramPagesComponent:()=>c.gridComponent,
    sceneClaimedCcs:()=>[],
    shadow_get_display_mode:()=>1,
    getSlotParam:(slot,key)=>{const k=slot+'|'+key;return k in params?params[k]:'';},
    host_get_module_metadata:id=>metas[id]||{},
    host_claim_ccs:list=>claims.push([...list]),
    debugLog(){},
  };
  vm.createContext(c);
  vm.runInContext(block('const CC_CLAIM_VIEWS = {};','let ccClaimed = "";'),c);
  for(const name of ['chainComponentId','getComponentParamPrefix','componentModuleIdKey',
    'coRunUiActive','hierarchyActiveModuleIdRaw','currentEditFocus','moduleClaimedCcs',
    'reconcileCcClaim'])vm.runInContext(fn(name),c);
  const claimed=()=>vm.runInContext('ccClaimed',c);
  const tick=()=>vm.runInContext('reconcileCcClaim()',c);
  // What enterComponentEditFallback + loadModuleUi leave behind for a module
  // with its own ui_chain.js. Neither touches the hierarchy pair.
  const openOwnUi=(slot,key)=>{
    Object.assign(c,{selectedSlot:slot,editingComponentKey:key,loadedModuleSlot:slot,
      loadedModuleComponent:key,loadedModuleUi:{tick(){},onMidiMessageInternal(){}},
      view:VIEWS.COMPONENT_EDIT});
  };
  return {c,claims,params,metas,claimed,tick,openOwnUi};
}

test('a module drawing its own screen gets the buttons it claims',()=>{
  const h=harness();
  h.params['1|fx1_module']='trance-gate';
  h.metas['trance-gate']={capabilities:{claims_ccs:[62,63]}};
  h.openOwnUi(1,'fx1');h.tick();
  assert.equal(h.claimed(),'62,63');
  assert.deepEqual(h.claims.at(-1),[62,63]);
});

test('the first MIDI FX is keyed midiFx and read as midi_fx1',()=>{
  const h=harness();
  h.params['2|midi_fx1_module']='stepper';
  h.metas.stepper={capabilities:{claims_edit_ccs:true}};
  h.openOwnUi(2,'midiFx');h.tick();
  assert.equal(h.claimed(),'56,60,119');
});

test('the list editor left behind does not lend its claim to the next own screen',()=>{
  const h=harness();
  // A jump out of the hierarchy editor skips exitHierarchyEditor, so the pair
  // still names dr32, which claims Undo / Copy / Delete.
  Object.assign(h.c,{hierEditorSlot:0,hierEditorComponent:'synth'});
  h.params['0|synth_module']='dr32';
  h.metas.dr32={capabilities:{claims_edit_ccs:true}};
  h.params['3|fx2_module']='plain-fx';
  h.metas['plain-fx']={capabilities:{}};
  h.openOwnUi(3,'fx2');h.tick();
  assert.equal(h.claimed(),'',"plain-fx's own screen must not withhold dr32's buttons from Move");
});

test('the preset-browser fallback claims nothing: no one there handles the button',()=>{
  const h=harness();
  h.params['1|fx1_module']='trance-gate';
  h.metas['trance-gate']={capabilities:{claims_ccs:[62,63]}};
  Object.assign(h.c,{selectedSlot:1,editingComponentKey:'fx1',view:VIEWS.COMPONENT_EDIT});
  Object.assign(h.c,{hierEditorSlot:1,hierEditorComponent:'fx1'});   // even with a pair to read
  h.tick();
  assert.equal(h.claimed(),'');
});

test('leaving the module screen releases the claim',()=>{
  const h=harness();
  h.params['1|fx1_module']='trance-gate';
  h.metas['trance-gate']={capabilities:{claims_ccs:[62,63]}};
  h.openOwnUi(1,'fx1');h.tick();
  assert.equal(h.claimed(),'62,63');
  h.c.view=VIEWS.CHAIN_EDIT;h.tick();
  assert.equal(h.claimed(),'');
  assert.deepEqual(h.claims.at(-1),[]);
});

test('co-run never hands the claim to a module screen',()=>{
  const h=harness();
  h.params['1|fx1_module']='trance-gate';
  h.metas['trance-gate']={capabilities:{claims_ccs:[62,63]}};
  h.openOwnUi(1,'fx1');h.c.coRunChainEditSlot=1;h.tick();
  assert.equal(h.claimed(),'');
});

test('a module-id read that did not answer claims nothing and is asked again',()=>{
  const h=harness();
  h.params['1|fx1_module']=null;                     // timed out
  h.metas['trance-gate']={capabilities:{claims_ccs:[62,63]}};
  h.openOwnUi(1,'fx1');h.tick();
  assert.equal(h.claimed(),'');
  assert.equal(h.claims.length,0);
  h.params['1|fx1_module']='trance-gate';h.tick();   // same screen, next tick
  assert.equal(h.claimed(),'62,63');
});

test('the knob grid still claims from its own slot and component',()=>{
  const h=harness();
  h.params['2|synth_module']='dr32';
  h.metas.dr32={capabilities:{claims_edit_ccs:true}};
  Object.assign(h.c,{view:VIEWS.PARAM_PAGES,gridActive:true,gridSlot:2,gridComponent:'synth'});
  h.tick();
  assert.equal(h.claimed(),'56,60,119');
});

test('the list editor still claims from its own pair',()=>{
  const h=harness();
  h.params['0|synth_module']='dr32';
  h.metas.dr32={capabilities:{claims_edit_ccs:true}};
  Object.assign(h.c,{view:VIEWS.HIERARCHY_EDITOR,hierEditorSlot:0,hierEditorComponent:'synth'});
  h.tick();
  assert.equal(h.claimed(),'56,60,119');
});
