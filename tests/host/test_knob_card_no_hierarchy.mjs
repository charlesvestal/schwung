import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

/* A module with NO ui_hierarchy (9W9 2.7 serves "" plus 99 chain_params; Radio
 * Garden declares three) has its knob grid paginated from chain_params, and its
 * chain-editor knobs map chain_params[i]. The knob card must show that row too,
 * not fall back to the one-line header. Runs the REAL knobCardOpen. */
const src=readFileSync(new URL('../../src/shadow/shadow_ui.js',import.meta.url),'utf8');
function fn(name){
  const start=src.indexOf('function '+name+'(');assert.ok(start>=0,name);
  const end=src.indexOf('\n}',start);assert.ok(end>start,name);
  return src.slice(start,end+2);
}
const PARAMS=[
  {key:'notmap',name:'Note Map',type:'enum',options:['Drum Rack','General MIDI']},
  {key:'bd_tune',name:'BD Tune',type:'int',min:0,max:127},
  {key:'bd_atk',name:'BD Attack',type:'int',min:0,max:127},
  {key:'bd_dec',name:'BD Decay',type:'int',min:0,max:127},
];
function harness(hierarchy){
  const c={NUM_KNOBS:8,reads:[],
    knobCardKnob:-1,knobCardSlot:-1,knobCardCompKey:null,knobCardKeys:null,knobCardMeta:null,
    knobCardRowValues:null,knobCardViz:null,knobCardModKey:null,knobCardFullKey:null,
    chainTargetIsModulePosition:()=>true,
    chainTargetHierarchy:()=>hierarchy,
    chainTargetChainParams:()=>PARAMS,
    getKnobContext:i=>PARAMS[i]?{key:PARAMS[i].key}:null,
    buildMetaIndex:({chainParams})=>({fromChainParams:chainParams.length}),
    resolveViz:()=>({groups:[]}),
    getSlotParam:(slot,k)=>{c.reads.push(k);return '54';},
  };
  vm.createContext(c);
  vm.runInContext(fn('knobCardOpen'),c);
  const focus={target:{slot:0,key:(comp,k)=>comp+':'+k},comp:{key:'synth'}};
  vm.runInContext('knobCardOpen(1, focus)',Object.assign(c,{focus}));
  return c;
}

test('no hierarchy: the card still gets the 4-up row from chain_params',()=>{
  const c=harness(null);
  assert.deepEqual([...c.knobCardKeys.slice(0,4)],['notmap','bd_tune','bd_atk','bd_dec']);
  assert.ok(c.knobCardRowValues,'row values read');
  assert.deepEqual([...Object.keys(c.knobCardRowValues)],['notmap','bd_tune','bd_atk','bd_dec']);
  assert.equal(c.knobCardMeta.fromChainParams,4);
});
test('a declared hierarchy still opens the row exactly as before',()=>{
  const c=harness({levels:{root:{knobs:['notmap','bd_tune','bd_atk','bd_dec']}}});
  assert.ok(c.knobCardRowValues);assert.equal(c.reads.length,4);
});
