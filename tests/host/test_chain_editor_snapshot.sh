#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE PIXEL BASELINE FOR THE TWO CHAIN EDITORS, CAPTURED BEFORE THEY CONVERGE.
#
# Step 4a of the Master FX variable-length design converges drawChainEdit and
# drawMasterFx into ONE editor parameterised by a chain target, and its whole
# claim is "no behaviour change". src/shadow/shadow_ui.js is ~18,000 lines, runs
# under QuickJS on hardware, and has no behavioural harness beyond the lifted
# block trick in test_chain_edit_read_budget.sh. Nothing in code review can
# substantiate that claim. A per-case pixel hash of the screens as they are TODAY
# can.
#
# THE ORDER IS THE POINT. A baseline regenerated after the refactor compares the
# new code against itself and proves exactly nothing -- it is not a weaker test,
# it is a test of a tautology that still prints PASS. So the 46 chain/ hashes in
# tests/fixtures/chain-editor-baseline.txt must be re-derivable from the commit
# BEFORE 4a touches either draw function, and a reviewer should check that by
# regenerating from that parent commit and byte-comparing. The same trap was
# avoided once already on this branch, for render_page_movy (commit c4f61538).
#
# The 25 master/ hashes were refreshed ONCE, at step 4a-3, whose entire purpose
# was to move those pixels: Master FX drew no footer, wore the older header and
# sat 6px low, and 4a-3 gave it the slot editor chrome. The refresh is what this
# repo calls a reviewed fixture change (tools/param-pages/regenerate.mjs says so
# explicitly) -- the renders were read case by case first, and the check that it
# was not a cover for a refactor mistake is that ZERO chain/ hashes moved in the
# same commit.
#
# HOW THE SCREENS ARE DRIVEN
#   drawChainEdit  is LIFTED out of shadow_ui.js with `new Function` and an
#                  explicit dependency list, the technique
#                  test_chain_edit_read_budget.sh established. That file also
#                  taught the trap this one has to survive: a free identifier
#                  under the lift is a ReferenceError, so the tempting fix -- a
#                  typeof guard -- makes a whole block silently unreachable and
#                  the test then measures a screen with a feature switched off.
#                  An incomplete dependency list here would snapshot a screen
#                  that is MISSING things, and the baseline would bless it.
#   drawMasterFx   is lifted out of shadow_ui_master_fx.mjs the same way, but it
#                  takes its shared state through the `ctx` object, so the state
#                  itself is supplied directly rather than reconstructed.
#
# THE CONTENT FLOOR is the defence against exactly that. Every case must light a
# minimum number of pixels AND leave none of its bands empty -- header, diagram,
# label/info and footer. Since 4a-3 that is the SAME band list for both screens.
# A silently blank region cannot pass as a baseline.
#
# Renderers are the real ones (chain_diagram.mjs, render_page_movy.mjs,
# menu_layout.mjs, knob_card.mjs) and the native draw primitives are supplied,
# because the device always supplies them and the renderers take a different
# path when they are absent.
#
# Regenerate (only when a change to these screens is INTENDED):
#     UPDATE_CHAIN_EDITOR_BASELINE=1 bash tests/host/test_chain_editor_snapshot.sh

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync, writeFileSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { drawChainDiagram, DEFAULT_Y as DIAGRAM_Y, BOX_H as DIAGRAM_BOX_H }
  from "./src/shared/chain_diagram.mjs";
import { DIAGRAM_W } from "./src/shared/chain_diagram.mjs";
import { chainComponents, emptyChain, parseId as parseChainId, MAX_FX, MAX_MIDI_FX }
  from "./src/shared/chain_model.mjs";
import { drawHeader as drawMovyHeader, drawFooter as drawMovyFooter,
         RULE_Y as MOVY_RULE_Y, HEADER_H as MOVY_HEADER_H }
  from "./src/shared/param_pages/render_page_movy.mjs";
import { drawKnobCard } from "./src/shared/param_pages/knob_card.mjs";
import { drawMenuHeader } from "./src/shared/menu_layout.mjs";
import { truncateText } from "./src/shared/chain_ui_views.mjs";
import { drawChainEditorBands } from "./src/shared/chain_editor_chrome.mjs";

const BASELINE_PATH = "tests/fixtures/chain-editor-baseline.txt";
const SCREEN_WIDTH = 128;
const CHAIN_CAP = { midiFx: MAX_MIDI_FX, fx: MAX_FX };
const noop = () => {};
const sha1 = (buf) => createHash("sha1").update(buf).digest("hex");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* ------------------------------------------------------------------ lifting */

const uiSrc = readFileSync("src/shadow/shadow_ui.js", "utf8");
const mfxSrc = readFileSync("src/shadow/shadow_ui_master_fx.mjs", "utf8");

/* Same lift as test_chain_edit_read_budget.sh: pull a top-level function out of
   a device UI module -- which cannot be imported, being full of host globals --
   and hand it its dependencies as parameters, so what runs is the REAL body. */
function liftFrom(src, what, name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone from " + what); return () => null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name + " in " + what); return () => null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}
const lift = (name, deps) => liftFrom(uiSrc, "shadow_ui.js", name, deps);

/* ----------------------------------------------------------- device globals */
/*
 * The renderers below reach for these by NAME, exactly as they do on the
 * device. The native primitives are included on purpose: chain_diagram, the
 * movy header/footer and the knob card all branch on whether the host has them,
 * and the device always does -- a harness without them would exercise the
 * fallbacks and snapshot a screen no user ever sees.
 */
const DEVICE_GLOBAL_NAMES = ["clear_screen", "fill_rect", "draw_rect", "print",
  "text_width", "set_pixel", "draw_line", "fill_circle", "draw_circle",
  "draw_arc", "shadow_get_param", "shadow_get_display_mode",
  "host_send_screenreader"];

function installGlobals(fb, getParam) {
  const d = drawContext(fb);
  const g = {
    clear_screen: () => fb.clearScreen(),
    fill_rect: fb.fillRect,
    /* js_display_draw_rect, pixel for pixel (src/host/js_display.c). */
    draw_rect: (x, y, w, h, v) => {
      if (w <= 0 || h <= 0) return;
      for (let yi = y; yi < y + h; yi++) { fb.setPixel(x, yi, v); fb.setPixel(x + w - 1, yi, v); }
      for (let xi = x; xi < x + w; xi++) { fb.setPixel(xi, y, v); fb.setPixel(xi, y + h - 1, v); }
    },
    print: fb.print,
    text_width: fb.textWidth,
    set_pixel: fb.setPixel,
    draw_line: d.line,
    fill_circle: d.fillCircle,
    draw_circle: d.drawCircle,
    draw_arc: d.drawArc,
    shadow_get_param: getParam,
    /* announce() bails unless the shadow UI is on screen; keep it silent. */
    shadow_get_display_mode: () => 0,
    host_send_screenreader: noop,
  };
  for (const k of DEVICE_GLOBAL_NAMES) globalThis[k] = g[k];
  return g;
}
function clearGlobals() { for (const k of DEVICE_GLOBAL_NAMES) delete globalThis[k]; }

/* ======================================================================== */
/* THE SLOT CHAIN EDITOR                                                     */
/* ======================================================================== */

/* Two abbrevs in the cache so both label paths are drawn: the declared one
   (three characters, which is what the synth band had to make room for) and the
   two-character fallback getModuleAbbrev computes. */
const ABBREV_CACHE = { "settings": "*", "empty": "--", "cloudseed": "CLD", "sf2": "SF2" };

function chainWorld(state) {
  const getSlotParam = (slot, key) => (state[key] !== undefined ? state[key] : "");

  const chainConfigs = [emptyChain()];
  const chainConfigFresh = [];
  const createEmptyChainConfig = () => emptyChain();

  const loadChainConfigFromSlot = lift("loadChainConfigFromSlot",
    ["chainConfigs", "createEmptyChainConfig", "getSlotParam", "CHAIN_CAP",
     "fxDisplayNameCache", "fxDisplayNameSkip", "fxDisplayNameBackoff", "chainConfigFresh"])(
    chainConfigs, createEmptyChainConfig, getSlotParam, CHAIN_CAP, {}, {}, {}, chainConfigFresh);
  const ensureChainConfigFresh = lift("ensureChainConfigFresh",
    ["chainConfigFresh", "chainConfigs", "createEmptyChainConfig", "loadChainConfigFromSlot"])(
    chainConfigFresh, chainConfigs, createEmptyChainConfig, loadChainConfigFromSlot);

  const chainEditorComponents = lift("chainEditorComponents", ["chainComponents"])(chainComponents);
  const chainComponentId = lift("chainComponentId", [])();
  const isChainModuleKey = lift("isChainModuleKey", ["chainComponentId", "parseChainId"])(
    chainComponentId, parseChainId);
  const chainComponentParamKey = lift("chainComponentParamKey",
    ["isChainModuleKey", "chainComponentId"])(isChainModuleKey, chainComponentId);
  const getChainComponentModule = lift("getChainComponentModule",
    ["chainComponentId", "parseChainId"])(chainComponentId, parseChainId);
  const getComponentParamPrefix = lift("getComponentParamPrefix", ["chainComponentId"])(chainComponentId);
  const getModuleAbbrev = lift("getModuleAbbrev", ["moduleAbbrevCache"])(ABBREV_CACHE);
  const slotChainComponents = (i) => chainEditorComponents(chainConfigs[i] || emptyChain());
  const getSlotParamCached = lift("getSlotParamCached",
    ["slotParamCache", "SLOT_PARAM_CACHE_TTL_MS", "getSlotParam"])({}, 5000, getSlotParam);

  /* The CHAIN TARGET and the two draw helpers that take one -- shared with the
     Master FX editor. Lifted rather than restated: a target rebuilt here would
     spell the keys itself, which is exactly the drift it exists to end. */
  const slotChainTarget = lift("slotChainTarget",
    ["chainComponentParamKey", "slotChainComponents"])(chainComponentParamKey, slotChainComponents);
  const chainTargetGetParam = lift("chainTargetGetParam", ["getSlotParam"])(getSlotParam);
  const chainLfoTargetMap = lift("chainLfoTargetMap", ["getSlotParam"])(getSlotParam);
  const chainComponentBypassed = lift("chainComponentBypassed",
    ["chainTargetGetParam"])(chainTargetGetParam);

  return { getSlotParam, getSlotParamCached, chainConfigs, chainConfigFresh,
           createEmptyChainConfig, ensureChainConfigFresh, chainComponentParamKey,
           getChainComponentModule, getComponentParamPrefix, getModuleAbbrev,
           slotChainComponents, slotChainTarget, chainTargetGetParam,
           chainLfoTargetMap, chainComponentBypassed };
}

/*
 * The dependency list. It is the one test_chain_edit_read_budget.sh already
 * drives drawChainEdit through, which is the reason to trust it: it is exercised
 * by a second test with a different purpose, so a name silently dropped from it
 * fails there too rather than only quietly emptying a band here.
 */
const CHAIN_DRAW_DEPS = [
  "clear_screen", "slotDirtyCache", "selectedSlot", "isExistingPreset", "slots",
  "truncateText", "fill_rect", "print", "text_width", "set_pixel",
  "chainConfigs", "createEmptyChainConfig", "selectedChainComponent",
  "getSlotParamCached", "drawMovyHeader", "DIAGRAM_Y", "MOVY_RULE_Y", "draw_rect",
  "getSlotParam", "slotChainComponents", "drawChainDiagram", "getChainComponentModule",
  "getModuleAbbrev", "chainComponentParamKey", "DIAGRAM_BOX_H", "SCREEN_WIDTH",
  "getComponentParamPrefix", "drawMovyFooter", "isShiftHeld", "ensureChainConfigFresh",
  "knobCardDrawState", "drawKnobCard",
  "slotChainTarget", "chainLfoTargetMap", "chainComponentBypassed",
  /* The shared bands (header / label / info / footer), 4a-3. Supplied REAL --
     a noop here would empty three of the four bands the content floor checks,
     which is the whole point of checking them. */
  "drawChainEditorBands",
];
const mkChainDraw = lift("drawChainEdit", CHAIN_DRAW_DEPS);

function renderChain(c) {
  const fb = createFramebuffer();
  const g = installGlobals(fb, (slot, key) => (c.state[key] !== undefined ? c.state[key] : ""));
  const w = chainWorld(c.state);
  const draw = mkChainDraw(
    g.clear_screen, c.dirty ? { 0: true } : {}, 0,
    lift("isExistingPreset", ["slots"])([{ name: c.patchName || "" }]),
    [{ name: c.patchName || "" }], truncateText,
    g.fill_rect, g.print, g.text_width, g.set_pixel,
    w.chainConfigs, w.createEmptyChainConfig, c.sel,
    w.getSlotParamCached, drawMovyHeader, DIAGRAM_Y, MOVY_RULE_Y, g.draw_rect,
    w.getSlotParam, w.slotChainComponents, drawChainDiagram, w.getChainComponentModule,
    w.getModuleAbbrev, w.chainComponentParamKey, DIAGRAM_BOX_H, SCREEN_WIDTH,
    w.getComponentParamPrefix, drawMovyFooter, () => !!c.shift, w.ensureChainConfigFresh,
    () => (c.card || null), drawKnobCard,
    w.slotChainTarget, w.chainLfoTargetMap, w.chainComponentBypassed,
    drawChainEditorBands);
  draw();
  clearGlobals();
  return fb;
}

/* ======================================================================== */
/* MASTER FX                                                                 */
/* ======================================================================== */

const MASTER_FX_SLOTS = 8;
const MASTER_FX_CHAIN_COMPONENTS = (function () {
  const comps = [];
  for (let i = 0; i < MASTER_FX_SLOTS; i++)
    comps.push({ key: "fx" + (i + 1), kind: "fx", label: "FX " + (i + 1),
                 position: i, paramPrefix: "master_fx:fx" + (i + 1) + ":" });
  comps.push({ key: "settings", kind: "settings", label: "Settings",
               position: MASTER_FX_SLOTS, paramPrefix: "" });
  return comps;
})();

/* drawMasterFx takes its shared state through ctx, so the state goes in
   directly. The six sibling draws it can early-return into are supplied as
   deps that FAIL if reached -- none of the snapshot cases raise a modal, and a
   case that silently drew a preset picker instead of the chain would otherwise
   be baselined as if it were the chain. */
const boom = (what) => () => { fail("drawMasterFx fell through to " + what); };
const MFX_DRAW_DEPS = ["ctx", "drawHeader", "drawChainDiagram", "DIAGRAM_W",
  "DIAGRAM_Y", "SCREEN_WIDTH", "truncateText", "drawMasterNamePreview",
  "drawMasterConfirmOverwrite", "drawMasterConfirmDelete", "drawMasterPresetPicker",
  "drawMasterFxSettingsMenu", "drawMasterFxModuleSelect",
  /* Same shared bands the slot editor draws, 4a-3 -- which is what makes the
     two screens the same screen from the header rule down. */
  "drawChainEditorBands"];
const mkMasterDraw = liftFrom(mfxSrc, "shadow_ui_master_fx.mjs", "drawMasterFx", MFX_DRAW_DEPS);

function renderMaster(c) {
  const fb = createFramebuffer();
  installGlobals(fb, (slot, key) => (c.state[key] !== undefined ? c.state[key] : ""));
  /* The master chain target and the two draw helpers, lifted from shadow_ui.js
     -- the same ones renderChain drives. drawMasterFx now paints its LFO and
     bypass markers through them, so this harness must supply the REAL ones or
     it would be snapshotting a screen the device never draws. */
  const mGetSlotParam = (slot, key) => (c.state[key] !== undefined ? c.state[key] : "");
  const mTarget = new Function("parseChainId", "MASTER_FX_SLOTS",
    uiSrc.slice(uiSrc.indexOf("const MASTER_CHAIN_TARGET = {"),
                uiSrc.indexOf("\n};\n", uiSrc.indexOf("const MASTER_CHAIN_TARGET = {")) + 4) +
    "\nreturn MASTER_CHAIN_TARGET;")(parseChainId, MASTER_FX_SLOTS);
  const mChainTargetGetParam = lift("chainTargetGetParam", ["getSlotParam"])(mGetSlotParam);
  const mLfoMap = lift("chainLfoTargetMap", ["getSlotParam"])(mGetSlotParam);
  const mBypassed = lift("chainComponentBypassed",
    ["chainTargetGetParam"])(mChainTargetGetParam);
  const mfxCtx = {
    MASTER_CHAIN_TARGET: mTarget,
    chainLfoTargetMap: mLfoMap,
    chainComponentBypassed: mBypassed,
    masterShowingNamePreview: false, masterConfirmingOverwrite: false,
    masterConfirmingDelete: false, helpDetailScrollState: null, helpNavStack: [],
    inMasterPresetPicker: false, inMasterFxSettingsMenu: false,
    selectingMasterFxModule: false,
    selectedMasterFxComponent: c.sel,
    masterFxConfig: c.config,
    MASTER_FX_CHAIN_COMPONENTS,
    MASTER_FX_OPTIONS: c.options || [],
    currentMasterPresetName: c.presetName || "",
    getMasterFxParam: (i, key) =>
      (c.state["master_fx:fx" + (i + 1) + ":" + key] || ""),
    getModuleAbbrev: (m) => (!m ? "--" :
      (ABBREV_CACHE[String(m).toLowerCase()] || String(m).substring(0, 2).toUpperCase())),
    isTextEntryActive: () => false,
    drawTextEntry: boom("drawTextEntry"),
    drawHelpDetail: boom("drawHelpDetail"),
    drawHelpList: boom("drawHelpList"),
  };
  const draw = mkMasterDraw(mfxCtx, drawMenuHeader, drawChainDiagram, DIAGRAM_W,
    DIAGRAM_Y, SCREEN_WIDTH, truncateText, boom("drawMasterNamePreview"),
    boom("drawMasterConfirmOverwrite"), boom("drawMasterConfirmDelete"),
    boom("drawMasterPresetPicker"), boom("drawMasterFxSettingsMenu"),
    boom("drawMasterFxModuleSelect"), drawChainEditorBands);
  draw();
  clearGlobals();
  return fb;
}

/* ======================================================================== */
/* THE CASE MATRIX                                                           */
/* ======================================================================== */

/* A slot state map, as the DSP would answer it. */
function chainState(o) {
  const { fx = [], midiFx = [], synth = "sf2" } = o;
  const s = {};
  if (synth) { s.synth_module = synth; s["synth:name"] = synth === "sf2" ? "SoundFont" : synth; }
  s.fx_count = String(fx.length);
  s.midi_fx_count = String(midiFx.length);
  fx.forEach((m, i) => { s["fx" + (i + 1) + "_module"] = m; s["fx" + (i + 1) + ":name"] = m; });
  midiFx.forEach((m, i) => { s["midi_fx" + (i + 1) + "_module"] = m; });
  return s;
}
const rep = (n, m) => Array.from({ length: n }, () => m);

/* Positions in the editor component list, resolved by KEY so a case name says
   what it means rather than encoding an index that shifts with chain length. */
function chainIndexOf(state, key) {
  const w = chainWorld(state);
  w.ensureChainConfigFresh(0);
  return w.slotChainComponents(0).findIndex((c) => c.key === key);
}

const chainCases = [];
const addChain = (id, o) => {
  const state = Object.assign(chainState(o), o.extra || {});
  const sel = o.selKey === null ? -1 : chainIndexOf(state, o.selKey);
  if (sel === undefined || (o.selKey !== null && sel < 0))
    fail("case " + id + " names a component that does not exist: " + o.selKey);
  chainCases.push({ id, state, sel, shift: !!o.shift, card: o.card || null,
                    patchName: o.patchName, dirty: !!o.dirty });
};

const SHORT = { fx: ["freeverb", "cloudseed"] };
const FIVE = { fx: ["freeverb", "cloudseed", "tapescam", "psxverb"] };   /* 5 boxes with the synth */
const EIGHT = { fx: rep(8, "freeverb") };
const FULL = { midiFx: rep(8, "arp"), fx: rep(8, "cloudseed") };

/* --- length x selection ------------------------------------------------- */
addChain("chain/len0/sel-synth",    { fx: [], selKey: "synth" });
addChain("chain/len0/sel-settings", { fx: [], selKey: "settings" });
addChain("chain/len0/sel-patch",    { fx: [], selKey: null });
addChain("chain/len0/sel-add-midi", { fx: [], selKey: "add_midi" });
addChain("chain/len0/sel-add-fx",   { fx: [], selKey: "add_fx" });
addChain("chain/len0/no-synth",     { fx: [], synth: "", selKey: "synth" });
addChain("chain/len1/sel-first",    { fx: ["freeverb"], selKey: "add_midi" });
addChain("chain/len1/sel-fx1",      { fx: ["freeverb"], selKey: "fx1" });
addChain("chain/len2/sel-synth",    Object.assign({ selKey: "synth" }, SHORT));
addChain("chain/len2/sel-fx1",      Object.assign({ selKey: "fx1" }, SHORT));
addChain("chain/len2/sel-fx2",      Object.assign({ selKey: "fx2" }, SHORT));
addChain("chain/len2/sel-settings", Object.assign({ selKey: "settings" }, SHORT));
addChain("chain/len5/sel-synth",    Object.assign({ selKey: "synth" }, FIVE));
addChain("chain/len5/sel-fx4",      Object.assign({ selKey: "fx4" }, FIVE));
addChain("chain/len5/sel-add-fx",   Object.assign({ selKey: "add_fx" }, FIVE));
addChain("chain/len8/sel-add-midi", Object.assign({ selKey: "add_midi" }, EIGHT));
addChain("chain/len8/sel-fx4",      Object.assign({ selKey: "fx4" }, EIGHT));
addChain("chain/len8/sel-fx8",      Object.assign({ selKey: "fx8" }, EIGHT));
addChain("chain/len8/sel-settings", Object.assign({ selKey: "settings" }, EIGHT));
addChain("chain/len8/sel-patch",    Object.assign({ selKey: null }, EIGHT));
addChain("chain/full/sel-add-midi", Object.assign({ selKey: "add_midi" }, FULL));
addChain("chain/full/sel-midiFx",   Object.assign({ selKey: "midiFx" }, FULL));
addChain("chain/full/sel-synth",    Object.assign({ selKey: "synth" }, FULL));
addChain("chain/full/sel-fx8",      Object.assign({ selKey: "fx8" }, FULL));
addChain("chain/full/sel-add-fx",   Object.assign({ selKey: "add_fx" }, FULL));
addChain("chain/full/sel-settings", Object.assign({ selKey: "settings" }, FULL));
addChain("chain/full/sel-patch",    Object.assign({ selKey: null }, FULL));

/* --- shift: the footer swaps SEL/CLK for MOVE --------------------------- */
addChain("chain/len2/shift",  Object.assign({ selKey: "fx1", shift: true }, SHORT));
addChain("chain/len5/shift",  Object.assign({ selKey: "fx4", shift: true }, FIVE));
addChain("chain/full/shift",  Object.assign({ selKey: "fx8", shift: true }, FULL));
addChain("chain/len0/shift",  { fx: [], selKey: "synth", shift: true });

/* --- marks: bypass B, LFO tildes, and both at once ---------------------- */
addChain("chain/len2/bypassed", Object.assign({ selKey: "fx2",
  extra: { "fx1:bypassed": "1" } }, SHORT));
addChain("chain/len2/bypassed-selected", Object.assign({ selKey: "fx1",
  extra: { "fx1:bypassed": "1" } }, SHORT));
addChain("chain/len2/bypassed-synth", Object.assign({ selKey: "fx1",
  extra: { "synth:bypassed": "1" } }, SHORT));
addChain("chain/len2/lfo1", Object.assign({ selKey: "fx2",
  extra: { "lfo1:enabled": "1", "lfo1:target": "fx1" } }, SHORT));
addChain("chain/len2/lfo2", Object.assign({ selKey: "fx2",
  extra: { "lfo2:enabled": "1", "lfo2:target": "fx1" } }, SHORT));
addChain("chain/len2/lfo1+2", Object.assign({ selKey: "fx2",
  extra: { "lfo1:enabled": "1", "lfo1:target": "fx1",
           "lfo2:enabled": "1", "lfo2:target": "fx1" } }, SHORT));
addChain("chain/full/lfo-midi-fx1", Object.assign({ selKey: "add_midi",
  extra: { "lfo1:enabled": "1", "lfo1:target": "midi_fx1" } }, FULL));
addChain("chain/len5/bypassed+lfo1+2", Object.assign({ selKey: "fx1",
  extra: { "fx2:bypassed": "1", "lfo1:enabled": "1", "lfo1:target": "fx2",
           "lfo2:enabled": "1", "lfo2:target": "fx2" } }, FIVE));

/* --- header and info line ----------------------------------------------- */
addChain("chain/len2/patch-named", Object.assign({ selKey: "fx1",
  patchName: "Deep Pad" }, SHORT));
addChain("chain/len2/patch-dirty", Object.assign({ selKey: "fx1",
  patchName: "Deep Pad", dirty: true }, SHORT));
addChain("chain/len2/patch-selected-named", Object.assign({ selKey: null,
  patchName: "Deep Pad" }, SHORT));
addChain("chain/len2/info-preset", Object.assign({ selKey: "fx1",
  extra: { "fx1:preset_name": "Cathedral" } }, SHORT));
addChain("chain/len2/info-rnbo", Object.assign({ selKey: "fx1",
  fx: ["rnbo-fx-shimmer", "cloudseed"] }));

/* --- the knob card, over the diagram ------------------------------------ */
addChain("chain/len1/knob-card", { fx: ["freeverb"], selKey: "fx1",
  card: { name: "CUTOFF", value: "0.62", row: 0, touched: 0, page: null,
          metaIndex: null, values: null, viz: null, modulated: null } });
addChain("chain/len5/knob-card", Object.assign({ selKey: "fx4",
  card: { name: "RESONANCE", value: "0.31", row: 0, touched: 0, page: null,
          metaIndex: null, values: null, viz: null, modulated: null } }, FIVE));

/* --- master fx ----------------------------------------------------------- */
const masterCases = [];
const addMaster = (id, o) => {
  const config = {};
  (o.modules || []).forEach((m, i) => { if (m) config["fx" + (i + 1)] = { module: m }; });
  let sel = o.sel;
  if (typeof sel === "string") sel = MASTER_FX_CHAIN_COMPONENTS.findIndex((c) => c.key === sel);
  if (sel === undefined || sel < -1) fail("master case " + id + " has no selection");
  masterCases.push({ id, sel, config, state: o.extra || {},
                     presetName: o.presetName || "",
                     options: o.options || [{ id: "cloudseed", name: "CloudSeed" }] });
};

const M2 = ["freeverb", "cloudseed"];
const M5 = ["freeverb", "cloudseed", "tapescam", "psxverb", "freeverb"];
const M8 = rep(8, "cloudseed");

addMaster("master/len0/sel-fx1",      { modules: [], sel: "fx1" });
addMaster("master/len0/sel-settings", { modules: [], sel: "settings" });
addMaster("master/len0/sel-preset",   { modules: [], sel: -1 });
addMaster("master/len1/sel-fx1",      { modules: ["freeverb"], sel: "fx1" });
addMaster("master/len1/sel-fx2-empty",{ modules: ["freeverb"], sel: "fx2" });
addMaster("master/len2/sel-fx1",      { modules: M2, sel: "fx1" });
addMaster("master/len2/sel-fx2",      { modules: M2, sel: "fx2" });
addMaster("master/len5/sel-fx1",      { modules: M5, sel: "fx1" });
addMaster("master/len5/sel-fx3",      { modules: M5, sel: "fx3" });
addMaster("master/len5/sel-fx5",      { modules: M5, sel: "fx5" });
addMaster("master/len8/sel-fx1",      { modules: M8, sel: "fx1" });
addMaster("master/len8/sel-fx4",      { modules: M8, sel: "fx4" });
addMaster("master/len8/sel-fx8",      { modules: M8, sel: "fx8" });
addMaster("master/len8/sel-settings", { modules: M8, sel: "settings" });
addMaster("master/len8/sel-preset",   { modules: M8, sel: -1 });
addMaster("master/hole/sel-fx3",      { modules: ["freeverb", null, "cloudseed"], sel: "fx3" });
addMaster("master/len2/bypassed",     { modules: M2, sel: "fx2",
  extra: { "master_fx:fx1:bypassed": "1" } });
addMaster("master/len2/bypassed-selected", { modules: M2, sel: "fx1",
  extra: { "master_fx:fx1:bypassed": "1" } });
addMaster("master/len2/lfo1",         { modules: M2, sel: "fx2",
  extra: { "master_fx:lfo1:enabled": "1", "master_fx:lfo1:target": "fx1" } });
addMaster("master/len2/lfo2",         { modules: M2, sel: "fx2",
  extra: { "master_fx:lfo2:enabled": "1", "master_fx:lfo2:target": "fx1" } });
addMaster("master/len2/lfo1+2",       { modules: M2, sel: "fx2",
  extra: { "master_fx:lfo1:enabled": "1", "master_fx:lfo1:target": "fx1",
           "master_fx:lfo2:enabled": "1", "master_fx:lfo2:target": "fx1" } });
addMaster("master/len5/bypassed+lfo1+2", { modules: M5, sel: "fx1",
  extra: { "master_fx:fx2:bypassed": "1",
           "master_fx:lfo1:enabled": "1", "master_fx:lfo1:target": "fx2",
           "master_fx:lfo2:enabled": "1", "master_fx:lfo2:target": "fx2" } });
addMaster("master/len1/preset-named", { modules: ["freeverb"], sel: -1,
  presetName: "Glue Bus" });
addMaster("master/len2/info-preset",  { modules: M2, sel: "fx2",
  extra: { "master_fx:fx2:preset_name": "Cathedral" } });
addMaster("master/len2/info-optname", { modules: M2, sel: "fx2",
  options: [{ id: "cloudseed", name: "CloudSeed Reverb" }] });

/* ======================================================================== */
/* RENDER, FLOOR, HASH                                                       */
/* ======================================================================== */

/*
 * THE CONTENT FLOOR.
 *
 * The lift makes a missing dependency a ReferenceError, but the fix that hides
 * one is a typeof guard, and a guarded-away block draws NOTHING while the case
 * still hashes cleanly. So each band is checked for ink independently: a screen
 * that lost its footer, its diagram or its header cannot be blessed as a
 * baseline just because the remaining pixels are stable.
 *
 * ONE band list, for both screens, since 4a-3. It used to be two: Master FX
 * drew no footer, wore the taller menu_layout header (hence TITLE_RULE_Y) and
 * sat its boxes at y=20. Those were historical rather than chosen, and 4a-3
 * removed them -- so a single list is now the stronger check, because a Master
 * FX case that lost its footer or drifted back up the screen fails here rather
 * than being described by its own private geometry.
 */
const EDITOR_BANDS = [
  ["header", 0, MOVY_HEADER_H - 1],
  ["diagram", DIAGRAM_Y, DIAGRAM_Y + DIAGRAM_BOX_H - 1],
  ["label/info", DIAGRAM_Y + DIAGRAM_BOX_H + 3, MOVY_RULE_Y - 1],
  ["footer", MOVY_RULE_Y, 63],
];
const BANDS = { chain: EDITOR_BANDS, master: EDITOR_BANDS };
const MIN_LIT = 120;

function inkInBand(fb, y0, y1) {
  let n = 0;
  for (let y = y0; y <= y1 && y < fb.height; y++)
    for (let x = 0; x < fb.width; x++) if (fb.pixels[y * fb.width + x]) n++;
  return n;
}

const current = {};
const run = (cases, render, kind) => {
  for (const c of cases) {
    const fb = render(c);
    if (fb.clipped() !== 0)
      fail(c.id + " drew " + fb.clipped() + " pixels outside the 128x64 display");
    if (fb.missingGlyphs.size)
      fail(c.id + " asked for glyphs the device font does not have: " +
           [...fb.missingGlyphs].join(""));
    const lit = fb.countLit();
    if (lit < MIN_LIT)
      fail(c.id + " lit only " + lit + " pixels -- a screen this empty is a missing " +
           "dependency, not a baseline");
    for (const [band, y0, y1] of BANDS[kind]) {
      if (inkInBand(fb, y0, y1) === 0)
        fail(c.id + " drew NOTHING in its " + band + " band (rows " + y0 + ".." + y1 +
             ") -- some block of the editor is unreachable under the lift");
    }
    if (current[c.id]) fail("duplicate case id " + c.id);
    current[c.id] = { sha: sha1(Buffer.from(fb.pixels)), fb };
  }
};
run(chainCases, renderChain, "chain");
run(masterCases, renderMaster, "master");

const ids = Object.keys(current);
if (ids.length < 50) fail("only " + ids.length + " cases -- the matrix has collapsed");

/* Eyeball one case: DUMP_CASE=chain/len5/sel-fx4 bash tests/host/... */
if (process.env.DUMP_CASE) {
  for (const id of ids) {
    if (id.indexOf(process.env.DUMP_CASE) < 0) continue;
    console.log("--- " + id + " ---");
    console.log(current[id].fb.toBlocks());
  }
}

/* Two screens that render identically would mean one of them is not being
   driven at all. */
{
  const bySha = {};
  for (const id of ids) {
    if (bySha[current[id].sha]) fail("cases " + bySha[current[id].sha] + " and " + id +
      " render the SAME pixels -- one of them is not varying what it claims to");
    bySha[current[id].sha] = id;
  }
}

if (process.env.UPDATE_CHAIN_EDITOR_BASELINE) {
  const lines = ids.slice().sort().map((id) => id + " " + current[id].sha);
  const header = [
    "# One line per chain-editor case: <id> <sha1-of-the-128x64-pixel-buffer>, sorted.",
    "#",
    "# Captured BEFORE step 4a of the Master FX variable-length design converges",
    "# drawChainEdit and drawMasterFx into one editor. That order is the whole",
    "# point: a baseline regenerated after the refactor compares the new code",
    "# against itself.",
    "#",
    "# The 46 chain/ hashes are still the ORIGINAL capture and must stay",
    "# re-derivable from the commit before 4a touched either draw function.",
    "#",
    "# The 25 master/ hashes were refreshed ONCE, at step 4a-3, which unified the",
    "# two editors chrome on purpose: Master FX gained the movy header band and",
    "# the hint footer and moved up to the slot editors box row. Every one of the",
    "# 25 was rendered and read before the refresh, and no chain/ hash moved with",
    "# them -- which is what said the screen that was not supposed to move did",
    "# not. Any FURTHER master/ movement is a regression again.",
    "#",
    "# A mismatch names which case moved, and the runner prints the render.",
    "#",
    "# Regenerate with:",
    "#     UPDATE_CHAIN_EDITOR_BASELINE=1 bash tests/host/test_chain_editor_snapshot.sh",
    "",
  ].join("\n");
  writeFileSync(BASELINE_PATH, header + lines.join("\n") + "\n");
  console.log("UPDATED " + BASELINE_PATH + " (" + lines.length + " cases)");
}

if (!existsSync(BASELINE_PATH))
  fail("no baseline file -- run with UPDATE_CHAIN_EDITOR_BASELINE=1");

if (!failures) {
  const baseline = {};
  for (const line of readFileSync(BASELINE_PATH, "utf8").split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const sp = line.lastIndexOf(" ");
    if (sp < 0) continue;
    baseline[line.slice(0, sp)] = line.slice(sp + 1);
  }
  const moved = ids.filter((id) => id in baseline && baseline[id] !== current[id].sha);
  if (moved.length) {
    console.error("--- " + moved[0] + ", as it renders NOW ---");
    console.error(current[moved[0]].fb.toBlocks());
    fail(moved.length + " chain-editor case(s) changed: " + moved.join(", ") +
         " -- if this change is intended, regenerate with " +
         "UPDATE_CHAIN_EDITOR_BASELINE=1 and review the diff");
  }
  const seen = new Set(ids);
  const gone = Object.keys(baseline).filter((k) => !seen.has(k));
  if (gone.length) fail("the baseline names " + gone.length + " case(s) this file no longer " +
    "renders, so they are no longer protected: " + gone.slice(0, 6).join(", "));
  const extra = ids.filter((k) => !(k in baseline));
  if (extra.length) fail(extra.length + " case(s) are not in the baseline: " +
    extra.slice(0, 6).join(", "));
}

if (failures) process.exit(1);
console.log("PASS: chain editor snapshot — " + chainCases.length + " slot-chain and " +
            masterCases.length + " Master FX renders match the baseline, " +
            "every one of them inside the display, in the device font, and with ink in " +
            "each band");
'
