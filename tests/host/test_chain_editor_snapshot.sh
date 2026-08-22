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
# it is a test of a tautology that still prints PASS. So the 46 ORIGINAL chain/
# hashes in tests/fixtures/chain-editor-baseline.txt must be re-derivable from
# the commit BEFORE 4a touches either draw function, and a reviewer should check
# that by regenerating from that parent commit and byte-comparing. The same trap
# was avoided once already on this branch, for render_page_movy (c4f61538).
#
# The 25 original master/ hashes were refreshed ONCE, at step 4a-3, whose entire
# purpose was to move those pixels: Master FX drew no footer, wore the older
# header and sat 6px low, and 4a-3 gave it the slot editor chrome. The refresh is
# what this repo calls a reviewed fixture change (tools/param-pages/regenerate.mjs
# says so explicitly) -- the renders were read case by case first, and the check
# that it was not a cover for a refactor mistake is that ZERO chain/ hashes moved
# in the same commit.
#
# Step 4b ADDED four cases and moved none. The knob card now draws on Master FX
# too, which is new behaviour and gets its own cases rather than a regeneration
# of somebody else`s. Three of the four are the card, and the two *-strip cases
# are deliberately the same card payload on both screens: they must differ only
# in what is BEHIND the card.
#
# The two MODULE PICKERS were added next, and moved NOTHING -- 12 new cases,
# six payloads rendered down both editors. They had never been rendered here at
# all: drawComponentSelect had no case and drawMasterFxModuleSelect was one of
# the fail-if-reached stubs, so the harness that exists to stop these two
# screens drifting was blind to the one screen that had drifted furthest. A user
# found it on the device instead -- "the module select here is different than
# the module select in slots" -- and it was: the same chainMoveEntries-built row
# list drawn in the movy chrome on one side and the old menu chrome on the
# other. They are ONE function now (drawChainPicker) and the cases are PAIRED,
# so a future divergence is a diff between two pictures built from one payload.
#
# Step 4e made Master FX a VARIABLE-LENGTH chain, so all 19 remaining master/
# cases moved: the row is the loaded chain, one `+`, and Settings, instead of a
# fixed run of cap boxes with the unloaded ones drawn empty. That is the change
# the step exists for -- an empty Master FX showed eight boxes of nothing -- and
# it is a reviewed refresh again, read case by case first. Two cases were
# DELETED rather than refreshed because the state they named no longer exists
# (an empty position past the end of the chain), and eight were added for the
# states that now do. Again: ZERO chain/ hashes moved with them, which is what
# says the screen that was not supposed to move did not.
#
# Step 4f ADDED six settings/ cases and moved NONE. The Master FX Settings
# position now opens the KNOB GRID -- four pages, Volume / LFO 1 / LFO 2 /
# Actions -- instead of drawMasterFxSettingsMenu, which was one of the
# fail-if-reached stubs and had therefore never been rendered here. Leaving the
# replacement uncovered would put this harness back in the blind spot that let
# the two module pickers diverge. They are driven through the REAL controller
# and the REAL synthesised contract, and the LFO pages are built by the SAME
# lfoParams/lfoLevels the slot Settings grid uses -- so a change aimed at a slot
# LFO moves a master hash here, which is the point.
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
#   the pickers    are lifted the same two ways -- drawComponentSelect from
#                  shadow_ui.js with a dependency list, drawMasterFxModuleSelect
#                  from shadow_ui_master_fx.mjs with its ctx -- and both end in
#                  the one shared drawChainPicker.
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
/* The REAL shiftHintsFor from the shared chrome. These renders are pixel
   baselines, so a stub would bake in a footer nobody actually draws. */
const CHROME = await import("./src/shared/chain_editor_chrome.mjs");
const CHROME_SHIFT_HINTS = CHROME.shiftHintsFor;
const CHROME_REST_HINTS = CHROME.CHAIN_HINTS_AT_REST;
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
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { drawMenuHeader } from "./src/shared/menu_layout.mjs";
import { truncateText } from "./src/shared/chain_ui_views.mjs";
import { drawChainEditorBands, drawChainPicker } from "./src/shared/chain_editor_chrome.mjs";

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
  "getComponentParamPrefix", "drawMovyFooter", "isShiftHeld", "shiftHintsFor", "CHAIN_HINTS_AT_REST", "ensureChainConfigFresh",
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
    w.getComponentParamPrefix, drawMovyFooter, () => !!c.shift, CHROME_SHIFT_HINTS,
    CHROME_REST_HINTS, w.ensureChainConfigFresh,
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

/*
 * The Master FX component list is DERIVED from the chain now, not from the cap
 * -- `count` modules, then one `+`, then Settings -- so it is built here by the
 * SAME two functions the device builds it with (masterFxChainConfig and
 * chainEditorComponents, both lifted) rather than by a fixed table. A fixed
 * table here would keep snapshotting the eight empty boxes that step 4e exists
 * to remove.
 */
const chainEditorComponents = lift("chainEditorComponents", ["chainComponents"])(chainComponents);
function masterComponents(config) {
  const held = { c: config };
  const decls = new Function("masterFxConfig", "MASTER_FX_SLOTS",
    uiSrc.slice(uiSrc.indexOf("let masterFxChainLength = -1;"),
                uiSrc.indexOf("\n}\n", uiSrc.indexOf("function masterFxChainConfig("))) +
    "\n}\nreturn masterFxChainConfig;")(held.c, MASTER_FX_SLOTS);
  return chainEditorComponents(decls(), { hasSynth: false, hasMidiFx: false });
}

/* drawMasterFx takes its shared state through ctx, so the state goes in
   directly. The six sibling draws it can early-return into are supplied as
   deps that FAIL if reached -- none of the snapshot cases raise a modal, and a
   case that silently drew a preset picker instead of the chain would otherwise
   be baselined as if it were the chain.

   The stubs STAY even for drawMasterFxModuleSelect, which now has cases of its
   own further down: what the stub asserts is that a CHAIN case did not fall
   through into the picker, which is a different claim from the picker being
   rendered somewhere. The four still uncovered here -- text entry, name
   preview, and the two confirms, plus the preset picker and the settings menu
   -- keep failing loudly rather than quietly drawing the wrong screen, which is
   exactly the silence that let the two pickers diverge. */
const boom = (what) => () => { fail("drawMasterFx fell through to " + what); };
const MFX_DRAW_DEPS = ["ctx", "drawHeader", "drawChainDiagram", "DIAGRAM_W",
  "DIAGRAM_Y", "SCREEN_WIDTH", "truncateText", "drawMasterNamePreview",
  "drawMasterConfirmOverwrite", "drawMasterConfirmDelete", "drawMasterPresetPicker",
  "drawMasterFxSettingsMenu", "drawMasterFxModuleSelect",
  /* Same shared bands the slot editor draws, 4a-3 -- which is what makes the
     two screens the same screen from the header rule down. */
  "drawChainEditorBands",
  /* And the Shift footer helper that lives beside them, for the same reason:
     one spelling of a gesture, drawn by both screens. REAL, not a stub -- a
     stub would baseline a footer nobody draws. */
  "shiftHintsFor", "CHAIN_HINTS_AT_REST",
  /* The knob card, 4b. A module IMPORT in shadow_ui_master_fx.mjs, so it is a
     free identifier under the lift and MUST be a dependency: leave it out and
     the card block throws, and the tempting fix -- a typeof guard -- would make
     it silently unreachable and baseline a Master FX screen with the feature
     switched off. That exact bug already happened once here (5c9fcd51).
     knobCardDrawState is deliberately NOT here: drawMasterFx destructures it
     from ctx, and a const cannot shadow a parameter of the same name. It is
     supplied on mfxCtx below instead, where a missing one is a TypeError. */
  "drawKnobCard"];
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
    /* Derived per case, from that case`s config. */
    MASTER_FX_CHAIN_COMPONENTS: masterComponents(c.config),
    ensureMasterFxConfigFresh: () => {},
    isShiftHeld: () => !!c.shift,
    chainLfoTargetMap: mLfoMap,
    chainComponentBypassed: mBypassed,
    masterShowingNamePreview: false, masterConfirmingOverwrite: false,
    masterConfirmingDelete: false, helpDetailScrollState: null, helpNavStack: [],
    inMasterPresetPicker: false, inMasterFxSettingsMenu: false,
    selectingMasterFxModule: false,
    selectedMasterFxComponent: c.sel,
    masterFxConfig: c.config,
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
    /* Same shape renderChain passes drawChainEdit, so a card case on one
       screen and a card case on the other are driven from identical data. */
    knobCardDrawState: () => (c.card || null),
  };
  const draw = mkMasterDraw(mfxCtx, drawMenuHeader, drawChainDiagram, DIAGRAM_W,
    DIAGRAM_Y, SCREEN_WIDTH, truncateText, boom("drawMasterNamePreview"),
    boom("drawMasterConfirmOverwrite"), boom("drawMasterConfirmDelete"),
    boom("drawMasterPresetPicker"), boom("drawMasterFxSettingsMenu"),
    boom("drawMasterFxModuleSelect"), drawChainEditorBands, CHROME_SHIFT_HINTS,
    CHROME_REST_HINTS, drawKnobCard);
  draw();
  clearGlobals();
  return fb;
}

/* ======================================================================== */
/* THE TWO MODULE PICKERS                                                    */
/* ======================================================================== */
/*
 * Both editors open a picker on a position, and until now NEITHER was rendered
 * here: drawComponentSelect had no case at all and drawMasterFxModuleSelect was
 * one of the six fail-if-reached stubs above. So the only harness that draws
 * these screens drew neither of them, and the two pickers were free to diverge
 * completely -- which they did, and a user found it on the device before any
 * test did. drawMenuHeader/drawMenuList/"Back: cancel" on one side, the movy
 * band and renderPicker on the other, over the SAME chainMoveEntries-built row
 * list.
 *
 * They are snapshotted as a PAIR, from one payload, the same trick the
 * *-strip knob-card cases use: a divergence then shows up as a diff between two
 * pictures that were built to be the same screen. The stubs stay for the four
 * modals that still are not covered, so the harness keeps failing loudly rather
 * than quietly drawing the wrong screen.
 */

/* The picker`s rows, built by the REAL chainMoveEntries -- the function both
   editors already share -- so a case cannot quietly pin rows the device does
   not produce. */
const chainComponentIdTop = lift("chainComponentId", [])();
const chainMoveEntries = lift("chainMoveEntries",
  ["parseChainId", "chainComponentId"])(parseChainId, chainComponentIdTop);

/* The rows as BOTH callers assemble them: the module scan, with this position`s
   Move rows tucked under whichever entry is currently loaded. One helper for
   both sides on purpose -- an entry list that differed between them would make
   the paired cases prove nothing. */
function pickerEntries(config, key, options, loadedId) {
  const rows = options.slice();
  const at = loadedId ? rows.findIndex((o) => o.id === loadedId) : -1;
  rows.splice(at >= 0 ? at + 1 : 0, 0, ...chainMoveEntries(config, key));
  return rows;
}

const CHAIN_PICKER_DEPS = ["clear_screen", "slotChainComponents", "selectedSlot",
  "selectedChainComponent", "fill_rect", "print", "text_width",
  "getChainComponentModule", "chainConfigs", "availableModules",
  "selectedModuleIndex", "drawChainPicker"];
const mkChainPicker = lift("drawComponentSelect", CHAIN_PICKER_DEPS);
/* drawMasterFxModuleSelect reads everything off ctx, so it needs exactly the
   shared draw plus that object -- which is itself the evidence that the screen
   is now one function with two callers. */
const mkMasterPicker = liftFrom(mfxSrc, "shadow_ui_master_fx.mjs",
  "drawMasterFxModuleSelect", ["ctx", "drawChainPicker"]);

function renderChainPicker(c) {
  const fb = createFramebuffer();
  const g = installGlobals(fb, () => "");
  const w = chainWorld(c.state);
  w.ensureChainConfigFresh(0);
  const sel = w.slotChainComponents(0).findIndex((x) => x.key === c.selKey);
  if (sel < 0) fail("picker case " + c.id + " names a component that does not exist: " + c.selKey);
  const draw = mkChainPicker(g.clear_screen, w.slotChainComponents, 0, sel,
    g.fill_rect, g.print, g.text_width, w.getChainComponentModule, w.chainConfigs,
    c.entries, c.index, drawChainPicker);
  draw();
  clearGlobals();
  return fb;
}

function renderMasterPicker(c) {
  const fb = createFramebuffer();
  installGlobals(fb, () => "");
  const comps = masterComponents(c.config);
  const sel = comps.findIndex((x) => x.key === c.selKey);
  if (sel < 0) fail("picker case " + c.id + " names a component that does not exist: " + c.selKey);
  const draw = mkMasterPicker({
    selectedMasterFxComponent: sel,
    MASTER_FX_CHAIN_COMPONENTS: comps,
    masterFxPickerItems: c.entries,
    selectedMasterFxModuleIndex: c.index,
    masterFxConfig: c.config,
  }, drawChainPicker);
  draw();
  clearGlobals();
  return fb;
}

/* ======================================================================== */
/* MASTER FX SETTINGS, AS THE KNOB GRID                                      */
/* ======================================================================== */
/*
 * The Settings position of the Master FX chain used to open a scrolling list
 * (drawMasterFxSettingsMenu), which is one of the fail-if-reached stubs above
 * -- so the screen behind that box has never been rendered here. It is four
 * knob-grid pages now (Volume, LFO 1, LFO 2, Actions), and leaving the
 * REPLACEMENT uncovered would put the harness back in exactly the blind spot
 * that let the two module pickers diverge until a user found it.
 *
 * Driven through the REAL page controller and the REAL synthesised contract
 * from shadow_ui_slot_grid.mjs -- no lift is needed, because both are pure
 * modules with no host globals in them. That is also what makes these cases
 * worth something: the LFO pages are built by the SAME lfoParams/lfoLevels the
 * slot contract uses, so a change that only meant to touch a slot LFO moves a
 * master hash here.
 */
const { createController } = await import("./src/shared/param_pages/page_controller.mjs");
const { LAYOUT_MOVY } = await import("./src/shared/param_pages/render_page_movy.mjs");
const { createMasterGridIo } = await import("./src/shadow/shadow_ui_slot_grid.mjs");

/* The hint pairs footerHints() produces for these two page kinds. Spelled out
   rather than imported: shadow_ui_param_pages.mjs resolves its imports from
   /data/UserData/schwung on the device and cannot be loaded here. The FOOTER
   GRAMMAR itself is pinned by test_footer_canon.sh; what these buy is that the
   band is not empty. */
const SETTINGS_FOOTER = {
  knobs: [["JOG", "PAGE"], ["CLK", "MENU"]],
  menu:  [["JOG", "PAGE"], ["CLK", "ENTER"]],
};

function renderSettings(c) {
  const fb = createFramebuffer();
  const store = c.state;
  installGlobals(fb, (slot, key) => (store[key] !== undefined ? store[key] : ""));
  const io = createMasterGridIo({
    readParam: (k) => (store[k] !== undefined ? store[k] : ""),
    writeParam: (k, v) => { store[k] = String(v); },
    hasPreset: () => !!c.presetName,
    /* The host resolves an LFO target to a NAME; stubbed with the shape
       shared/lfo_target_label.mjs returns. */
    describeTarget: (i) => (c.targets ? (c.targets[i] || null) : null),
    isModulated: () => false,
    runAction: () => { fail(c.id + " ran an action while merely rendering"); },
  });
  /* The visibility evaluator the host binds to the master bus. A condition key
     carrying its own ":" is used verbatim -- which is the whole reason the LFO
     params are declared with the "master_fx:" prefix on both the key AND the
     condition. */
  io.visible = (cond) => {
    if (!cond || !cond.param) return true;
    const v = store[cond.param] !== undefined ? store[cond.param] : "";
    return String(v) === String(cond.equals);
  };
  const ctl = createController(Object.assign({ announce: noop }, io));
  ctl.load({ slot: 0, component: "master_settings", prefix: "master_settings",
             visible: io.visible });
  ctl.setLayout(LAYOUT_MOVY);
  const names = ctl.pages.map((p) => p.name);
  const at = names.indexOf(c.page);
  if (at < 0) fail(c.id + " names a page that does not exist: " + c.page +
                   " (pages: " + names.join(", ") + ")");
  ctl.goToPage(Math.max(0, at));
  /* The controller reads ONE param per tick on purpose (a round trip is ~2.8ms
     on device), so a render straight after load would draw a page of blanks.
     Wound forward until every declared key has been picked up. */
  for (let i = 0; i < 400; i++) ctl.tick();
  ctl.render(drawContext(fb), {
    title: "MFX > Settings",
    footer: SETTINGS_FOOTER[ctl.page.kind] || SETTINGS_FOOTER.knobs,
  });
  clearGlobals();
  return fb;
}

/* A master-bus state map, as the shim would answer it. Both LFOs are always
   fully populated: an unread key draws as an empty cell, which would make a
   case pass its content floor on the OTHER cells and quietly stop protecting
   the one that went missing. */
function masterSettingsState(o) {
  const s = {
    "master_fx:volume": o.volume === undefined ? "1.00" : o.volume,
    /* WIRE value, not the option index: -1 is All. The io maps it on read,
       and mocking the index here would baseline the mapping as a no-op. */
    "master_fx:midi_channel": o.midiChannel === undefined ? "-1" : o.midiChannel,
  };
  for (const n of [1, 2]) {
    const l = o["lfo" + n] || {};
    const p = "master_fx:lfo" + n + ":";
    s[p + "target"] = l.target || "";
    s[p + "target_param"] = l.targetParam || "";
    s[p + "enabled"] = l.enabled || "0";
    s[p + "polarity"] = l.polarity || "0";
    s[p + "sync"] = l.sync || "0";
    s[p + "shape"] = l.shape || "0";
    s[p + "rate_hz"] = l.rateHz || "1.0";
    s[p + "rate_div"] = l.rateDiv || "19";
    s[p + "depth"] = l.depth || "1.0";
    s[p + "phase_offset"] = l.phase || "0";
  }
  return s;
}

const settingsCases = [];
const addSettings = (id, o) => settingsCases.push({
  id, page: o.page, state: masterSettingsState(o),
  presetName: o.presetName || "", targets: o.targets || null,
});

/* The VALUES page: one cell, and that is the point. A page with fewer than
   eight params draws fewer than eight cells (arp is baselined at four), so a
   lone Volume knob is the smallest case of an existing shape rather than a
   half-drawn grid -- and this case is what would notice if it ever became one. */
addSettings("settings/master/main", { page: "Main", volume: "0.85", presetName: "Glue Bus" });
addSettings("settings/master/main-unity", { page: "Main", volume: "1.00" });

/* An LFO page in each of its two states. The pair matters because ONE rate
   cell is on the page at a time -- rate_hz when Free, rate_div when Sync -- and
   a visibility condition resolved against the wrong slot reads empty, compares
   false and hides BOTH, which is a page with a hole in it rather than an error. */
addSettings("settings/master/lfo1-free", {
  page: "LFO 1",
  lfo1: { target: "fx1", targetParam: "room_size", enabled: "1", polarity: "1",
          sync: "0", shape: "0", rateHz: "2.4", depth: "0.65", phase: "0.25" },
  targets: { 0: { short: "F1 ROOM", header: "FX 1", long: "FX 1: Room Size" } },
});
addSettings("settings/master/lfo2-sync", {
  page: "LFO 2",
  lfo2: { target: "fx2", targetParam: "mix", enabled: "1", polarity: "0",
          sync: "1", shape: "3", rateDiv: "19", depth: "0.4", phase: "0" },
  targets: { 1: { short: "F2 MIX", header: "FX 2", long: "FX 2: Mix" } },
});

/* The ACTIONS menu, both lengths. Save As and Delete mean nothing until a
   preset exists, and the filter is the same one the list applied -- so the
   no-preset case is a one-entry menu, and it must still draw a menu. */
addSettings("settings/master/actions", { page: "Actions", presetName: "Glue Bus" });
addSettings("settings/master/actions-nopreset", { page: "Actions" });

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

/* --- the knob card, over the diagram ------------------------------------ *
 *
 * FULL_CARD is the card with its widget strip -- the four cells of the touched
 * knob`s row, drawn by the SAME drawKnobRow the knob grid uses. The two cases
 * below it are header-only (page: null), which is the card a knob with no
 * resolvable row raises. Both shapes are used on BOTH screens, per the rule
 * that any chain-editor behaviour is tested against both targets: a card that
 * came out different on Master FX would show as a diff between two cases that
 * were built from identical payloads.
 *
 * `name` is the BARE parameter name, and must stay that way. The device sends
 * the card a short name and the SCREEN READER the composed
 * "MFX: cloudseed Mix" -- two answers to two questions, because a sighted user
 * has the diagram behind the card and a screen-reader user has nothing. That
 * split lives in shadow_ui.js and is pinned by
 * tests/host/test_knob_card_header_name.sh; these payloads only have to keep
 * modelling what the renderer is actually handed. Feeding a composed title in
 * here would snapshot a picture the device no longer draws.
 */
const CARD_PARAMS = [
  { key: "a", name: "Room", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "b", name: "Damp", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "c", name: "Mode", type: "enum", options: ["Hall", "Room", "Plate"] },
  { key: "d", name: "Mix",  type: "float", min: 0, max: 1, step: 0.01 },
];
const FULL_CARD = {
  name: "ROOM SIZE", value: "0.62", row: 0, touched: 1,
  page: { kind: "knobs", keys: ["a", "b", "c", "d", null, null, null, null] },
  metaIndex: buildMetaIndex({ hierarchy: null, chainParams: CARD_PARAMS }),
  values: { a: 0.62, b: 0.25, c: 1, d: 0.8 },
  viz: null, modulated: null,
};

addChain("chain/len1/knob-card", { fx: ["freeverb"], selKey: "fx1",
  card: { name: "CUTOFF", value: "0.62", row: 0, touched: 0, page: null,
          metaIndex: null, values: null, viz: null, modulated: null } });
addChain("chain/len5/knob-card", Object.assign({ selKey: "fx4",
  card: { name: "RESONANCE", value: "0.31", row: 0, touched: 0, page: null,
          metaIndex: null, values: null, viz: null, modulated: null } }, FIVE));
addChain("chain/len2/knob-card-strip", Object.assign({ selKey: "fx1",
  card: FULL_CARD }, SHORT));

/* --- master fx ----------------------------------------------------------- */
const masterCases = [];
const addMaster = (id, o) => {
  const config = {};
  for (let i = 1; i <= MASTER_FX_SLOTS; i++) config["fx" + i] = { module: "" };
  (o.modules || []).forEach((m, i) => { if (m) config["fx" + (i + 1)] = { module: m }; });
  let sel = o.sel;
  /* Resolved by KEY against THIS case`s list, because the list is only as long
     as the chain: `settings` is at index 2 on an empty Master FX and at index 9
     on a full one. An index baked into a case name would drift with it. */
  if (typeof sel === "string") {
    sel = masterComponents(config).findIndex((c) => c.key === sel);
    /* -1 is the PRESET row, so a key that resolves to it is a case naming a box
       that is not there -- which is how "sel-fx1 on an empty chain" would have
       gone on quietly snapshotting the preset row instead. */
    if (sel < 0) fail("master case " + id + " names a component that does not exist: " + o.sel);
  }
  if (sel === undefined || sel < -1)
    fail("master case " + id + " has no selection");
  masterCases.push({ id, sel, config, state: o.extra || {},
                     card: o.card || null, shift: !!o.shift,
                     presetName: o.presetName || "",
                     options: o.options || [{ id: "cloudseed", name: "CloudSeed" }] });
};

const M2 = ["freeverb", "cloudseed"];
const M5 = ["freeverb", "cloudseed", "tapescam", "psxverb", "freeverb"];
const M8 = rep(8, "cloudseed");

/* NOTE: there is no `master/len0/sel-fx1` any more, and no
   `master/len1/sel-fx2-empty`. Both named an EMPTY POSITION past the end of the
   chain, which is exactly what step 4e removed: a Master FX holding one module
   has one module box and a `+`, not eight boxes with seven of them blank. Their
   replacements are the sel-add-fx cases below. */
addMaster("master/len0/sel-settings", { modules: [], sel: "settings" });
addMaster("master/len0/sel-preset",   { modules: [], sel: -1 });
addMaster("master/len1/sel-fx1",      { modules: ["freeverb"], sel: "fx1" });
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

/* --- the knob card, over the Master FX diagram (4b) ---------------------- *
 *
 * New cases rather than a regeneration: a card on Master FX is new behaviour
 * and needs its own protection, the way the chain editor has had two card
 * cases since the feature shipped. These mirror chain/len1/knob-card and
 * chain/len5/knob-card exactly -- same card payload, same selection depth --
 * so a card that renders differently on the two screens shows up as a diff
 * between two cases that were built to be the same picture.
 */
addMaster("master/len1/knob-card", { modules: ["freeverb"], sel: "fx1",
  card: { name: "CUTOFF", value: "0.62", row: 0, touched: 0, page: null,
          metaIndex: null, values: null, viz: null, modulated: null } });
addMaster("master/len5/knob-card", { modules: M5, sel: "fx4",
  card: { name: "RESONANCE", value: "0.31", row: 0, touched: 0, page: null,
          metaIndex: null, values: null, viz: null, modulated: null } });
addMaster("master/len2/knob-card-strip", { modules: M2, sel: "fx1",
  card: FULL_CARD });

/* --- the variable-length chain (4e) -------------------------------------- *
 *
 * The states that did not exist before: a chain shorter than the cap ENDS, and
 * the box after its last module is a `+` rather than the sixth of eight empty
 * ones. An empty Master FX is two boxes. And Shift now means MOVE here, so the
 * footer changes under it the way the slot editor`s does.
 */
addMaster("master/len0/sel-add-fx",   { modules: [], sel: "add_fx" });
addMaster("master/len1/sel-add-fx",   { modules: ["freeverb"], sel: "add_fx" });
addMaster("master/len2/sel-add-fx",   { modules: M2, sel: "add_fx" });
/* At the cap the `+` is still offered -- and still refused, with an
   announcement, by beginChainInsertFromAddBox. The slot chain draws its `+` at
   its own cap too (chain/full/sel-add-fx), and a Master FX that hid it there
   would be a difference between the two editors for no reason. */
addMaster("master/len8/sel-add-fx",   { modules: M8, sel: "add_fx" });
addMaster("master/len2/shift",        { modules: M2, sel: "fx1", shift: true });
addMaster("master/len5/shift",        { modules: M5, sel: "fx3", shift: true });
addMaster("master/len8/shift",        { modules: M8, sel: "fx8", shift: true });
addMaster("master/len0/shift",        { modules: [], sel: "add_fx", shift: true });

/* --- the pickers, one payload per PAIR ----------------------------------- *
 *
 * Every case below is rendered TWICE, once through drawComponentSelect and once
 * through drawMasterFxModuleSelect, from the same modules / position / rows /
 * cursor. The two are not expected to hash the same -- the header says which
 * chain you are in, "S1 > FX 2" against "MFX > FX 2" -- but everything under it
 * is, so a divergence is a diff between two pictures built to be one screen.
 */
const PICKER_OPTIONS = [
  { id: "freeverb", name: "Freeverb" },
  { id: "cloudseed", name: "CloudSeed" },
  { id: "psxverb", name: "PSX Reverb" },
];
const pickerCases = [];
const addPicker = (id, o) => {
  const modules = o.modules || [];
  const config = {};
  for (let i = 1; i <= MASTER_FX_SLOTS; i++) config["fx" + i] = { module: "" };
  modules.forEach((m, i) => { if (m) config["fx" + (i + 1)] = { module: m }; });
  const state = chainState({ fx: modules });
  /* The row list comes from the SLOT config, and is then handed to both sides.
     Both editors build it from the same chainMoveEntries on the device, so one
     list here is the honest model of that -- and it means the paired renders
     differ only in how the rows are DRAWN. */
  const w = chainWorld(state);
  w.ensureChainConfigFresh(0);
  const at = parseChainId(o.selKey);
  const loadedId = at ? (modules[at.index] || "") : "";
  const entries = o.entries !== undefined ? o.entries
    : pickerEntries(w.chainConfigs[0], o.selKey, o.options || PICKER_OPTIONS, loadedId);
  pickerCases.push({ id, selKey: o.selKey, state, config, entries,
                     index: o.index === undefined ? 0 : o.index });
};

/* Nothing installed at all -- the one branch that draws no list, and the one
   the old Master FX picker left with no footer on it. */
addPicker("picker/empty", { modules: ["freeverb"], selKey: "fx1", entries: [] });
/* A loaded module, marked with the `*`, with the Move rows under it. One
   neighbour to the left only, so exactly one Move row is offered. */
addPicker("picker/loaded-last", { modules: ["freeverb", "cloudseed"], selKey: "fx2", index: 3 });
/* Mid-chain: both Move rows, and the cursor on one of them. */
addPicker("picker/moves-both", { modules: ["freeverb", "cloudseed", "psxverb"],
  selKey: "fx2", index: 2 });
/* First position of a chain with somewhere to go: Move Right only. */
addPicker("picker/moves-right", { modules: ["freeverb", "cloudseed"], selKey: "fx1", index: 0 });
/* A name far wider than the list column, which renderPicker must fit rather
   than run off the edge -- and the `*` still has to land in its column. */
addPicker("picker/long-name", { modules: ["cloudseed"], selKey: "fx1", index: 1,
  options: [{ id: "freeverb", name: "Freeverb" },
            { id: "cloudseed", name: "CloudSeed Algorithmic Reverb XL" }] });
/* More rows than fit, so the window scrolls and the selection centres. */
addPicker("picker/scrolled", { modules: ["freeverb"], selKey: "fx1", index: 8,
  options: Array.from({ length: 12 }, (_, i) => ({ id: "m" + i, name: "Module " + i })) });

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
/*
 * The picker has its own three, because it has no diagram and its list is one
 * band rather than a label and an info line. Same principle: a screen that lost
 * its header or its footer -- which is exactly what the old Master FX picker
 * did, it drew no hints at all in the empty case -- cannot pass as a baseline.
 */
const PICKER_BANDS = [
  ["header", 0, MOVY_HEADER_H - 1],
  ["list", MOVY_HEADER_H, MOVY_RULE_Y - 1],
  ["footer", MOVY_RULE_Y, 63],
];
/*
 * The knob grid has three bands. Same principle again: the header carries
 * "MFX > SETTINGS" and the page name, the body is the grid or the menu, and
 * the footer is the hint pills. A settings page that lost its footer -- which
 * is exactly what the Master FX screens kept doing before 4a-3 -- cannot pass
 * as a baseline.
 */
const SETTINGS_BANDS = [
  ["header", 0, MOVY_HEADER_H - 1],
  ["body", MOVY_HEADER_H, MOVY_RULE_Y - 1],
  ["footer", MOVY_RULE_Y, 63],
];
const BANDS = { chain: EDITOR_BANDS, master: EDITOR_BANDS, picker: PICKER_BANDS,
                settings: SETTINGS_BANDS };
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
    /*
     * Nothing may fall off the display. Not "almost nothing".
     *
     * SETTINGS_GAP was briefly allowed to push the settings box past the right
     * edge, which needed a per-case allowance here and in two other tests --
     * three guards weakened to buy a gap. The gap is paid for by the strip now
     * (chain_diagram.mjs: a narrower settings box and a wider DIAGRAM_W), so
     * this is unconditional again. It has caught two real overruns; keep it
     * that way.
     */
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
/* The same six payloads down both pickers. Ids are prefixed by which editor
   drew them so a mismatch names the side that moved. */
run(pickerCases.map((c) => Object.assign({}, c, { id: "picker/slot/" + c.id.slice(7) })),
    renderChainPicker, "picker");
run(pickerCases.map((c) => Object.assign({}, c, { id: "picker/master/" + c.id.slice(7) })),
    renderMasterPicker, "picker");
run(settingsCases, renderSettings, "settings");

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
   driven at all.
   
   DECLARED exceptions, because one pair is identical on purpose: on an empty
   Master FX chain the only selectable cell is the `+`, and Shift changes
   NOTHING there -- shift+click opens the same picker a plain click does. The
   footer rule is that an action Shift does not change keeps its place, so the
   two renders are supposed to match, and that is worth pinning rather than
   working around by pointing the case at a different cell. */
const SAME_ON_PURPOSE = [["master/len0/sel-add-fx", "master/len0/shift"]];
const sameAllowed = (a, b) =>
  SAME_ON_PURPOSE.some((p) => p.indexOf(a) >= 0 && p.indexOf(b) >= 0);
{
  const bySha = {};
  for (const id of ids) {
    if (bySha[current[id].sha] && !sameAllowed(bySha[current[id].sha], id))
      fail("cases " + bySha[current[id].sha] + " and " + id +
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
    "# The 46 original chain/ hashes are still the ORIGINAL capture and must",
    "# stay re-derivable from the commit before 4a touched either draw function.",
    "#",
    "# The 25 original master/ hashes were refreshed ONCE, at 4a-3, which unified",
    "# the two editors chrome on purpose: Master FX gained the movy header band",
    "# and the hint footer and moved up to the slot editors box row. Every one of",
    "# the 25 was rendered and read before the refresh, and no chain/ hash moved",
    "# with them -- which is what said the screen that was not supposed to move",
    "# did not. Any FURTHER master/ movement is a regression again.",
    "#",
    "# Step 4b ADDED the four knob-card cases below and moved NONE.",
    "#",
    "# The 12 picker/ cases ADDED the two module pickers, which had never been",
    "# rendered here at all, and moved NONE. Six payloads, each drawn down both",
    "# editors, so slot and master must differ only in the header.",
    "#",
    "# Step 4f ADDED the six settings/ cases -- the Master FX Settings position",
    "# as the knob grid, a screen this harness had never rendered -- and moved NONE.",
    "#",
    "# Step 4e made Master FX a variable-length chain: every master/ hash moved,",
    "# two cases naming an empty position past the end of the chain were deleted,",
    "# and eight were added for the `+` box and the Shift footer. Reviewed case by",
    "# case, and NO chain/ hash moved with them.",
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
console.log("PASS: chain editor snapshot — " + chainCases.length + " slot-chain, " +
            masterCases.length + " Master FX, " + (pickerCases.length * 2) +
            " module-picker and " + settingsCases.length +
            " Master FX settings renders match the baseline, " +
            "every one of them inside the display, in the device font, and with ink in " +
            "each band");
'
