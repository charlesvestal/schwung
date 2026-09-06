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
import { readFileSync, writeFileSync, existsSync, mkdirSync } from "node:fs";
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
import { drawMenuHeader, drawMenuList, drawMenuFooter }
  from "./src/shared/menu_layout.mjs";
/* The list rect the FX-bus picker hands drawMenuList, from the same module the
   device reads it from. */
import { LIST_TOP_Y, FOOTER_RULE_Y } from "./src/shared/chain_ui_views.mjs";
import { truncateText } from "./src/shared/chain_ui_views.mjs";
import { drawChainEditorBands, drawChainPicker } from "./src/shared/chain_editor_chrome.mjs";
/* The bus MODEL — pure, and the same module shadow_ui.js and
   shadow_ui_buses.mjs both import. The views file cannot be imported (it
   resolves /data/UserData/schwung paths), so its four screens are LIFTED
   below; the model is the half that can just be used. */
import * as BusModel from "./src/shared/bus_model.mjs";
import { drawConfirmModal } from "./src/shared/menu_layout.mjs";
import { ctx as BUS_CTX } from "./src/shadow/shadow_ui_ctx.mjs";

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
const busSrc = readFileSync("src/shadow/shadow_ui_buses.mjs", "utf8");

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

  /* The REAL one, lifted: it is the rule that decides whether the `Buses` row
     exists at all, and it reads through the same cached-read helper the device
     uses. Not a dependency of drawChainEdit -- the chain editor has no bus
     affordance -- but the settings-list cases below drive their row filter
     through it. BusModel is a free identifier under the lift (shadow_ui.js
     imports it) and is supplied from the shared module itself. */
  const chainSynthSplits = lift("chainSynthSplits",
    ["chainConfigs", "getSlotParamCached", "BusModel"])(
    chainConfigs, getSlotParamCached, BusModel);

  return { getSlotParam, getSlotParamCached, chainSynthSplits, chainConfigs, chainConfigFresh,
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
  /* The knob-card / diagram primitive set, extracted to its own top-level
     function so the bus screens (shadow_ui_buses.mjs, via ctx.movyCtx) and
     drawChainEdit stopped carrying two copies of the same literal. A free
     identifier under the lift, same as every other dep here. */
  "movyPrimitives",
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
    drawChainEditorBands,
    () => ({ fillRect: g.fill_rect, print: g.print, textWidth: g.text_width, setPixel: g.set_pixel }));
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
/* The REAL send entries, lifted rather than restated: the master row heads are
   derived from FX_BUSES, and a stub here would let the harness baseline a row
   the device does not draw. */
const masterFxSendEntries = (() => {
  const busAt = uiSrc.indexOf("const FX_BUSES = [");
  const busEnd = uiSrc.indexOf("\n];", busAt);
  const fnAt = uiSrc.indexOf("function masterFxSendEntries(");
  const fnEnd = uiSrc.indexOf("\n}\n", fnAt);
  if (busAt < 0 || fnAt < 0) { fail("could not lift masterFxSendEntries/FX_BUSES"); return () => []; }
  return new Function(uiSrc.slice(busAt, busEnd + 3) + uiSrc.slice(fnAt, fnEnd + 2) +
    "\nreturn masterFxSendEntries;")();
})();

/* `isMaster` false for a SEND case: only the master bus heads its row with the
   send entries, so a send editor showing its own box would be a box that
   reopens the screen it is drawn on. */
function masterComponents(config, isMaster = true) {
  const held = { c: config };
  const decls = new Function("masterFxConfig", "MASTER_FX_SLOTS",
    uiSrc.slice(uiSrc.indexOf("let masterFxChainLength = -1;"),
                uiSrc.indexOf("\n}\n", uiSrc.indexOf("function masterFxChainConfig("))) +
    "\n}\nreturn masterFxChainConfig;")(held.c, MASTER_FX_SLOTS);
  const rows = chainEditorComponents(decls(), { hasSynth: false, hasMidiFx: false });
  /* A SEND heads its row with the way back, where the master heads its row with
     the two sends -- so neither case is a bare chain, and a harness that built
     one would baseline a row the device does not draw. */
  const head = isMaster ? masterFxSendEntries()
    : [{ id: "busback", key: "busback", kind: "busback", busIndex: 0, label: "MFX" }];
  return head.concat(rows).map((c, i) => ({ ...c, position: i }));
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
  /* fxBusHints, the file own Back-word rewriter. REAL, lifted from the same
     file: what it changes is a word in the footer, and a stub would baseline a
     footer nobody draws. */
  "fxBusHints",
  "drawKnobCard"];
/* The real rewriter, from the same file, applied to whatever pairs the
   chrome hands it. */
const FX_BUS_HINTS = liftFrom(mfxSrc, "shadow_ui_master_fx.mjs", "fxBusHints",
                              ["FX_BUS_BACK_LABEL"])("BUS");
/* The three FX buses, as shadow_ui.js declares them. Written out here rather
   than lifted because FX_BUSES sits inside a 1500-line declaration block that
   this harness has no other reason to evaluate; test_fx_bus_contract.sh is what
   fails if the two drift. */
/* Which bus renderMaster is currently drawing. A CELL, not a captured value:
   the lifted target and its key rule are built once and every case changes the
   bus underneath them. */
let mBusCell = { cur: null };
const FX_BUS_STUBS = {
  master: { id: "master", label: "Master FX", short: "MFX", prefix: "master_fx:",
            send: -1, hasLfos: true,  hasPresets: true,  busLevelKeys: [] },
  send1:  { id: "send1",  label: "Send A",    short: "SNDA", prefix: "send1:",
            send: 0,  hasLfos: false, hasPresets: false,
            busLevelKeys: ["return", "to_send2"] },
  send2:  { id: "send2",  label: "Send B",    short: "SNDB", prefix: "send2:",
            send: 1,  hasLfos: false, hasPresets: false, busLevelKeys: ["return"] },
};

const mkMasterDraw = liftFrom(mfxSrc, "shadow_ui_master_fx.mjs", "drawMasterFx", MFX_DRAW_DEPS);

function renderMaster(c) {
  const fb = createFramebuffer();
  installGlobals(fb, (slot, key) => (c.state[key] !== undefined ? c.state[key] : ""));
  /* The master chain target and the two draw helpers, lifted from shadow_ui.js
     -- the same ones renderChain drives. drawMasterFx now paints its LFO and
     bypass markers through them, so this harness must supply the REAL ones or
     it would be snapshotting a screen the device never draws. */
  const mGetSlotParam = (slot, key) => (c.state[key] !== undefined ? c.state[key] : "");
  mBusCell.cur = c.bus || FX_BUS_STUBS.master;
  const mTarget = new Function("parseChainId", "MASTER_FX_SLOTS", "fxBus",
    uiSrc.slice(uiSrc.indexOf("const MASTER_CHAIN_TARGET = {"),
                uiSrc.indexOf("\n};\n", uiSrc.indexOf("const MASTER_CHAIN_TARGET = {")) + 4) +
    "\nreturn MASTER_CHAIN_TARGET;")(parseChainId, MASTER_FX_SLOTS,
    /* Which FX bus the target addresses — its whole key rule is the bus prefix,
       so this MUST follow the case or a send case would silently read the
       master bus keys and render identically to it. Held in a cell rather
       than captured, because the target is built once and the case changes. */
    () => mBusCell.cur);
  const mChainTargetGetParam = lift("chainTargetGetParam", ["getSlotParam"])(mGetSlotParam);
  const mLfoMap = lift("chainLfoTargetMap", ["getSlotParam"])(mGetSlotParam);
  const mBypassed = lift("chainComponentBypassed",
    ["chainTargetGetParam"])(mChainTargetGetParam);
  const mfxCtx = {
    MASTER_CHAIN_TARGET: mTarget,
    /* Derived per case, from that case`s config. */
    MASTER_FX_CHAIN_COMPONENTS: masterComponents(c.config, !c.bus),
    /* Cached on entry on the device; fixed per case here. A send box draws its
       RETURN as a dial and names what is in the bus on the info band. */
    fxBusSummary: (i) => (c.busSummaries || {})[i] || "",
    fxBusReturn: (i) => ((c.busReturns || {})[i] || 0),
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
    /* WHICH FX bus this case is. drawMasterFx is parameterised by it — the key
       prefix, the header text and side, whether a preset name may appear, and
       whether the LFO markers are asked for at all — so a case that does not
       say defaults to the master bus and the send cases say so explicitly.
       These stubs mirror FX_BUSES in shadow_ui.js. */
    fxBus: () => (c.bus || FX_BUS_STUBS.master),
    /* The bus-level scalars the settings band prints for a send. null is "the
       read did not complete" and must print as "--", which is one of the
       cases below. */
    sendBusLevelRead: (k) => (c.levels && (k in c.levels)) ? c.levels[k] : null,
  };
  const draw = mkMasterDraw(mfxCtx, drawMenuHeader, drawChainDiagram, DIAGRAM_W,
    DIAGRAM_Y, SCREEN_WIDTH, truncateText, boom("drawMasterNamePreview"),
    boom("drawMasterConfirmOverwrite"), boom("drawMasterConfirmDelete"),
    boom("drawMasterPresetPicker"), boom("drawMasterFxSettingsMenu"),
    boom("drawMasterFxModuleSelect"), drawChainEditorBands, CHROME_SHIFT_HINTS,
    CHROME_REST_HINTS, FX_BUS_HINTS, drawKnobCard);
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
    /* The picker header names the bus -- master unless the case says
       otherwise. Every case before the sends existed left this unset and
       keeps meaning "Master FX". */
    fxBus: () => (c.bus ? FX_BUS_STUBS[c.bus] : FX_BUS_STUBS.master),
  }, drawChainPicker);
  draw();
  clearGlobals();
  return fb;
}

/* ======================================================================== */
/* THE FX-BUS PICKER                                                         */
/* ======================================================================== */
/*
 * The screen Shift+Vol+Menu and hold-Menu now open: three rows, Master FX,
 * Send A and Send B, on the ONE list engine (drawMenuHeader / drawMenuList /
 * drawMenuFooter). Rendered here because it is the only way into the sends and
 * because the module pickers already proved what an unrendered screen does —
 * they diverged completely and a user found it before any test did.
 *
 * drawFxBusPicker is lifted out of shadow_ui.js with its free identifiers
 * supplied. FX_BUSES is supplied as the same stub table the send editor cases
 * use, so a row label that changed in one place and not the other is a diff in
 * a picture.
 */
const mkBusPicker = lift("drawFxBusPicker",
  ["clear_screen", "drawHeader", "drawMenuList", "drawFooter", "FX_BUSES",
   "selectedFxBusRow", "fxBusSummaries", "LIST_TOP_Y", "FOOTER_RULE_Y"]);

function renderBusPicker(c) {
  const fb = createFramebuffer();
  installGlobals(fb, () => "");
  mkBusPicker(fb.clearScreen, drawMenuHeader, drawMenuList, drawMenuFooter,
              [FX_BUS_STUBS.master, FX_BUS_STUBS.send1, FX_BUS_STUBS.send2],
              c.row, c.summaries, LIST_TOP_Y, FOOTER_RULE_Y)();
  clearGlobals();
  return fb;
}

const busPickerCases = [
  /* Every row, so the highlight and the value column are pinned on each. */
  { id: "buspicker/row-master", row: 0, summaries: ["2 FX", "Empty", "Empty"] },
  { id: "buspicker/row-send-a", row: 1, summaries: ["2 FX", "1 FX", "Empty"] },
  { id: "buspicker/row-send-b", row: 2, summaries: ["2 FX", "1 FX", "3 FX"] },
  /* A summary read that did not complete prints "--", never "Empty" — that is
     what would send someone looking for the reverb they just loaded. */
  { id: "buspicker/unread",     row: 0, summaries: ["--", "--", "--"] },
];

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
   lone MIDI Ch knob is the smallest case of an existing shape rather than a
   half-drawn grid -- and this case is what would notice if it ever became one.

   THE PAIR USED TO VARY VOLUME AND NOW VARIES THE CHANNEL. master_fx:volume
   was removed because nothing served it end to end (see MASTER_GRID_PARAMS);
   with it gone the two cases differed only in presetName, which the values
   page does not draw, so they rendered identically -- caught by the
   same-pixels check below rather than by review, which is the check earning
   its keep. All vs a numbered channel keeps the pair varying the one cell that
   is left, and exercises the wire -> option-index mapping in both branches. */
addSettings("settings/master/main-all", { page: "Main", midiChannel: "-1", presetName: "Glue Bus" });
addSettings("settings/master/main-ch10", { page: "Main", midiChannel: "9" });

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
/* THE SEND SETTINGS MENU                                                    */
/* ======================================================================== */
/*
 * A send Settings box does NOT open the knob grid above -- getMasterFxSettingsItems
 * takes the `!fxBusIsMaster()` branch and returns sendBusLevelItems(), which
 * drawMasterFxSettingsMenu (shadow_ui_master_fx.mjs) still draws as the plain
 * scrolling list: Return / -> Send B, "int" rows, click-to-edit in place. That
 * function is ALSO one of the six fail-if-reached stubs renderMaster supplies
 * (boom("drawMasterFxSettingsMenu")) -- so this screen, the one a send actually
 * shows for its Settings box, had never been rendered here at all. The knob-grid
 * cases above cover the master REPLACEMENT for this screen, not this screen.
 *
 * Driven through the REAL sendBusLevelItems / getMasterFxSettingValue, lifted
 * from shadow_ui.js the same way FX_BUS_HINTS is lifted from the master file --
 * so the row labels and the "--" for an unread level come from the actual
 * source, not a hand-typed guess that could drift from it.
 *
 * getMasterFxSettingsItems itself is NOT lifted: its master branch touches
 * currentMasterPresetName and MASTER_FX_SETTINGS_ITEMS_BASE, free identifiers
 * this harness has no reason to supply for a screen that never takes that
 * branch, and fxBusIsMaster is a one-line function liftFrom cannot isolate (no
 * "\n}\n" of its own -- it would swallow everything up to the next one). The
 * dispatch this file needs is the one line the send branch actually is:
 * `return sendBusLevelItems();`, called directly.
 */
const mkSendBusLevelItems = lift("sendBusLevelItems", ["fxBus", "SEND_LEVEL_ROW_LABELS"]);
const mkGetMasterFxSettingValue = lift("getMasterFxSettingValue", ["sendBusLevelRead"]);
const mkSendSettingsMenu = liftFrom(mfxSrc, "shadow_ui_master_fx.mjs",
  "drawMasterFxSettingsMenu",
  ["ctx", "drawHeader", "truncateText", "drawMenuList", "LIST_TOP_Y", "FOOTER_RULE_Y", "drawFooter"]);
/* Mirrors SEND_LEVEL_ROW_LABELS in shadow_ui.js, the way FX_BUS_STUBS mirrors
   FX_BUSES -- a label that changes in one and not the other is a picture that
   stops meaning what its row name says. */
const SEND_LEVEL_ROW_LABELS_STUB = { return: "Return", to_send2: "-> Send B" };

const sendSettingsCases = [];
const addSendSettings = (id, o) => sendSettingsCases.push({
  id, bus: FX_BUS_STUBS[o.bus], sel: o.sel, levels: o.levels || null,
});
/* Send A: both rows, cursor on each in turn. */
addSendSettings("settings/send1/sel-return",     { bus: "send1", sel: 0, levels: { return: 100, to_send2: 40 } });
addSendSettings("settings/send1/sel-to-send2",   { bus: "send1", sel: 1, levels: { return: 100, to_send2: 40 } });
/* Send B: one row only -- it has no -> Send C, so its list is shorter. */
addSendSettings("settings/send2/sel-return",     { bus: "send2", sel: 0, levels: { return: 64 } });
/* A level whose read did not complete prints "--", never a zero -- same rule
   the settings BAND on the chain diagram follows for the same key. */
addSendSettings("settings/send1/unread",         { bus: "send1", sel: 0, levels: null });

function renderSendSettings(c) {
  const fb = createFramebuffer();
  installGlobals(fb, () => "");
  const fxBusFn = () => c.bus;
  const sendBusLevelReadStub = (k) => (c.levels && (k in c.levels)) ? c.levels[k] : null;
  const menuCtx = {
    currentMasterPresetName: "",
    selectedMasterFxSetting: c.sel,
    getMasterFxSettingsItems: mkSendBusLevelItems(fxBusFn, SEND_LEVEL_ROW_LABELS_STUB),
    getMasterFxSettingValue: mkGetMasterFxSettingValue(sendBusLevelReadStub),
    fxBus: fxBusFn,
  };
  const draw = mkSendSettingsMenu(menuCtx, drawMenuHeader, truncateText, drawMenuList,
                                   LIST_TOP_Y, FOOTER_RULE_Y, drawMenuFooter);
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

/* --- a splittable synth changes NOTHING on the chain editor --------------- *
 *
 * The bus door is a row on the slot`s SETTINGS, not a gesture here. Down was
 * briefly it, and it was wrong for a reason worth keeping a test for: up and
 * down are Move`s octave shift, and only Down was ever claimed -- so the pair
 * broke, at the chain editor`s default resting cursor position.
 *
 * These three cases are what says the gesture and its footer hint are really
 * gone. Each is byte-identical to its twin with no `split_voices` at all, and
 * the two that HAVE a twin are declared in SAME_ON_PURPOSE below: a footer hint
 * or a claimed arrow coming back would break that equality rather than sitting
 * unnoticed in a hash nobody rederives.
 */
const SPLITS = { "synth:split_voices": JSON.stringify(
  [{ id: "kick", label: "Kick" }, { id: "chh", label: "Closed Hat" }]) };
addChain("chain/len2/synth-splits", Object.assign({ selKey: "synth",
  extra: SPLITS }, SHORT));
addChain("chain/len2/synth-splits-shift", Object.assign({ selKey: "synth",
  shift: true, extra: SPLITS }, SHORT));
addChain("chain/len2/fx1-splits", Object.assign({ selKey: "fx1",
  extra: SPLITS }, SHORT));

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
    sel = masterComponents(config, !o.bus).findIndex((c) => c.key === sel);
    /* -1 is the PRESET row, so a key that resolves to it is a case naming a box
       that is not there -- which is how "sel-fx1 on an empty chain" would have
       gone on quietly snapshotting the preset row instead. */
    if (sel < 0) fail("master case " + id + " names a component that does not exist: " + o.sel);
  }
  if (sel === undefined || sel < -1)
    fail("master case " + id + " has no selection");
  masterCases.push({ id, sel, config, state: o.extra || {},
                     busSummaries: o.busSummaries || {},
                     busReturns: o.busReturns || {},
                     card: o.card || null, shift: !!o.shift,
                     presetName: o.presetName || "",
                     /* Which FX bus. Absent means the master bus, so every case
                        written before the sends existed keeps its meaning. */
                     bus: o.bus ? FX_BUS_STUBS[o.bus] : null,
                     levels: o.levels || null,
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
/* THE SEND ENTRIES. Nothing covered them at all: masterComponents called
   chainEditorComponents directly, so the harness never built a row that had
   one, and every send-box pixel was unbaselined while the suite was green.
   Selected and not, at three levels, because the dial is the whole point. */
addMaster("master/sends/a-zero", { modules: ["freeverb"], sel: "sendbus1",
  busSummaries: { 1: "Empty" }, busReturns: { 1: 0 } });
addMaster("master/sends/a-full", { modules: ["freeverb"], sel: "sendbus1",
  busSummaries: { 1: "2 FX" }, busReturns: { 1: 127 } });
addMaster("master/sends/a-mid-unselected", { modules: ["freeverb"], sel: "fx1",
  busSummaries: { 1: "1 FX", 2: "3 FX" }, busReturns: { 1: 64, 2: 100 } });
addMaster("master/sends/b-selected", { modules: ["freeverb"], sel: "sendbus2",
  busSummaries: { 2: "3 FX" }, busReturns: { 2: 40 } });
addMaster("master/sends/gap-with-settings", { modules: [], sel: "settings",
  busReturns: { 1: 20, 2: 90 } });

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

/* --- THE TWO SEND BUSES, through the same editor -------------------------
 *
 * The whole point of the FX-bus work is that these are not a second screen:
 * every case here is renderMaster with a different `bus`, so a difference
 * between a send and the master bus can only come from something that reads
 * fxBus(). What SHOULD differ, and what these cases pin:
 *   - the header names the bus (SNDA / SNDB, and no preset name),
 *   - the settings band prints the LEVELS instead of a verb,
 *   - Send A has a -> Send B row and Send B does not,
 *   - no LFO markers, ever, even with the master LFO keys set in `extra`.
 */
addMaster("send/a/len0/sel-add-fx", { bus: "send1", modules: [], sel: "add_fx" });
addMaster("send/a/len2/sel-fx1",    { bus: "send1", modules: M2, sel: "fx1" });
addMaster("send/a/len2/sel-fx2",    { bus: "send1", modules: M2, sel: "fx2" });
addMaster("send/b/len2/sel-fx1",    { bus: "send2", modules: M2, sel: "fx1" });
addMaster("send/a/len5/sel-fx3",    { bus: "send1", modules: M5, sel: "fx3" });
/* The settings box: Send A shows BOTH levels, Send B shows one. */
addMaster("send/a/len2/sel-settings", { bus: "send1", modules: M2, sel: "settings",
  levels: { return: 100, to_send2: 40 } });
addMaster("send/b/len2/sel-settings", { bus: "send2", modules: M2, sel: "settings",
  levels: { return: 64 } });
/* A level whose read did not complete prints "--", never a zero. */
addMaster("send/a/len2/sel-settings-unread", { bus: "send1", modules: M2, sel: "settings" });
/* Bypass still marks a box; the LFO keys are set and must mark NOTHING,
   because a send has no LFOs and is never asked. */
addMaster("send/a/len2/bypassed-no-lfo", { bus: "send1", modules: M2, sel: "fx2",
  extra: { "send1:fx1:bypassed": "1",
           "send1:lfo1:enabled": "1", "send1:lfo1:target": "fx1" } });

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
                     index: o.index === undefined ? 0 : o.index,
                     /* Which bus renderMasterPicker draws the header for.
                        Absent means the master bus, so every case above keeps
                        its meaning. renderChainPicker ignores it -- the slot
                        picker has no bus concept. */
                     bus: o.bus || null });
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
/* The header names the bus: "SNDA > FX 3" here, against "MFX > FX 3" for the
   same payload above. renderMasterPicker hardcoded the master bus for every
   case until now, so a send picker header -- new copy this task added -- had
   never been rendered here. Only run through renderMasterPicker: the slot
   picker (renderChainPicker) has no bus concept, and this payload does not
   collide with the six slot payloads above so it still gets its own render
   under "picker/slot/...". */
addPicker("picker/send-header", { modules: ["freeverb", "cloudseed", "tapescam"],
  selKey: "fx3", index: 2, bus: "send1" });


/* ======================================================================== */
/* THE SLOT BUS SCREENS                                                      */
/* ======================================================================== */
/*
 * The four screens that hang BELOW the synth box: the bus list, one bus`s menu,
 * the voice multi-select and a bus`s insert chain (with its own module picker).
 * Rendered here for the reason the two module pickers taught: a screen this
 * harness cannot see is a screen free to drift, and these four are the only way
 * a user reaches buses at all.
 *
 * Lifted out of shadow_ui_buses.mjs the way drawMasterFx is lifted out of
 * shadow_ui_master_fx.mjs -- it resolves its imports from
 * /data/UserData/schwung and cannot be imported here -- and driven through the
 * REAL shared ctx object, the same one the device populates, plus the REAL
 * bus_model.mjs. So a rule that changes in the model moves a picture here.
 *
 * drawWaiting is lifted too and passed in: it is a module-scope helper, so
 * under the lift it is a free identifier, and stubbing it would blank the one
 * screen state a failed read is allowed to produce.
 */
const liftBus = (name, deps) => liftFrom(busSrc, "shadow_ui_buses.mjs", name, deps);
const BUS_WAITING = liftBus("drawWaiting",
  ["ctx", "drawHeader", "drawFooter", "LIST_TOP_Y"])(
  BUS_CTX, drawMenuHeader, drawMenuFooter, LIST_TOP_Y);

const mkBusList = liftBus("drawBusList",
  ["ctx", "drawWaiting", "drawHeader", "drawMenuList", "drawFooter",
   "LIST_TOP_Y", "FOOTER_RULE_Y", "busListRows", "busRowLabel", "busRowValue"]);
const mkBusActions = liftBus("drawBusActions",
  ["ctx", "drawWaiting", "drawHeader", "drawMenuList", "drawFooter",
   "drawConfirmModal", "truncateText", "LIST_TOP_Y", "FOOTER_RULE_Y",
   "busListRows", "busActionItems", "busSendValue"]);
const mkBusVoices = liftBus("drawBusVoices",
  ["ctx", "drawWaiting", "drawHeader", "drawMenuList", "drawFooter",
   "truncateText", "LIST_TOP_Y", "FOOTER_RULE_Y", "voiceRows", "voiceRowValue"]);
const mkBusChain = liftBus("drawBusChain",
  ["ctx", "drawBusModuleSelect", "drawWaiting", "drawChainDiagram",
   "drawChainEditorBands", "truncateText", "busChainComponents"]);
const mkBusPicker2 = liftBus("drawBusModuleSelect",
  ["ctx", "drawChainPicker", "truncateText", "busChainComponents"]);

/* A `buses:config` document, as bus_emit_config writes it -- positional, four
   entries always, present or not. Built here rather than hand-written per case
   so a case says only what it varies. */
function busesConfig(o) {
  const buses = [];
  for (let b = 0; b < BusModel.SLOT_BUSES; b++) {
    const d = (o.buses || [])[b];
    buses.push(d
      ? { present: 1, name: d.name, orphans: d.orphans || 0,
          voices: d.voices || [],
          sends: d.sends || [0, 0],
          fx: Array.from({ length: BusModel.BUS_FX_SLOTS }, (_, k) =>
            ({ module: (d.fx || [])[k] || "", bypassed: !!((d.bypassed || [])[k]) })) }
      : { present: 0, name: "Bus " + (b + 1), orphans: 0, voices: [],
          sends: [0, 0],
          fx: Array.from({ length: BusModel.BUS_FX_SLOTS }, () => ({ module: "", bypassed: 0 })) });
  }
  return JSON.stringify({ buses, main_sends: o.mainSends || [0, 0] });
}

const voicesJson = (n) => JSON.stringify(
  Array.from({ length: n }, (_, i) => ({ id: "v" + i, label: "Voice " + (i + 1) })));

/* The ctx the device populates, populated with one case`s state. Assigned
   directly onto the REAL shared object: a private stand-in would let a screen
   read a property this file happens to define and the device does not. */
function installBusCtx(fb, c) {
  const g = installGlobals(fb, () => "");
  const movy = {
    fillRect: g.fill_rect, print: g.print, textWidth: g.text_width,
    setPixel: g.set_pixel, line: g.draw_line, fillCircle: g.fill_circle,
    drawCircle: g.draw_circle, drawArc: g.draw_arc,
  };
  Object.assign(BUS_CTX, {
    clearScreen: () => fb.clearScreen(),
    print: g.print,
    movyCtx: () => movy,
    getModuleAbbrev: (m) => (!m ? "--" :
      (ABBREV_CACHE[String(m).toLowerCase()] || String(m).substring(0, 2).toUpperCase())),
    slotLabel: () => c.slotLabel || "S1",
    /* `unresolved` is the whole point of one of the cases below: a read that
       did not complete must draw the waiting screen, never an empty list. */
    busConfig: c.config === null ? null : BusModel.parseBusesConfig(c.config),
    /* THE ROW LIST, as the device supplies it (busRowsNow): one list for the
       draw and for the input paths, so a screen cannot be drawn from rows a
       click would not find. The device also drops the Sends row when the knob
       grid is not the Param View; that is an input-path fact and these are
       pictures, so the grid form is what is rendered here. */
    busRows: () => BusModel.busListRows(
      c.config === null ? null : BusModel.parseBusesConfig(c.config),
      BUS_CTX.getModuleAbbrev),
    busVoices: c.voices === undefined ? { unresolved: false, voices: [] }
                                      : BusModel.parseSplitVoices(c.voices),
    busListIndex: c.index || 0,
    busActionsRow: c.actionsRow === undefined ? 0 : c.actionsRow,
    busActionsIndex: c.index || 0,
    busActionsEditing: !!c.editing,
    busConfirmingDelete: !!c.confirming,
    busConfirmIndex: c.confirmIndex || 0,
    busVoicesBus: c.bus === undefined ? 0 : c.bus,
    busVoicesIndex: c.index || 0,
    busChainBus: c.bus === undefined ? 0 : c.bus,
    busChainPos: c.pos || 0,
    selectingBusModule: !!c.picking,
    busPickerItems: c.entries || [],
    busPickerIndex: c.pickIndex || 0,
  });
  return movy;
}

function renderBusScreen(c) {
  const fb = createFramebuffer();
  installBusCtx(fb, c);
  const draw = c.screen === "list" ? mkBusList(BUS_CTX, BUS_WAITING, drawMenuHeader,
      drawMenuList, drawMenuFooter, LIST_TOP_Y, FOOTER_RULE_Y,
      BusModel.busListRows, BusModel.busRowLabel, BusModel.busRowValue)
    : c.screen === "actions" ? mkBusActions(BUS_CTX, BUS_WAITING, drawMenuHeader,
      drawMenuList, drawMenuFooter, drawConfirmModal, truncateText,
      LIST_TOP_Y, FOOTER_RULE_Y, BusModel.busListRows, BusModel.busActionItems,
      BusModel.busSendValue)
    : c.screen === "voices" ? mkBusVoices(BUS_CTX, BUS_WAITING, drawMenuHeader,
      drawMenuList, drawMenuFooter, truncateText, LIST_TOP_Y, FOOTER_RULE_Y,
      BusModel.voiceRows, BusModel.voiceRowValue)
    : mkBusChain(BUS_CTX,
      mkBusPicker2(BUS_CTX, drawChainPicker, truncateText, BusModel.busChainComponents),
      BUS_WAITING, drawChainDiagram, drawChainEditorBands, truncateText,
      BusModel.busChainComponents);
  draw();
  clearGlobals();
  return fb;
}

const busCases = [];
const addBus = (id, o) => busCases.push(Object.assign({ id }, o));

const KICK = { name: "Kick", voices: ["v0"], sends: [20, 0], fx: ["tapescam"] };
const HATS = { name: "Hats", voices: ["v1", "v2"], sends: [0, 15],
               fx: ["cloudseed", "psxverb"] };
const SNARE = { name: "Snare", voices: ["v3"], sends: [5, 5], fx: [] };
const TOMS = { name: "Toms", voices: ["v4"], sends: [40, 40], fx: ["freeverb"] };

/* Extra buses so the list and the mixer can be shown AT THE CAP. Derived from
   SLOT_BUSES rather than written out, so raising the cap again widens these
   cases instead of quietly leaving them short of it. */
const EXTRA_BUSES = Array.from({ length: Math.max(0, BusModel.SLOT_BUSES - 4) },
  (_, i) => ({ name: "Aux " + (i + 1), voices: ["x" + i], sends: [10 + i, 0],
               fx: [] }));
const ALL_BUSES = [KICK, HATS, SNARE, TOMS].concat(EXTRA_BUSES);

/* THE LIST AT EVERY LENGTH IT CAN HAVE. One bus is three rows (it, Send Mixer,
   New Bus); SLOT_BUSES buses is SLOT_BUSES + 1, and there is no more -- the New
   Bus row is gone once none is free, so that is the maximum this screen can ever
   draw. It SCROLLS well before then: the list shows five rows. */
addBus("bus/list/1", { screen: "list", config: busesConfig({ buses: [KICK] }), index: 0 });
addBus("bus/list/3", { screen: "list",
  config: busesConfig({ buses: [KICK, HATS, SNARE], mainSends: [5, 30] }), index: 1 });
addBus("bus/list/4", { screen: "list",
  config: busesConfig({ buses: [KICK, HATS, SNARE, TOMS], mainSends: [5, 30] }), index: 3 });
/* AT THE CAP: every bus present, so no New Bus row, and the cursor on the last
   one -- the scrolled end of the longest list this screen can hold. */
addBus("bus/list/full", { screen: "list",
  config: busesConfig({ buses: ALL_BUSES, mainSends: [5, 30] }),
  index: BusModel.SLOT_BUSES });
/* The Send Mixer row and the New Bus row, each under the cursor: the footer
   names the verb of the row it is ON, and these are the two rows whose verb
   differs from a bus`s. The Send Mixer row carries no level of its own -- the
   slot`s own two sends are real, but they belong to the SLOT and are edited in
   Slot Settings, not on this screen. */
addBus("bus/list/sends-row", { screen: "list",
  config: busesConfig({ buses: [KICK, HATS], mainSends: [5, 30] }), index: 2 });
addBus("bus/list/new-row", { screen: "list",
  config: busesConfig({ buses: [KICK, HATS], mainSends: [5, 30] }), index: 3 });
/* ORPHANS. The count is the whole reason ids are retained, so a bus carrying
   them must be visibly different from one that is not. */
addBus("bus/list/orphans", { screen: "list",
  config: busesConfig({ buses: [Object.assign({}, KICK, { orphans: 2,
    voices: ["v0", "gone1", "gone2"] })] }), index: 0 });
/* A HOLE: bus 2 deleted, bus 3 still there. The config is positional and never
   compacted, so the list must show two buses and offer a New Bus that fills the
   hole rather than renumbering behind it. */
addBus("bus/list/hole", { screen: "list",
  config: busesConfig({ buses: [KICK, null, SNARE] }), index: 1 });
/* THE READ THAT DID NOT COMPLETE. Not an empty list -- an empty list is a claim
   about the slot and a failed read makes none. */
addBus("bus/list/waiting", { screen: "list", config: null });

/* The bus menu, one case per row kind, plus the two modes a row can be in. */
const CFG3 = busesConfig({ buses: [KICK, HATS, SNARE], mainSends: [5, 30] });
addBus("bus/actions/voices", { screen: "actions", config: CFG3, actionsRow: 0, index: 0 });
addBus("bus/actions/send-a", { screen: "actions", config: CFG3, actionsRow: 0, index: 2 });
addBus("bus/actions/editing", { screen: "actions", config: CFG3, actionsRow: 0, index: 2,
  editing: true });
addBus("bus/actions/delete-row", { screen: "actions", config: CFG3, actionsRow: 0, index: 5 });
addBus("bus/actions/confirm", { screen: "actions", config: CFG3, actionsRow: 0,
  confirming: true, confirmIndex: 1 });
/* There is no menu for the Sends row -- it opens the MIXER -- so the only
   actions screen is a bus`s, covered by the cases above. */
/* The orphan count rides the HEADER of this menu, so it is on screen whichever
   row the cursor is on. */
addBus("bus/actions/orphans", { screen: "actions",
  config: busesConfig({ buses: [Object.assign({}, KICK, { orphans: 2,
    voices: ["v0", "gone1", "gone2"] })] }), actionsRow: 0, index: 0 });

/* SIXTEEN VOICES, which is a drum rack, scrolling. Two are on this bus, one is
   on another (and says which, so moving it is an informed choice) and the rest
   are on Main. */
const CFG_VOICES = busesConfig({ buses: [
  { name: "Kick", voices: ["v0", "v5"], sends: [20, 0], fx: ["tapescam"] },
  { name: "Hats", voices: ["v1"], sends: [0, 15], fx: [] }] });
addBus("bus/voices/16-top", { screen: "voices", config: CFG_VOICES,
  voices: voicesJson(16), bus: 0, index: 0 });
addBus("bus/voices/16-scrolled", { screen: "voices", config: CFG_VOICES,
  voices: voicesJson(16), bus: 0, index: 9 });
/* A voice owned by ANOTHER bus, under the cursor: the footer says `add`, and
   the value column says whose it is. */
addBus("bus/voices/other-bus", { screen: "voices", config: CFG_VOICES,
  voices: voicesJson(16), bus: 0, index: 1 });
/* ORPHANS AS ROWS. The ids are all that is left of them, so they are printed,
   and clearing one is the only thing that clears the count. */
addBus("bus/voices/orphans", { screen: "voices",
  config: busesConfig({ buses: [{ name: "Kick", orphans: 2,
    voices: ["v0", "gone1", "gone2"], sends: [20, 0], fx: [] }] }),
  voices: voicesJson(4), bus: 0, index: 4 });
/* The voice list read did not complete. Same rule as the bus list. */
addBus("bus/voices/waiting", { screen: "voices", config: CFG_VOICES,
  voices: null, bus: 0, index: 0 });

/* A bus`s inserts at 0, 1 and 8 positions. Zero is ONE box -- the `+` -- not
   eight empty ones, which is the shape Master FX settled on for the same
   reason. */
addBus("bus/chain/0", { screen: "chain",
  config: busesConfig({ buses: [SNARE] }), bus: 0, pos: 0 });
addBus("bus/chain/1", { screen: "chain",
  config: busesConfig({ buses: [KICK] }), bus: 0, pos: 0 });
addBus("bus/chain/1-add", { screen: "chain",
  config: busesConfig({ buses: [KICK] }), bus: 0, pos: 1 });
addBus("bus/chain/8", { screen: "chain",
  config: busesConfig({ buses: [{ name: "Kick", sends: [20, 0],
    fx: rep(8, "cloudseed") }] }), bus: 0, pos: 7 });
addBus("bus/chain/8-first", { screen: "chain",
  config: busesConfig({ buses: [{ name: "Kick", sends: [20, 0],
    fx: rep(8, "cloudseed") }] }), bus: 0, pos: 0 });
/* A HOLE mid-chain draws as `--`, because bus_emit_config never compacts one
   away and neither may the picture of it. */
addBus("bus/chain/hole", { screen: "chain",
  config: busesConfig({ buses: [{ name: "Kick", sends: [20, 0],
    fx: ["tapescam", "", "cloudseed"] }] }), bus: 0, pos: 2 });
/* Bypass marks a box here exactly as it does on the slot chain. */
addBus("bus/chain/bypassed", { screen: "chain",
  config: busesConfig({ buses: [{ name: "Hats", sends: [0, 15],
    fx: ["cloudseed", "psxverb"], bypassed: [1, 0] }] }), bus: 0, pos: 1 });
/* THE HOLE UNDER THE CURSOR. Its footer must say ADD, not EDIT: an empty
   position holds no plugin, so there is nothing behind it to open -- and the
   verb is the only thing on screen that distinguishes it from the loaded box
   beside it. */
addBus("bus/chain/hole-selected", { screen: "chain",
  config: busesConfig({ buses: [{ name: "Kick", sends: [20, 0],
    fx: ["tapescam", "", "cloudseed"] }] }), bus: 0, pos: 1 });
/* And the picker this screen opens on a position. */
addBus("bus/chain/picker", { screen: "chain", picking: true,
  config: busesConfig({ buses: [KICK] }), bus: 0, pos: 0, pickIndex: 1,
  entries: [{ id: "", name: "None" }, { id: "cloudseed", name: "CloudSeed" },
            { id: "psxverb", name: "PSX Reverb" }] });

/* ======================================================================== */
/* THE BUS SEND MIXER, AS THE KNOB GRID                                      */
/* ======================================================================== */
/*
 * Main`s row on the bus list opens a two-page grid -- Send A, Send B -- with
 * every bus`s level on an encoder. Rendered here for the same reason the
 * Master FX settings pages are: it is a synthesised contract driven through
 * the REAL controller and the REAL bus_model.mjs, so a change to the contract
 * (a row that stops appearing, a name that stops fitting) moves a picture.
 *
 * busSendsGridIo is LIFTED out of shadow_ui.js rather than restated: the
 * mapping from a grid key to the two real spellings is the whole of what this
 * io does, and a hand-typed copy of it would baseline a mapping nobody runs.
 */
const mkBusSendsIo = lift("busSendsGridIo",
  ["busSlot", "busConfig", "busVoices", "BusModel", "getSlotParam", "setSlotParam",
   "busConfigStale"]);

const busSendsCases = [];
const addBusSends = (id, o) => busSendsCases.push(Object.assign({ id }, o));

/* Two buses, and the CAP -- SLOT_BUSES cells, the widest this page can ever be
   and exactly the number of knobs. (There is no Main cell: the slot`s own two
   send levels are real but belong to the slot, and are edited in Slot Settings.)
   Both pages of each, because the two levels are built from the same rows and a
   mapping that lost the send index would draw them identically. */
addBusSends("bus/sends/2-send-a", { page: "Send A",
  buses: [KICK, HATS], mainSends: [5, 30] });
addBusSends("bus/sends/2-send-b", { page: "Send B",
  buses: [KICK, HATS], mainSends: [5, 30] });
addBusSends("bus/sends/full-send-a", { page: "Send A",
  buses: ALL_BUSES, mainSends: [5, 30] });
/* A HOLE: bus 2 deleted. The rows are the LIST`s rows, so the page must show
   two faders and no third -- and the one that is there must still be bus 3`s
   own level, not the second row`s. */
addBusSends("bus/sends/hole-send-a", { page: "Send A",
  buses: [KICK, null, SNARE], mainSends: [5, 30] });

/* NO VOICE PAGES. The mixer carried a fader per voice for a while ("Voices A" /
   "Voices B", paginated because a rack can declare 32 of them); those levels
   are the module`s own parameters now — its pages, its state blob — so the
   mixer is the buses again and its pages are pinned. See
   src/host/voice_send_source.h. */

function renderBusSends(c) {
  const fb = createFramebuffer();
  const cfg = BusModel.parseBusesConfig(busesConfig(
    { buses: c.buses, mainSends: c.mainSends }));
  /* null, not [], for a slot with no voices: that is what refreshBusVoices
     leaves behind, and the io has to survive it. */
  const voices = c.voices
    ? BusModel.parseSplitVoices(JSON.stringify(c.voices))
    : null;
  /* The store, keyed by the REAL spelling -- "busN:sendM". Seeded from the
     same config the rows come from, so a case says its levels once. A key the
     mixer must never ask for is seeded with a value it would be obvious about:
     "buses:main_send1" is the SLOT`s send, edited in Slot Settings and never on
     this mixer, and a cell reading 99 would say so. */
  const store = { "buses:main_send1": "99", "buses:main_send2": "99" };
  cfg.buses.forEach((b) => {
    if (!b.present) return;
    store[`bus${b.index + 1}:send1`] = String(b.sends[0]);
    store[`bus${b.index + 1}:send2`] = String(b.sends[1]);
  });
  installGlobals(fb, (slot, key) => (store[key] !== undefined ? store[key] : ""));
  const io = mkBusSendsIo(0, cfg, voices, BusModel,
    (slot, k) => (store[k] !== undefined ? store[k] : ""),
    (slot, k, v) => { store[k] = String(v); return true; },
    false)();
  const ctl = createController(Object.assign({ announce: noop }, io));
  ctl.load({ slot: 0, component: "bus_sends", prefix: "bus_sends",
             /* THE SAME VALUE enterBusSendsGrid passes. A bus page is at most
                SLOT_BUSES cells against eight knobs, so it never has to split
                and the grouping is AUTHORED. */
             paginate: false });
  ctl.setLayout(LAYOUT_MOVY);
  const names = ctl.pages.map((p) => p.name);
  const at = names.indexOf(c.page);
  if (at < 0) fail(c.id + " names a page that does not exist: " + c.page +
                   " (pages: " + names.join(", ") + ")");
  ctl.goToPage(at);
  /* One read per tick, as on the device: wound forward until every cell has a
     value, exactly as renderSettings does. */
  for (let i = 0; i < 400; i++) ctl.tick();
  ctl.render(drawContext(fb), {
    title: "S1 > Send Mixer",
    footer: SETTINGS_FOOTER[ctl.page.kind] || SETTINGS_FOOTER.knobs,
  });
  clearGlobals();
  return fb;
}

/* ======================================================================== */
/* THE TWO SLOT SETTINGS LISTS                                               */
/* ======================================================================== */
/*
 * A slot has two settings screens -- CHAIN_SETTINGS (the chain editor`s
 * Settings position, as a list) and SLOT_SETTINGS (the slot list`s own) -- and
 * they overlap heavily by long-standing accident. The `Buses` door is a row on
 * BOTH, so it is rendered down both, the way the two module pickers are: a row
 * that appeared on one and not the other is exactly the drift this harness
 * exists to catch.
 *
 * The ROW LISTS are the real ones. getChainSettingsItems and slotSettingsItems
 * are lifted, and the predicate that hides the row (chainSynthSplits) is lifted
 * too and reads through the same cached-read helper the device uses -- so a
 * case that sets no `synth:split_voices` gets "" (served, does not split) and
 * no row, which is what makes "a module that cannot split shows no bus
 * affordance" a PIXEL claim rather than a written one.
 *
 * The VALUE of the Buses row is real as well (slotBusCountLabel, over the real
 * bus_model.mjs parse), because the count is the half of this row that can be
 * silently wrong.
 */
const settingsSrc = readFileSync("src/shadow/shadow_ui_settings.mjs", "utf8");
const slotsSrc = readFileSync("src/shadow/shadow_ui_slots.mjs", "utf8");

/* An exported const ARRAY, evaluated out of its own source rather than retyped:
   a mirrored copy that drifted would baseline a list the device does not draw. */
function liftArray(src, what, name) {
  const at = src.indexOf("const " + name + " = [");
  if (at < 0) { fail(name + " is gone from " + what); return []; }
  const end = src.indexOf("\n];", at);
  if (end < 0) { fail("could not find the end of " + name + " in " + what); return []; }
  return new Function(src.slice(at, end + 3) + "\nreturn " + name + ";")();
}
const CHAIN_SETTINGS_ITEMS = liftArray(uiSrc, "shadow_ui.js", "CHAIN_SETTINGS_ITEMS");
const SLOT_SETTINGS = liftArray(slotsSrc, "shadow_ui_slots.mjs", "SLOT_SETTINGS");

const mkChainSettingsDraw = liftFrom(settingsSrc, "shadow_ui_settings.mjs",
  "drawChainSettings",
  ["ctx", "drawNamePreview", "drawConfirmModal", "drawHeader", "drawMenuList",
   "LIST_TOP_Y", "FOOTER_RULE_Y"]);
const mkSlotSettingsDraw = liftFrom(slotsSrc, "shadow_ui_slots.mjs",
  "drawSlotSettings",
  ["ctx", "drawHeader", "drawMenuList", "drawFooter", "truncateText",
   "LIST_TOP_Y", "FOOTER_RULE_Y", "slotSettingsItems", "getSlotSettingValue",
   "selectedSetting", "editingSettingValue"]);
const mkSlotSettingsItems = liftFrom(slotsSrc, "shadow_ui_slots.mjs",
  "slotSettingsItems", ["ctx", "SLOT_SETTINGS"]);
const mkIsSlotMpe = liftFrom(slotsSrc, "shadow_ui_slots.mjs", "isSlotMpe", ["ctx"]);
const mkGetSlotSettingValue = liftFrom(slotsSrc, "shadow_ui_slots.mjs",
  "getSlotSettingValue", ["ctx", "isSlotMpe"]);

const settingsListCases = [];
const addSettingsList = (id, o) => settingsListCases.push(Object.assign({ id }, o));

/* Two voices is a splittable synth; absent, the key reads "" and the row is
   gone. The bus config is the same document bus_emit_config writes. */
/* `synth_module`, underscored: that is the GET spelling (set_param uses
   `synth:module`), and it is what loadChainConfigFromSlot reads. */
const SPLIT_STATE = { "synth_module": "sf2",
  "synth:split_voices": JSON.stringify([{ id: "kick", label: "Kick" },
                                        { id: "snare", label: "Snare" }]) };
const NOSPLIT_STATE = { "synth_module": "sf2" };
const SLOT_VALUES = {
  "slot:volume": "1.00", "slot:muted": "0", "slot:soloed": "0",
  "slot:receive_channel": "1", "slot:forward_channel": "-1",
  "slot:transpose": "0", "midi_fx_pre_mode": "0",
  /* The slot sends, stored by the CHAIN under the slot-level "buses:" route --
     not "slot:", which is why they are spelled out here rather than picked up
     by a prefix. One non-zero so the render shows a level and not just a zero
     column: a row whose value is always 0 cannot show that it is drawn wrong. */
  "buses:main_send1": "40", "buses:main_send2": "0",
};

/* THE THREE STATES OF THE ROW, down both lists: absent (the synth cannot
   split), present with no buses yet, and present with a count. */
addSettingsList("settings/slot/no-buses",
  { screen: "chain", state: NOSPLIT_STATE, sel: 0, preset: "Deep Pad" });
addSettingsList("settings/slot/buses-none",
  { screen: "chain", state: SPLIT_STATE, buses: [], sel: 0, preset: "Deep Pad" });
addSettingsList("settings/slot/buses-2",
  { screen: "chain", state: SPLIT_STATE, buses: [KICK, HATS], sel: 0, preset: "Deep Pad" });
/* The cursor ON the row. The list draws the caret and the value together, so
   this is the only case that shows the row as the user activates it. */
addSettingsList("settings/slot/buses-cursor",
  { screen: "chain", state: SPLIT_STATE, buses: [KICK, HATS], sel: 1, preset: "Deep Pad" });
/* A slot with nothing saved drops Delete and keeps Buses: the two conditions
   are unrelated, and one filter answering both is the mistake being avoided. */
addSettingsList("settings/slot/buses-nopreset",
  { screen: "chain", state: SPLIT_STATE, buses: [KICK], sel: 1, preset: "" });

addSettingsList("settings/slotlist/no-buses",
  { screen: "slots", state: NOSPLIT_STATE, sel: 0 });
addSettingsList("settings/slotlist/buses-2",
  { screen: "slots", state: SPLIT_STATE, buses: [KICK, HATS], sel: 0 });
/* On the row, so the FOOTER is drawn beside it: drawFooter drops a pair that
   does not fit and every pair after it, silently. */
addSettingsList("settings/slotlist/buses-cursor",
  { screen: "slots", state: SPLIT_STATE, buses: [KICK, HATS], sel: 2 });

function renderSettingsList(c) {
  const fb = createFramebuffer();
  const state = Object.assign({}, SLOT_VALUES, c.state);
  if (c.buses) state["buses:config"] = busesConfig({ buses: c.buses });
  const g = installGlobals(fb, (slot, key) => (state[key] !== undefined ? state[key] : ""));
  const w = chainWorld(state);
  w.ensureChainConfigFresh(0);

  /* The count, over the REAL parse. busSlot is -1 and busConfig null, which is
     the state a settings list is actually in: the bus screens have not been
     opened, so the cached read is the only source. */
  const slotBusCountLabel = lift("slotBusCountLabel",
    ["chainConfigs", "busSlot", "busConfig", "BusModel", "getSlotParamCached"])(
    w.chainConfigs, -1, null, BusModel, w.getSlotParamCached);
  const isSlotMpeMode = lift("isSlotMpeMode", ["getSlotParam"])(w.getSlotParam);
  const getChainSettingValue = lift("getChainSettingValue",
    ["isSlotMpeMode", "slotBusCountLabel", "getSlotParam"])(
    isSlotMpeMode, slotBusCountLabel, w.getSlotParam);
  const isExistingPreset = lift("isExistingPreset", ["slots"])([{ name: c.preset || "" }]);
  /* FALSE, because this harness renders the LIST form of the settings screen.
   * That is the branch that carries the two plain `Send A` / `Send B` rows: the
   * Send Mixer they replace is a knob grid, and a grid has nothing for a screen
   * reader to read out, so the list must keep a way to reach the slot sends. */
  const getChainSettingsItems = lift("getChainSettingsItems",
    ["isExistingPreset", "chainSynthSplits", "CHAIN_SETTINGS_ITEMS", "paramPagesEnabled"])(
    isExistingPreset, w.chainSynthSplits, CHAIN_SETTINGS_ITEMS, () => false);

  Object.assign(BUS_CTX, {
    slots: [{ name: c.preset || "Untitled" }],
    selectedSlot: 0,
    getSlotParam: w.getSlotParam,
    chainSynthSplits: w.chainSynthSplits,
    slotBusCountLabel,
    showingNamePreview: false, confirmingOverwrite: false, confirmingDelete: false,
    confirmIndex: 0, pendingSaveName: "", namePreviewIndex: 0,
    selectedChainSetting: c.sel || 0,
    editingChainSettingValue: false,
    getChainSettingsItems, getChainSettingValue,
  });

  if (c.screen === "chain") {
    mkChainSettingsDraw(BUS_CTX, noop, noop, drawMenuHeader, drawMenuList,
      LIST_TOP_Y, FOOTER_RULE_Y)();
  } else {
    const isSlotMpe = mkIsSlotMpe(BUS_CTX);
    const getSlotSettingValue = mkGetSlotSettingValue(BUS_CTX, isSlotMpe);
    const slotSettingsItems = mkSlotSettingsItems(BUS_CTX, SLOT_SETTINGS);
    mkSlotSettingsDraw(BUS_CTX, drawMenuHeader, drawMenuList, drawMenuFooter,
      truncateText, LIST_TOP_Y, FOOTER_RULE_Y, slotSettingsItems,
      getSlotSettingValue, c.sel || 0, false)();
  }
  clearGlobals();
  return fb;
}

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
/*
 * The two settings LISTS. drawChainSettings draws no footer at all -- it never
 * has -- so its list band runs to the bottom of the screen; drawSlotSettings
 * draws one, and gets the three-band form. Two lists rather than one, because a
 * band list that demanded a footer of both would either fail every chain case
 * or have to be weakened for both.
 */
const LIST_BANDS = [
  ["header", 0, LIST_TOP_Y - 1],
  ["list", LIST_TOP_Y, 63],
];
const FOOTED_LIST_BANDS = [
  ["header", 0, LIST_TOP_Y - 1],
  ["list", LIST_TOP_Y, FOOTER_RULE_Y - 1],
  ["footer", FOOTER_RULE_Y, 63],
];
const BANDS = { chain: EDITOR_BANDS, master: EDITOR_BANDS, picker: PICKER_BANDS,
                settings: SETTINGS_BANDS, list: LIST_BANDS,
                footedlist: FOOTED_LIST_BANDS };
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
run(sendSettingsCases, renderSendSettings, "picker");
run(busPickerCases, renderBusPicker, "picker");
/* The bus screens: the three lists wear the list bands, the insert chain wears
   the editor bands -- it IS the chain editor`s diagram and its four bands. */
run(busCases.filter((c) => c.screen !== "chain"), renderBusScreen, "picker");
run(busCases.filter((c) => c.screen === "chain" && !c.picking), renderBusScreen, "chain");
run(busCases.filter((c) => c.screen === "chain" && c.picking), renderBusScreen, "picker");
/* The send mixer wears the knob grid`s three bands, like every other page the
   controller draws. */
run(busSendsCases, renderBusSends, "settings");
run(settingsListCases.filter((c) => c.screen === "chain"), renderSettingsList, "list");
run(settingsListCases.filter((c) => c.screen === "slots"), renderSettingsList, "footedlist");

const ids = Object.keys(current);
if (ids.length < 50) fail("only " + ids.length + " cases -- the matrix has collapsed");

/* LOOK at cases as pictures: DUMP_PNG=/some/dir [DUMP_CASE=picker/] bash ...
 *
 * Text art hides overlaps -- a scroll arrow drawn into the corner of the
 * bracket frame reads as a plausible corner in half-blocks and as a smudge on
 * the OLED. Half the point of a pixel baseline is that a human can review the
 * change before regenerating it, and that review has to be visual. */
if (process.env.DUMP_PNG) {
  const dir = process.env.DUMP_PNG;
  mkdirSync(dir, { recursive: true });
  for (const id of Object.keys(current)) {
    if (process.env.DUMP_CASE && id.indexOf(process.env.DUMP_CASE) < 0) continue;
    writeFileSync(dir + "/" + id.replace(/\//g, "_") + ".png", current[id].fb.toPng(5));
  }
}

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
const SAME_ON_PURPOSE = [["master/len0/sel-add-fx", "master/len0/shift"],
  /* A splittable synth draws the chain editor EXACTLY as an unsplittable one
     does -- there is no bus affordance on this screen at all. Both pairs are
     the claim those cases exist to make, so matching is the PASS, not a
     collision; a footer hint or a reclaimed arrow would break the equality. */
  ["chain/len2/sel-fx1", "chain/len2/fx1-splits"],
  ["chain/len2/sel-synth", "chain/len2/synth-splits"]];
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
    "# The 28 bus/ cases ADDED the four SLOT BUS screens -- the bus list, one",
    "# bus`s menu, the voice multi-select and a bus`s insert chain -- and moved",
    "# NOTHING. The three chain/*splits cases were added with them and moved",
    "# nothing either, which is the POINT: a synth that publishes no",
    "# split_voices renders byte for byte as it did before buses existed, so",
    "# \"a module that cannot split shows no bus affordance\" is a pixel fact,",
    "# not a claim.",
    "#",
    "# MOVING THE BUS DOOR off the DOWN arrow and onto a settings row moved six",
    "# hashes and added eight cases. Up and Down are Move`s octave shift and only",
    "# Down was ever claimed, so the pair broke at the chain editor`s resting",
    "# cursor -- the gesture and the two footer hints that named it are gone.",
    "# chain/len2/synth-splits lost `DN BUS` and now equals chain/len2/sel-synth,",
    "# and the five bus/list rows lost `Dn: fx`; both are declared in",
    "# SAME_ON_PURPOSE or visible in the render. The eight new settings/slot and",
    "# settings/slotlist cases are the two settings LISTS, which this harness had",
    "# never drawn, with the `Buses` row absent, present-with-no-buses and",
    "# present-with-a-count.",
    "#",
    "# Step 4e made Master FX a variable-length chain: every master/ hash moved,",
    "# two cases naming an empty position past the end of the chain were deleted,",
    "# and eight were added for the `+` box and the Shift footer. Reviewed case by",
    "# case, and NO chain/ hash moved with them.",
    "#",
    "# RETIERING PER-VOICE SENDS onto the MODULE deleted five bus/sends cases --",
    "# the four `Voices` pages and the mixed slot -- and moved NOTHING. The mixer",
    "# rides buses again; a voice`s send level is one of the module`s own",
    "# parameters, on its own pages (src/host/voice_send_source.h). The bus pages",
    "# rendering byte for byte as they did is the point: the audio machinery and",
    "# the bus half of the screen were not supposed to change, and did not.",
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
            masterCases.length + " FX-bus (master and send), " +
            (pickerCases.length * 2) + " module-picker, " + busPickerCases.length +
            " FX-bus-picker, " + settingsCases.length + " Master FX settings and " +
            sendSettingsCases.length + " send settings-menu and " +
            busCases.length + " slot-bus and " + settingsListCases.length +
            " slot-settings-list renders match the baseline, " +
            "every one of them inside the display, in the device font, and with ink in " +
            "each band");
'
