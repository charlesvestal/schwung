#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# What it COSTS to look at a chain, and what keeps that view honest.
#
# drawChainEdit reloaded the whole slot from the DSP on every frame: `3 +
# <chain length>` IPC round trips at ~2.8ms each -- 10 reads on a two-FX chain
# and 25 on a full one, against a 16.67ms frame, on the one screen you pass
# through to reach every other. It grew with the chain, which is exactly what a
# variable-length chain makes possible, so the biggest patches drew the worst.
#
# The fix is a cache, and the whole risk of a cache is the stale frame: an
# editor drawing a module that is no longer loaded is worse than the cost it
# saves. So the second half runs each edit path for real and looks at what
# reached the screen. Two of them are there because the obvious cache designs
# get them wrong: a same-module reorder writes no module ids at all (nothing
# keyed on the module signature would notice), and a picker swap mutates the
# config object IN PLACE (nothing comparing object identity would notice).

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { drawChainDiagram, DEFAULT_Y as DIAGRAM_Y, BOX_H as DIAGRAM_BOX_H }
  from "./src/shared/chain_diagram.mjs";
import { chainComponents, emptyChain, parseId as parseChainId, moveBy as chainMoveBy,
         removeAt as chainRemoveAt, MAX_FX, MAX_MIDI_FX } from "./src/shared/chain_model.mjs";

const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* Lift a top-level function out of shadow_ui.js and hand it its dependencies
   as parameters. The file cannot be imported -- it is a device UI module full
   of host globals -- but a function that closes over a few named things can be
   RUN, which is the difference between pinning the source text and pinning the
   behaviour. */
function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

const CHAIN_CAP = { midiFx: MAX_MIDI_FX, fx: MAX_FX };
const SCREEN_WIDTH = 128, MOVY_RULE_Y = 54;
const VIEWS = { CHAIN_EDIT: "chain_edit" };
const noop = () => {};
const truncateText = (t, n) => String(t).slice(0, n);

/*
 * One fake slot, wired to the REAL functions.
 *
 * The fake device behaves like the DSP in the way that matters: writing
 * <id>:module unloads and reloads the instance, so its opaque state is gone the
 * instant the write lands (chain_host.c, v2_load_audio_fx_slot). Every read is
 * counted, which is the whole point of half these tests.
 */
function world() {
  const state = {};
  let reads = [];
  const getSlotParam = (slot, key) => { reads.push(key); return state[key] !== undefined ? state[key] : ""; };
  const setSlotParam = (slot, key, val) => {
    const mod = /^(.*):module$/.exec(key);
    if (mod) { state[mod[1] + "_module"] = val; state[mod[1] + ":state"] = ""; }
    else state[key] = val;
    return true;
  };
  const w = { state, getSlotParam, setSlotParam,
              get reads() { return reads; }, resetReads() { reads = []; } };

  w.chainConfigs = [emptyChain()];
  w.chainConfigFresh = [];
  const createEmptyChainConfig = () => emptyChain();

  const loadChainConfigFromSlot = lift("loadChainConfigFromSlot",
    ["chainConfigs", "createEmptyChainConfig", "getSlotParam", "CHAIN_CAP",
     "fxDisplayNameCache", "fxDisplayNameSkip", "fxDisplayNameBackoff", "chainConfigFresh"])(
    w.chainConfigs, createEmptyChainConfig, getSlotParam, CHAIN_CAP, {}, {}, {}, w.chainConfigFresh);
  const invalidateChainConfig = lift("invalidateChainConfig", ["chainConfigFresh"])(w.chainConfigFresh);
  const ensureChainConfigFresh = lift("ensureChainConfigFresh",
    ["chainConfigFresh", "chainConfigs", "createEmptyChainConfig", "loadChainConfigFromSlot"])(
    w.chainConfigFresh, w.chainConfigs, createEmptyChainConfig, loadChainConfigFromSlot);
  Object.assign(w, { loadChainConfigFromSlot, invalidateChainConfig, ensureChainConfigFresh,
                     createEmptyChainConfig });

  const chainEditorComponents = lift("chainEditorComponents", ["chainComponents"])(chainComponents);
  const chainComponentId = lift("chainComponentId", [])();
  const isChainModuleKey = lift("isChainModuleKey", ["chainComponentId", "parseChainId"])(chainComponentId, parseChainId);
  const chainComponentParamKey = lift("chainComponentParamKey", ["isChainModuleKey", "chainComponentId"])(isChainModuleKey, chainComponentId);
  const getChainComponentModule = lift("getChainComponentModule", ["chainComponentId", "parseChainId"])(chainComponentId, parseChainId);
  const setChainComponentModule = lift("setChainComponentModule", ["chainComponentId", "parseChainId"])(chainComponentId, parseChainId);
  const getComponentParamPrefix = lift("getComponentParamPrefix", ["chainComponentId"])(chainComponentId);
  const chainHasAnyModule = lift("chainHasAnyModule", [])();
  const chainSectionId = lift("chainSectionId", [])();
  const slotChainComponents = (i) => chainEditorComponents(w.chainConfigs[i] || emptyChain());
  const slotChainComponentIndex = lift("slotChainComponentIndex", ["slotChainComponents"])(slotChainComponents);
  const chainEditorKeyAt = lift("chainEditorKeyAt", [])();
  Object.assign(w, { chainEditorComponents, chainComponentId, chainComponentParamKey,
                     getChainComponentModule, setChainComponentModule, slotChainComponents,
                     getComponentParamPrefix, chainSectionId });

  const writeChainOrder = lift("writeChainOrder",
    ["chainConfigs", "CHAIN_CAP", "getSlotParam", "setSlotParam", "chainSectionId",
     "parseChainId", "invalidateChainConfig"])(
    w.chainConfigs, CHAIN_CAP, getSlotParam, setSlotParam, chainSectionId,
    parseChainId, invalidateChainConfig);
  w.writeChainOrder = writeChainOrder;

  w.moveChainComponent = lift("moveChainComponent",
    ["chainConfigs", "chainComponentId", "parseChainId", "chainMoveBy", "writeChainOrder",
     "resetLfoTargetLabels", "invalidateKnobContextCache"])(
    w.chainConfigs, chainComponentId, parseChainId, chainMoveBy, writeChainOrder, noop, noop);

  const applyPickerChoiceToChain = lift("applyPickerChoiceToChain",
    ["chainComponentId", "parseChainId", "chainRemoveAt", "setChainComponentModule"])(
    chainComponentId, parseChainId, chainRemoveAt, setChainComponentModule);

  const getSlotModuleSignature = lift("getSlotModuleSignature", ["getSlotParam", "CHAIN_CAP"])(getSlotParam, CHAIN_CAP);

  w.lastChainComponent = [];
  w.slotUserCleared = [];
  const applyComponentSelectionConfirmed = lift("applyComponentSelectionConfirmed",
    ["writeChainOrder", "host_log", "setSlotParam", "print", "host_track_event",
     "loadChainConfigFromSlot", "lastSlotModuleSignatures", "getSlotModuleSignature",
     "invalidateKnobContextCache", "selectedSlot", "slotChainComponents",
     "selectedChainComponent", "lastChainComponent", "setView", "VIEWS", "needsRedraw"])(
    writeChainOrder, undefined, setSlotParam, noop, undefined,
    loadChainConfigFromSlot, [], getSlotModuleSignature,
    noop, 0, slotChainComponents, 0, w.lastChainComponent, noop, VIEWS, false);

  /* availableModules / selectedModuleIndex are what the picker was showing when
     the user clicked; they are set per test. */
  w.picker = { availableModules: [], selectedModuleIndex: 0, selectedChainComponent: 0 };
  /* The feedback gate. `auto` is the ordinary case (no line-in module, the
     callback fires straight away); holding it parks the pick behind an open
     modal, which is the state the optimistic model is visible in. */
  w.gate = { auto: true, pending: null };
  w.applyComponentSelection = () => lift("applyComponentSelection",
    ["slotChainComponents", "selectedSlot", "selectedChainComponent", "availableModules",
     "selectedModuleIndex", "isChainModuleKey", "setView", "VIEWS", "getChainComponentModule",
     "chainConfigs", "enterPresetBrowser", "getComponentParamPrefix", "enterStorePicker",
     "moveChainComponent", "slotChainComponentIndex", "chainEditorKeyAt", "lastChainComponent",
     "announce", "needsRedraw", "createEmptyChainConfig", "applyPickerChoiceToChain",
     "invalidateChainConfig", "slotUserCleared", "chainHasAnyModule", "resetLfoTargetLabels",
     "chainComponentParamKey", "host_get_module_metadata", "host_log",
     "maybeConfirmForModule", "applyComponentSelectionConfirmed", "loadChainConfigFromSlot",
     "print"])(
    slotChainComponents, 0, w.picker.selectedChainComponent, w.picker.availableModules,
    w.picker.selectedModuleIndex, isChainModuleKey, noop, VIEWS, getChainComponentModule,
    w.chainConfigs, noop, getComponentParamPrefix, noop,
    w.moveChainComponent, slotChainComponentIndex, chainEditorKeyAt, w.lastChainComponent,
    noop, false, createEmptyChainConfig, applyPickerChoiceToChain,
    invalidateChainConfig, w.slotUserCleared, chainHasAnyModule, noop,
    chainComponentParamKey, undefined, undefined,
    (meta, cb) => { if (w.gate.auto) cb(true); else w.gate.pending = cb; },
    applyComponentSelectionConfirmed, loadChainConfigFromSlot,
    noop)();

  /* The DRAW. Two of them: one with a recording diagram so a test can see what
     reached the screen, one with the REAL diagram so the read count is the read
     count of the real thing (it draws at most five boxes, whatever the chain). */
  w.drawn = [];
  const recording = (ctx, comps, sel, opts) => {
    w.drawn = comps.map((c) => opts.abbrev(c));
    comps.forEach((c) => opts.marks(c));
  };
  const drawDeps = [
    "clear_screen", "slotDirtyCache", "selectedSlot", "isExistingPreset", "slots",
    "truncateText", "fill_rect", "print", "text_width", "set_pixel",
    "chainConfigs", "createEmptyChainConfig", "selectedChainComponent",
    "getSlotParamCached", "drawMovyHeader", "DIAGRAM_Y", "MOVY_RULE_Y", "draw_rect",
    "getSlotParam", "slotChainComponents", "drawChainDiagram", "getChainComponentModule",
    "getModuleAbbrev", "chainComponentParamKey", "DIAGRAM_BOX_H", "SCREEN_WIDTH",
    "getComponentParamPrefix", "drawMovyFooter", "isShiftHeld", "ensureChainConfigFresh",
  ];
  const mk = lift("drawChainEdit", drawDeps);
  const makeDraw = (diagram) => mk(noop, {}, 0, () => false, [{ name: "s" }], truncateText,
    noop, noop, () => 10, noop, w.chainConfigs, createEmptyChainConfig, 0,
    getSlotParam, noop, DIAGRAM_Y, MOVY_RULE_Y, noop, getSlotParam,
    slotChainComponents, diagram, getChainComponentModule, (m) => m,
    chainComponentParamKey, DIAGRAM_BOX_H, SCREEN_WIDTH, getComponentParamPrefix,
    noop, () => false, ensureChainConfigFresh);
  w.draw = makeDraw(recording);
  w.drawReal = makeDraw(drawChainDiagram);
  /* What the diagram would put in the boxes, module positions only. */
  w.screen = () => { w.draw(); return w.drawn.filter((a) => a !== "+" && a !== "*").join(","); };
  return w;
}

function loadSlot(w, opts) {
  const { fx = [], midiFx = [], synth = "sf2" } = opts || {};
  for (const k of Object.keys(w.state)) delete w.state[k];
  w.state.synth_module = synth;
  w.state.fx_count = String(fx.length);
  w.state.midi_fx_count = String(midiFx.length);
  fx.forEach((m, i) => {
    w.state["fx" + (i + 1) + "_module"] = m;
    w.state["fx" + (i + 1) + ":state"] = "tuned-" + m + "-at" + (i + 1);
  });
  midiFx.forEach((m, i) => { w.state["midi_fx" + (i + 1) + "_module"] = m; });
  w.chainConfigs[0] = emptyChain();
  w.chainConfigFresh[0] = false;
}

/* ==================================================================== */
/* A. THE READ BUDGET                                                    */
/* ==================================================================== */
{
  const steady = (fxN, midiN) => {
    const w = world();
    loadSlot(w, { fx: Array.from({ length: fxN }, () => "freeverb"),
                  midiFx: Array.from({ length: midiN }, () => "arp") });
    const per = [];
    for (let f = 0; f < 5; f++) { w.resetReads(); w.drawReal(); per.push(w.reads.length); }
    return per;
  };
  const short = steady(2, 0);
  const full = steady(8, 8);
  const shortSteady = short[short.length - 1];
  const fullSteady = full[full.length - 1];

  /* The number that matters: a chain four times longer must not cost four times
     as much to LOOK at. Before this cache, a frame was 3 + <chain length> IPC
     round trips on top of the fixed ones -- 10 reads on the short chain and 25
     on the full one, at ~2.8ms each, against a 16.67ms frame. */
  if (fullSteady > 8)
    fail("drawChainEdit costs " + fullSteady + " IPC reads per frame on a full chain " +
         "(~" + (fullSteady * 2.8).toFixed(0) + "ms, several frame budgets)");
  /* The residual difference is the diagram window -- at most five boxes are on
     screen, each asking whether it is bypassed -- and that is bounded by the
     SCREEN, not by the chain. */
  if (fullSteady - shortSteady > 5)
    fail("per-frame reads still grow with chain length: " + shortSteady +
         " on a 2-FX chain, " + fullSteady + " on a full one");
  /* And the first frame still pays: the cache is not a stale-forever cache, it
     is a per-edit one. */
  if (full[0] <= fullSteady)
    fail("the first frame did not load the slot at all (" + full.join(",") + ")");
}

/* ==================================================================== */
/* B. INVALIDATION, ONE EDIT PATH AT A TIME                              */
/* ==================================================================== */

/* B1. Shift+jog reorder, and the picker Move rows -- both go through
       moveChainComponent -> writeChainOrder. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb", "cloudseed", "tapescam"] });
  if (w.screen() !== "sf2,freeverb,cloudseed,tapescam") fail("B1 setup: " + w.screen());
  if (!w.moveChainComponent(0, "fx1", 1)) fail("B1: the move was refused");
  if (w.screen() !== "sf2,cloudseed,freeverb,tapescam")
    fail("B1: the editor still draws the OLD order after a reorder: " + w.screen());
}

/* B2. THE SUBTLE ONE. Two of the SAME module swap places: not one `<id>:module`
       write goes out (neither position needs reloading -- they are the same
       plugin), so the slot module signature is byte-identical before and after.
       What did change is each position`s `<id>:state` and the LFO aimed at one
       of them. A cache that watches the signature sees nothing here. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb", "freeverb"] });
  w.state["lfo1:target"] = "fx1";
  w.screen();
  const before = w.chainConfigFresh[0];
  if (before !== true) fail("B2 setup: the draw did not mark the slot fresh");
  const prev = w.chainConfigs[0].fx.slice();
  if (!w.moveChainComponent(0, "fx1", 1)) fail("B2: the move was refused");
  const moduleWrites = Object.keys(w.state).filter((k) => /_module$/.test(k)).length;
  if (w.state["fx1_module"] !== "freeverb" || w.state["fx2_module"] !== "freeverb")
    fail("B2 setup: the two positions should still hold the same module");
  if (w.state["fx1:state"] !== "tuned-freeverb-at2")
    fail("B2 setup: the state did not travel with the module: " + w.state["fx1:state"]);
  if (w.chainConfigFresh[0] === true)
    fail("B2: a reorder that wrote only state and LFO routing left the cache marked FRESH " +
         "-- the module signature is unchanged, so nothing else will notice");
}

/* B3. A picker SWAP, which mutates the config object IN PLACE -- so a cache
       comparing object identity would see the same object and believe it. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb"] });
  if (w.screen() !== "sf2,freeverb") fail("B3 setup: " + w.screen());
  w.picker.availableModules.length = 0;
  w.picker.availableModules.push({ id: "cloudseed", name: "CloudSeed" });
  w.picker.selectedModuleIndex = 0;
  w.picker.selectedChainComponent = w.slotChainComponents(0).findIndex((c) => c.key === "fx1");
  w.applyComponentSelection();
  if (w.screen() !== "sf2,cloudseed")
    fail("B3: the editor still draws the module that was swapped OUT: " + w.screen());
}

/* B3b. And the invalidation is what makes the editor HONEST while the feedback
        modal is up: the pick is in the model but nothing has been loaded yet,
        so the boxes must still show what is actually running. Left marked
        fresh, the editor draws a module the DSP has never heard of. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb"] });
  if (w.screen() !== "sf2,freeverb") fail("B3b setup: " + w.screen());
  w.gate.auto = false;
  w.picker.availableModules.length = 0;
  w.picker.availableModules.push({ id: "cloudseed", name: "CloudSeed" });
  w.picker.selectedModuleIndex = 0;
  w.picker.selectedChainComponent = w.slotChainComponents(0).findIndex((c) => c.key === "fx1");
  w.applyComponentSelection();
  if (!w.gate.pending) fail("B3b setup: the feedback gate did not park the pick");
  if (w.screen() !== "sf2,freeverb")
    fail("B3b: with the confirmation still on screen the editor already draws a module " +
         "that was never loaded: " + w.screen());
  w.gate.pending(true);
  if (w.screen() !== "sf2,cloudseed")
    fail("B3b: confirming the gate did not apply the pick: " + w.screen());
}

/* B4. A picker `None` on a list position, which REMOVES and compacts -- a
       different object AND a section rewrite. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb", "cloudseed"] });
  if (w.screen() !== "sf2,freeverb,cloudseed") fail("B4 setup: " + w.screen());
  w.picker.availableModules.length = 0;
  w.picker.availableModules.push({ id: "", name: "None" });
  w.picker.selectedModuleIndex = 0;
  w.picker.selectedChainComponent = w.slotChainComponents(0).findIndex((c) => c.key === "fx1");
  w.applyComponentSelection();
  if (w.screen() !== "sf2,cloudseed")
    fail("B4: removing fx1 left the editor drawing it: " + w.screen());
}

/* B5. The `+` box materialises a trailing empty position in the MODEL only,
       and backing out of the picker is supposed to drop it. It is a RELOAD that
       drops it -- loadChainConfigFromSlot discards trailing empties -- so if
       the click does not invalidate, a cancelled `+` stays on screen forever. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb"] });
  w.screen();
  const cfg = w.chainConfigs[0];
  cfg.fx = cfg.fx.concat([null]);
  w.chainConfigs[0] = cfg;
  w.invalidateChainConfig(0);
  w.draw();
  if (w.chainConfigs[0].fx.length !== 1)
    fail("B5: a cancelled `+` position survived the return to the editor");
}

/* ==================================================================== */
/* C. IS THE LIST COMPLETE?                                              */
/* ==================================================================== */
/* Every assignment to chainConfigs[...] is a point where the cached view and
   the DSP can disagree. There are only a handful, which is why the invalidation
   can be enumerated at all -- so pin that, and a sixth one added later has to
   say what it does about the cache. */
{
  const sites = [];
  const re = /^.*chainConfigs\[[^\]]+\]\s*=[^=]/gm;
  let m;
  while ((m = re.exec(src)) !== null) {
    const lineNo = src.slice(0, m.index).split("\n").length;
    /* the loader IS the refresh, and initChainConfigs rebuilds the flags with
       the configs */
    const after = src.slice(m.index, m.index + 900);
    /* writeChainOrder counts: it invalidates for its callers, and B2 above
       proves it does so even when it writes no module ids at all. */
    const ok = /chainConfigFresh\[slotIndex\] = true/.test(after.slice(0, 300)) ||
               /invalidateChainConfig\(/.test(after) ||
               /writeChainOrder\(/.test(after.slice(0, 400));
    if (!ok) sites.push(lineNo + ": " + m[0].trim());
  }
  if (sites.length)
    fail("a chain config is replaced without saying anything about the cached view, " +
         "so the editor can draw a chain that is no longer loaded:\n    " + sites.join("\n    "));
  if (!/let chainConfigFresh/.test(src)) fail("chainConfigFresh is gone");
}

if (failures) process.exit(1);
console.log("PASS: chain edit read budget — the editor reloads a slot on an EDIT, not on a " +
            "frame (reads no longer scale with chain length), and every path that mutates the " +
            "chain invalidates, including a same-module reorder that writes no module ids at all");
'
