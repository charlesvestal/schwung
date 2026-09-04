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
         removeAt as chainRemoveAt, insertAt as chainInsertAt, MAX_FX, MAX_MIDI_FX }
  from "./src/shared/chain_model.mjs";

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

/* The pending `+` insert is module STATE shared by four functions, so they come
   out as one block. Lifted one at a time each would close over its own copy of
   `pendingChainInsert` -- assigning to it inside a lifted function writes to
   that function own parameter -- and none of them would see what the others
   did, which is exactly the bug the block is there to prevent. */
function liftBlock(fromMarker, lastFn, deps, names) {
  const at = src.indexOf(fromMarker);
  const fnAt = at >= 0 ? src.indexOf("function " + lastFn + "(", at) : -1;
  const end = fnAt >= 0 ? src.indexOf("\n}\n", fnAt) : -1;
  if (at < 0 || end < 0) { fail(fromMarker + " / " + lastFn + " is gone"); return null; }
  return new Function(...deps,
    src.slice(at, end + 2) + "\nreturn {" + names.join(", ") + "};");
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
  const writes = [];
  /* Recompute <section>_count the way chain_host keeps it: a high-water mark
     that shrinks only when the TRAILING position is cleared. The editor reads
     it back to learn how long a section is, so a fake that never moves it makes
     a section that grew invisible on the next load. */
  const recount = (prefix) => {
    let n = 0;
    for (let i = 8; i >= 1; i--) { if (state[prefix + i + "_module"]) { n = i; break; } }
    state[prefix + "_count"] = String(n);
  };
  /* The REORDER VERBS, modelled as what the DSP actually does: permute the
     per-position arrays and re-aim the routings that name a position, leaving
     every instance running. Modelling this is the point -- a fake that merely
     recorded the verb could not tell a permutation that carried a module state
     from one that quietly dropped it. */
  const permute = (prefix, mapFrom) => {
    const cap = 8;
    const snap = {};
    for (const k of Object.keys(state)) {
      const m = new RegExp("^" + prefix + "(\\d+)(_module|:.*)$").exec(k);
      if (m) { snap[k] = state[k]; delete state[k]; }
    }
    for (const k of Object.keys(snap)) {
      const m = new RegExp("^" + prefix + "(\\d+)(_module|:.*)$").exec(k);
      const to = mapFrom(parseInt(m[1], 10) - 1);
      if (to >= 0 && to < cap) state[prefix + (to + 1) + m[2]] = snap[k];
    }
    for (let li = 1; li <= 2; li++) {
      const cur = state["lfo" + li + ":target"];
      if (!cur) continue;
      const m = new RegExp("^" + prefix + "(\\d+)$").exec(cur);
      if (!m) continue;
      const to = mapFrom(parseInt(m[1], 10) - 1);
      if (to < 0) { state["lfo" + li + ":target"] = ""; state["lfo" + li + ":target_param"] = ""; }
      else state["lfo" + li + ":target"] = prefix + (to + 1);
    }
    recount(prefix);
  };
  const setSlotParam = (slot, key, val) => {
    writes.push(key + "=" + val);
    const verb = /^(midi_fx|fx):(insert|remove|move)$/.exec(key);
    if (verb) {
      const prefix = verb[1];
      if (verb[2] === "insert") {
        const at = parseInt(val, 10) - 1;
        permute(prefix, (i) => (i >= at ? i + 1 : i));
      } else if (verb[2] === "remove") {
        const at = parseInt(val, 10) - 1;
        for (const k of Object.keys(state)) {
          if (k.indexOf(prefix + (at + 1) + "_module") === 0 ||
              k.indexOf(prefix + (at + 1) + ":") === 0) delete state[k];
        }
        permute(prefix, (i) => (i === at ? -1 : (i > at ? i - 1 : i)));
      } else {
        const parts = String(val).split(">");
        const from = parseInt(parts[0], 10) - 1, to = parseInt(parts[1], 10) - 1;
        permute(prefix, (i) => {
          if (i === from) return to;
          if (to > from) return (i > from && i <= to) ? i - 1 : i;
          return (i >= to && i < from) ? i + 1 : i;
        });
      }
      return true;
    }
    const mod = /^(.*):module$/.exec(key);
    if (mod) {
      state[mod[1] + "_module"] = val; state[mod[1] + ":state"] = "";
      /* Unloading a position also drops its modulation table entries
         (chain_mod_clear_target_entries, chain_host.c), and a modulated param
         base lives only there -- so it goes with them. Without this a base read
         after the write answers the old value and a broken carry looks fine. */
      for (const k of Object.keys(state)) {
        if (k.indexOf(mod[1] + ":") === 0 && /:base$/.test(k)) delete state[k];
      }
      const sec = /^(midi_fx|fx)\d+$/.exec(mod[1]);
      if (sec) recount(sec[1]);
    }
    else state[key] = val;
    return true;
  };
  const w = { state, getSlotParam, setSlotParam, writes,
              get reads() { return reads; }, resetReads() { reads = []; },
              resetWrites() { writes.length = 0; } };

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
  const slotChainComponents = (i) => chainEditorComponents(w.chainConfigs[i] || emptyChain());
  const slotChainComponentIndex = lift("slotChainComponentIndex", ["slotChainComponents"])(slotChainComponents);
  const chainEditorKeyAt = lift("chainEditorKeyAt", [])();
  Object.assign(w, { chainEditorComponents, chainComponentId, chainComponentParamKey,
                     getChainComponentModule, setChainComponentModule, slotChainComponents,
                     getComponentParamPrefix });

  /* The CHAIN TARGET, and the two shared draw helpers that take one. Lifted,
     not restated: a target rebuilt here would spell the keys itself, which is
     the drift the target exists to end -- and the read counts below are only
     the real ones if the real addressing is what runs. */
  /* Declared BEFORE the target, which writes to it through setSelection. */
  w.lastChainComponent = [];
  const slotChainTarget = lift("slotChainTarget",
    ["chainComponentParamKey", "slotChainComponents", "chainConfigs",
     "createEmptyChainConfig", "invalidateChainConfig", "loadChainConfigFromSlot",
     "CHAIN_CAP", "selectedChainComponent", "lastChainComponent", "selectedSlot"])(
    chainComponentParamKey, slotChainComponents, w.chainConfigs, createEmptyChainConfig,
    invalidateChainConfig, loadChainConfigFromSlot, CHAIN_CAP, 0, w.lastChainComponent, 0);
  w.slotChainTarget = slotChainTarget;
  w.target = slotChainTarget(0);
  const chainTargetGetParam = lift("chainTargetGetParam", ["getSlotParam"])(getSlotParam);
  const chainLfoTargetMap = lift("chainLfoTargetMap", ["getSlotParam"])(getSlotParam);
  const chainComponentBypassed = lift("chainComponentBypassed",
    ["chainTargetGetParam"])(chainTargetGetParam);

  const chainSectionPrefix = lift("chainSectionPrefix", [])();
  const writeChainShape = lift("writeChainShape",
    ["chainSectionPrefix", "setSlotParam"])(chainSectionPrefix, setSlotParam);
  w.writeChainShape = writeChainShape;

  w.moveChainComponent = lift("moveChainComponent",
    ["chainComponentId", "parseChainId", "chainMoveBy", "writeChainShape",
     "resetLfoTargetLabels", "invalidateKnobContextCache"])(
    chainComponentId, parseChainId, chainMoveBy, writeChainShape, noop, noop);

  const applyPickerChoiceToChain = lift("applyPickerChoiceToChain",
    ["chainComponentId", "parseChainId", "chainRemoveAt", "setChainComponentModule",
     "getChainComponentModule"])(
    chainComponentId, parseChainId, chainRemoveAt, setChainComponentModule,
    getChainComponentModule);

  const getSlotModuleSignature = lift("getSlotModuleSignature", ["getSlotParam", "CHAIN_CAP"])(getSlotParam, CHAIN_CAP);

  w.slotUserCleared = [];
  /* A picker swap drops any LFO routing aimed at the position it replaces --
     see test_chain_lfo_routing_swap.sh. Lifted rather than stubbed so the
     reads and writes it makes are counted in this file budget too. */
  const pickerReplacedModule = lift("pickerReplacedModule", [])();
  const clearLfoRoutingForComponent = lift("clearLfoRoutingForComponent",
    ["getSlotParam", "setSlotParam"])(getSlotParam, setSlotParam);
  const applyComponentSelectionConfirmed = lift("applyComponentSelectionConfirmed",
    ["writeChainShape", "slotChainTarget", "host_log", "setSlotParam", "print", "host_track_event",
     "loadChainConfigFromSlot", "lastSlotModuleSignatures", "getSlotModuleSignature",
     "invalidateKnobContextCache", "selectedSlot", "slotChainComponents",
     "selectedChainComponent", "lastChainComponent", "setView", "VIEWS", "needsRedraw",
     "pickerReplacedModule", "clearLfoRoutingForComponent", "getComponentParamPrefix"])(
    writeChainShape, slotChainTarget, undefined, setSlotParam, noop, undefined,
    loadChainConfigFromSlot, [], getSlotModuleSignature,
    noop, 0, slotChainComponents, 0, w.lastChainComponent, noop, VIEWS, false,
    pickerReplacedModule, clearLfoRoutingForComponent, getComponentParamPrefix);

  const pend = liftBlock("let pendingChainInsert = null;", "withPendingChainInsert",
    ["chainEditorKeyAt", "getChainComponentModule", "announce", "chainInsertAt"],
    ["beginPendingChainInsert", "beginChainInsertFromAddBox", "pendingChainInsertFor",
     "cancelPendingChainInsert", "withPendingChainInsert"])(
    chainEditorKeyAt, getChainComponentModule, noop, chainInsertAt);
  Object.assign(w, pend);

  /* availableModules / selectedModuleIndex are what the picker was showing when
     the user clicked; they are set per test. */
  w.picker = { availableModules: [], selectedModuleIndex: 0, selectedChainComponent: 0 };
  /* The feedback gate. `auto` is the ordinary case (no line-in module, the
     callback fires straight away); holding it parks the pick behind an open
     modal, which is the state the optimistic model is visible in. */
  w.gate = { auto: true, pending: null };
  /* applyChainComponentPick is the tail end of applyComponentSelection,
     pulled into its own function (see src/shadow/shadow_ui.js) so a "Remove
     Module" row elsewhere in the UI can reach the same sequence without going
     through the picker. Lifted the same way applyComponentSelectionConfirmed
     is above it -- its call happens INSIDE applyComponentSelection now, so
     leaving it unlifted would make that call a ReferenceError the moment the
     lifted body runs. `w.chainComponentPickEntries` counts real entries (via
     the resetLfoTargetLabels slot, which the real function calls
     unconditionally before any branch) so a future dependency-list drift that
     makes the call silently no-op is caught rather than measured as budget. */
  w.chainComponentPickEntries = 0;
  const countedResetLfoTargetLabels = () => { w.chainComponentPickEntries++; };
  const applyChainComponentPick = lift("applyChainComponentPick",
    ["slotChainComponentIndex", "slotChainComponents", "chainConfigs", "createEmptyChainConfig",
     "withPendingChainInsert", "applyPickerChoiceToChain", "invalidateChainConfig",
     "slotUserCleared", "chainHasAnyModule", "resetLfoTargetLabels", "chainComponentParamKey",
     "host_get_module_metadata", "host_log", "maybeConfirmForModule",
     "applyComponentSelectionConfirmed", "loadChainConfigFromSlot", "setView", "VIEWS",
     "needsRedraw"])(
    slotChainComponentIndex, slotChainComponents, w.chainConfigs, createEmptyChainConfig,
    pend.withPendingChainInsert, applyPickerChoiceToChain, invalidateChainConfig,
    w.slotUserCleared, chainHasAnyModule, countedResetLfoTargetLabels, chainComponentParamKey,
    undefined, undefined,
    (meta, cb) => { if (w.gate.auto) cb(true); else w.gate.pending = cb; },
    applyComponentSelectionConfirmed, loadChainConfigFromSlot, noop, VIEWS, false);
  w.applyChainComponentPick = applyChainComponentPick;

  /* applyComponentSelection own dependency list is trimmed to what its
     body (the picker non-shape branches, plus handing off to
     applyChainComponentPick) actually references post-extraction --
     createEmptyChainConfig, applyPickerChoiceToChain, invalidateChainConfig,
     slotUserCleared, chainHasAnyModule, resetLfoTargetLabels,
     chainComponentParamKey, host_get_module_metadata, host_log,
     maybeConfirmForModule, applyComponentSelectionConfirmed,
     loadChainConfigFromSlot, print and withPendingChainInsert all moved into
     applyChainComponentPick with it. Verified against the source slice, not
     guessed. */
  w.applyComponentSelection = () => lift("applyComponentSelection",
    ["slotChainComponents", "selectedSlot", "selectedChainComponent", "availableModules",
     "selectedModuleIndex", "isChainModuleKey", "setView", "VIEWS", "getChainComponentModule",
     "chainConfigs", "enterPresetBrowser", "getComponentParamPrefix", "enterStorePicker",
     "moveChainComponent", "slotChainTarget", "slotChainComponentIndex", "chainEditorKeyAt",
     "lastChainComponent",
     "announce", "needsRedraw", "pendingChainInsertFor", "cancelPendingChainInsert",
     "applyChainComponentPick",
     /* The list-filter row cycles in place at the top of this function and
        returns, so its four names are referenced before any branch this test
        exercises. None of them is reachable here -- the fixture picker never
        selects the filter row -- but an unbound name is a ReferenceError at
        CALL time, not at branch time. */
     "PICKER_FILTER_ID", "ModuleLists", "componentSelectFilter",
     "pickerEligibleLists", "scanModulesForType", "enterComponentSelect"])(
    slotChainComponents, 0, w.picker.selectedChainComponent, w.picker.availableModules,
    w.picker.selectedModuleIndex, isChainModuleKey, noop, VIEWS, getChainComponentModule,
    w.chainConfigs, noop, getComponentParamPrefix, noop,
    w.moveChainComponent, slotChainTarget, slotChainComponentIndex, chainEditorKeyAt,
    w.lastChainComponent,
    noop, false, pend.pendingChainInsertFor, pend.cancelPendingChainInsert,
    applyChainComponentPick,
    "__list_filter__", null, null, noop, noop, noop)();

  /*
   * The `+` click, lifted out of the CHAIN_EDIT jog-click switch.
   *
   * It is inline in a switch rather than a function, so it comes out as a text
   * slice wrapped in a switch of its own -- it ends in `break`. Lifted rather
   * than reimplemented because the whole question here is what the REAL handler
   * inserts and where, and a reimplementation would happily agree with itself.
   */
  w.opened = null;
  const enterComponentSelect = (slotIndex, componentIndex) => {
    const c = slotChainComponents(slotIndex)[componentIndex];
    w.opened = { index: componentIndex, key: c && c.key, label: c && c.label };
    w.picker.selectedChainComponent = componentIndex;
  };
  {
    const at = src.indexOf("if (comp && comp.kind === \"add\") {");
    const close = at >= 0 ? src.indexOf("\n                }", at) : -1;
    if (close < 0) fail("the `+` click handler is gone from the CHAIN_EDIT switch");
    else {
      const deps = ["beginChainInsertFromAddBox", "slotChainTarget", "selectedSlot",
                    "enterComponentSelect"];
      w.clickAdd = new Function(...deps,
        "return function(comp) { switch (1) { case 1:\n" +
        src.slice(at, close + 18) + "\n} };")(
        pend.beginChainInsertFromAddBox, slotChainTarget, 0, enterComponentSelect);
    }
  }
  /*
   * The BACK out of the picker, lifted from the handleBack switch for the same
   * reason as the `+` click: calling cancelPendingChainInsert directly would
   * test the drop and not the WIRING, and the wiring is where a cancelled `+`
   * gets forgotten about.
   */
  {
    const at = src.indexOf("case VIEWS.COMPONENT_SELECT:",
                           src.indexOf("announce(\"Chain Editor\")") - 900);
    const stop = at >= 0 ? src.indexOf("break;", at) : -1;
    if (stop < 0) fail("the COMPONENT_SELECT back handler is gone");
    else {
      w.back = new Function("cancelPendingChainInsert", "setView", "VIEWS", "announce",
                            "needsRedraw",
        "return function() { switch (1) { case 1:\n" +
        src.slice(at, stop + 6) + "\n} };")(
        pend.cancelPendingChainInsert, noop, VIEWS, noop, false);
    }
  }

  /* Press the `+` for a section, the way the editor reaches it. */
  w.pressAdd = (section) => {
    const key = section === "midiFx" ? "add_midi" : "add_fx";
    w.clickAdd(slotChainComponents(0).find((c) => c.key === key));
  };
  /* ...and then choose `id` in the picker it opened ("" is the None row). */
  w.pick = (id) => {
    w.picker.availableModules.length = 0;
    w.picker.availableModules.push({ id, name: id || "None" });
    w.picker.selectedModuleIndex = 0;
    w.applyComponentSelection();
  };

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
    /* The knob card. These are deps rather than absent identifiers ON PURPOSE:
       a free identifier under the lift is a ReferenceError, so the obvious way
       to keep the card out of the way is a typeof guard -- which is exactly
       what made the card block silently unreachable here, leaving this test
       measuring the chain editor with the card switched off. The card is the
       newest thing on this screen and its whole claim is that it costs nothing
       per frame, so it belongs INSIDE the budget, not outside it. */
    "knobCardDrawState", "drawKnobCard",
    /* The chain target and the two helpers shared with the Master FX editor. */
    "slotChainTarget", "chainLfoTargetMap", "chainComponentBypassed",
    /* The shared bands (header / label / info / footer). A noop here: they draw
       through the injected ctx and read no params, so they cost nothing this
       test is measuring -- what it measures is the info line the editor
       COMPUTES before handing it over, which is still done above. */
    "drawChainEditorBands",
    /* ...and the Shift footer helper beside them, for the same reason: it is a
       free identifier under the lift, and a typeof guard would make the footer
       block unreachable. A noop is right here -- it reads no params. */
    "shiftHintsFor", "CHAIN_HINTS_AT_REST",
  ];
  const mk = lift("drawChainEdit", drawDeps);
  const makeDraw = (diagram, cardState) => mk(noop, {}, 0, () => false, [{ name: "s" }], truncateText,
    noop, noop, () => 10, noop, w.chainConfigs, createEmptyChainConfig, 0,
    getSlotParam, noop, DIAGRAM_Y, MOVY_RULE_Y, noop, getSlotParam,
    slotChainComponents, diagram, getChainComponentModule, (m) => m,
    chainComponentParamKey, DIAGRAM_BOX_H, SCREEN_WIDTH, getComponentParamPrefix,
    noop, () => false, ensureChainConfigFresh,
    () => (cardState === undefined ? null : cardState),
    () => { w.cardDraws++; },
    slotChainTarget, chainLfoTargetMap, chainComponentBypassed, noop,
    () => [], []);
  w.cardDraws = 0;
  w.draw = makeDraw(recording);
  w.drawReal = makeDraw(drawChainDiagram);
  /* The same lift with the card UP. The state object is opaque here -- what is
     being measured is whether drawing it costs a read, not what it looks like
     (tests/host/test_knob_card.sh owns the pixels). */
  w.drawRealWithCard = makeDraw(drawChainDiagram, { name: "CUTOFF", value: "0.62" });
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

  /* The knob card CALL SITE must be free.
     drawKnobCard is a stub here, so what this pins is drawChainEdit: that the
     card block runs, and that raising it adds no read of its own -- everything
     it draws was read once on touch-down (knobCardOpen). The RENDERER is a
     separate claim with a separate test, tests/host/test_chain_knob_card_reads.sh,
     which drives the real knob_card.mjs. A read here would be the worst
     possible place for one: the card is up precisely while a finger is moving
     an encoder, which is when the screen most needs to keep up.
     The cardDraws counter is not decoration -- without it this assertion passes
     just as happily when the card block never runs at all, which is the
     failure this whole dependency plumbing exists to prevent. */
  {
    const w = world();
    loadSlot(w, { fx: Array.from({ length: 8 }, () => "freeverb"),
                  midiFx: Array.from({ length: 8 }, () => "arp") });
    for (let f = 0; f < 5; f++) { w.resetReads(); w.drawReal(); }
    const withoutCard = w.reads.length;
    if (w.cardDraws !== 0) fail("the card drew while it was not up");
    w.cardDraws = 0;
    for (let f = 0; f < 5; f++) { w.resetReads(); w.drawRealWithCard(); }
    const withCard = w.reads.length;
    if (w.cardDraws !== 5)
      fail("the card block ran " + w.cardDraws + " times across 5 frames, expected 5 -- " +
           "this test is not exercising the card at all, so its budget is unmeasured");
    if (withCard !== withoutCard)
      fail("a frame with the knob card up costs " + withCard + " reads against " +
           withoutCard + " without it -- the call site must hand the card what " +
           "knobCardOpen already read, never read on the draw path");
  }
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
  if (!w.moveChainComponent(w.target, "fx1", 1)) fail("B1: the move was refused");
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
  if (!w.moveChainComponent(w.target, "fx1", 1)) fail("B2: the move was refused");
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
       proves it does so even when it writes no module ids at all.

       The freshness assignment is matched on the LEFT-HAND SIDE only, not on
       `= true`. The one in the loader is conditional now -- `= !incomplete` --
       because a load whose reads timed out has not read the chain and must not
       latch as authoritative (see loadChainConfigFromSlot). What this check is
       for is a site that says NOTHING about the cache; a site that says "clean
       only if it worked" is saying more than one that hardcodes true, not less.

       It also uses the full window rather than 300 chars, since that reasoning
       is written out between the assignment and the flag. */
    const ok = /chainConfigFresh\[slotIndex\]\s*=/.test(after) ||
               /invalidateChainConfig\(/.test(after) ||
               /writeChainShape\(/.test(after.slice(0, 400));
    if (!ok) sites.push(lineNo + ": " + m[0].trim());
  }
  if (sites.length)
    fail("a chain config is replaced without saying anything about the cached view, " +
         "so the editor can draw a chain that is no longer loaded:\n    " + sites.join("\n    "));
  if (!/let chainConfigFresh/.test(src)) fail("chainConfigFresh is gone");
}

/* ==================================================================== */
/* D. EACH `+` ADDS WHERE ITS BOX IS DRAWN                               */
/* ==================================================================== */
/* The diagram reads [+ Add MIDI FX][MIDI FX...][Synth][FX...][+ Add FX], so the
   MIDI `+` is the LEFTMOST box and the audio one is at the right. Both used to
   append, which put a new MIDI FX at the far end of its section from the button
   that was pressed -- nearest the synth. Now the MIDI one inserts at the head.

   Inserting at the head RENUMBERS: midi_fx1 becomes midi_fx2. So these tests
   are as much about what the EXISTING modules keep across that shift as about
   where the new one lands. */

const sectionOf = (w, prefix) => {
  const n = parseInt(w.state[prefix + "_count"] || "0", 10);
  const out = [];
  for (let i = 1; i <= n; i++) out.push(w.state[prefix + i + "_module"] || "-");
  return out.join(",");
};

/* D1. THE REPORTED BUG. The MIDI `+` is drawn at the front, so it adds at the
       front -- the new module comes FIRST, not nearest the synth. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp"] });
  w.screen();
  w.pressAdd("midiFx");
  w.pick("chord");
  if (sectionOf(w, "midi_fx") !== "chord,arp")
    fail("D1: the MIDI `+` is the leftmost box but its module landed at " +
         sectionOf(w, "midi_fx") + " (it should be chord,arp)");
  /* Proof the lifted applyChainComponentPick was actually ENTERED, not just
     defined -- a future dependency-list drift that leaves it unreachable
     (see the knob-card note above about typeof guards making a block dead)
     would still leave D1 green through some other path, so this checks the
     function itself ran rather than just its downstream effect. */
  if (w.chainComponentPickEntries < 1)
    fail("D1: applyChainComponentPick was never entered -- the lift is dead");
}

/* D2. ...and the audio `+`, drawn at the right, still APPENDS. The asymmetry is
       the rule. Making both ends behave the same is what caused this. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb"] });
  w.screen();
  w.pressAdd("fx");
  w.pick("cloudseed");
  if (sectionOf(w, "fx") !== "freeverb,cloudseed")
    fail("D2: the audio `+` no longer appends: " + sectionOf(w, "fx"));
}

/* D3. The picker opens on the NEW position and NAMES it. A front insert makes
       the new module MIDI FX 1, and a picker headed "MIDI FX 2" would be
       describing the module the user just pushed out of the way. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp"] });
  w.screen();
  w.pressAdd("midiFx");
  if (!w.opened) fail("D3: the `+` never opened a picker");
  else if (w.opened.key !== "midiFx")
    fail("D3: the picker opened on " + w.opened.key + " rather than the new front position");
  else if (w.opened.label !== "MIDI FX 1")
    fail("D3: the picker names the new front position " + w.opened.label);
  /* and the audio side names the position it appended */
  const v = world();
  loadSlot(v, { fx: ["freeverb"] });
  v.screen();
  v.pressAdd("fx");
  if (!v.opened || v.opened.key !== "fx2")
    fail("D3: the audio `+` opened on " + (v.opened && v.opened.key) + " rather than fx2");
}

/* D4. BACKING OUT WRITES NOTHING. A half-applied insert -- the renumber landed
       but no module arrived -- is worse than the bug being fixed, so the
       cancel path must leave the DSP byte for byte as it was. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp", "chord"] });
  w.screen();
  const before = JSON.stringify(w.state);
  w.pressAdd("midiFx");
  w.resetWrites();
  w.cancelPendingChainInsert();
  if (w.writes.length)
    fail("D4: backing out of a `+` picker WROTE to the slot: [" + w.writes.join(" ") + "]");
  if (JSON.stringify(w.state) !== before)
    fail("D4: backing out of a `+` picker changed the slot");
  if (w.screen().indexOf("sf2") < 0 || sectionOf(w, "midi_fx") !== "arp,chord")
    fail("D4: the chain no longer reads arp,chord after a cancelled insert: " +
         sectionOf(w, "midi_fx"));
  if (w.chainConfigs[0].midiFx.length !== 2)
    fail("D4: the cancelled position survived in the model");
}

/* D5. And `None` from a `+` picker is the same no-op. It is NOT a removal: the
       position was never written, so routing it through the removal path would
       hand writeChainOrder a prevList containing a hole the DSP never had --
       every identity lookup off by one, every state pushed onto its neighbour. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp", "chord"] });
  w.screen();
  w.pressAdd("midiFx");
  w.resetWrites();
  w.pick("");
  if (w.writes.length)
    fail("D5: choosing None in a `+` picker WROTE to the slot: [" + w.writes.join(" ") + "]");
  if (sectionOf(w, "midi_fx") !== "arp,chord")
    fail("D5: choosing None in a `+` picker changed the chain: " + sectionOf(w, "midi_fx"));
}

/* D6. THE ONE THIS FEATURE LIVES OR DIES ON. midi_fx1 is TUNED, MODULATED and
       an LFO TARGET all at once; inserting in front of it makes it midi_fx2,
       and all three have to still be true of it there.

       Note what is NOT asserted any more: that the editor carried them. It
       used to, because the insert was expressed as a run of module writes and
       every one of those destroyed a position. The insert is now one verb that
       permutes the DSP arrays, so the arp instance is never unloaded -- its
       state, its modulation entry (and the base value inside it) and the
       routings that name it are still the originals. The editor writing them
       back would now be overwriting the real thing with a snapshot of it. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp"] });
  w.state["midi_fx1:state"] = "tuned-arp";
  w.state["lfo1:target"] = "midi_fx1";
  w.state["lfo1:target_param"] = "rate";
  w.state["midi_fx1:rate:base"] = "0.25";
  w.state["midi_fx1:rate"] = "0.61";   /* the EFFECTIVE value, mid-sweep */
  w.screen();
  w.pressAdd("midiFx");
  w.resetWrites();
  w.pick("chord");
  if (sectionOf(w, "midi_fx") !== "chord,arp")
    fail("D6 setup: " + sectionOf(w, "midi_fx"));
  if (w.state["midi_fx2:state"] !== "tuned-arp")
    fail("D6: the arp was pushed to midi_fx2 and lost its tuning: " + w.state["midi_fx2:state"]);
  if (w.state["lfo1:target"] !== "midi_fx2")
    fail("D6: the LFO still aims at " + w.state["lfo1:target"] +
         ", which is now the module that was inserted IN FRONT of the one it was routed to");
  if (w.state["lfo1:target_param"] !== "rate")
    fail("D6: the pushed module LFO lost its param: " + w.state["lfo1:target_param"]);
  if (w.state["midi_fx2:rate:base"] !== "0.25")
    fail("D6: the modulation base did not survive the insert: midi_fx2:rate:base=" +
         w.state["midi_fx2:rate:base"]);
  /* The arp was NOT rebuilt. A module write on its position is the thing that
     would destroy the instance -- and with it the phase a state blob cannot
     carry -- so it must not be there. */
  if (w.writes.some((x) => x.startsWith("midi_fx2:module=")))
    fail("D6: the pushed module was RELOADED, which is what this whole change is " +
         "about not doing: [" + w.writes.join(" ") + "]");
  if (w.writes.some((x) => /:state=|:base=|^lfo\d:target/.test(x)))
    fail("D6: the editor carried state or routing that nothing destroyed: [" +
         w.writes.join(" ") + "]");
  /* Exactly two writes: open the hole, then load into it. */
  if (w.writes.join(" ") !== "midi_fx:insert=1 midi_fx1:module=chord")
    fail("D6: an insert should be one verb plus one module write, got [" +
         w.writes.join(" ") + "]");
}

/* D7. A cancelled `+` leaves no RECORD either. Left set, the next ordinary swap
       at that same key would be mistaken for the insert and rewritten as a
       renumber -- against a prevList describing an edit that never happened. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp", "chord"] });
  w.state["midi_fx2:state"] = "tuned-chord";
  w.screen();
  w.pressAdd("midiFx");
  w.cancelPendingChainInsert();
  w.picker.selectedChainComponent =
    w.slotChainComponents(0).findIndex((c) => c.key === "midiFx");
  w.resetWrites();
  w.pick("velocity");
  if (sectionOf(w, "midi_fx") !== "velocity,chord")
    fail("D7: an ordinary swap after a cancelled `+` renumbered the section: " +
         sectionOf(w, "midi_fx"));
  if (w.writes.filter((x) => x.includes(":module=")).length !== 1)
    fail("D7: a swap after a cancelled `+` wrote more than the one position: [" +
         w.writes.join(" ") + "]");
}

/* D7b. ...and BACK is the gesture that actually drops it. The pending record
        is cleared by the back handler in the editor, not by anything the tests
        can reach directly -- a cancel path that drops the model entry but keeps
        the record leaves the next swap at that key looking like the insert. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp", "chord"] });
  w.screen();
  w.pressAdd("midiFx");
  w.back();
  w.picker.selectedChainComponent =
    w.slotChainComponents(0).findIndex((c) => c.key === "midiFx");
  w.resetWrites();
  w.pick("velocity");
  if (w.writes.some((x) => x.includes(":insert=")))
    fail("D7b: a swap after BACKING OUT of a `+` was rewritten as an insert: [" +
         w.writes.join(" ") + "]");
  if (sectionOf(w, "midi_fx") !== "velocity,chord")
    fail("D7b: a swap after backing out changed the shape: " + sectionOf(w, "midi_fx"));
}

/* D7c. And the record is dropped on CONFIRM too, not only on cancel. Left set,
        the very next pick at that key inserts a second position. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp"] });
  w.screen();
  w.pressAdd("midiFx");
  w.pick("chord");
  w.picker.selectedChainComponent =
    w.slotChainComponents(0).findIndex((c) => c.key === "midiFx");
  w.resetWrites();
  w.pick("velocity");
  if (w.writes.some((x) => x.includes(":insert=")))
    fail("D7c: the pending record outlived the pick it belonged to: [" +
         w.writes.join(" ") + "]");
  if (sectionOf(w, "midi_fx") !== "velocity,arp")
    fail("D7c: swapping the just-inserted module inserted another: " +
         sectionOf(w, "midi_fx"));
}

/* D7d. The record is claimed by SLOT and by POSITION, and only while that
        position is still the hole the `+` opened. Each of those is what stops a
        pick made somewhere else from being rewritten as somebody else edit. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp"] });
  w.screen();
  w.pressAdd("midiFx");
  if (!w.pendingChainInsertFor(w.target, "midiFx"))
    fail("D7d: the `+` did not record a pending insert at all");
  if (w.pendingChainInsertFor(w.slotChainTarget(1), "midiFx"))
    fail("D7d: another SLOT pick adopted this slot pending insert");
  if (w.pendingChainInsertFor(w.target, "midi_fx2"))
    fail("D7d: another POSITION adopted the pending insert");
  /* An empty position in the OTHER section is the case the emptiness guard
     cannot catch on its own -- nothing is loaded there either, so only the key
     says this record is not about it. */
  if (w.pendingChainInsertFor(w.target, "fx1"))
    fail("D7d: a position in the other SECTION adopted the pending insert");
  /* Anything that reloaded the slot underneath the picker has already dropped
     the hole; the record left behind would be a lie. */
  w.chainConfigs[0].midiFx[0] = { module: "chord", params: {} };
  if (w.pendingChainInsertFor(w.target, "midiFx"))
    fail("D7d: the pending insert was claimed for a position that is no longer empty");
}

/* D7e. DECLINING THE FEEDBACK GATE leaves nothing behind either. This is the
        one path where the pending position goes back to being EMPTY after the
        pick -- the gate reloads the slot -- so the emptiness guard cannot be
        what forgets the record, and a stale one would turn the users next
        ordinary swap into an insert. */
{
  const w = world();
  loadSlot(w, { midiFx: ["arp"] });
  w.screen();
  w.gate.auto = false;
  w.pressAdd("midiFx");
  w.pick("chord");
  if (!w.gate.pending) fail("D7e setup: the gate did not park the pick");
  w.gate.pending(false);
  if (sectionOf(w, "midi_fx") !== "arp")
    fail("D7e: declining the gate still changed the chain: " + sectionOf(w, "midi_fx"));
  /* And the MODEL is put back too, not just the DSP: the decline reloads the
     slot, which is what drops both the optimistic pick and the hole the `+`
     opened. Left alone the editor would draw a chain that was never loaded. */
  if (w.screen() !== "arp,sf2")
    fail("D7e: after declining, the editor still draws " + w.screen());
  w.gate.auto = true;   /* the next pick is an ordinary one and must APPLY */
  w.picker.selectedChainComponent =
    w.slotChainComponents(0).findIndex((c) => c.key === "midiFx");
  w.resetWrites();
  w.pick("velocity");
  if (!w.writes.length) fail("D7e setup: the follow-up swap wrote nothing at all");
  if (w.writes.some((x) => x.includes(":insert=")))
    fail("D7e: a declined `+` left a pending record that rewrote the next swap " +
         "as an insert: [" + w.writes.join(" ") + "]");
  if (sectionOf(w, "midi_fx") !== "velocity")
    fail("D7e: the swap after a declined gate gave " + sectionOf(w, "midi_fx"));
}

/* D7f. Cancelling puts the SELECTION back on the `+` the user pressed, and it
        has to be the index in the reloaded chain -- with the cancelled hole
        still in the model the audio `+` sits one place further right, so a
        cancel that restored the selection without dropping the model first
        would leave the cursor on the box past it. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb"] });
  w.screen();
  const addAt = w.slotChainComponents(0).findIndex((c) => c.key === "add_fx");
  w.pressAdd("fx");
  w.back();
  if (w.lastChainComponent[0] !== addAt)
    fail("D7f: after cancelling, the selection sits at " + w.lastChainComponent[0] +
         " rather than on the `+` at " + addAt);
}

/* D7g. A FULL section does not open a picker at all. Without the cap check the
        `+` would record a pending insert for a position that cannot exist and
        open the picker on nothing. */
{
  const w = world();
  loadSlot(w, { fx: ["a", "b", "c", "d", "e", "f", "g", "h"] });
  w.screen();
  w.opened = null;
  w.resetWrites();
  w.pressAdd("fx");
  if (w.opened) fail("D7g: a full section opened a picker on " + w.opened.key);
  if (w.writes.length) fail("D7g: a refused `+` wrote to the slot");
  if (w.chainConfigs[0].fx.length !== 8)
    fail("D7g: a refused `+` grew the section to " + w.chainConfigs[0].fx.length);
}

/* D8. An APPEND stays one write. Nothing is to its right, so nothing renumbers,
       and routing it through the section rewrite would spend a section`s worth
       of IPC on a gesture the user is watching. */
{
  const w = world();
  loadSlot(w, { fx: ["freeverb", "cloudseed", "tapescam"] });
  w.screen();
  w.pressAdd("fx");
  w.resetWrites();
  w.pick("psxverb");
  const moduleWrites = w.writes.filter((x) => x.includes(":module="));
  if (moduleWrites.join(" ") !== "fx4:module=psxverb")
    fail("D8: appending rewrote positions that did not change: [" + moduleWrites.join(" ") + "]");
}

if (failures) process.exit(1);
console.log("PASS: chain edit read budget — the editor reloads a slot on an EDIT, not on a " +
            "frame (reads no longer scale with chain length), and every path that mutates the " +
            "chain invalidates, including a same-module reorder that writes no module ids at all; " +
            "and each `+` adds where its box is DRAWN — the MIDI one at the head, with the " +
            "modules it pushes along keeping their state, routing and modulation base because " +
            "they are permuted rather than reloaded, and backing out or None writing nothing");
'
