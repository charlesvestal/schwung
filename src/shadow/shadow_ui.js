import * as os from 'os';
import * as std from 'std';

/* Import unified logger */
import { log as unifiedLog, installConsoleOverride } from '/data/UserData/schwung/shared/logger.mjs';

/* Install console.log override to route to unified debug.log */
installConsoleOverride('shadow');

/* Debug logging function - now uses unified logger */
function debugLog(msg) {
    unifiedLog('shadow', msg);
}

/* Per-event MIDI tracing in overtake mode. Off unless
 * /data/UserData/schwung/overtake_midi_log_on exists — it emits two lines for
 * every MIDI event while an overtake tool is up, which under a knob sweep is
 * hundreds of lines a second. Read once at load; touch the file and restart
 * shadow_ui to enable. */
const OVERTAKE_MIDI_LOG = (typeof host_file_exists === "function") &&
    host_file_exists("/data/UserData/schwung/overtake_midi_log_on");

/* Log at startup */
debugLog("shadow_ui.js loaded");

/* Import shared utilities - single source of truth */
import {
    MoveMainKnob,      // CC 14 - jog wheel
    MoveMainButton,    // CC 3 - jog click
    MoveBack,          // CC 51 - back button
    MoveRow1, MoveRow2, MoveRow3, MoveRow4,  // Track buttons (CC 43, 42, 41, 40)
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveKnob1Touch, MoveKnob8Touch,  // Capacitive touch notes (0-7)
    MidiNoteOn, MidiNoteOff
} from '/data/UserData/schwung/shared/constants.mjs';

import {
    SCREEN_WIDTH, SCREEN_HEIGHT,
    TITLE_Y, TITLE_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_TEXT_Y, FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';

import { decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';
/* The knob-grid chrome's footer rule row, which the chain editor's slot
 * indicator column stops above. The header/footer/list DRAWING that used to be
 * imported here went to chain_editor_chrome.mjs, so both editors do it once. */
import { RULE_Y as MOVY_RULE_Y,
         drawHeader as drawMovyHeader, drawFooter as drawMovyFooter }
    from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';
/* The bands around a chain editor's row of boxes — header, label, info,
 * footer — and the module picker it opens on a position. Both shared with
 * Master FX so the two editors wear the same furniture. */
import { drawChainEditorBands, drawChainPicker, shiftHintsFor, CHAIN_HINTS_AT_REST }
    from '/data/UserData/schwung/shared/chain_editor_chrome.mjs';
/* The chain editor's knob feedback card, and the two resolvers it needs to be
 * handed a row: what each key IS (metaIndex) and which cells a viz group
 * covers. Both are pure and both are already on the device for the knob grid. */
import { drawKnobCard } from '/data/UserData/schwung/shared/param_pages/knob_card.mjs';
import { buildMetaIndex } from '/data/UserData/schwung/shared/param_pages/param_meta.mjs';
import { resolveViz } from '/data/UserData/schwung/shared/param_pages/viz.mjs';
import { listKnobInit, listKnobStep } from '/data/UserData/schwung/shared/param_pages/list_knob.mjs';
import { describeLfoTarget } from '/data/UserData/schwung/shared/lfo_target_label.mjs';
import { emptyChain, parseId as parseChainId, chainComponents, moveBy as chainMoveBy,
         removeAt as chainRemoveAt, insertAt as chainInsertAt, MAX_FX, MAX_MIDI_FX }
    from '/data/UserData/schwung/shared/chain_model.mjs';
import { drawChainDiagram, DEFAULT_Y as DIAGRAM_Y, BOX_H as DIAGRAM_BOX_H }
    from '/data/UserData/schwung/shared/chain_diagram.mjs';
import { runDrawBench } from '/data/UserData/schwung/shared/draw_bench.mjs';
import { installParamTally, paramTallyTick, paramTallyArmed } from '/data/UserData/schwung/shared/param_tally.mjs';
import { knobInit, knobStep } from '/data/UserData/schwung/shared/knob_engine.mjs';
import {
    formatParamValue as ufFormatParamValue,
    formatParamForSet as ufFormatParamForSet,
} from '/data/UserData/schwung/shared/param_format.mjs';

/* Volume touch note for Shift+Vol+Jog detection */
const VOLUME_TOUCH_NOTE = 8;

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList,
    drawStatusOverlay,
    drawMessageOverlay,
    showOverlay,
    hideOverlay,
    tickOverlay,
    drawOverlay,
    menuLayoutDefaults,
    LIST_INDICATOR_BOTTOM_Y,
    VALUE_RIGHT_CLEARANCE
} from '/data/UserData/schwung/shared/menu_layout.mjs';

import {
    wrapText,
    createScrollableText,
    handleScrollableTextJog,
    isActionSelected,
    drawScrollableText
} from '/data/UserData/schwung/shared/scrollable_text.mjs';

import {
    fetchCatalog, getModuleStatus,
    removeModule as sharedRemoveModule,
    scanInstalledModules, getHostVersion, isNewerVersion,
    fetchReleaseJsonQuick,
    CATEGORIES
} from '/data/UserData/schwung/shared/store_utils.mjs';

import {
    openTextEntry,
    isTextEntryActive,
    handleTextEntryMidi,
    drawTextEntry,
    tickTextEntry,
    padSelectGlobal,
    setPadSelectGlobal,
    textPreviewGlobal,
    setTextPreviewGlobal
} from '/data/UserData/schwung/shared/text_entry.mjs';

import {
    announce,
    announceMenuItem,
    announceParameter
} from '/data/UserData/schwung/shared/screen_reader.mjs';

import {
    fetchAndParseManual,
    refreshManualBackground,
    processDownloadedHtml
} from '/data/UserData/schwung/shared/parse_move_manual.mjs';

import {
    OVERLAY_NONE,
    OVERLAY_SAMPLER,
    OVERLAY_SKIPBACK,
    OVERLAY_SHIFT_KNOB,
    OVERLAY_SET_PAGE,
    drawSamplerOverlay,
    drawSkipbackToast,
    drawShiftKnobOverlay,
    drawSetPageToast,
    SHIFT_KNOB_BOX_X,
    SHIFT_KNOB_BOX_Y,
    SHIFT_KNOB_BOX_W,
    SHIFT_KNOB_BOX_H,
    SET_PAGE_BOX_X,
    SET_PAGE_BOX_Y,
    SET_PAGE_BOX_W,
    SET_PAGE_BOX_H
} from '/data/UserData/schwung/shared/sampler_overlay.mjs';

import {
    maybeConfirmForModule,
    confirmLineInput,
    consumesLineInput,
    feedbackGateActive,
    feedbackGateCancel,
    feedbackGateDraw,
    feedbackGateInput,
} from '/data/UserData/schwung/shared/feedback_gate.mjs';

import {
    buildFilepathBrowserState,
    refreshFilepathBrowser,
    moveFilepathBrowserSelection,
    activateFilepathBrowserItem
} from '/data/UserData/schwung/shared/filepath_browser.mjs';

/* Shared context for view modules */
import { ctx as _ctx } from './shadow_ui_ctx.mjs';

/* Extracted view modules */
import {
    SLOT_SETTINGS,
    getSlotSettingValue,
    drawSlots as _drawSlots,
    drawSlotSettings as _drawSlotSettings,
    enterSlotSettings as _enterSlotSettings,
    handleSlotsJog, handleSlotSettingsJog,
    handleSlotsSelect, handleSlotSettingsSelect,
    handleSlotsBack, handleSlotSettingsBack
} from './shadow_ui_slots.mjs';
import {
    PATCH_INDEX_NONE,
    loadPatchList, findPatchIndexByName, findPatchByName,
    applyPatchSelection as _applyPatchSelection,
    drawPatches as _drawPatches,
    drawPatchDetail as _drawPatchDetail,
    drawComponentParams as _drawComponentParams,
    enterPatchBrowser as _enterPatchBrowser,
    enterPatchDetail as _enterPatchDetail,
    enterComponentParams as _enterComponentParams,
    handlePatchesJog, handlePatchDetailJog, handleComponentParamsJog,
    handlePatchesSelect, handlePatchDetailSelect, handleComponentParamsSelect,
    handlePatchesBack, handlePatchDetailBack, handleComponentParamsBack
} from './shadow_ui_patches.mjs';
import {
    drawPresets as _drawPresets,
    drawPresetDetail as _drawPresetDetail,
    enterPresetBrowser as _enterPresetBrowser,
    handlePresetsJog, handlePresetDetailJog,
    handlePresetsSelect, handlePresetDetailSelect,
    handlePresetsBack, handlePresetDetailBack,
    tickPresetPreview, isPresetPreviewActive
} from './shadow_ui_presets.mjs';
import {
    paramPagesEnabled, enterParamPages, exitParamPages, paramPagesActive,
    tickParamPages, drawParamPages, handleParamPagesMidi, currentParamPage,
    paramPagesComponent, paramPagesSlot, clearParamPagesTouch,
    enumPickerFooterHints, CONTRACT_SETTLE_MS, LAYOUT_LIST
} from './shadow_ui_param_pages.mjs';
import { createSlotGridIo, createMasterGridIo,
         MFX_MIDI_CHANNEL_OPTIONS, mfxMidiChannelToIndex,
         mfxMidiChannelFromIndex } from './shadow_ui_slot_grid.mjs';
import { createGlobalGridIo, GLOBAL_SECTIONS } from './shadow_ui_global_grid.mjs';
import {
    drawMasterFx as _drawMasterFx,
    getMasterFxDisplayName as _getMasterFxDisplayName,
    enterMasterFxSettings as _enterMasterFxSettings
} from './shadow_ui_master_fx.mjs';
import {
    scanForToolModules as _scanForToolModules,
    enterToolsMenu as _enterToolsMenu,
    drawToolsMenu as _drawToolsMenu,
    drawToolFileBrowser as _drawToolFileBrowser,
    drawToolEngineSelect as _drawToolEngineSelect,
    drawToolConfirm as _drawToolConfirm,
    drawToolProcessing as _drawToolProcessing,
    drawToolResult as _drawToolResult,
    drawToolStemReview as _drawToolStemReview,
    drawToolSetPicker as _drawToolSetPicker
} from './shadow_ui_tools.mjs';
import {
    drawStorePickerResult as _drawStorePickerResult
} from './shadow_ui_store.mjs';
import {
    drawChainSettings as _drawChainSettings,
    drawGlobalSettings as _drawGlobalSettings
} from './shadow_ui_settings.mjs';

/* Track buttons - derive from imported constants */
const TRACK_CC_START = MoveRow4;  // CC 40
const TRACK_CC_END = MoveRow1;    // CC 43
const SHADOW_UI_SLOTS = 4;

/* UI flags from shim (must match SHADOW_UI_FLAG_* in shim) */
const SHADOW_UI_FLAG_JUMP_TO_SLOT = 0x01;
const SHADOW_UI_FLAG_JUMP_TO_MASTER_FX = 0x02;
const SHADOW_UI_FLAG_JUMP_TO_OVERTAKE = 0x04;
const SHADOW_UI_FLAG_SAVE_STATE = 0x08;
const SHADOW_UI_FLAG_JUMP_TO_SCREENREADER = 0x10;
const SHADOW_UI_FLAG_SET_CHANGED = 0x20;
const SHADOW_UI_FLAG_JUMP_TO_SETTINGS = 0x40;
const SHADOW_UI_FLAG_JUMP_TO_TOOLS = 0x80;

/* Knob CC range for parameter control */
const KNOB_CC_START = MoveKnob1;  // CC 71
const KNOB_CC_END = MoveKnob8;    // CC 78
const NUM_KNOBS = 8;

/* Overtake encoder accumulation — batch knob/jog deltas and flush once per tick.
 * Without this, every encoder tick generates a separate IPC round-trip to the
 * module, causing sluggish response when turning fast. */
let overtakeKnobDelta = [0, 0, 0, 0, 0, 0, 0, 0];  // Accumulated delta per knob (CC 71-78)
let overtakeJogDelta = 0;                             // Accumulated delta for jog wheel (CC 14)

/* Co-run mode: chain editor runs alongside an active tool module. Tool keeps
 * pads/steps/knobs/transport; jog + jog-click + track buttons + OLED route to
 * the chain editor. coRunView tracks the chain editor's own navigation state
 * so deeper views (patch browser, component edit, etc.) work without touching
 * the outer `view`, which stays at VIEWS.OVERTAKE_MODULE so the tool ticks. */
let coRunChainEditSlot = -1;
let coRunView = -1;  // initialized to VIEWS.CHAIN_EDIT on first co-run entry

/* Co-run control-surface groups — JS mirror of the canonical map in
 * shadow_constants.h (corun_group_for_event). The tool declares which groups it
 * KEEPS via shadow_corun_begin; the chain-edit intercept below routes a control
 * to the editor only when its group CEDES (not kept). The shim owns the same
 * decision for move_native; this mirror is just for the in-process chain editor. */
const CORUN_GRP_OLED = 1 << 0, CORUN_GRP_PADS = 1 << 1, CORUN_GRP_STEPS = 1 << 2,
      CORUN_GRP_TRANSPORT = 1 << 3, CORUN_GRP_JOG = 1 << 4, CORUN_GRP_TRACK_BUTTONS = 1 << 5,
      CORUN_GRP_KNOBS = 1 << 6, CORUN_GRP_MASTER = 1 << 7, CORUN_GRP_SHIFT = 1 << 8,
      CORUN_GRP_BACK = 1 << 9, CORUN_GRP_MENU = 1 << 10, CORUN_GRP_TOUCH = 1 << 11;
const CORUN_KEEP_DEFAULT = CORUN_GRP_PADS | CORUN_GRP_STEPS | CORUN_GRP_TRANSPORT | CORUN_GRP_MENU;
let coRunKeepMask = 0;  // polled from shadow_control; 0 = default split
/* True when the tool CEDES this group to the co-run UI (so the editor handles it). */
function coRunCedes(grp) {
    const m = coRunKeepMask ? coRunKeepMask : CORUN_KEEP_DEFAULT;
    return grp !== 0 && (m & grp) === 0;
}

/* True while shadow_ui is drawing a co-run screen over a still-running tool —
 * either the chain editor (coRunChainEditSlot >= 0) or an addressed-view overlay
 * (corunOverlayId != null, declared below). In BOTH, the outer `view` stays
 * OVERTAKE_MODULE and coRunView holds the drawn screen, so the dispatcher keeps
 * delegating pads/steps/transport + LEDs to the tool. */
function coRunUiActive() { return coRunChainEditSlot >= 0 || corunOverlayId != null; }

/* Should shadow_ui's co-run intercept handle this control group? ONE uniform rule
 * for every UI element, with no per-element special-casing:
 *  - chain-edit: handle it when the tool CEDES it (peer is shadow_ui, same
 *    process, so ceded events arrive here).
 *  - addressed-view overlay: handle it when the tool KEEPS it — the tool keeps
 *    exactly the UI elements it wants the overlay to drive (kept events reach this
 *    process; ceded ones go to Move firmware). So "keeps" is the overlay's
 *    analogue of chain-edit's "cedes".
 * A tool thus enables, e.g., overlay knob editing simply by keeping
 * CORUN_GRP_KNOBS — no view-specific code in the dispatcher. */
function coRunWants(grp) {
    return corunOverlayId != null ? !coRunCedes(grp) : coRunCedes(grp);
}

/* Param-shim originals. When a chain module's UI is "loaded" (or when
 * enterHierarchyEditor / setupModuleParamShims fires), the shadow_ui
 * shims host_module_get_param / host_module_set_param so chain-editor
 * queries route to the slot's DSP. That's correct for native chain
 * editing — but during co-run, the active tool module ALSO calls those
 * globals expecting to talk to ITS OWN DSP. Without a swap, the tool
 * silently misroutes every IPC at the chain slot, which manifests as
 * pads not emitting MIDI, transport not working, and audio glitches.
 * setupModuleParamShims now caches the originals and we restore them
 * around every tool callback (tick + onMidiMessageInternal). */
let originalHostGetParam = null;
let originalHostSetParam = null;
let paramShimsInstalled = false;

const CONFIG_PATH = "/data/UserData/schwung/shadow_chain_config.json";
const PATCH_DIR = "/data/UserData/schwung/patches";
const SLOT_STATE_DIR_DEFAULT = "/data/UserData/schwung/slot_state";
let activeSlotStateDir = SLOT_STATE_DIR_DEFAULT;
const AUTOSAVE_INTERVAL = 300;  /* ~10 seconds at 30fps */
const DEFAULT_SLOTS = [
    { channel: 1, name: "" },
    { channel: 2, name: "" },
    { channel: 3, name: "" },
    { channel: 4, name: "" }
];

/* View constants */
const VIEWS = {
    SLOTS: "slots",           // List of 4 chain slots + Master FX
    SLOT_SETTINGS: "settings", // Per-slot settings (volume, channels) - legacy
    CHAIN_EDIT: "chainedit",  // Horizontal chain component editor
    CHAIN_SETTINGS: "chainsettings", // Chain settings (volume, channels, knob mapping)
    PATCHES: "patches",       // Patch list for selected slot
    PATCH_DETAIL: "detail",   // Show synth/fx info for selected patch
    COMPONENT_PARAMS: "params", // Edit component params (Phase 3)
    PRESETS: "modpresets",      // Module preset list (synth-only) for selected slot
    PRESET_DETAIL: "modpresetdetail", // Load/Delete a selected module preset
    COMPONENT_SELECT: "compselect", // Select module for a component
    COMPONENT_EDIT: "compedit",  // Edit component (presets, params) via Shift+Click
    MASTER_FX: "masterfx",    // Master FX selection
    HIERARCHY_EDITOR: "hierarch", // Hierarchy-based parameter editor
    PARAM_PAGES: "parampages", // Knob-grid parameter view (preview; Param View setting)
    CANVAS: "canvas",         // Full-screen canvas overlay/editor
    FILEPATH_BROWSER: "filepathbrowser", // Generic filepath picker for filepath params
    KNOB_EDITOR: "knobedit",  // Edit knob assignments for a slot
    KNOB_PARAM_PICKER: "knobpick", // Pick parameter for a knob assignment
    DYNAMIC_PARAM_PICKER: "dynamicpick", // Dedicated picker UI for module_picker/parameter_picker
    STORE_PICKER_RESULT: "storepickerresult",  // Store: success/error message
    OVERTAKE_MENU: "overtakemenu",   // Overtake module selection menu
    OVERTAKE_MODULE: "overtakemodule", // Running an overtake module
    GLOBAL_SETTINGS: "globalsettings",  // Global settings menu (display, audio, etc.)
    TOOLS: "tools",                     // Tools menu (Stem Separation, Timestretch)
    TOOL_FILE_BROWSER: "toolfilebrowser",   // Browse directories/files for tool input
    TOOL_CONFIRM: "toolconfirm",            // Confirm file selection before processing
    TOOL_PROCESSING: "toolprocessing",      // Progress/status while tool runs
    TOOL_RESULT: "toolresult",               // Success/failure result
    TOOL_ENGINE_SELECT: "toolengineselect",  // Pick engine (e.g. 3-stem vs 4-stem)
    TOOL_STEM_REVIEW: "toolstemreview",       // Review produced stems before saving
    TOOL_SET_PICKER: "toolsetpicker",         // Browse sets for render tool
    LFO_EDIT: "lfoedit",                      // LFO sub-menu editor
    ANALYTICS_PROMPT: "analyticsprompt",       // First-run analytics opt-out prompt
    LFO_TARGET_COMPONENT: "lfotargetcomp",    // LFO target picker step 1: component
    LFO_TARGET_PARAM: "lfotargetparam",       // LFO target picker step 2: parameter
    ENUM_PICKER: "enumpick"                   // Option list for an enum param
};

/* ==== CO-RUN VIEW ADDRESSING ====
 * A curated registry of addressable Schwung screens a co-running tool may open as
 * a temporary OVERLAY over its co-run target, then return from — generalizing
 * co-run beyond the two hardcoded targets. Tool + shadow_ui share one QuickJS
 * globalThis, so the three verbs are plain globals the tool calls directly:
 *   shadow_corun_entries()              -> array of openable screen ids (discovery)
 *   shadow_corun_open(id, keep_mask, a) -> true if opened (false on unknown id)
 *   shadow_corun_close()                -> dismiss, return to the underlay
 * The only C addition is shadow_corun_overlay(active, keep_mask), which flips the
 * OLED owner + keep_mask WITHOUT touching corun.target (so the consumer tool's
 * state machine is undisturbed). Entries are curated and added deliberately —
 * NEVER auto-derived from VIEWS (most VIEWS are context-dependent sub-views). */
const CORUN_ENTRIES = {
    slots:           { enter: function() { view = VIEWS.SLOTS; } },
    chain_editor:    { enter: function(a) { enterChainEdit((a && a.slot) | 0); } },
    master_fx:       { enter: function() { enterMasterFxSettings(); } },
    global_settings: { enter: function() { enterGlobalSettings(); } },
};

let corunOverlayId = null;        /* active overlay entry id, or null */
let corunOverlayPrevMask = 0;     /* keep_mask to restore on close */
let corunOverlayRootView = -1;    /* the entry's top-level view; Back here closes */

globalThis.shadow_corun_entries = function() {
    return Object.keys(CORUN_ENTRIES);
};

globalThis.shadow_corun_open = function(id, keep_mask, args) {
    const entry = CORUN_ENTRIES[id];
    if (!entry) return false;
    const st = (typeof shadow_corun_state === 'function') ? shadow_corun_state() : null;
    corunOverlayPrevMask = st ? (st.keep_mask | 0) : 0;
    corunOverlayId = id;
    /* Flip OLED to shadow_ui + apply the overlay's keep_mask; corun.target stays
     * put so the consumer tool's state machine is undisturbed. */
    if (typeof shadow_corun_overlay === 'function') shadow_corun_overlay(1, keep_mask | 0);
    /* Mirror chain-edit co-run: keep the outer view at OVERTAKE_MODULE (so the
     * dispatcher keeps delegating pads/steps/transport + LEDs to the tool) and let
     * the entry's view change land in coRunView, which the co-run draw path
     * renders. runCoRunChainEdit captures view -> coRunView around the enter. */
    coRunView = VIEWS.OVERTAKE_MODULE;
    runCoRunChainEdit(function() { entry.enter(args); });
    corunOverlayRootView = coRunView;
    needsRedraw = true;
    return true;
};

globalThis.shadow_corun_close = function() {
    if (corunOverlayId == null) return;
    corunOverlayId = null;
    corunOverlayRootView = -1;
    coRunView = VIEWS.OVERTAKE_MODULE;
    /* Restore the underlay's OLED owner + keep_mask. view never left
     * OVERTAKE_MODULE, so the tool stayed addressable throughout the overlay. */
    if (typeof shadow_corun_overlay === 'function') shadow_corun_overlay(0, corunOverlayPrevMask | 0);
    needsRedraw = true;
};
/* ==== END CO-RUN VIEW ADDRESSING ==== */

/* Special action key for swap module option */
const SWAP_MODULE_ACTION = "__swap_module__";

/* Upper bound on a section's length. The DSP reports how many positions it
 * actually holds; this only stops a garbled reply from turning into a long
 * run of IPC reads. */
const CHAIN_CAP = { midiFx: MAX_MIDI_FX, fx: MAX_FX };

/*
 * The editor's positions, in signal order, DERIVED from the chain model.
 *
 * The model bookends its list with Patch, the two `+` boxes and Settings. Only
 * Patch is dropped: the editor reaches it at selection index -1, where it has
 * always been. The `+` boxes ARE part of the editor's list, because they are
 * how a chain of variable length grows — with no fixed empty positions left to
 * click, they are the only way in.
 *
 * `key` is what the rest of the file addresses a position by, and it is
 * unchanged for everything that existed before: "synth", "midiFx" for the first
 * MIDI FX, "fx1"/"fx2"…, "settings". A second MIDI FX takes its model id
 * ("midi_fx2") rather than colliding on "midiFx".
 *
 * `caps` says which SECTIONS the chain has — `{ hasSynth, hasMidiFx }`, which is
 * exactly what a chain target carries, so the target IS the argument. Master FX
 * is one audio-FX section with no synth and no MIDI FX, so those positions are
 * dropped and what is left is `fx1..fxN`, the `+`, and Settings. Branching on
 * the CAPABILITY rather than on which chain this is: a third chain with a synth
 * and no MIDI FX would need no new case here, and "does this chain have a MIDI
 * FX section" states the reason where "is this master" would not. Absent, both
 * are assumed present, which is what every caller that predates Master FX means.
 */
function chainEditorComponents(cfg, caps) {
    const hasSynth = !caps || caps.hasSynth !== false;
    const hasMidiFx = !caps || caps.hasMidiFx !== false;
    /*
     * A FULL section has no `+`.
     *
     * chainComponents emits both boxes unconditionally -- it models the chain,
     * not the caps -- so Master FX kept offering "New effect" with all 8 slots
     * taken, and clicking it either did nothing or silently landed on a
     * position that could not exist. Reported from the device.
     *
     * The limit comes from the target's own cap(), which both chains already
     * publish (CHAIN_CAP for a slot, MASTER_FX_SLOTS for the master bus), so
     * this reads the number rather than keeping a third copy of it -- the
     * thing that has gone wrong every previous time a chain cap moved.
     */
    const full = (section) => {
        if (!caps || typeof caps.cap !== "function") return false;
        const limit = caps.cap(section);
        if (!(limit > 0)) return false;
        const held = section === "midiFx" ? (cfg.midiFx || []).length
                                          : (cfg.fx || []).length;
        return held >= limit;
    };

    const out = [];
    for (const pos of chainComponents(cfg)) {
        if (pos.kind === "patch") continue;
        if (!hasSynth && pos.kind === "synth") continue;
        if (!hasMidiFx && pos.section === "midiFx") continue;
        if (pos.kind === "add" && full(pos.section)) continue;
        const key = pos.kind === "synth" ? "synth"
            : pos.kind === "add" ? pos.id
            : pos.kind === "settings" ? "settings"
            : (pos.section === "midiFx" && pos.index === 0) ? "midiFx" : pos.id;
        /* The `+` boxes draw as "+" but they are ANNOUNCED and labelled in
         * words — "+" read aloud is nothing at all. */
        const label = pos.kind === "add"
            ? (pos.section === "midiFx" ? "Add MIDI FX" : "Add FX")
            : (pos.section === "midiFx" && cfg.midiFx.length === 1) ? "MIDI FX"
            : pos.label;
        out.push({ ...pos, key, label, position: out.length });
    }
    return out;
}

/*
 * The positions of ONE slot's chain — now genuinely per-slot, because the
 * length is whatever that slot's DSP instance holds.
 */
function slotChainComponents(slotIndex) {
    return chainEditorComponents(chainConfigs[slotIndex] || createEmptyChainConfig());
}

/* Where the selection lands when the editor is entered with no history: the
 * synth, which is the one position every chain has and the landmark the
 * diagram's scroll is anchored on. Position 0 is a `+` box, which is a poor
 * thing to be pointed at on arrival. */
function defaultChainComponent(slotIndex) {
    const at = slotChainComponentIndex(slotIndex, "synth");
    return at >= 0 ? at : 0;
}

/*
 * Load the slot and put the selection somewhere that EXISTS.
 *
 * The remembered index is no longer safe on its own: the list is as long as
 * the chain, so a slot whose FX were removed elsewhere comes back shorter than
 * the index left pointing into it, and every caller downstream reads
 * `comps[selectedChainComponent].key` without checking. -1 is kept as-is; it
 * is the patch selection, not an out-of-range index.
 */
function restoreChainComponent(slotIndex) {
    loadChainConfigFromSlot(slotIndex);
    const len = slotChainComponents(slotIndex).length;
    const want = lastChainComponent[slotIndex];
    selectedChainComponent = (typeof want === "number" && want >= -1 && want < len)
        ? want : defaultChainComponent(slotIndex);
}

/*
 * The chain-model id of a component key — which is also its DSP param prefix,
 * by construction, so this and getComponentParamPrefix agree.
 */
function chainComponentId(componentKey) {
    return componentKey === "midiFx" ? "midi_fx1" : componentKey;
}

/*
 * The editor key of position `index` in a section — the inverse of
 * chainComponentId, carrying the same single exception: the first MIDI FX is
 * keyed "midiFx" for everything that predates the list.
 *
 * A reorder needs this because a module's key CHANGES as it moves. Following
 * it by remembering the selection index instead would follow the position,
 * which is the thing the gesture just moved out from under it.
 */
function chainEditorKeyAt(section, index) {
    if (section === "midiFx") return index === 0 ? "midiFx" : `midi_fx${index + 1}`;
    return `fx${index + 1}`;
}

/* True for a key that addresses a module position (i.e. not "settings"). */
function isChainModuleKey(componentKey) {
    return componentKey === "synth" || parseChainId(chainComponentId(componentKey)) !== null;
}

/*
 * The module occupying a component position, or null.
 *
 * Replaces the old `cfg[comp.key]`, which stopped meaning anything once the FX
 * became a list: "fx2" is a position in that list, not a property.
 */
function getChainComponentModule(cfg, componentKey) {
    if (!cfg) return null;
    if (componentKey === "synth") return cfg.synth || null;
    const at = parseChainId(chainComponentId(componentKey));
    if (!at) return null;
    const list = cfg[at.section];
    return (list && list[at.index]) || null;
}

/*
 * Put a module (or null for empty) at a component position, IN PLACE.
 *
 * Deliberately not the model's removeAt: that compacts the list, which would
 * renumber every module downstream of a box the user only meant to clear. The
 * DSP still keeps an empty fx1 in front of a loaded fx2, and this mirrors it.
 */
function setChainComponentModule(cfg, componentKey, module) {
    if (!cfg) return;
    if (componentKey === "synth") { cfg.synth = module; return; }
    const at = parseChainId(chainComponentId(componentKey));
    if (!at) return;
    const list = cfg[at.section];
    while (list.length <= at.index) list.push(null);
    list[at.index] = module;
}

/*
 * Where a component key sits in a slot's editor list, or -1.
 *
 * The model's indexOfId answers the same question about ITS list, which keeps
 * the Patch and `+` bookends this one drops — so the lookup runs against the
 * editor's list, and the index it returns is the one selectedChainComponent
 * speaks.
 */
function slotChainComponentIndex(slotIndex, componentKey) {
    return slotChainComponents(slotIndex).findIndex(c => c.key === componentKey);
}

/*
 * Is anything loaded anywhere in this chain?
 *
 * INVARIANT this relies on: a non-null list entry always carries a non-empty
 * `module`. Both construction sites hold it — loadChainConfigFromSlot stores
 * null rather than an entry when the DSP reports "", and the picker only builds
 * an entry from a non-empty selected.id. The callers this replaced tested mere
 * object presence, so an entry with an empty module id would have counted as
 * loaded there and does not count as loaded here — which would read as a slot
 * silently autosaving itself empty. Keep the invariant, or make this test
 * presence again.
 */
function chainHasAnyModule(cfg) {
    if (!cfg) return false;
    if (cfg.synth && cfg.synth.module) return true;
    return cfg.midiFx.some(m => m && m.module) || cfg.fx.some(m => m && m.module);
}

/* Module abbreviations cache - populated from module.json "abbrev" field */
const moduleAbbrevCache = {
    /* Built-in fallbacks for special cases */
    "settings": "*",
    "empty": "--"
};

/* In-memory chain configuration (for future save/load).
 *
 * `{ midiFx: [], synth: null, fx: [] }` — a list per section, each entry
 * `{ module: "cloudseed", params: {} }` or null for an empty position.
 * Positions are addressed by id ("fx2") through the chain model, never by
 * property, so an unoccupied trailing position is simply absent. */
function createEmptyChainConfig() {
    return emptyChain();
}

/* Master FX options - populated by scanning modules directory */
let MASTER_FX_OPTIONS = [{ id: "", name: "None" }];

let slots = [];
let patches = [];
let selectedSlot = 0;
let selectedPatch = 0;
/* selectedDetailItem moved to shadow_ui_patches.mjs */
/* selectedSetting, editingSettingValue moved to shadow_ui_slots.mjs */
let view = VIEWS.SLOTS;
let needsRedraw = true;
let refreshCounter = 0;
let autosaveCounter = 0;
/* Which step of the spread-out autosave pass is next: 0..SHADOW_UI_SLOTS-1 is
 * that slot, SHADOW_UI_SLOTS is the master FX chain, null means idle. One step
 * per tick — see the drain block in globalThis.tick. */
let autosaveJob = null;
/* Exact bytes last written to each slot_N.json, so an unchanged slot skips the
 * eMMC write entirely (measured ~120ms per write — the single most expensive
 * thing the UI thread did). Cleared whenever the file set changes underneath
 * us, so the next pass rewrites unconditionally. */
let lastWrittenSlotJson = [null, null, null, null];
function invalidateAutosaveWriteCache() {
    lastWrittenSlotJson = [null, null, null, null];
}
let autosaveSuppressUntil = 0;  /* suppress autosave after set change */
let slotDirtyCache = [false, false, false, false];
/* Module signature ("synth|midi_fx1|fx1|fx2", one field per chain position, in
 * signal order — see getSlotModuleSignature) from the last successful autosave.
 * Used to relax the "empty state → bail" guard when the user swaps to a module
 * that lacks state get/set — a module change makes the prior file stale anyway. */
let lastSavedSlotSignature = ["", "", "", ""];
/* Set when the user explicitly empties every component in a slot via the
 * picker. Lets autosave bypass the "shim reports empty but slot has a
 * preset name" guard (which protects against transient boot-load failures)
 * for genuine user removals. Reset when the user picks any module, when a
 * set is loaded, or after the empty marker has been written. */
let slotUserCleared = [false, false, false, false];
/*
 * Which USER PRESET each component is on — {name, hash} per slot+prefix.
 *
 * Pure UI bookkeeping: the DSP never sees it, so there is no param, no struct
 * field and no SHM change. It rides slot_N.json because that is already the
 * file that survives a reboot for this component.
 */
const currentUserPresets = Object.create(null);
const userPresetKey = (slot, prefix) => `${slot}:${prefix}`;

function getUserPresetRecord(slot, prefix) {
    return currentUserPresets[userPresetKey(slot, prefix)] || null;
}
function setUserPresetRecord(slot, prefix, record) {
    if (record) currentUserPresets[userPresetKey(slot, prefix)] = record;
    else delete currentUserPresets[userPresetKey(slot, prefix)];
}
/* Pull {name, hash} out of a saved chain-position entry (synth / a midi_fx
 * item / an audio_fx item), or null when it never carried one — including
 * every patch written before user_preset existed. */
function entryUserPreset(entry) {
    if (!entry || !entry.user_preset || !entry.user_preset.name) return null;
    return { name: entry.user_preset.name, hash: entry.user_preset.hash || null };
}

/* Splash screen state */
let splashActive = true;
let splashTick = 0;

const SPLASH_BALL_Y = 26;
const SPLASH_RAISED_Y = 17;
const SPLASH_NUM_BALLS = 5;
const SPLASH_GROUP_X = [45, 56, 67, 78, 89];
const SPLASH_LEFT_RAISED  = { x: 40, y: 17 };
const SPLASH_RIGHT_RAISED = { x: 95, y: 17 };
const SPLASH_IMPACT_LINE_LEN = 1;
const SPLASH_IMPACT_GAP = 2;
const SPLASH_IMPACT_FLASH_TICKS = 20;
const SPLASH_PRE_HOLD_TICKS = 55;  /* ~1.25s — logo visible while Move boots */
const SPLASH_TENSION_TICKS = 10;
const SPLASH_RELEASE_TICKS = 5;
const SPLASH_HOLD_TICKS = 79;  /* ~1.8s hold after hit */
const SPLASH_TOTAL_TICKS = SPLASH_PRE_HOLD_TICKS + SPLASH_TENSION_TICKS +
    SPLASH_RELEASE_TICKS + SPLASH_HOLD_TICKS;

const SPLASH_CIRCLE_PATH = "/data/UserData/schwung/host/logo-circle.png";
const SPLASH_LOGO_PATH = "/data/UserData/schwung/host/logo-text.png";

function splashEaseInHard(t) { return t * t * t * t * t; }
function splashEaseOutHard(t) { return 1 - Math.pow(1 - t, 4); }

function splashArcPos(raised, restX, restY, progress) {
    const x = raised.x + (restX - raised.x) * progress;
    const angle = progress * Math.PI / 2;
    const y = raised.y + (restY - raised.y) * Math.sin(angle);
    return { x: Math.round(x), y: Math.round(y) };
}

function drawSplashScreen() {
    clear_screen();

    let leftProgress = 0;
    let rightProgress = 0;
    const t = splashTick;

    const preHoldEnd = SPLASH_PRE_HOLD_TICKS;
    const tensionEnd = preHoldEnd + SPLASH_TENSION_TICKS;
    const releaseEnd = tensionEnd + SPLASH_RELEASE_TICKS;

    if (t < preHoldEnd) {
        leftProgress = 0;
    } else if (t < tensionEnd) {
        const p = (t - preHoldEnd) / SPLASH_TENSION_TICKS;
        leftProgress = splashEaseInHard(p);
    } else if (t < tensionEnd + 1) {
        leftProgress = 1;
        rightProgress = 0;
    } else if (t < releaseEnd) {
        const p = (t - tensionEnd) / SPLASH_RELEASE_TICKS;
        leftProgress = 1;
        rightProgress = splashEaseOutHard(p);
    } else {
        leftProgress = 1;
        rightProgress = 1;
    }

    for (let i = 0; i < SPLASH_NUM_BALLS; i++) {
        let x = SPLASH_GROUP_X[i];
        let y = SPLASH_BALL_Y;
        if (i === 0) {
            const pos = splashArcPos(SPLASH_LEFT_RAISED, SPLASH_GROUP_X[0], SPLASH_BALL_Y, leftProgress);
            x = pos.x; y = pos.y;
        } else if (i === SPLASH_NUM_BALLS - 1) {
            const pos = splashArcPos(SPLASH_RIGHT_RAISED, SPLASH_GROUP_X[4], SPLASH_BALL_Y, 1 - rightProgress);
            x = pos.x; y = pos.y;
        }
        draw_image(SPLASH_CIRCLE_PATH, x - 4, y - 4, 128, 0);
    }

    /* Impact flash lines on ball 4 */
    const impactStart = tensionEnd;
    const ticksSinceImpact = t - impactStart;
    if (ticksSinceImpact >= 0 && ticksSinceImpact < SPLASH_IMPACT_FLASH_TICKS) {
        const rx = SPLASH_GROUP_X[3] + 4 + SPLASH_IMPACT_GAP;
        const ry = SPLASH_BALL_Y;
        const d45 = Math.round(SPLASH_IMPACT_LINE_LEN * 0.707);
        draw_line(rx, ry, rx + SPLASH_IMPACT_LINE_LEN, ry, 1);
        draw_line(rx, ry - 5, rx + d45, ry - 5 - d45, 1);
        draw_line(rx, ry + 5, rx + d45, ry + 5 + d45, 1);
    }

    draw_image(SPLASH_LOGO_PATH, 9, 37, 128, 0);

    const ver = "v" + getHostVersion();
    const verW = text_width(ver);
    print(Math.round((128 - verW) / 2), 56, ver, 1);
}

/* Overlay state (sampler/skipback from shim via SHM) */
let lastOverlaySeq = 0;
let overlayState = null;

/* FX display_name cache for change-based announcements (e.g. key detection) */
let fxDisplayNameCache = {};  /* key: "slot:component" -> last display_name string */
/*
 * Per-component backoff for the display_name poll.
 *
 * Almost no FX implements display_name — it exists for the handful that report
 * something live, like key detection. Every other loaded FX was asked once a
 * second forever and errored, and an errored read still costs the full ~2.8ms
 * round trip. The error is free; the round trip is not.
 *
 * Backed off rather than switched off: a module could in principle start
 * answering later. Reset wherever fxDisplayNameCache is reset (module swap),
 * since that is exactly when support can change.
 */
let fxDisplayNameSkip = {};   /* key -> polls still to skip */
let fxDisplayNameBackoff = {};/* key -> current skip length */
const FX_NAME_BACKOFF_MAX = 32;   /* ~32s at one poll/sec */

/* Returns the name, or null when the poll was skipped or unsupported. */
function pollFxDisplayName(slot, key, cacheKey) {
    if (fxDisplayNameSkip[cacheKey] > 0) { fxDisplayNameSkip[cacheKey]--; return null; }
    const name = getSlotParam(slot, key);
    if (name === null || name === undefined) {
        const next = Math.min((fxDisplayNameBackoff[cacheKey] || 1) * 2, FX_NAME_BACKOFF_MAX);
        fxDisplayNameBackoff[cacheKey] = next;
        fxDisplayNameSkip[cacheKey] = next;
        return null;
    }
    fxDisplayNameBackoff[cacheKey] = 0;
    fxDisplayNameSkip[cacheKey] = 0;
    return name;
}

/*
 * Cache for the chain-edit info line (module display name + preset).
 *
 * drawChainEdit() read `<prefix>:name`, `<prefix>:preset_name` and
 * `<prefix>:preset` on EVERY FRAME — three IPC round-trips per frame in a
 * draw function, ~8.4ms of a 16.67ms budget. Measured on hardware
 * 2026-08-19: ~50 errored reads/sec on `fx2:name` alone, because an FX that
 * does not implement the key still costs the full round trip to say so. That
 * load is served inside the SPI callback, where `param` was spiking to 700us.
 *
 * None of these values can change without a module swap or a preset load, so
 * a frame is the wrong cadence. Re-read when the module id changes (caught
 * immediately) or when the entry is older than the refresh interval (catches
 * a preset loaded underneath us). ~165 reads/sec becomes ~6.
 */
const SLOT_PARAM_CACHE_TTL_MS = 500;
let slotParamCache = {};   /* "slot:key" -> {module, value, ts} */

/*
 * Cached slot-param read for DRAW paths only. Returns the same value a bare
 * getSlotParam would, including null.
 *
 * Pass the loaded module id so a swap invalidates immediately; the TTL is
 * what catches a value changing underneath us (a preset loaded, a bank
 * switched). Do NOT use this where the value must be exact right now — use
 * getSlotParam directly on entry/commit paths.
 */
function getSlotParamCached(slot, key, moduleId) {
    const ck = `${slot}:${key}`;
    const hit = slotParamCache[ck];
    const now = Date.now();
    if (hit && hit.module === moduleId && (now - hit.ts) < SLOT_PARAM_CACHE_TTL_MS) {
        return hit.value;
    }
    const value = getSlotParam(slot, key);

    /*
     * A FAILED read is never cached.
     *
     * null from getSlotParam is the third answer: the read did not complete --
     * the claim was refused, or the response timed out, or it belonged to
     * somebody else. It is not news about the module, and storing it turns a
     * momentary channel stall into a 500ms lie about the chain.
     *
     * That is what a blank slot after loading granny is: granny reads its WAV
     * synchronously inside set_param, on the SPI thread that also serves param
     * requests, so every read during the load fails. Cached, the slot rendered
     * empty and STAYED empty for the TTL -- and because the cache is keyed by
     * slot, coming back to it hit the same poisoned entry, which is why it
     * took switching slots a few times to clear.
     *
     * "" is a different thing and IS cached: the channel served us and the key
     * produced nothing.
     *
     * On failure the last known-good value for the same module is preferred
     * over propagating the failure. It is stale by at most the stall, and it
     * is a value the module really did report -- where null makes the caller
     * draw a blank it will only redraw on the next frame anyway.
     */
    if (value === null || value === undefined) {
        return (hit && hit.module === moduleId) ? hit.value : value;
    }

    slotParamCache[ck] = { module: moduleId, value, ts: now };
    return value;
}

/* Helper to change view and announce it */
function setView(newView, customLabel) {
    if (view === newView) return;  /* No change */
    /* The card belongs to the chain editor and to one knob gesture; it must
     * not survive a screen change.
     *
     * The touch set goes with it, because a screen change can EAT the release:
     * handleParamPagesMidi claims knob-touch notes and returns before the
     * handlers below ever see them, so holding a knob here and letting go
     * inside the knob grid leaves this entry stuck true — and a stuck-true
     * entry stamps the next card as held-with-no-deadline, which nothing then
     * clears. Cheapest correct answer: no view change can begin with a finger
     * already down on a knob it knows about. */
    knobCardClose();
    knobTouched.fill(false);
    view = newView;
    needsRedraw = true;

    /* Note: View announcements now happen in enter*() functions with full context */
}
let redrawCounter = 0;
const REDRAW_INTERVAL = 2; // ~30fps at 16ms tick

/* Overtake module state */
let overtakeModules = [];        // List of available overtake modules
let selectedOvertakeModule = 0;  // Currently selected module in menu
let overtakeModuleLoaded = false; // True if an overtake module is running
let overtakeModulePath = "";      // Path to loaded overtake module
let overtakeModuleId = "";         // ID of loaded overtake module (for per-module exit hooks)
let previousView = VIEWS.SLOTS;   // View to return to after overtake
let overtakeModuleCallbacks = null;
let overtakeModuleCaps = null;      // capabilities of the loaded overtake module
let overtakeSuspendKeepsJs = false; // Current module opted in to JS-alive suspend
let overtakeSuspendSelfManaged = false; // Module owns Back; suspends via host_suspend_overtake()
let overtakePassthroughCCs = [];    // CCs declared in capabilities.button_passthrough — shim lets these
                                    // reach Move firmware directly, and we skip them during LED clear.
/* Map of suspended overtake modules keyed by moduleId. Each entry:
 *   { id, path, uiPath, basePath, capabilities, callbacks, suspendedAt }
 * Ticks are fired for every entry every frame until the module is resumed
 * (by re-selecting it in the overtake menu) or fully exited. */
let suspendedOvertakes = {};

/* Most-recently-suspended tool id. Shift+Vol+Step13 double-tap resumes it. */
let lastSuspendedToolId = "";
/* Most recent successful tool/overtake launch, for the Tools-shortcut
 * relaunch gesture. { kind: 'overtake'|'interactive', module, filePath }.
 * Session-scoped by design — not persisted across reboots, same as
 * lastSuspendedToolId. */
let lastLaunchedTool = null;
let lastToolsShortcutMs = 0;
const TOOLS_DOUBLE_TAP_MS = 500;

/* Analytics prompt state */
const ANALYTICS_PROMPTED_PATH = "/data/UserData/schwung/analytics-prompted";
let analyticsPromptSelection = 0;  // 0 = Yes (default), 1 = No

/* Auto-update state */
let autoUpdateCheckEnabled = true;   // Default: enabled (opt-out)
let pendingUpdates = [];              // Updates found on startup

/* Bootstrap-needed banner state. The self-heal mechanism (schwung-heal
 * setuid + entrypoint that invokes it at boot) requires one-time root
 * setup that the on-device update path can't perform. Detect at startup
 * whether the live entrypoint at /opt/move/Move is the new version
 * (contains the 'schwung-heal' invocation); if not, flag for a one-shot
 * banner that points the user at the web manager / GUI installer. */
let shimBootstrapNeeded = false;
let shimBootstrapPromptShown = false;
let pendingUpdateIndex = 0;           // Selected update in prompt

/* Host-side tracking for Shift+Vol+Jog escape (redundant with shim, but ensures escape always works) */
let hostVolumeKnobTouched = false;
let hostShiftHeld = false;  /* Local shift tracking - shim tracking doesn't work in overtake mode */
let hostMuteHeld = false;   /* Mute (CC 88) held — used as a modifier for Mute+JogClick bypass */

/* Deferred module init - clear LEDs and wait before calling init() */
let overtakeInitPending = false;
let overtakeInitTicks = 0;
const OVERTAKE_INIT_DELAY_TICKS = 30; // ~500ms at 16ms tick
/* Upper bound on waiting for the worker-side DSP load before running init()
 * anyway. The worker polls at 200ms, so a load lands within ~2 ticks past the
 * init delay; this is the give-up point for a DSP that never comes up. */
const OVERTAKE_DSP_READY_MAX_TICKS = 90; // ~1.5s

/* Progressive LED clearing - buffer only holds ~60 packets, so clear in batches */
const LEDS_PER_BATCH = 20;
let ledClearIndex = 0;

function clearLedBatch() {
    /* Clear LEDs in batches. Notes for pads/steps, CCs for buttons/knob indicators. */
    const noteLeds = [];
    /* Knob touch LEDs (0-7) */
    for (let i = 0; i <= 7; i++) noteLeds.push(i);
    /* Steps (16-31) */
    for (let i = 16; i <= 31; i++) noteLeds.push(i);
    /* Pads (68-99) */
    for (let i = 68; i <= 99; i++) noteLeds.push(i);

    /* All button/indicator CCs with LEDs. Full set candidates below; any CC
     * the module declared in capabilities.button_passthrough is filtered out
     * so Move firmware keeps owning its LED. */
    const passthrough = new Set(overtakePassthroughCCs);
    const candidates = [];
    for (let i = 16; i <= 31; i++) candidates.push(i);       // step icon LEDs
    candidates.push(40, 41, 42, 43);                         // tracks/rows
    candidates.push(49);                                     // shift
    candidates.push(50, 51, 52);                             // menu, back, capture
    candidates.push(54, 55);                                 // -, +
    candidates.push(56);                                     // undo
    candidates.push(58);                                     // loop
    candidates.push(60);                                     // copy
    candidates.push(62, 63);                                 // left, right
    candidates.push(71, 72, 73, 74, 75, 76, 77, 78);         // knob indicators
    candidates.push(85, 86);                                 // play, rec
    candidates.push(88);                                     // mute
    candidates.push(118, 119);                               // record, delete
    const ccLeds = candidates.filter((c) => !passthrough.has(c));

    const totalItems = noteLeds.length + ccLeds.length;
    const start = ledClearIndex;
    const end = Math.min(start + LEDS_PER_BATCH, totalItems);

    for (let i = start; i < end; i++) {
        if (i < noteLeds.length) {
            /* Note LED - send note on with velocity 0 */
            move_midi_internal_send([0x09, 0x90, noteLeds[i], 0]);
        } else {
            /* CC LED - send CC with value 0 */
            const ccIdx = i - noteLeds.length;
            move_midi_internal_send([0x0B, 0xB0, ccLeds[ccIdx], 0]);
        }
    }

    ledClearIndex = end;
    return ledClearIndex >= totalItems;
}

/* LED output queue for overtake modules - prevents SHM buffer flooding.
 * Intercepts move_midi_internal_send during overtake mode.
 * LED messages (note-on, CC on cable 0) are queued with last-writer-wins.
 * Non-LED messages pass through immediately.
 * Queue is flushed after each tick(), sending at most LED_QUEUE_MAX_PER_TICK. */
const LED_QUEUE_MAX_PER_TICK = 16;
let ledQueueNotes = {};      /* note -> [cin, status, note, color] */
let ledQueueCCs = {};        /* cc -> [cin, status, cc, color] */
let ledQueueActive = false;
let originalMidiInternalSend = null;

function activateLedQueue() {
    originalMidiInternalSend = globalThis.move_midi_internal_send;
    ledQueueNotes = {};
    ledQueueCCs = {};
    ledQueueActive = true;

    globalThis.move_midi_internal_send = function(arr) {
        if (!ledQueueActive || !originalMidiInternalSend) {
            return originalMidiInternalSend ? originalMidiInternalSend(arr) : undefined;
        }
        const type = arr[1] & 0xF0;
        if (type === 0x90) {
            ledQueueNotes[arr[2]] = [arr[0], arr[1], arr[2], arr[3]];
        } else if (type === 0xB0) {
            ledQueueCCs[arr[2]] = [arr[0], arr[1], arr[2], arr[3]];
        } else {
            /* Non-LED messages (sysex, etc.) pass through immediately */
            return originalMidiInternalSend(arr);
        }
    };
}

function deactivateLedQueue() {
    if (originalMidiInternalSend) {
        globalThis.move_midi_internal_send = originalMidiInternalSend;
        originalMidiInternalSend = null;
    }
    ledQueueNotes = {};
    ledQueueCCs = {};
    ledQueueActive = false;
}

function flushLedQueue() {
    if (!ledQueueActive || !originalMidiInternalSend) return;
    let count = 0;

    /* Flush note LEDs (pads, steps) */
    for (let note in ledQueueNotes) {
        if (count >= LED_QUEUE_MAX_PER_TICK) break;
        originalMidiInternalSend(ledQueueNotes[note]);
        delete ledQueueNotes[note];
        count++;
    }

    /* Flush CC LEDs (buttons, knob indicators) */
    for (let cc in ledQueueCCs) {
        if (count >= LED_QUEUE_MAX_PER_TICK) break;
        originalMidiInternalSend(ledQueueCCs[cc]);
        delete ledQueueCCs[cc];
        count++;
    }
}

/* Knob mapping state (overlay uses shared menu_layout.mjs) */
let knobMappings = [];       // {cc, name, value} for each knob
let lastKnobSlot = -1;       // Track slot changes to refresh mappings

/* Throttled knob overlay - only refresh value once per frame to avoid display lag */
let pendingKnobRefresh = false;  // True if we need to refresh overlay value
let pendingKnobIndex = -1;       // Which knob to refresh (-1 = none)
let pendingKnobDelta = 0;        // Accumulated delta for global slot knob adjustment

/* Throttled hierarchy knob adjustment - accumulate deltas, apply once per frame */
let pendingHierKnobIndex = -1;   // Which knob is being turned (-1 = none)
let pendingHierKnobDelta = 0;    // Accumulated delta to apply

/* Local knob value cache - avoids blocking getSlotParam during active knob turning.
 * Value is read once on first touch/turn, then updated locally by JS math.
 * Only setSlotParam (fire-and-forget write) is done during turning. */
let knobValueCache = new Array(8).fill(null);  // null = not cached, number = cached value
let knobValueCacheKey = new Array(8).fill("");  // fullKey that was cached (auto-invalidates on key change)

/* Knob acceleration settings */
const KNOB_BASE_STEP_FLOAT = 0.002; // Base step for floats (acceleration multiplies this)
const KNOB_BASE_STEP_INT = 1;       // Base step for ints
const TRIGGER_ENUM_TURN_THRESHOLD = 1;  // Positive detents required before firing trigger action
const TRIGGER_ENUM_WINDOW_MS = 700;     // Pause longer than this to start a new trigger gesture

/* Time tracking for knob acceleration */
let triggerEnumAccum = [0, 0, 0, 0, 0, 0, 0, 0];
let triggerEnumLastMs = [0, 0, 0, 0, 0, 0, 0, 0];
let triggerEnumLatched = [false, false, false, false, false, false, false, false];

/*
 * `access`, on every knob surface that is not the param-pages grid.
 *
 * The axis was implemented on the grid and never reached this file, so a
 * trigger was an ordinary enum everywhere else -- and "everywhere else" is
 * wider than it looks. getKnobContext serves the CHAIN EDITOR, MASTER FX and
 * the hierarchy list editor alike, so all three turned a trigger by writing
 * the value the turn walked onto: magneto's `clear` wipes the deck,
 * euclidrum's `rnd_preset` randomises all eight lanes, from a knob nudge.
 *
 * Worth being exact about, because the list editor is on its way out: if this
 * were only the list editor it would be near-dead code. The chain editor and
 * Master FX overlays are not going anywhere.
 *
 * These read the raw declaration rather than param_meta's normalised form,
 * which belongs to the grid and is not built here.
 */
/*
 * When each trigger last fired, keyed by full param key.
 *
 * The knob card draws its row with the SAME drawKnobRow the grid uses, and
 * that renderer is pure: it takes the fire times and the clock off its options
 * object. page_controller passes them; this file did not, so a trigger drew
 * its idle phase forever and the press animation was invisible everywhere
 * outside the param-pages grid.
 *
 * One renderer, two hosts, and the plumbing is the thing that diverges -- the
 * same omission that shipped once already on the grid side.
 *
 * Bounded by construction: one entry per parameter key that has ever been
 * fired in this session, which is a handful.
 */
const triggerFiredAt = Object.create(null);
const TRIGGER_KEEP_MS = 1200;

function noteTriggerFired(fullKey) {
    if (!fullKey) return;
    const t = Date.now();
    const prev = triggerFiredAt[fullKey] || [];
    /* An ARRAY, because two presses close together must read as two: the
     * renderer restarts its animation per timestamp. Old ones are dropped so
     * this cannot grow. */
    triggerFiredAt[fullKey] = prev.filter((p) => t - p < TRIGGER_KEEP_MS).concat(t);
}

/* The fire times for the row the card is showing, in the renderer shape: it
 * keys by the PARAM key, not the full slot-qualified one. */
function triggerFiredAtForRow(keys, prefixed) {
    const out = Object.create(null);
    if (!keys) return out;
    for (const k of keys) {
        if (!k) continue;
        const full = prefixed ? prefixed(k) : k;
        if (triggerFiredAt[full]) out[k] = triggerFiredAt[full];
    }
    return out;
}

function isTriggerParam(meta) {
    return !!(meta && String(meta.access || "").toLowerCase() === "write");
}
function isReadoutParam(meta) {
    return !!(meta && String(meta.access || "").toLowerCase() === "read");
}

/*
 * The value that FIRES a trigger, in the format the module reports.
 *
 * Option 1, never a bare index unless the module is already speaking indices:
 * euclidrum declares ["\u2014","Rnd!"] and fires on anything that is not the
 * em-dash, so writing "1" as a number would be read as a name it does not
 * know -- and writing "0" MEANS the em-dash, i.e. "do nothing", which is the
 * write that destroys a kit.
 */
function triggerFireValue(meta, currentVal) {
    const opts = (meta && Array.isArray(meta.options)) ? meta.options : null;
    if (!opts || opts.length < 2) return null;
    const usesIndex = opts.indexOf(currentVal) < 0 && !isNaN(parseInt(currentVal, 10));
    return usesIndex ? "1" : opts[1];
}

function isTriggerEnumMeta(meta) {
    return !!(meta &&
              meta.type === "enum" &&
              Array.isArray(meta.options) &&
              meta.options.length === 2 &&
              meta.options[0] === "idle" &&
              meta.options[1] === "trigger");
}

function updateTriggerEnumAccum(knobIndex, delta) {
    const now = Date.now();
    const last = triggerEnumLastMs[knobIndex] || 0;
    let accum = triggerEnumAccum[knobIndex] || 0;
    let latched = !!triggerEnumLatched[knobIndex];

    if (last === 0 || (now - last) > TRIGGER_ENUM_WINDOW_MS) {
        accum = 0;
        latched = false;
    }

    triggerEnumLastMs[knobIndex] = now;

    if (latched && delta < 0) {
        accum = 0;
        latched = false;
    }

    if (latched) {
        triggerEnumAccum[knobIndex] = TRIGGER_ENUM_TURN_THRESHOLD;
        triggerEnumLatched[knobIndex] = true;
        return false;
    }

    if (delta > 0) {
        accum += delta;
    } else if (delta < 0) {
        accum = Math.max(0, accum + delta);
    }

    triggerEnumAccum[knobIndex] = accum;

    if (accum >= TRIGGER_ENUM_TURN_THRESHOLD) {
        triggerEnumAccum[knobIndex] = TRIGGER_ENUM_TURN_THRESHOLD;
        triggerEnumLatched[knobIndex] = true;
        return true;
    }

    return false;
}

function getTriggerEnumOverlayValue(knobIndex) {
    const latched = !!triggerEnumLatched[knobIndex];
    const progress = triggerEnumAccum[knobIndex] || 0;
    if (latched) return "Triggered";
    if (progress > 0) return `Turn? ${progress}/${TRIGGER_ENUM_TURN_THRESHOLD}`;
    return "Turn?";
}

/* Cached knob contexts - avoid IPC calls on every CC message */
let cachedKnobContexts = [];     // Array of 8 contexts (one per knob)
let cachedKnobContextsView = ""; // View when cache was built
let cachedKnobContextsSlot = -1; // Slot when cache was built
let cachedKnobContextsComp = -1; // Component when cache was built
let cachedKnobContextsLevel = ""; // Hierarchy level when cache was built
let cachedKnobContextsChildIndex = -1; // Child index when cache was built

/*
 * The chain editor's knob card (shared/param_pages/knob_card.mjs).
 *
 * Raised by TOUCH, not by turn: resting a finger tells you what the knob does
 * before you move it, and it is the same signal the knob grid already follows.
 * A turn with no touch raises it too and decays, because a cap sensor that
 * misses must not be able to strand the feature.
 */
const KNOB_CARD_DECAY_MS = 700;
const knobTouched = new Array(NUM_KNOBS).fill(false);
let knobCardKnob = -1;          /* physical knob the card follows, or -1 */
let knobCardExpiry = 0;         /* ms deadline; 0 means held, so no deadline */
let knobCardSlot = -1;          /* target slot the row below was resolved against */
let knobCardCompKey = null;     /* component key ditto — see showKnobFeedback */
let knobCardKeys = null;        /* param key per physical knob, or null */
/* param key -> the slot-qualified key it was read with. Captured when the card
 * opens because that is the only place the target and component are in scope,
 * and the fire-time lookup needs the same spelling the writes used. */
let knobCardFullKey = null;
/* One capture per boot: the trigger file is emptied rather than removed (there
 * is no host_remove_file), so this stops a re-run on the next tick. */
let contractDumpDone = false;
let contractDumpCheckedMs = 0;
let knobCardMeta = null;        /* metaIndex for the focused component */
let knobCardRowValues = null;   /* raw values, keyed by param key */
let knobCardViz = null;
let knobCardModKey = null;      /* the ONE key known to be modulated (see below) */
let knobCardName = null;        /* the ANNOUNCED name, null when the card is not up */
let knobCardCardName = null;    /* the name DRAWN in the header band — see below */
let knobCardHeaderValue = null; /* header value, ditto */
let knobCardAnnouncedKnob = -1; /* which knob the last announcement was about */

function knobCardClose() {
    if (knobCardKnob < 0) return;
    knobCardKnob = -1;
    knobCardExpiry = 0;
    knobCardSlot = -1;
    knobCardCompKey = null;
    knobCardKeys = null;
    knobCardFullKey = null;
    knobCardMeta = null;
    knobCardRowValues = null;
    knobCardViz = null;
    knobCardModKey = null;
    /* Cleared so the NEXT raise announces. The `changed` test in
     * showKnobFeedback is a content comparison, and content that survived a
     * close matched itself: touch a knob, release, touch it again without
     * moving it and the screen reader said nothing — which is precisely the
     * gesture a screen-reader user makes to re-check a value. showOverlay does
     * not have this bug because its comparison is `overlayActive && ...`, so a
     * newly raised overlay always announces; this is that `overlayActive`. */
    knobCardName = null;
    knobCardCardName = null;
    knobCardHeaderValue = null;
    knobCardAnnouncedKnob = -1;
    needsRedraw = true;
}

/* Deliberately mutating for a predicate, and the only place in this feature
 * where the draw path writes state: the frame that finds the card expired is
 * the frame that must draw without it, and expiry moves one way, so doing it
 * here rather than in tick() removes a second place to forget. */
function knobCardActive() {
    if (knobCardKnob < 0) return false;
    if (knobCardExpiry && Date.now() > knobCardExpiry) { knobCardClose(); return false; }
    return true;
}

/*
 * Everything drawKnobCard needs, or null when the card is not up.
 *
 * ONE accessor rather than nine module-level reads at the call site, and the
 * reason is a test rather than tidiness: drawChainEdit is LIFTED out of this
 * file by tests/host/test_chain_edit_read_budget.sh with `new Function` and an
 * explicit dependency list, where any free identifier is a ReferenceError.
 * Nine free identifiers there is nine chances for that test to stop exercising
 * the card — and it very nearly did, behind a `typeof knobCardActive ===
 * "function"` guard that made the whole block silently unreachable under the
 * lift. The test that measures the chain editor's per-frame read cost was
 * therefore measuring it with the card switched off, which is the one
 * configuration nobody needed reassurance about.
 *
 * Costs no IPC: every value was read on touch-down. See knobCardOpen.
 */
function knobCardDrawState() {
    if (!knobCardActive()) return null;
    return {
        /*
         * THE BARE PARAMETER NAME, not the announced title.
         *
         * The band is 116px of content shared with the value, and the value
         * never loses a collision (drawCardHeader truncates the name), so a
         * composed "MFX: cloudseed mix" was chewed down to a few letters of
         * "MFX: clou" — spending the whole band saying what the diagram behind
         * the card already shows. The announcement keeps the full string; only
         * the pixels get the short one. See showKnobFeedback.
         *
         * ONE fallback, and it lives at the assignment in showKnobFeedback, not
         * here: a second `|| knobCardName` in this line would make removing
         * that one invisible to every test.
         */
        name: knobCardCardName,
        value: knobCardHeaderValue,
        row: knobCardKnob >> 2,
        touched: knobCardKnob,
        page: knobCardKeys ? { kind: "knobs", keys: knobCardKeys } : null,
        metaIndex: knobCardMeta,
        values: knobCardRowValues,
        viz: knobCardViz,
        modulated: knobCardModKey ? ((k) => k === knobCardModKey) : null,
        /* The press animation. Pure renderer: without these two the button
         * draws its idle phase forever -- see triggerFiredAt. */
        triggerFiredAt: triggerFiredAtForRow(knobCardKeys, knobCardFullKey),
        nowMs: Date.now(),
    };
}

/*
 * Everything the card needs, resolved ONCE on touch-down.
 *
 * The reads happen here, on an input event, and never on the draw path: an IPC
 * round trip is ~2.8ms against a 1.68ms whole-page render, so a read costs more
 * than redrawing the whole screen. The turned knob is updated by local
 * arithmetic afterwards (showKnobFeedback), so the card costs nothing per frame
 * while it is up.
 *
 * SIX reads, not four, and the difference is the two below: `ui_hierarchy` and
 * `chain_params` are each an IPC round trip in their own right, on top of one
 * per key in the touched knob ROW. That is ~17ms — a whole frame, spent on an
 * input event. tests/host/test_knob_card_open_budget.sh pins the number,
 * because this comment used to say four and nothing contradicted it.
 *
 * The SAME six on Master FX, and structurally rather than by coincidence: every
 * read here goes through the chain TARGET, and a target answers in one round
 * trip whatever the key is spelled like. "master_fx:fx1:cutoff" is a longer
 * string than "fx1:cutoff", not another read. The budget test asserts the two
 * bills are equal as well as asserting each is six, because two independent
 * "this one is six" assertions would still pass if one were re-baselined.
 *
 * It could be four: buildKnobContextForKnob fetched both of these for this same
 * component moments earlier and dropped them. Carrying them would mean a second
 * cache of module metadata with its own staleness window, next to the one whose
 * staleness is the bug documented in showKnobFeedback — not worth 5.6ms on a
 * gesture, off the draw path.
 *
 * The neighbours do not animate under modulation. That is the trade: animating
 * them means four reads EVERY frame to move a pointer nobody is looking at.
 */
function knobCardOpen(knobIndex, focus) {
    const target = focus.target;
    const comp = focus.comp;
    knobCardKnob = knobIndex;
    knobCardSlot = target.slot;
    knobCardCompKey = null;
    knobCardKeys = null;
    knobCardMeta = null;
    knobCardRowValues = null;
    knobCardViz = null;
    knobCardModKey = null;

    knobCardCompKey = comp ? comp.key : null;
    /* Not a module position — the whole-chain selection, a "+" box, the slot
     * settings box or Master FX's settings box. Short card, and free. */
    if (!chainTargetIsModulePosition(target, comp && comp.key)) return;

    const hierarchy = chainTargetHierarchy(target, comp.key);
    const chainParams = chainTargetChainParams(target, comp.key);
    if (!hierarchy || !chainParams || !chainParams.length) return;

    const keys = new Array(NUM_KNOBS).fill(null);
    for (let i = 0; i < NUM_KNOBS; i++) {
        const kc = getKnobContext(i);
        keys[i] = (kc && kc.key) ? kc.key : null;
    }
    if (!keys.some(Boolean)) return;

    knobCardMeta = buildMetaIndex({ hierarchy, chainParams });
    knobCardKeys = keys;
    knobCardFullKey = (k) => target.key(comp.key, k);
    knobCardViz = resolveViz({ keys, metaIndex: knobCardMeta }).groups;

    const base = (knobIndex >> 2) * 4;
    const values = {};
    for (let c = 0; c < 4; c++) {
        const k = keys[base + c];
        if (!k) continue;
        const raw = getSlotParam(target.slot, target.key(comp.key, k));
        /* An unserved key reads back as "", NOT as an error — the shim answers
         * error=4 with a zeroed buffer and js_shadow_get_param never looks at
         * error. Left as "" it would reach formatParamValue, where Number("")
         * is 0 and finite, and the cell would confidently read 0.00 for a
         * parameter that was never answered. null is the renderer's "--". */
        values[k] = (raw === null || raw === undefined || raw === "") ? null : raw;
    }
    knobCardRowValues = values;
}

/*
 * EITHER chain editor answers a knob with the CARD; every other view keeps the
 * centred name/value box. Both announce, so the screen reader does not care
 * which is up.
 *
 * The card shipped 2026-08-20 gated on `view !== VIEWS.CHAIN_EDIT`, so a Master
 * FX knob still raised the old `Value: 0.62` box — one reasonable-sounding
 * scope boundary, one day of drift, and the concrete example §1b of the Master
 * FX variable-length design exists to end. chainEditorFocus answers "which
 * chain, which position" for both, so there is no view test left here to
 * forget to widen next time.
 */
/*
 * `name` is what gets SPOKEN; `cardName` is what gets DRAWN. Two questions,
 * two answers — see the note above buildChainKnobContext. Callers pass
 * `ctx.title` and `ctx.cardName`; a caller whose name is already bare passes
 * one argument and the card falls back to it.
 */
function showKnobFeedback(knobIndex, name, value, raw, cardName) {
    const focus = chainEditorFocus();
    if (!focus) { showOverlay(name, value); return; }

    /* A centred box left over from the screen we came in through would draw
     * ON TOP of the card — drawOverlay runs after the view switch and nothing
     * dismisses it on input. Only one of the two is ever allowed up. */
    hideOverlay();

    /*
     * The card is resolved against a SLOT and a COMPONENT, and both of them
     * move without a view change — the jog steps selectedChainComponent, Track
     * 1-4 sets selectedSlot, and a chain shape edit clamps the selection. Only
     * setView closes the card, so reopening on the knob index alone gives a
     * WRONG READING, not a stale-looking one: turn knob 1, jog once inside the
     * 700ms decay window, turn knob 1 again, and the row still carries the
     * previous module's keys, labels and meta while the line below writes the
     * NEW component's value in under the OLD key. The number is current; the
     * name beside it belongs to a parameter you are not touching.
     *
     * chainEditorFocus reads chainConfigs (or a constant list, on Master FX),
     * not the DSP, so this costs nothing.
     *
     * The slot compared is the TARGET's, which is selectedSlot for a slot chain
     * and the constant 0 for Master FX — where Track 1-4 moves selectedSlot
     * without changing anything the card is showing, so comparing selectedSlot
     * there would re-resolve, and pay six IPC reads, for nothing.
     */
    const idCompKey = focus.comp ? focus.comp.key : null;
    if (knobCardKnob !== knobIndex || knobCardSlot !== focus.target.slot ||
        knobCardCompKey !== idCompKey) {
        /*
         * A malformed ui_hierarchy can throw in here — buildMetaIndex iterates
         * `lvl.params`, so a module serving `"params": 5` is a TypeError, and
         * buildKnobContextForKnob never touched that field so this is a new
         * exception surface on the touch path. Uncaught it would abort the rest
         * of the MIDI handler AND strand the card: knobCardKnob is set at the
         * top of knobCardOpen but the expiry is stamped below, so it would sit
         * there as "held, no deadline" with no note-off coming to clear it.
         */
        try {
            knobCardOpen(knobIndex, focus);
        } catch (e) {
            debugLog(`knobCardOpen failed for knob ${knobIndex}: ${e}`);
            /* Drop the half-built row, then re-establish the identity: what is
             * left is the short header-only card, which still tells the truth
             * (the name and value below do not come from the row). */
            knobCardClose();
            knobCardKnob = knobIndex;
            knobCardSlot = focus.target.slot;
            knobCardCompKey = idCompKey;
        }
    }
    /* Held keeps it up with no deadline; a turn with no touch gets a decay. */
    knobCardExpiry = knobTouched[knobIndex] ? 0 : Date.now() + KNOB_CARD_DECAY_MS;

    /* The turned knob, updated by local arithmetic — the only thing that moves
     * while the card is up, and the reason it costs no IPC per frame. Empty
     * normalises to null for the same reason it does in knobCardOpen. */
    if (knobCardRowValues && knobCardKeys && knobCardKeys[knobIndex] && raw !== undefined) {
        knobCardRowValues[knobCardKeys[knobIndex]] = (raw === null || raw === "") ? null : raw;
    }
    /*
     * Only the TOUCHED key's modulation is known, because that read is one
     * showKnobOverlay already pays for. Marking the neighbours would cost up to
     * three more reads each.
     *
     * STICKY for as long as the card follows this knob, and never cleared here:
     * the tilde only ever arrives from the touch path, because the turn path
     * (processPendingHierKnob) deliberately avoids showKnobOverlay to dodge
     * isHierarchyParamModulated's 1-3 reads and so passes a title with no
     * tilde. Recomputing per call therefore made the mark vanish the instant
     * you moved the knob. knobCardOpen clears it, which is the only moment a
     * different parameter can appear under this knob — and modulation routing
     * cannot change while a finger is on it.
     */
    if (name && name.endsWith("~") && knobCardKeys) {
        knobCardModKey = knobCardKeys[knobIndex];
    }

    /* The knob index is part of the comparison because a title alone is not
     * unique: the noModule branch gives every knob the same ctx.title, so two
     * knobs in a row would announce once between them. */
    const changed = (knobCardAnnouncedKnob !== knobIndex ||
                     knobCardName !== name || knobCardHeaderValue !== value);
    knobCardName = name;
    /* The tilde stays on `name` and never reaches here: the modulation mark is
     * appended to the ANNOUNCED title by showKnobOverlay, and knobCardModKey
     * above still reads it off `name`. The card shows modulation as a tick in
     * the cell, not as a character in the band. */
    knobCardCardName = (cardName === undefined || cardName === null) ? name : cardName;
    knobCardHeaderValue = value;
    if (changed) {
        knobCardAnnouncedKnob = knobIndex;
        announceParameter(name, value);
    }
    needsRedraw = true;
}

/* Knob editor state - for creating/editing knob assignments */
let knobEditorSlot = 0;          // Which slot we're editing knobs for
let knobEditorIndex = 0;         // Selected knob (0-7) in editor
let knobEditorAssignments = [];  // Array of 8 {target, param} for current slot
let knobParamPickerFolder = null; // null = main (targets), string = target name for params
let knobParamPickerIndex = 0;    // Selected index in param picker
let knobParamPickerParams = [];  // Available params in current folder
let knobParamPickerHierarchy = null; // Parsed ui_hierarchy for current target
let knobParamPickerLevel = null;     // Current level name in hierarchy (null = flat mode)
let knobParamPickerPath = [];        // Navigation path for back in hierarchy
let dynamicPickerMeta = null;
let dynamicPickerKey = "";
let dynamicPickerTargetKey = "";
let dynamicPickerMode = "target";  // target or param
let dynamicPickerIndex = 0;
let dynamicPickerTargets = [];
let dynamicPickerParams = [];
let dynamicPickerSelectedTarget = "";
let lastSlotModuleSignatures = [];  // Track per-slot module changes for knob cache refresh

/* Master FX state */
let currentMasterFxId = "";  // Currently loaded master FX module ID
let currentMasterFxPath = ""; // Full path to currently loaded DSP

/* Number of Master FX slots. This is a MIRROR of MASTER_FX_SLOTS in
 * src/host/shadow_chain_mgmt.h — the shim owns the actual array and this side
 * only addresses it by "master_fx:fx<N>:" key, so the two names must move
 * together or the UI silently stops seeing the slots past its own idea of the
 * cap. Every Master FX enumeration in this file and in
 * shadow_ui_master_fx.mjs is derived from this name; nothing hand-lists
 * fx1..fxN. Pinned by tests/host/test_master_fx_slots_js.sh, which reads both
 * values out of source and fails if they disagree. */
const MASTER_FX_SLOTS = 8;

/*
 * The Master FX chain as a chain-model config.
 *
 * masterFxConfig is a fixed fx1..fxN dictionary because that is how the chain
 * is PERSISTED (one `master_fx_N.json` per position). The editor wants a LIST,
 * and the list is bounded by how many positions are actually loaded — not by
 * the cap. That distinction is the whole of the 8-slot complaint: a fixed array
 * of eight empty boxes says nothing, a chain of one module and a `+` says
 * everything.
 *
 * Trailing empties are dropped and a hole in FRONT of a loaded module is KEPT,
 * exactly as loadChainConfigFromSlot does for a slot chain, and for the same
 * reason: position i of this list IS `fx(i+1)` in the DSP, so compacting a hole
 * away on READ would leave the editor addressing fx1's params while the audio
 * ran through fx2. The user's own edit compacts it (removeAt + the `remove`
 * verb), which renumbers the DSP at the same time.
 *
 * Once the DSP publishes `master_fx:fx_count` (step 4d) this becomes a read of
 * that count rather than a walk of the cap; the list it produces is the same
 * either way, which is why the display does not wait on it.
 */
/*
 * HOW LONG the Master FX chain is. -1 means "derive it from what is loaded".
 *
 * This is the published count, held client-side until the DSP publishes
 * `master_fx:fx_count` (step 4d). It has to be a value rather than always a
 * derivation for one reason: the position a `+` box opens is EMPTY, and an
 * empty position at the end is indistinguishable from the end of the chain. A
 * derivation would drop it the moment it was created, and the picker would be
 * standing on a position that no longer exists.
 */
let masterFxChainLength = -1;

function masterFxChainConfig() {
    const fx = [];
    for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
        const m = masterFxConfig[`fx${i}`];
        fx.push(m && m.module ? m : null);
    }
    /* Derived: trailing empties are dropped so the list says what is actually
     * loaded, and a hole in FRONT of a loaded module is KEPT — exactly as
     * loadChainConfigFromSlot does for a slot chain, and for the same reason.
     * The explicit length may only EXTEND past that end, never truncate: the
     * one thing it knows that the derivation cannot is a trailing hole a `+`
     * box just opened, and clamping it this way means a stale value can at
     * worst leave one empty box until the next reload rather than hide a
     * module. */
    let n = fx.length;
    while (n > 0 && !fx[n - 1]) n--;
    if (masterFxChainLength > n) n = Math.min(masterFxChainLength, MASTER_FX_SLOTS);
    fx.length = Math.max(0, n);
    return { midiFx: [], synth: null, fx };
}

/* Write a chain-model config back into the persisted fx1..fxN dictionary. The
 * positions past the chain's end are CLEARED, which is what makes a removal
 * close the gap rather than leave the old tail behind, and the LENGTH is kept
 * so a trailing hole survives until the picker resolves it. */
function setMasterFxChainConfig(cfg) {
    const list = cfg.fx || [];
    for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
        const m = list[i - 1];
        masterFxConfig[`fx${i}`] = { module: (m && m.module) || "" };
    }
    masterFxChainLength = Math.min(list.length, MASTER_FX_SLOTS);
}

/*
 * Master FX chain components: the loaded positions, then a `+`, then Settings.
 *
 * DERIVED from the same model the slot chain's list is derived from, through
 * the same chainEditorComponents — never hand-listed, and never bounded by the
 * cap. `kind` therefore comes from the model too: "module" for a position,
 * "add" for the `+`, "settings" for the last box. The kind that matters by its
 * absence is "synth" — the diagram paints a filled band across the top of a
 * synth box as the landmark the scroll leans on, and Master FX has no synth, so
 * no entry may ever claim that kind. The MASTER_CHAIN_TARGET's `hasSynth:false`
 * is what guarantees it.
 *
 * ONE `+`, appended: Master FX has one section, and its `+` is the audio-FX end
 * of a slot chain wearing the same rules.
 */
function masterFxChainComponents() {
    return chainEditorComponents(masterFxChainConfig(), MASTER_CHAIN_TARGET);
}

/* Is the Master FX selection on a module POSITION — as opposed to the preset
 * row, the `+`, the Settings box, or an index left over from a chain that got
 * shorter? The gates that used to compare the index against a fixed settings
 * position ask this instead; there is no fixed settings position any more. */
function masterFxSelectedIsModule() {
    if (selectedMasterFxComponent < 0) return false;
    const comp = masterFxChainComponents()[selectedMasterFxComponent];
    return !!comp && comp.kind === "module";
}

/* ============================================================================
 * CHAIN TARGETS — which chain an editor operation is talking about
 * ============================================================================
 *
 * There are two chain editors in this file and they are the SAME SCREEN wearing
 * two implementations. That is not an aesthetic complaint: features land in one
 * and not the other, one reasonable-sounding scope boundary at a time. The knob
 * card went in on 2026-08-20 for the slot chain only, so a Master FX knob still
 * raises the old `Value: 0.62` box — one day of drift. Master FX had no
 * windowed scroll until three commits ago, and it still has no reorder.
 *
 * The two differ in exactly TWO things:
 *
 *   | | Slot chain                  | Master FX                             |
 *   | param key | "fx1:cutoff" @ slot N | "master_fx:fx1:cutoff" @ slot 0     |
 *   | components| slotChainComponents(N) | MASTER_FX_CHAIN_COMPONENTS         |
 *
 * plus which SECTIONS exist, which is what `hasSynth` / `hasMidiFx` state.
 *
 * Branch on those CAPABILITIES, never on `kind`. A shared function containing
 * `if (target.kind === "master") return;` drifts exactly as well as two
 * functions did, and states no reason for the difference.
 *
 * `slot` is the IPC slot index: N for a slot chain, and 0 for Master FX — by
 * convention only. Master FX is NOT instrument slot 0; its keys are merely
 * addressed there. Never conflate it with selectedSlot.
 */

/* The Master FX component key at index i (0-based), or null when i is outside
 * the cap. This IS the Master FX bounds guard, written once — the four
 * accessors below used to each carry their own copy of `i < 0 || i >= CAP`. */
function masterFxComponentKey(i) {
    if (typeof i !== "number" || i < 0 || i >= MASTER_FX_SLOTS) return null;
    return `fx${i + 1}`;
}

/*
 * The Master FX position a HIERARCHY-EDITOR component key names, or -1.
 *
 * The inverse of the `master_fx:${fxKey}` spelling resetHierarchyEditorFor
 * documents: Master FX carries the PREFIXED key ("master_fx:fx2") where a slot
 * chain carries the bare one ("fx2"), which is what makes the two chains'
 * params addressable through one string. Everything that takes a component key
 * from the editor — or from the knob grid, which stores the same key — needs to
 * be able to get back to "which Master FX position is this", because the master
 * entry points are indexed.
 *
 * Bounded by MASTER_FX_SLOTS: an out-of-range "fx9" would otherwise be routed
 * as a real position and land on whatever the shim does with an unmatched key,
 * which is slot 0 under a garbage param name (see shadow_chain_mgmt.c).
 */
function masterFxIndexFromComponentKey(componentKey) {
    const m = /^master_fx:fx(\d+)$/.exec(String(componentKey || ""));
    if (!m) return -1;
    const i = Number(m[1]) - 1;
    return (i >= 0 && i < MASTER_FX_SLOTS) ? i : -1;
}

/*
 * The knob grid's CHROME for a component key — its header label, the key that
 * names the module behind it, and where Back goes — or null for the slot-chain
 * defaults.
 *
 * The grid is one view serving both chain editors, and the three things above
 * are all it has to be told to serve either. Built HERE, from the chain target,
 * because shadow_ui.js is the one place that knows there are two chains;
 * shadow_ui_param_pages.mjs deliberately takes them as data instead of testing
 * the key prefix itself. See currentChrome there.
 */
function paramPagesChromeFor(componentKey) {
    const mfx = masterFxIndexFromComponentKey(componentKey);
    if (mfx < 0) return null;
    return {
        label: MASTER_CHAIN_TARGET.label,
        /*
         * ":name", which serves the module ID -- NOT ":module", which serves
         * the plugin PATH, and not the slot chain's "master_fx:fx2_module"
         * underscore spelling, which is unserved here.
         *
         * All three spellings fail differently, and the middle one is the
         * nasty one. The underscore form is unserved, and an unserved read
         * comes back as "" so the header quietly loses its name. ":module" IS
         * served -- with a filesystem path -- and the abbreviation fallback
         * turned that into "/D", so every Master FX module showed the same
         * confident wrong label. A bad value believed because it parsed.
         *
         * The path key is deliberately left alone; other callers use it.
         */
        moduleKey: MASTER_CHAIN_TARGET.key(masterFxComponentKey(mfx), "name"),
        returnView: VIEWS.MASTER_FX,
    };
}

/* The chain of one instrument slot. */
function slotChainTarget(slotIndex) {
    return {
        kind: "slot",
        /* A STABLE NAME for this chain, so a record made against it (the pending
         * `+` insert) can be matched later. Identity cannot do that job: this
         * function builds a fresh object on every call. */
        id: `slot${slotIndex}`,
        slot: slotIndex,
        /* How this chain names itself in a knob title or an announcement —
         * "S2: CloudSeed Room Size". DATA, not a kind test: it is the one thing
         * the two chains genuinely have to say differently, and buildChainKnobContext
         * reads it rather than asking which chain it is looking at. The two
         * builders it replaced spelled the whole title twice, which is how the
         * fallback rule below came to differ between them unnoticed. */
        label: `S${slotIndex + 1}`,
        key: (componentKey, suffix) => chainComponentParamKey(componentKey, suffix),
        /* A key belonging to the CHAIN rather than to a position in it — the
         * two LFOs, and whatever else the bus grows. */
        chainKey: (suffix) => suffix,
        components: () => slotChainComponents(slotIndex),
        /* The chain as a MODEL config, and how to put an edited one back. The
         * shape editors (insert / remove / move) are written once against these
         * two, so neither has to know where a chain keeps its list. */
        config: () => chainConfigs[slotIndex] || createEmptyChainConfig(),
        setConfig: (cfg) => { chainConfigs[slotIndex] = cfg; },
        /* The cached view of this chain is no longer known to match the DSP.
         * LAZY: the next draw reloads it. */
        invalidate: () => { invalidateChainConfig(slotIndex); },
        /* Re-read it from the DSP NOW. The one caller that cannot wait is the
         * `+` cancel: it has to resolve the `+` box's index in the chain the
         * DSP actually holds, and with the cancelled hole still in the model
         * the `+` sits one place further right. */
        reload: () => loadChainConfigFromSlot(slotIndex),
        /* Which position the editor is pointing at. -1 is the patch row and is
         * not a position; it is preserved rather than clamped. */
        selection: () => selectedChainComponent,
        setSelection: (i) => {
            selectedChainComponent = i;
            lastChainComponent[slotIndex] = i;
        },
        /* Is this the chain the editor is pointing at? There are four of these
         * and the shim can switch between them underneath a picker. */
        isSelectedChain: () => selectedSlot === slotIndex,
        cap: (section) => CHAIN_CAP[section],
        hasSynth: true,
        hasMidiFx: true,
    };
}

/* The master bus chain. One section, no synth, addressed at slot 0 under the
 * "master_fx:" prefix. */
const MASTER_CHAIN_TARGET = {
    kind: "master",
    /* See slotChainTarget.id. */
    id: "master",
    slot: 0,
    /* See slotChainTarget.label. "MFX", never "S1" — Master FX is addressed at
     * slot 0 but it is not instrument slot 1, and a title that said so would be
     * the conflation that comment warns about. */
    label: "MFX",
    key: (componentKey, suffix) => {
        /* "settings" is a box in the list but not a module position, so it has
         * no params — same rule chainComponentParamKey applies for the slot
         * chain via isChainModuleKey. */
        if (!componentKey || componentKey === "settings") return null;
        const at = parseChainId(componentKey);
        if (!at || at.section !== "fx" || at.index >= MASTER_FX_SLOTS) return null;
        return `master_fx:${componentKey}:${suffix}`;
    },
    chainKey: (suffix) => `master_fx:${suffix}`,
    components: () => masterFxChainComponents(),
    config: () => masterFxChainConfig(),
    setConfig: (cfg) => { setMasterFxChainConfig(cfg); },
    /* masterFxConfig is the model, so "the cached view is stale" means "re-read
     * the positions from the DSP". LAZY, exactly as invalidateChainConfig is,
     * and for a reason that is not about cost: the `+` box materialises a
     * position IN THE MODEL ONLY, and an eager reload here would read the chain
     * back from a DSP that has never heard of it and wipe the hole out from
     * under the picker that was just opened on it. drawMasterFx reloads on its
     * next diagram frame instead — and the picker draws before that point, so
     * the pending position survives for exactly as long as it has to. */
    invalidate: () => { invalidateMasterFxConfig(); },
    reload: () => loadMasterFxChainConfig(),
    selection: () => selectedMasterFxComponent,
    setSelection: (i) => { selectedMasterFxComponent = i; },
    /* There is exactly one master bus, so it is always the master chain the
     * editor is pointing at. */
    isSelectedChain: () => true,
    /* One section, and its cap is the shim's array size — the constant this
     * file already mirrors — not the slot chain's. */
    cap: () => MASTER_FX_SLOTS,
    hasSynth: false,
    hasMidiFx: false,
};

/* Read one param of one component of a chain. null when the component has no
 * params to read (the settings box, or an id outside the chain) — which is the
 * same answer getSlotParam gives for an unreachable key, so callers that
 * coerce with `|| ""` are unaffected. */
function chainTargetGetParam(target, componentKey, suffix) {
    const key = target.key(componentKey, suffix);
    if (!key) return null;
    return getSlotParam(target.slot, key);
}

function chainTargetSetParam(target, componentKey, suffix, value) {
    const key = target.key(componentKey, suffix);
    if (!key) return false;
    return setSlotParam(target.slot, key, value);
}

/* Parse a JSON param out of a component, with the shape its callers expect on
 * failure. Both editors had their own copy of these two, differing only in how
 * the key was spelled. */
function chainTargetChainParams(target, componentKey) {
    const json = chainTargetGetParam(target, componentKey, "chain_params");
    if (!json) return [];
    try { return JSON.parse(json); } catch (e) { return []; }
}

/*
 * Does this component hold a module whose parameters can be addressed?
 *
 * Answered by the target's OWN key rule rather than by a second copy of it:
 * both targets already return null from key() for a box that is not a module
 * position (the settings box, a "+", an id past the cap). isChainModuleKey is
 * that rule for the slot chain; asking the target gets the same answer for
 * either chain without the caller knowing which it holds.
 */
function chainTargetIsModulePosition(target, componentKey) {
    return !!componentKey && target.key(componentKey, "module") !== null;
}

function chainTargetHierarchy(target, componentKey) {
    const json = chainTargetGetParam(target, componentKey, "ui_hierarchy");
    if (!json) return null;
    try { return JSON.parse(json); } catch (e) { return null; }
}

/*
 * Is the Master FX CHAIN DIAGRAM the thing on screen right now?
 *
 * drawMasterFx is a dispatcher: nine flags can put a text entry, a confirm, a
 * help page, the preset browser, the settings menu or the module picker in
 * FRONT of the diagram, and it early-returns into each of them. The knob card
 * is a modal over the diagram, so raising it while one of those is up would
 * leave the knob with no feedback at all — the card would be state-set and
 * never drawn, and the centred name/value box it replaces would not be shown
 * either. The slot chain has no equivalent because each of its sub-screens is
 * its own `view`.
 *
 * This list MIRRORS drawMasterFx's dispatch chain and the two must not drift;
 * tests/host/test_master_fx_knob_card.sh derives both from source and fails
 * when they disagree, because nothing else would say so.
 */
function masterFxChainDiagramVisible() {
    if (isTextEntryActive()) return false;
    if (masterShowingNamePreview) return false;
    if (masterConfirmingOverwrite) return false;
    if (masterConfirmingDelete) return false;
    if (helpDetailScrollState) return false;
    if (helpNavStack.length > 0) return false;
    if (inMasterPresetPicker) return false;
    if (inMasterFxSettingsMenu) return false;
    if (selectingMasterFxModule) return false;
    return true;
}

/*
 * Which chain the editor is showing, and which position in it is selected —
 * for EITHER chain, or null when the screen in front of the user is not a
 * chain editor with its diagram up.
 *
 * THE ONE PLACE that knows there are two chain-editor views. Everything
 * downstream (the knob card, and buildChainKnobContext through it) takes the
 * target and stops caring: that is what makes a feature land on both screens
 * by construction instead of one reasonable-sounding scope boundary at a time.
 *
 * `comp` is null for the whole-chain selection (-1) and for a box that is not
 * a module position; callers test it rather than the index, because the two
 * editors number their lists differently and only one of them has a synth.
 */
function chainEditorFocus() {
    let target = null;
    let index = -1;
    if (view === VIEWS.CHAIN_EDIT) {
        target = slotChainTarget(selectedSlot);
        index = selectedChainComponent;
    } else if (view === VIEWS.MASTER_FX && masterFxChainDiagramVisible()) {
        target = MASTER_CHAIN_TARGET;
        index = selectedMasterFxComponent;
    } else {
        return null;
    }
    const comps = target.components();
    return { target, comp: (index >= 0 && index < comps.length) ? comps[index] : null };
}

/*
 * Mute + Jog Click on a populated module toggles its bypass — in EITHER chain.
 * The two editors held identical copies of this that differed only in how the
 * key was spelled, which is the whole of the difference between them.
 *
 * `label` is the component's, because the announcement names the box the user
 * is pointing at ("FX 2 bypassed"), not the module inside it.
 */
/*
 * Which components an LFO is pointed at — { key: {lfo1, lfo2} } — for EITHER
 * chain, which is what the diagram paints its "~" markers from.
 *
 * FOUR IPC reads, FIXED: the question is asked of the two LFOs, never of each
 * box, so it does not grow with the chain. Both editors had this loop; keeping
 * that property in two places is how one of them eventually asks per box.
 */
function chainLfoTargetMap(target) {
    const out = {};
    for (let li = 1; li <= 2; li++) {
        if (getSlotParam(target.slot, target.chainKey(`lfo${li}:enabled`)) !== "1") continue;
        let t = getSlotParam(target.slot, target.chainKey(`lfo${li}:target`)) || "";
        /* The first MIDI FX is keyed "midiFx" in the editor; every other
         * position is keyed by its model id, which is what the LFO stores.
         * Gated on the CAPABILITY, not the kind: a chain with no MIDI FX
         * section can never have stored that id, and asking "does this chain
         * have MIDI FX" says why the rewrite is skipped where "is this master"
         * would not. */
        if (target.hasMidiFx && t === "midi_fx1") t = "midiFx";
        if (!t) continue;
        if (!out[t]) out[t] = {};
        out[t][`lfo${li}`] = true;
    }
    return out;
}

/* Is this position bypassed? False, without an IPC read, for anything that has
 * no bypass parameter — the settings box and the `+` boxes. */
function chainComponentBypassed(target, componentKey) {
    return parseInt(chainTargetGetParam(target, componentKey, "bypassed") || "0", 10) === 1;
}

/*
 * The hierarchy level whose `knobs` array drives the physical knobs.
 *
 * Root, unless root declared no knobs and names a child level that did — a
 * module whose real controls live one level down (a preset browser at the top)
 * would otherwise offer nothing on the knobs at all.
 *
 * Shared by both chain editors, which each had a copy. The LEVEL is returned
 * rather than the mapped key, because the slot editor logs it when the lookup
 * misses and a key alone cannot say why.
 */
function knobLevelForHierarchy(hierarchy) {
    if (!hierarchy || !hierarchy.levels) return null;
    let levelDef = hierarchy.levels.root || hierarchy.levels[Object.keys(hierarchy.levels)[0]];
    /* If root has no knobs but has children, use first child level for knob mapping */
    if (levelDef && (!levelDef.knobs || levelDef.knobs.length === 0) && levelDef.children) {
        const childLevel = hierarchy.levels[levelDef.children];
        if (childLevel && childLevel.knobs && childLevel.knobs.length > 0) {
            levelDef = childLevel;
        }
    }
    return levelDef;
}

/*
 * A parameter's metadata as the LEVEL declares it, or null.
 *
 * The level's `params` list is mixed: plain strings (a key with no metadata),
 * `{level, label}` navigation rows, and `{key, name, type, ...}` declarations.
 * Only the last kind carries a type, so only the last kind is answered here.
 */
function hierarchyLevelParamMeta(levelDef, key) {
    if (!levelDef || !Array.isArray(levelDef.params)) return null;
    for (const p of levelDef.params) {
        if (p && typeof p === "object" && p.key === key) return p;
    }
    return null;
}

function toggleChainComponentBypass(target, componentKey, label) {
    const cur = parseInt(chainTargetGetParam(target, componentKey, "bypassed") || "0", 10);
    const next = cur ? 0 : 1;
    chainTargetSetParam(target, componentKey, "bypassed", String(next));
    announce(next ? `${label} bypassed` : `${label} active`);
    needsRedraw = true;
}

function makeEmptyMasterFxConfig() {
    const cfg = {};
    for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
        cfg[`fx${i}`] = { module: "" };
    }
    return cfg;
}

/* Master FX chain editing state */
let masterFxConfig = makeEmptyMasterFxConfig();
/*
 * -1 = the preset row; 0..N-1 = fx1..fxN; then the `+`, then Settings.
 *
 * AN INDEX INTO A LIST THAT CHANGES LENGTH. Every shape edit shifts it —
 * removing a position shortens the chain, so a selection that pointed at
 * Settings now points at the `+` — so nothing may carry it across an edit.
 * Re-anchor by the component's KEY through the target's component list, the way
 * chainReorderJog and applyMasterFxModuleSelection do.
 */
let selectedMasterFxComponent = 0;
let selectingMasterFxModule = false;  // True when selecting module for a component
let selectedMasterFxModuleIndex = 0;  // Index in MASTER_FX_OPTIONS during selection

/* Master FX settings (shown when Settings component is selected) */
const MASTER_FX_SETTINGS_ITEMS_BASE = [
    { key: "master_volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    /* Which channel Master FX hears. Lives here rather than in Global Settings
     * because this is where a Master FX setting is looked for — reported from
     * the device on the first cut, which put it under Global > Audio next to
     * the other master_fx:* shim settings and so was never found. "All" is the
     * default and the pre-existing behaviour; narrowing it is opt-in. */
    { key: "master_fx_midi_channel", label: "MIDI Ch", type: "enum",
      options: MFX_MIDI_CHANNEL_OPTIONS },
    { key: "mfx_lfo1", label: "LFO 1", type: "action" },
    { key: "mfx_lfo2", label: "LFO 2", type: "action" },
    { key: "save", label: "[Save MFX Preset]", type: "action" },
    { key: "save_as", label: "[Save As]", type: "action" },
    { key: "delete", label: "[Delete]", type: "action" }
];

/* Param View: 0 = the hierarchy list editor, 1 = the knob grid.
 *
 * The grid is now the default. It shipped as an opt-in preview because it
 * could not draw everything the list could, and that gap is what kept it
 * opt-in rather than any doubt about the layout: mode selectors, child levels
 * and enum lists all had to land first, and the fleet contract fixture had to
 * be recaptured (76 modules -> 95) before "the grid covers the fleet" was a
 * measurement rather than a hope.
 *
 * The list is not deprecated. It stays reachable from Global Settings, and it
 * remains the better view for a module whose contract the grid cannot serve
 * well -- 11 modules publish no ui_hierarchy at all, and a knob grid over a
 * flat paginated param list is worse than a list of them.
 *
 * Read through a global so the view module can ask without importing
 * shadow_ui.js. See shadow_ui_param_pages.mjs. */
let paramViewGlobal = 1;
const PARAM_VIEW_CONFIG_PATH = "/data/UserData/schwung/param_view.json";
globalThis.param_view_get_mode = function() { return paramViewGlobal; };

/* A param a knob cannot turn — a filepath, canvas, wav_position or string.
 * The grid does not reimplement those editors; it steps aside and hands the
 * component to the list, which already has all of them. Announced, because
 * otherwise the view changing under you looks like a glitch. */
/*
 * Open the editor for ONE hierarchy param, dispatched on its declared type.
 *
 * Extracted from the jog-click handler so the knob grid can open a param
 * directly. Clicking a bracketed cell used to call enterHierarchyEditor() and
 * nothing else, which drops you at the component hierarchy with the param
 * unselected — you asked to open Position and got granny's menu. "Open this"
 * has to mean this one.
 *
 * forceOpen: the grid has nothing to toggle, so it always opens. The jog-click
 * caller passes false and keeps its open/close toggle.
 */
/* Leave the hierarchy editor and put the knob grid back up, on the same slot
 * and component it handed off. */
/*
 * Back to the grid from the LFO target picker. Returns true when it handled it.
 * The controller was never torn down, so the grid comes back on the page and
 * the cell it left from.
 */
function returnToSlotGridFromLfoTarget() {
    if (!lfoTargetFromGrid) return false;
    lfoTargetFromGrid = false;
    if (!paramPagesActive()) return false;
    setView(VIEWS.PARAM_PAGES);
    needsRedraw = true;
    return true;
}

function returnToParamPagesFromEditor() {
    const slotIndex = hierEditorSlot;
    const componentKey = hierEditorComponent;
    const returnPage = paramEditorReturnPage;
    paramEditorOpenedFromGrid = false;
    paramEditorReturnPage = "";
    exitHierarchyEditor();
    enterParamPages(slotIndex, componentKey, getComponentParamPrefix(componentKey), returnPage,
                    null, paramPagesChromeFor(componentKey));
    needsRedraw = true;
}

function openHierarchyParamEditor(selectedKey, meta, forceOpen) {
    if (hierEditorEditMode && !forceOpen) {
        hierEditorEditMode = false;
        resetHierarchyEditState();
        invalidateKnobContextCache();
        return;
    }
    if (!hierEditorEditMode && meta && meta.type === "string") {
        const fullKey = buildHierarchyParamKey(selectedKey);
        const currentText = getSlotParam(hierEditorSlot, fullKey) || "";
        openTextEntry({
            title: meta.name || selectedKey,
            initialText: String(currentText),
            onAnnounce: announce,
            onConfirm: (nextText) => {
                setSlotParam(hierEditorSlot, fullKey, String(nextText || ""));
                refreshHierarchyVisibility();
                announceParameter(meta.name || selectedKey, String(nextText || ""));
                needsRedraw = true;
            },
            onCancel: () => { needsRedraw = true; }
        });
        return;
    }
    if (!hierEditorEditMode && meta && meta.type === "canvas") {
        openCanvasPreview(selectedKey, meta);
        needsRedraw = true;
        return;
    }
    if (!hierEditorEditMode && meta && meta.type === "filepath") {
        openHierarchyFilepathBrowser(selectedKey, meta);
        return;
    }
    /*
     * An ENUM opens its option list. The jog still steps it in place from the
     * row (adjustHierSelectedParam), so this is the other half of the same
     * affordance the knob grid gets — one behaviour on both editors, which is
     * the point of putting it here rather than only on the grid path.
     *
     * The write goes through the same auto-detect the row's own nudge uses:
     * ask what the plugin REPORTS, answer in kind. chord's set_param is a
     * strcmp ladder over the names with no trailing else, so an index would be
     * discarded in silence.
     */
    /*
     * A TRIGGER is pushed, not opened. Clicking it fires; it must not raise
     * the option picker -- a two-item list whose second item is the action is
     * a way to fire it by accident, and there is nothing to browse.
     */
    if (!hierEditorEditMode && isTriggerParam(meta)) {
        const fullKey = buildHierarchyParamKey(selectedKey);
        const fire = triggerFireValue(meta, getSlotParam(hierEditorSlot, fullKey));
        if (fire !== null) {
            setSlotParam(hierEditorSlot, fullKey, fire);
            noteTriggerFired(fullKey);
        }
        return;
    }
    /* A READOUT has nothing to set. Opening a picker on it discarded the
     * choice in silence, which is the keydetect case. */
    if (!hierEditorEditMode && isReadoutParam(meta)) return;

    if (!hierEditorEditMode && meta && meta.type === "enum" &&
        Array.isArray(meta.options) && meta.options.length > 0) {
        const fullKey = buildHierarchyParamKey(selectedKey);
        const currentVal = getSlotParam(hierEditorSlot, fullKey);
        const nameIdx = meta.options.indexOf(currentVal);
        const pluginUsesIndex = (nameIdx < 0);
        let index = nameIdx;
        if (pluginUsesIndex) {
            const parsed = parseInt(currentVal, 10);
            index = (!isNaN(parsed) && parsed >= 0 && parsed < meta.options.length) ? parsed : 0;
        }
        const slot = hierEditorSlot;
        openEnumPicker({
            title: meta.name || meta.label || selectedKey,
            options: meta.options,
            index,
            commit: (i) => {
                setSlotParam(slot, fullKey, pluginUsesIndex ? String(i) : meta.options[i]);
                if (shouldRefreshDynamicRateMeta(selectedKey)) refreshHierarchyChainParams();
                refreshHierarchyVisibility();
            },
            returnToGrid: false,
        });
        return;
    }
    /* Everything else — including wav_position, whose editor IS edit mode on a
     * selected wav_position (see isInWavPositionEditor). */
    if (beginHierarchyParamEdit(selectedKey)) {
        hierEditorEditMode = true;
        /* Knob context override depends on edit mode + multi-marker role;
         * force re-evaluation. */
        invalidateKnobContextCache();
    }
}

/* Index of `bare` in the CURRENT level's param list, or -1. */
function indexOfHierParam(bare) {
    if (!Array.isArray(hierEditorParams)) return -1;
    for (let i = 0; i < hierEditorParams.length; i++) {
        const entry = hierEditorParams[i];
        const k = (entry && typeof entry === "object") ? (entry.key || "") : String(entry || "");
        if (k && k === bare) return i;
    }
    return -1;
}

/* The level whose params[] actually lists `bare`, or null. A level's knobs[]
 * does not count: the list editor selects out of params[], so a knob-only
 * mention is not somewhere it can put a cursor. */
function findLevelListingParam(bare) {
    const levels = hierEditorHierarchy && hierEditorHierarchy.levels;
    if (!levels) return null;
    for (const [name, def] of Object.entries(levels)) {
        if (!def || !Array.isArray(def.params)) continue;
        for (const entry of def.params) {
            if (!entry) continue;
            if (typeof entry === "string") { if (entry === bare) return name; continue; }
            if (typeof entry === "object" && !entry.level && entry.key === bare) return name;
        }
    }
    return null;
}

function openParamEditorFromGrid(slotIndex, fullKey, meta) {
    const componentKey = paramPagesComponent();
    /*
     * Slot settings is a synthesised contract, not a component: there is no
     * "slot:ui_hierarchy" to fetch, so enterHierarchyEditor would fall through
     * to the no-presets fallback. The one divable thing on its pages is an LFO
     * Target, which has its own two-step picker — so open that and refuse
     * everything else rather than land somewhere wrong.
     */
    if (componentKey === "slot" || componentKey === MASTER_SETTINGS_COMPONENT) {
        const isMaster = componentKey === MASTER_SETTINGS_COMPONENT;
        const m = isMaster
            ? /^master_settings:master_fx:lfo([12]):target$/.exec(String(fullKey || ""))
            : /^slot:lfo([12]):target$/.exec(String(fullKey || ""));
        if (m) {
            /* enterLfoTargetPicker reads lfoCtx, so point it at this LFO first —
             * the same context the list editor builds. */
            lfoCtx = isMaster ? makeMfxLfoCtx(Number(m[1]) - 1)
                              : makeSlotLfoCtx(slotIndex, Number(m[1]) - 1);
            /* Mark the hand-off BEFORE opening, so every exit from the picker
             * knows where it came from. The grid controller stays alive. */
            lfoTargetFromGrid = true;
            /* The note-off for the knob being held will go to the PICKER, not
             * back here, so drop the touch now or the cell stays highlighted
             * forever once we return. */
            clearParamPagesTouch();
            enterLfoTargetPicker();
            needsRedraw = true;
        }
        return;
    }
    /* Read the grid page BEFORE exiting — the level it was on is where the
     * param lives, and exitParamPages tears the controller down. */
    const page = currentParamPage();
    const level = page && page.level;
    paramEditorReturnPage = (page && page.name) || "";
    /* The grid builds every fullKey as `${prefix}:${key}`, so strip THAT exact
     * prefix rather than "everything up to the first colon". Master FX's prefix
     * contains a colon of its own ("master_fx:fx2"), and the old rule left
     * "fx2:sample_path" as the param name — a key no level lists, so clicking
     * an opaque param there landed on the module menu instead of the editor. */
    const paramPrefix = `${getComponentParamPrefix(componentKey)}:`;
    const raw = String(fullKey || "");
    const bare = raw.startsWith(paramPrefix) ? raw.slice(paramPrefix.length)
                                             : raw.replace(/^[^:]+:/, "");

    exitParamPages();
    /* Without this the list entry below sees Param View = Knobs and bounces
     * straight back into the grid, forever. The flag is consumed by the next
     * enterHierarchyEditorWith and nothing else. */
    suppressParamPagesOnce = true;
    enterHierarchyEditor(slotIndex, componentKey);

    /* Land on the level the grid was on, not the hierarchy root. */
    if (level && hierEditorHierarchy && hierEditorHierarchy.levels &&
        hierEditorHierarchy.levels[level] && level !== hierEditorLevel) {
        hierEditorLevel = level;
        hierEditorPath = [];
        hierEditorChildIndex = -1;
        loadHierarchyLevel();
    }

    /* Select the param that was clicked and open ITS editor. Without this the
     * user asked to open one parameter and got the module menu.
     *
     * The grid page level is NOT necessarily where the list editor keeps the
     * param. granny's root declares knobs:["position",...] but params:
     * ["level:main","level:scan_menu",...] — navigation entries only — so
     * `position` is on a root knob and in no root param list at all. It lives
     * in the `main` level. Searching only the page level found nothing and fell
     * through to the module menu, which is exactly the bug. So if the page
     * level does not list it, find the level that does and go there. */
    let idx = indexOfHierParam(bare);
    if (idx < 0) {
        const owner = findLevelListingParam(bare);
        if (owner && owner !== hierEditorLevel) {
            hierEditorLevel = owner;
            hierEditorPath = [];
            hierEditorChildIndex = -1;
            loadHierarchyLevel();
            idx = indexOfHierParam(bare);
        }
    }
    if (idx < 0) {
        /* The param is not on this level after all — leave the user in the
         * editor rather than nowhere, and say so instead of silently landing
         * somewhere unexplained. */
        announce((meta && meta.label ? meta.label : "Parameter") + ", opening in list");
        return;
    }
    hierEditorSelectedIdx = idx;
    const liveMeta = (typeof getParamMetadata === "function" ? getParamMetadata(bare) : null) || meta;
    announce((liveMeta && (liveMeta.name || liveMeta.label)) || bare);
    paramEditorOpenedFromGrid = true;
    openHierarchyParamEditor(bare, liveMeta, true);
    needsRedraw = true;
}

/* One-shot override forcing the LIST editor for the next COMPONENT entry, so
 * the grid can hand a param it cannot edit to the screen that can. */
let suppressParamPagesOnce = false;
/*
 * The same idea for SLOT settings, deliberately a SEPARATE flag.
 *
 * Sharing one meant a pending suppress set by a component hand-off leaked into
 * the next slot-settings entry — which then showed the list for no reason the
 * user could see — and that consuming it there stole it from the component
 * entry it had been set for. Two unrelated hand-offs, two flags.
 */
let suppressSlotGridOnce = false;
/* A slot-action modal is up in the LIST because the grid handed it over; go
 * back to the grid when it finishes. See maybeReturnToSlotGrid. */
let slotModalFromGrid = false;

/* ...and the Master FX pair. THREE flags, not one shared set, for the reason
 * spelled out above: a suppress pending for one hand-off must not be spent by
 * an unrelated entry, and Master FX settings is a third independent hand-off. */
let suppressMasterGridOnce = false;
let masterModalFromGrid = false;

/* ...and the Global Settings half. There is no suppress twin: Global Settings
 * has no list to be handed back to, so the only thing outstanding is "go back
 * to the page once the help stack closes". See maybeReturnToGlobalGrid. */
let globalModalFromGrid = false;

function saveParamViewConfig() {
    try {
        host_write_file(PARAM_VIEW_CONFIG_PATH, JSON.stringify({ param_view: paramViewGlobal }));
    } catch (e) {}
}

function loadParamViewConfig() {
    try {
        const content = host_read_file(PARAM_VIEW_CONFIG_PATH);
        if (!content) return;
        const cfg = JSON.parse(content);
        if (typeof cfg.param_view === "number") paramViewGlobal = cfg.param_view;
    } catch (e) {}
}

/* Tools menu state */
let toolsMenuIndex = 0;
let toolModules = [];           // Populated by scanForToolModules()

/* Filebrowser state */
let filebrowserEnabled = false;    // Off by default, toggle in Settings > Services

/* MIDI channel indicator state. Backed by shadow_control->midi_indicator_enabled
 * (read by the SPI callback path) and persisted in features.json via the
 * midi_indicator_set host binding. */
let midiIndicatorEnabled = (typeof midi_indicator_get === "function") ? !!midi_indicator_get() : false;

/* Preview player state */
let previewEnabled = true;         // Global setting: auto-preview in file browser
let previewPendingPath = "";       // Path waiting for debounce
let previewPendingTime = 0;        // Date.now() when path was set
const PREVIEW_DEBOUNCE_MS = 300;
const PREVIEW_EXTENSIONS = [".wav", ".aif", ".aiff"];

function isPreviewableFile(path) {
    if (!path) return false;
    const lower = path.toLowerCase();
    return PREVIEW_EXTENSIONS.some(ext => lower.endsWith(ext));
}

function previewTick() {
    if (!previewEnabled || !previewPendingPath || !previewPendingTime) return;
    if (Date.now() - previewPendingTime >= PREVIEW_DEBOUNCE_MS) {
        if (typeof host_preview_play === "function") {
            host_preview_play(previewPendingPath);
        }
        previewPendingPath = "";
        previewPendingTime = 0;
    }
}

function previewStopIfPlaying() {
    previewPendingPath = "";
    previewPendingTime = 0;
    if (typeof host_preview_stop === "function") {
        host_preview_stop();
    }
}

/* Tool file browser state (shared filepath_browser) */
let toolBrowserState = null;
let toolSelectedFile = "";
let toolActiveTool = null;      // Currently active tool module descriptor
let toolProcessPid = -1;        // PID of background tool process
let toolOutputDir = "";          // Output directory for current tool run
let toolResultMessage = "";      // Result message to display
let toolResultSuccess = false;   // Whether the tool succeeded
let toolProcessingDots = 0;      // Animation counter for processing view
let toolProcessStartTime = 0;    // Date.now() when process started
let toolFileDurationSec = 0;     // Duration of input file in seconds
let toolStemsFound = 0;          // Number of stem WAV files found in output dir
let toolExpectedStems = 4;       // Expected number of stems
let toolOvertakeActive = false;  // True if an interactive tool is running as overtake
let toolHiddenFile = "";         // File path of hidden tool session (for reconnect detection)
let toolHiddenModulePath = "";   // Module path of hidden tool session (survives other tool loads)
let toolNonOvertake = false;     // True if the active tool runs without overtake (pads still work)
let toolSelectedEngine = null;   // Selected engine from engines array
let toolEngineIndex = 0;         // Jog position in engine list
let toolAvailableEngines = [];   // Engines filtered by installed commands
let toolStemFiles = [];          // Array of stem .wav filenames in output dir
let toolStemReviewIndex = 0;     // Selected index (0 = "Save All", 1+ = individual stems)
let toolStemKept = [];           // Parallel bool array — true if stem is marked to keep
let toolSetList = [];            // Array of {uuid, name} for set picker
let toolSetPickerIndex = 0;      // Selected index in set picker
let toolSelectedSetUuid = "";    // Chosen set UUID
let toolSelectedSetName = "";    // Chosen set display name

/* WAV Player preview state */
let wavPlayerLoaded = false;
let wavPlayerPendingFile = "";  /* deferred file_path after DSP load */
let wavPlayerLoadWait = 0;      /* ticks to wait after loading DSP */
const WAV_PLAYER_DSP = "/data/UserData/schwung/modules/tools/wav-player/dsp.so";

/* Slot 0 overtake-DSP slot is single-tenant. Tracking what's currently loaded
 * lets resumeOvertakeModule detect "another tool clobbered my DSP" and reload
 * (Bug C, captured 2026-05-04). All overtake_dsp:load/unload sites must go
 * through these helpers so the tracker stays accurate. */
let currentSlot0DspPath = "";
function loadOvertakeDsp(path) {
    if (typeof shadow_set_param !== "function") return;
    shadow_set_param(0, "overtake_dsp:load", path);
    currentSlot0DspPath = path || "";
}
function unloadOvertakeDsp() {
    if (typeof shadow_set_param !== "function") return;
    shadow_set_param(0, "overtake_dsp:unload", "1");
    currentSlot0DspPath = "";
}

function loadWavPlayerDsp() {
    if (wavPlayerLoaded) return;
    loadOvertakeDsp(WAV_PLAYER_DSP);
    wavPlayerLoaded = true;
    wavPlayerLoadWait = 2; /* wait 2 ticks for C-side to process load */
}

function unloadWavPlayerDsp() {
    if (!wavPlayerLoaded) return;
    unloadOvertakeDsp();
    wavPlayerLoaded = false;
    wavPlayerPendingFile = "";
    wavPlayerLoadWait = 0;
}

function wavPlayerPlay(filePath) {
    loadWavPlayerDsp();
    if (wavPlayerLoadWait > 0) {
        /* DSP just loaded — defer file_path to next tick */
        wavPlayerPendingFile = filePath;
        return;
    }
    if (typeof shadow_set_param !== "function") return;
    shadow_set_param(0, "overtake_dsp:file_path", filePath);
}

/* Call from tick() to flush deferred file_path after DSP load */
function wavPlayerTick() {
    if (wavPlayerLoadWait > 0) {
        wavPlayerLoadWait--;
        if (wavPlayerLoadWait === 0 && wavPlayerPendingFile) {
            if (typeof shadow_set_param === "function") {
                shadow_set_param(0, "overtake_dsp:file_path", wavPlayerPendingFile);
            }
            wavPlayerPendingFile = "";
        }
    }
}

function wavPlayerStop() {
    if (!wavPlayerLoaded) return;
    if (typeof shadow_set_param !== "function") return;
    wavPlayerPendingFile = "";
    shadow_set_param(0, "overtake_dsp:playing", "0");
}

/* The labels and the stored values are the contract's now — options /
 * short_options and GLOBAL_ENUM_VALUES in shadow_ui_global_grid.mjs. Two copies
 * of [0, 2] is exactly how an index gets written as a mode. */

/* Check Move's system Link setting via shim param (reads Settings.json) */
function checkSystemLinkEnabled() {
    try {
        const val = shadow_get_param(0, "master_fx:system_link_enabled");
        systemLinkEnabled = (val === "1");
    } catch (e) { /* keep previous value */ }
}

/* Show warning overlay if a Link Audio setting is on but system Link is off.
 * Optional settingName focuses the message on what was just toggled. */
function warnIfLinkDisabled(settingName) {
    checkSystemLinkEnabled();
    if (systemLinkEnabled === false) {
        const routingOn = shadow_get_param(0, "master_fx:link_audio_routing") === "1";
        const publishOn = shadow_get_param(0, "master_fx:link_audio_publish") === "1";
        if (routingOn || publishOn) {
            const name = settingName || (routingOn ? "Move->Schwung" : "Schwung->Link");
            warningTitle = name;
            warningLines = wrapText("requires Link enabled in Move System Settings", 18);
            warningActive = true;
        }
    }
}

function parseResampleBridgeMode(raw) {
    if (raw === null || raw === undefined) return 0;
    const text = String(raw).trim().toLowerCase();
    if (text === "0" || text === "off") return 0;
    if (text === "2" || text === "overwrite" || text === "replace") return 2;
    if (text === "1" || text === "mix") return 2;  // Backward compatibility
    return 0;
}

/* Get dynamic settings items based on whether preset is loaded */
function getMasterFxSettingsItems() {
    if (currentMasterPresetName) {
        /* Existing preset: show all items */
        return MASTER_FX_SETTINGS_ITEMS_BASE;
    }
    /*
     * Unsaved: hide DELETE, but keep Save As.
     *
     * Save and Save As are genuinely different with nothing saved yet — Save
     * offers a generated name to accept or edit, Save As goes straight to the
     * keyboard. Hiding Save As left Master FX with a ONE ENTRY actions menu,
     * which the knob grid draws as a menu page you have to enter to press a
     * single button. A slot never hit that because it also carries Knob
     * Mapping; the master bus has no knob-mapping table, so it did.
     */
    return MASTER_FX_SETTINGS_ITEMS_BASE.filter(item => item.key !== "delete");
}

let selectedMasterFxSetting = 0;
let editingMasterFxSetting = false;
let inMasterFxSettingsMenu = false;  /* True when in settings submenu */

/* Help viewer state - stack-based for arbitrary depth */
let helpContent = null;
let helpNavStack = [];            /* [{ items, selectedIndex, title }] */
let helpDetailScrollState = null;


/* Return-view trackers for sub-flows */
let storeReturnView = null;   /* View to return to from store/update flows */
let helpReturnView = null;    /* View to return to from help viewer */

/* SLOT_SETTINGS imported from shadow_ui_slots.mjs */

/* patchDetail, editingComponent, componentParams, selectedParam,
 * editingValue moved to shadow_ui_patches.mjs */

/* Chain editing state */
let chainConfigs = [];         // In-memory chain configs per slot
// -1 = chain/patch; 0..n = an index into slotChainComponents(), whose length
// follows the chain rather than being fixed at five.
let selectedChainComponent = 0;
/*
 * Remember the last selected component per slot -- or null for "never chosen".
 *
 * NULL, not 0. Index 0 of a slot chain is the MIDI FX `+` box (the editor list
 * is add_midi, [midi fx...], synth, [fx...], add_fx, settings), so seeding
 * these with 0 meant every slot opened on "add a MIDI effect" in a fresh
 * session -- reported from the device as slots defaulting to MIDI FX. And 0 is
 * a VALID index, so restoreChainComponent accepted it and defaultChainComponent
 * (which returns the synth) never ran.
 *
 * null makes "no memory" unrepresentable as a position, which is what lets the
 * default apply.
 */
let lastChainComponent = [null, null, null, null];
let selectingModule = false;   // True when in module selection for a component
let availableModules = [];     // Modules available for selected component type
let selectedModuleIndex = 0;   // Index in availableModules

/* Store picker state */
let storeCatalog = null;               // Cached catalog from store_utils
let storeInstalledModules = {};        // {moduleId: version} map
let storeHostVersion = '1.0.0';        // Current host version
let storePickerCategory = null;        // Category ID being browsed (sound_generator, audio_fx, midi_fx)
let storePickerModules = [];           // Modules available for download in current category
let storePickerCurrentModule = null;   // Module being viewed in detail
let storePickerActionIndex = 0;        // Selected action in detail view (0=Install/Update, 1=Remove)
let storePickerMessage = '';           // Result/error message
let storePickerResultTitle = '';       // Result screen header (empty = 'Module Store')
let storePickerFromOvertake = false;   // True if entered from overtake menu
let storePickerFromMasterFx = false;  // True if entered from master FX module select
let storePickerFromSettings = false;  // True if entered from MFX settings (full store)

/* Run detection + show a result screen listing what's outdated, with the
 * web-manager pointer. No install actions — those only happen via the
 * web manager. checkForUpdatesInBackground populates pendingUpdates as
 * a side effect; we read length, then never touch the install flow. */
function showUpdatesAvailableScreen() {
    announce("Checking for updates");
    checkForUpdatesInBackground();

    /* Distinguish host pointer from module updates so the user knows
     * what's actually outdated. checkForUpdatesInBackground pushes a
     * single _hostPointer entry to the queue when the catalog has a
     * newer host; everything else is a module update. */
    let hostNewer = '';
    let moduleCount = 0;
    for (let i = 0; i < pendingUpdates.length; i++) {
        const upd = pendingUpdates[i];
        if (upd._hostPointer) {
            hostNewer = upd.to || '';
        } else {
            moduleCount++;
        }
    }

    /* Reset detection state so a second visit starts clean.
     * here a second time without clearing state. */
    pendingUpdates = [];
    pendingUpdateIndex = 0;
    /* Route the result-screen click back to GLOBAL_SETTINGS (not the
     * old store browser fallback). */
    storePickerFromSettings = false;
    storeReturnView = VIEWS.GLOBAL_SETTINGS;

    storePickerResultTitle = 'Updates';
    if (!hostNewer && moduleCount === 0) {
        storePickerMessage = buildNoUpdatesMessage();
    } else {
        const lines = [];
        if (hostNewer) {
            lines.push('Schwung ' + hostNewer + ' available');
        }
        if (moduleCount > 0) {
            lines.push(moduleCount + ' module update' + (moduleCount === 1 ? '' : 's'));
        }
        lines.push('Update at');
        lines.push('http://move.local:7700');
        storePickerMessage = lines.join('\n');
    }
    view = VIEWS.STORE_PICKER_RESULT;
    needsRedraw = true;
    announce(storePickerMessage);
}

/* Detect whether the live entrypoint at /opt/move/Move includes the
 * boot-time `schwung-heal` invocation. If not, the self-heal mechanism
 * isn't running on this device, /usr/lib/schwung-shim.so will silently
 * drift out of sync with /data after web-manager updates, and the user
 * needs the one-time bootstrap (web manager, GUI installer, or SSH).
 *
 * Reads the entrypoint via std.loadFile (it's a ~4KB shell script).
 * Returns true when the bootstrap is missing, false when it's present
 * or the file can't be read (in which case we don't want to nag). */
function detectShimBootstrapNeeded() {
    try {
        const entry = std.loadFile('/opt/move/Move');
        if (!entry) return false;
        return entry.indexOf('schwung-heal') < 0;
    } catch (_e) {
        return false;
    }
}

/* Pointer to the web manager — same routing semantics as the updates
 * screen: result-click returns to GLOBAL_SETTINGS via storeReturnView. */
function showModuleStorePointer() {
    storePickerFromSettings = false;
    storeReturnView = VIEWS.GLOBAL_SETTINGS;
    storePickerResultTitle = 'Module Store';
    storePickerMessage = 'Module store available at\nhttp://move.local:7700';
    view = VIEWS.STORE_PICKER_RESULT;
    needsRedraw = true;
    announce(storePickerMessage);
}

/* Return a no-updates message that surfaces "host updates live in the
 * web manager" when the catalog says a newer host is available. Hiding
 * the on-device action without telling users where to update otherwise
 * leaves them silently stuck on old versions. */
function buildNoUpdatesMessage() {
    try {
        if (storeCatalog && storeCatalog.host && storeCatalog.host.latest_version) {
            const cur = storeHostVersion || getHostVersion();
            if (isNewerVersion(storeCatalog.host.latest_version, cur)) {
                return 'Schwung ' + storeCatalog.host.latest_version + ' available\n' +
                       'Update Schwung at\n' +
                       'http://move.local:7700';
            }
        }
    } catch (_e) { /* fall through */ }
    return 'No updates available';
}

/* Check for core and module updates (manual, called from Settings → Check Updates) */
function checkForUpdatesInBackground() {
    debugLog("checkForUpdatesInBackground: starting");
    const updates = [];

    clear_screen();
    drawStatusOverlay('Updates', 'Checking...');
    host_flush_display();

    /* Refresh host version (still needed for module compatibility checks).
     * Core updates are no longer offered on-device — users update via the
     * web manager (move.local:7700). The check is suppressed so the
     * "Update All" flow doesn't try to perform a core upgrade that the
     * JS layer can't complete with the right privileges. */
    storeHostVersion = getHostVersion();
    debugLog("checkForUpdatesInBackground: hostVersion=" + storeHostVersion);

    /* Check module updates */
    clear_screen();
    drawStatusOverlay('Updates', 'Checking modules...');
    host_flush_display();

    debugLog("checkForUpdatesInBackground: checking modules");
    const installed = scanInstalledModules();
    const catalogResult = fetchCatalog((title, name, idx, total) => {
        drawStatusOverlay('Checking', idx + '/' + total + ': ' + name);
        host_flush_display();
    });
    debugLog("checkForUpdatesInBackground: catalog success=" + catalogResult.success);
    if (catalogResult.success) {
        /* Cache catalog so buildNoUpdatesMessage can read host.latest_version
         * when there are no module updates — otherwise users hitting Check
         * for Updates have no signal that a host upgrade is available. */
        storeCatalog = catalogResult.catalog;
        /* Surface a non-actionable "Schwung X.Y.Z available" pointer at the
         * top of the update prompt when the catalog has a newer host. We
         * can't perform host upgrades on-device (privileged paths blocked
         * for ableton), so this is informational — selecting it shows the
         * web manager pointer message and Update All skips it entirely. */
        const cat = catalogResult.catalog;
        if (cat && cat.host && cat.host.latest_version &&
            isNewerVersion(cat.host.latest_version, storeHostVersion)) {
            updates.push({
                id: '__host_pointer__',
                name: 'Schwung ' + cat.host.latest_version,
                from: storeHostVersion,
                to: cat.host.latest_version,
                _hostPointer: true
            });
        }
        for (const mod of catalogResult.catalog.modules || []) {
            const status = getModuleStatus(mod, installed);
            if (status.installed && status.hasUpdate) {
                updates.push({
                    name: mod.name,
                    from: status.installedVersion,
                    to: mod.latest_version,
                    ...mod
                });
            }
        }
    }

    debugLog("checkForUpdatesInBackground: found " + updates.length + " updates");
    if (updates.length > 0) {
        /* Detection only — the caller summarizes; installs happen in the
         * web manager (the single install/update path). */
        pendingUpdates = updates;
        pendingUpdateIndex = 0;
        needsRedraw = true;
    } else {
        needsRedraw = true;
    }
}

/* Chain settings (shown when Settings component is selected) */
const CHAIN_SETTINGS_ITEMS = [
    { key: "knobs", label: "Knobs", type: "action" },  // Opens knob assignment editor
    /* 2.0 is +6 dB. It was 4.0 (+12 dB), which is more headroom than a slot
     * has any use for and reads as an alarming 400% now that the knob grid
     * shows it as a percentage. Capped in BOTH places or the two surfaces
     * disagree — see SLOT_GRID_PARAMS. An existing slot saved above 2.0 keeps
     * its stored gain until something turns the knob, which then pulls it into
     * range. */
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 2, step: 0.05 },
    { key: "slot:muted", label: "Muted", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:soloed", label: "Soloed", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 0, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -2, max: 15, step: 1 },  // -2 = passthrough, -1 = auto, 0-15 = ch 1-16
    { key: "slot:transpose", label: "Transpose", type: "int", min: -12, max: 12, step: 1 },
    { key: "midi_fx_pre_mode", label: "MIDI FX", type: "int", min: 0, max: 1, step: 1 },  // 0 = Post (slot synth only), 1 = Pre (also inject to Move native)
    { key: "mpe_mode", label: "MPE Mode", type: "int", min: 0, max: 1, step: 1 },
    { key: "lfo1", label: "LFO 1", type: "action" },
    { key: "lfo2", label: "LFO 2", type: "action" },
    { key: "save", label: "[Save]", type: "action" },  // Save slot preset (overwrite for existing)
    { key: "save_as", label: "[Save As]", type: "action" },  // Save as new preset
    { key: "delete", label: "[Delete]", type: "action" }  // Delete slot preset
];
let selectedChainSetting = 0;
let editingChainSettingValue = false;

/* LFO editor state — generic context drives slot or MFX LFO */
let lfoCtx = null;  /* Active LFO context: { lfoIdx, getParam, setParam, getTargetComponents, getTargetParams, title, returnView, returnAnnounce } */
let selectedLfoItem = 0;
let editingLfoValue = false;

const LFO_SHAPES = ["Sine", "Tri", "Saw", "Square", "S&H", "Swishy"];
const LFO_DIVISIONS = [
    "16bar", "15bar", "14bar", "13bar", "12bar", "11bar", "10bar", "9bar",
    "8bar", "7bar", "6bar", "5bar", "4bar", "3bar", "2bar",
    "1/1", "1/1T", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T",
    "1/16", "1/16T", "1/32", "1/32T"
];

/* Migration: old 14-entry division table index -> new 27-entry index.
 * Dotted divisions (1/4D, 1/8D) were removed; map to straight equivalent. */
const LFO_MIGRATE_14_TO_27 = [8, 12, 14, 15, 17, 19, 21, 23, 25, 20, 22, 24, 19, 21];

function migrateLfoDivisionIndex(idx, lfoConfig) {
    if (lfoConfig.division_table_version) return idx; /* already new format */
    if (idx >= 0 && idx < 14) return LFO_MIGRATE_14_TO_27[idx];
    return idx;
}

/* Restore a master FX LFO from a saved config object.
 * lfoIndex: 1 or 2, lfoConfig: parsed JSON config for this LFO. */
function restoreMasterFxLfo(lfoIndex, lfoConfig) {
    const pfx = "master_fx:lfo" + lfoIndex + ":";
    if (lfoConfig.target) shadow_set_param(0, pfx + "target", lfoConfig.target);
    if (lfoConfig.target_param) shadow_set_param(0, pfx + "target_param", lfoConfig.target_param);
    shadow_set_param(0, pfx + "shape", String(lfoConfig.shape || 0));
    shadow_set_param(0, pfx + "polarity", String(lfoConfig.polarity || 0));
    shadow_set_param(0, pfx + "sync", String(lfoConfig.sync || 0));
    shadow_set_param(0, pfx + "rate_hz", String(lfoConfig.rate_hz || 1.0));
    let rateDiv = Number.isFinite(Number(lfoConfig.rate_div))
        ? Number(lfoConfig.rate_div)
        : 15;
    rateDiv = migrateLfoDivisionIndex(rateDiv, lfoConfig);
    shadow_set_param(0, pfx + "rate_div", String(rateDiv));
    shadow_set_param(0, pfx + "depth", String(lfoConfig.depth || 0));
    shadow_set_param(0, pfx + "phase_offset", String(lfoConfig.phase_offset || 0));
    shadow_set_param(0, pfx + "enabled", String(lfoConfig.enabled || 0));
}

/* LFO target picker state */
let lfoTargetComponents = [];  /* Available components [{key, label}] */
let selectedLfoTargetComp = 0;
let lfoTargetParams = [];      /* Params for selected component [{key, label}] */
let selectedLfoTargetParam = 0;

/* Slot preset save state */
let pendingSaveName = "";
let overwriteTargetIndex = -1;
let confirmingOverwrite = false;
let confirmingDelete = false;
let confirmIndex = 0;
let overwriteFromKeyboard = false;  /* true if overwrite came from keyboard entry, false if from direct Save */
let showingNamePreview = false;     /* true when showing name preview with Edit/OK */
let namePreviewIndex = 0;           /* 0 = Edit, 1 = OK */

/* Master preset state */
let masterPresets = [];              // List of {name, index} from /presets_master/
let selectedMasterPresetIndex = 0;   // Index in picker (0 = [New])
let currentMasterPresetName = "";    // Name of loaded preset ("" if new/unsaved)
let inMasterPresetPicker = false;    // True when showing preset picker

/* Cached settings — written during save instead of reading from shim,
 * to avoid a race where the periodic autosave reads shim defaults before
 * loadMasterFxChainFromConfig() has restored the correct values. */
let cachedResampleBridgeMode = 0;
let cachedLinkAudioRouting = false;
let cachedLinkAudioPublish = false;
let cachedLatencyCompEnabled = false;
/* Default true: restoring Move's USB-C audio-out source is on unless the
 * user turns it off. Mirrors usbc_out_persist_enabled in the shim. */
let cachedUsbcOutPersist = true;
/* Default -1 (All): Master FX heard every channel before this setting
 * existed, so anything else here is a silent regression for every sidechain
 * already in the field. Mirrors master_fx_midi_channel in the shim. */
let cachedMasterFxMidiChannel = -1;
let systemLinkEnabled = null; /* null = not checked yet */

/* Master preset CRUD state (reuse pattern from slot presets) */
let masterPendingSaveName = "";
let masterOverwriteTargetIndex = -1;
let masterConfirmingOverwrite = false;
let masterConfirmingDelete = false;
let masterConfirmIndex = 0;
let masterOverwriteFromKeyboard = false;
let masterShowingNamePreview = false;
let masterNamePreviewIndex = 0;

/* Shift state - read from shim via shadow_get_shift_held() */
function isShiftHeld() {
    if (typeof shadow_get_shift_held === "function") {
        return shadow_get_shift_held() !== 0;
    }
    return false;
}

/* Component edit state (for Shift+Click editing) */
let editingComponentKey = "";    // a component key: "synth", "midiFx", or "fxN"
let editComponentPresetCount = 0;
let editComponentPreset = 0;
let editComponentPresetName = "";

/* Hierarchy editor state */
let hierEditorSlot = -1;
let hierEditorComponent = "";
let hierEditorHierarchy = null;
let hierEditorLevel = "root";
let hierEditorPath = [];          // breadcrumb path
let hierEditorChildIndex = -1;    // selected child index for child_prefix levels
let hierEditorChildCount = 0;     // number of child entries for child_prefix levels
let hierEditorChildLabel = "";    // label for child entries (e.g., "Tone")
let hierEditorParams = [];        // current level's params
let hierEditorKnobs = [];         // current level's knob-mapped params
let hierEditorAllParams = [];     // unfiltered current level params
let hierEditorAllKnobs = [];      // unfiltered current level knobs
let hierEditorSelectedIdx = 0;
let hierEditorEditMode = false;   // true when editing a param value
let hierEditorEditKey = "";       // full key currently being edited
let hierEditorEditValue = null;   // stable value during edit mode
let hierEditorChainParams = [];   // metadata from chain_params

/* wav_position view-local zoom state.
 * Storage keyed by:
 *   - view_group: "group:<slot>::<view_group>"  → shared by all sibling markers
 *   - else fullKey
 * Range: zoom 0..8 = 1× .. 256× viewport width. Pan auto-tracks the
 * active marker value (no separate pan field needed).
 * Cleared on hierarchy editor exit. */
const wavPositionZoomStates = new Map();
function getWavZoomStorageKey(slot, meta, fullKey) {
    if (meta && meta.view_group) return `group:${slot}::${meta.view_group}`;
    return fullKey;
}
function getWavZoomLevel(slot, meta, fullKey) {
    const st = wavPositionZoomStates.get(getWavZoomStorageKey(slot, meta, fullKey));
    return st ? st.zoom : 0;
}
function setWavZoomLevel(slot, meta, fullKey, zoom) {
    const k = getWavZoomStorageKey(slot, meta, fullKey);
    let z = Number(zoom);
    if (!Number.isFinite(z) || z < 0) z = 0;
    if (z > 8) z = 8;
    if (z <= 0.0001) {
        wavPositionZoomStates.delete(k);
        return 0;
    }
    wavPositionZoomStates.set(k, { zoom: z });
    return z;
}
/* Knob-engine accumulator state for the multi-marker zoom override (knob 8). */
const wavZoomKnobStates = new Map();
function getWavZoomKnobState(groupKey, currentZoom) {
    let st = wavZoomKnobStates.get(groupKey);
    if (!st) {
        st = knobInit(currentZoom);
        wavZoomKnobStates.set(groupKey, st);
    } else {
        st.value = currentZoom;
    }
    return st;
}
function clearWavZoomStates() {
    wavPositionZoomStates.clear();
    wavZoomKnobStates.clear();
}

/* Collect wav_position params in the current hierarchy level that declare
 * the given view_group. Returns ordered list of {key, fullKey, meta},
 * ordered as they appear in hierEditorParams (declaration order). */
function getWavViewGroupMembers(viewGroup) {
    if (!viewGroup) return [];
    const out = [];
    const seen = new Set();
    for (const p of hierEditorParams) {
        const key = (typeof p === "string") ? p : (p && p.key ? p.key : null);
        if (!key || seen.has(key)) continue;
        const meta = getParamMetadata(key);
        if (!meta || meta.ui_type !== "wav_position") continue;
        if (meta.view_group !== viewGroup) continue;
        seen.add(key);
        out.push({ key, fullKey: buildHierarchyParamKey(key), meta });
    }
    return out;
}

/* True when the user is currently inside a wav_position fullscreen editor. */
function isInWavPositionEditor() {
    if (view !== VIEWS.HIERARCHY_EDITOR) return false;
    if (!hierEditorEditMode) return false;
    const sel = getSelectedHierarchyEditableKey();
    if (!sel) return false;
    const meta = getParamMetadata(sel);
    return !!(meta && meta.ui_type === "wav_position");
}

/* Per-knob role while inside a wav_position editor (single or multi marker).
 *   { type: "zoom",   ... }    knobIndex === 7  → always the zoom knob
 *   { type: "marker", member } knobIndex < N    → group member (multi only)
 *   { type: "silent" }         multi-marker, other knob → swallowed
 *   null                       → not in editor, or single-marker non-zoom knob
 *                                (let normal flow handle it, with enum silencing). */
function getMultiMarkerKnobRole(knobIndex) {
    if (!isInWavPositionEditor()) return null;
    const sel = getSelectedHierarchyEditableKey();
    const selMeta = getParamMetadata(sel);
    const selFullKey = buildHierarchyParamKey(sel);
    const group = selMeta.view_group ? getWavViewGroupMembers(selMeta.view_group) : [];
    const isMulti = group.length > 1;

    /* Knob 8 zoom override is opt-in via enable_zoom. Modules without it
     * (MrDrums pad_start, REX slice points, etc.) keep their declared
     * knob 8 mapping. */
    if (knobIndex === 7 && selMeta.enable_zoom) {
        const anchor = isMulti
            ? group[0]
            : { key: sel, fullKey: selFullKey, meta: selMeta };
        return { type: "zoom", group, anchor };
    }
    /* Multi-marker knob remap is opt-in via view_group. */
    if (isMulti) {
        if (knobIndex >= 0 && knobIndex < group.length) {
            return { type: "marker", member: group[knobIndex], group };
        }
        return { type: "silent", group };
    }
    return null;
}

/* Set the multi-marker view's active marker to the given group member by
 * retargeting hierEditor selection state. */
function selectActiveWavMarker(member) {
    if (!member || !member.key) return;
    const idx = hierEditorParams.findIndex((p) => {
        const k = (typeof p === "string") ? p : (p && p.key ? p.key : null);
        return k === member.key;
    });
    if (idx < 0) return;
    if (hierEditorSelectedIdx === idx) return;
    hierEditorSelectedIdx = idx;
    hierEditorEditKey = member.fullKey;
    hierEditorEditValue = null;  /* force renderer to re-read the new marker's value */
    needsRedraw = true;
}

/* Knob state per fullKey for acceleration continuity across consecutive jog turns. */
const hierKnobStates = new Map();
function clearHierKnobStates() { hierKnobStates.clear(); }

/* Knob state per fullKey for the PHYSICAL knobs 1-8 (separate from jog edit mode). */
const physKnobStates = new Map();
function getPhysKnobState(fullKey, currentValue) {
    let st = physKnobStates.get(fullKey);
    if (!st) {
        st = knobInit(currentValue);
        physKnobStates.set(fullKey, st);
    } else {
        st.value = currentValue;
    }
    return st;
}
/* Master FX flag - when true, exit returns to MASTER_FX view instead of CHAIN_EDIT */
let hierEditorIsMasterFx = false;
let hierEditorMasterFxSlot = -1;      // Which Master FX slot (0..MASTER_FX_SLOTS-1) we're editing

/* Set by enterHierarchyEditorFromParamPages(): the list editor is only open
 * here because the grid handed off a non-grid page (preset browser, items
 * list, ...) it does not draw itself. Committing a preset in that state
 * should return to the grid rather than leave the user stranded in the list
 * UI — see the preset-edit-mode branch below. Cleared once consumed, and by
 * exitHierarchyEditor()/Back so a manual exit does not also bounce back. */
let cameFromParamPages = false;
/*
 * Set when the knob grid opened ONE param directly (openParamEditorFromGrid).
 * Back has to undo the thing the user actually did: they were on the grid, they
 * clicked a bracketed cell, so closing that editor belongs back on the grid.
 * Without it Back exits edit mode into the hierarchy LIST — a screen they never
 * opened and, with Param View = Knobs, cannot get out of except by leaving the
 * component. Distinct from cameFromParamPages, which is about the grid handing
 * off a whole PAGE (a preset browser) rather than a single param.
 */
let paramEditorOpenedFromGrid = false;
/* The grid page that was on screen when the hand-off happened, by NAME. Coming
 * back to page 1 after editing something on page 5 is its own small betrayal. */
let paramEditorReturnPage = "";
/*
 * True while the LFO target picker was opened from the knob grid.
 *
 * The picker returns to VIEWS.LFO_EDIT from three places — Back, clearing the
 * target, and committing one — because that is where the LIST enters it from.
 * Entered from a grid cell, all three land on a screen the user never opened.
 * The grid controller is left alive across the hand-off, so returning is just a
 * setView and the page position survives untouched.
 */
let lfoTargetFromGrid = false;

/* Preset browser state (for preset_browser type levels) */
let hierEditorIsPresetLevel = false;  // true when current level is a preset browser
let hierEditorPresetCount = 0;
let hierEditorPresetIndex = 0;
let hierEditorPresetName = "";
let hierEditorPresetEditMode = false; // true when editing params within a preset browser level

/* Dynamic items level state (for items_param type levels) */
let hierEditorIsDynamicItems = false; // true when current level uses items_param
let hierEditorSelectParam = "";       // param to set when item selected
let hierEditorNavigateTo = "";        // level to navigate to after item selection (optional)

/* Filepath browser state (for chain_params type: filepath) */
let filepathBrowserState = null;
let filepathBrowserParamKey = "";
let canvasParamKey = "";
let canvasParamMeta = null;
let canvasRuntime = null;
let canvasTickCounter = 0;
const FILEPATH_BROWSER_FS = {
    readdir(path) {
        const entries = os.readdir(path) || [];
        if (Array.isArray(entries[0])) return entries[0];
        return Array.isArray(entries) ? entries : [];
    },
    stat(path) {
        return os.stat(path);
    }
};

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const RATE_BASE_DENOMS = [2, 4, 8, 16, 32, 64];
const RATE_TRIPLET_BASE_DENOMS = [1, 2, 4, 8, 16, 32];
const RATE_BARS_SIMPLE = [16, 8, 4, 2, 1];
const RATE_BARS_EVERY = [...Array(16)].map((_, i) => 16 - i);
const WAV_PREVIEW_W = 120;
const WAV_PREVIEW_H = 7;
let wavDurationCache = {};
let wavPositionWaveformCache = { signature: "", path: "", points: [], error: "" };
let wavPositionWaveformErrorSignature = "";

function parseMetaBool(value) {
    if (value === true || value === 1) return true;
    if (value === false || value === 0 || value === null || value === undefined) return false;
    const v = String(value).trim().toLowerCase();
    return v === "1" || v === "true" || v === "on" || v === "yes";
}

function parseMetaNumber(value, fallback) {
    const num = Number(value);
    return Number.isFinite(num) ? num : fallback;
}

function getMetaOption(meta, key, fallback) {
    if (!meta || typeof meta !== "object") return fallback;
    if (meta[key] !== undefined) return meta[key];
    if (meta.options && !Array.isArray(meta.options) &&
        typeof meta.options === "object" && meta.options[key] !== undefined) {
        return meta.options[key];
    }
    return fallback;
}

function normalizeParamType(value) {
    return String(value || "").toLowerCase();
}

function buildNoteParamMeta(meta) {
    const mode = String(getMetaOption(meta, "mode", "multi")).toLowerCase() === "single" ? "single" : "multi";
    const defaultMin = mode === "single" ? 0 : 0;
    const defaultMax = mode === "single" ? 11 : 127;
    let minNote = Math.max(0, Math.floor(parseMetaNumber(getMetaOption(meta, "min_note", meta.min), defaultMin)));
    let maxNote = Math.min(127, Math.floor(parseMetaNumber(getMetaOption(meta, "max_note", meta.max), defaultMax)));
    if (maxNote < minNote) {
        const tmp = minNote;
        minNote = maxNote;
        maxNote = tmp;
    }

    const options = [];
    const optionLabels = {};
    for (let note = minNote; note <= maxNote; note++) {
        const noteName = NOTE_NAMES[((note % 12) + 12) % 12];
        const octave = Math.floor(note / 12) - 1;
        const raw = String(note);
        const label = mode === "single" ? noteName : `${noteName}${octave}`;
        options.push(raw);
        optionLabels[raw] = label;
    }

    return {
        ...meta,
        type: "enum",
        options: options.length > 0 ? options : ["0"],
        option_labels: optionLabels,
        expanded_type: "note",
        mode
    };
}

function buildRateParamMeta(meta) {
    const includeBars = parseMetaBool(getMetaOption(meta, "include_bars", true));
    const includeTriplets = parseMetaBool(getMetaOption(meta, "include_triplets", true));
    const rawBarsMode = String(getMetaOption(meta, "bars_mode", "bars-every")).toLowerCase();
    let barsMode = rawBarsMode;
    if (barsMode === "pow2") barsMode = "bars-simple";   /* legacy alias */
    if (barsMode === "all") barsMode = "bars-every";     /* legacy alias */
    if (barsMode !== "bars-simple" && barsMode !== "bars-every") {
        barsMode = "bars-every";
    }

    const options = [];
    const pushRate = (val) => {
        if (options.indexOf(val) < 0) options.push(val);
    };

    if (includeBars) {
        const bars = barsMode === "bars-every" ? RATE_BARS_EVERY : RATE_BARS_SIMPLE;
        for (const count of bars) {
            /* 1 bar only appears from bar options, not base rate divisions. */
            if (count <= 1) pushRate("1 bar");
            else pushRate(`${count} bars`);
        }
    }

    if (includeTriplets && RATE_TRIPLET_BASE_DENOMS.indexOf(1) >= 0) {
        pushRate("1/1T");
    }

    for (const denom of RATE_BASE_DENOMS) {
        pushRate(`1/${denom}`);
        if (includeTriplets && RATE_TRIPLET_BASE_DENOMS.indexOf(denom) >= 0) {
            pushRate(`1/${denom}T`);
        }
    }

    if (options.length === 0) {
        options.push("1/4");
    }

    return {
        ...meta,
        type: "enum",
        options,
        bars_mode: barsMode,
        expanded_type: "rate"
    };
}

function buildWavPositionParamMeta(meta) {
    const displayUnitRaw = String(getMetaOption(meta, "display_unit", "percent")).toLowerCase();
    const displayUnit = (displayUnitRaw === "ms" || displayUnitRaw === "sec" || displayUnitRaw === "s")
        ? displayUnitRaw : "percent";
    const modeRaw = String(getMetaOption(meta, "mode", "position")).toLowerCase();
    const mode = (modeRaw === "trim_front" || modeRaw === "start")
        ? "start"
        : ((modeRaw === "trim_end" || modeRaw === "end") ? "end" : "position");
    const defaultMax = 1;
    const min = parseMetaNumber(getMetaOption(meta, "min", 0), 0);
    const max = parseMetaNumber(getMetaOption(meta, "max", defaultMax), defaultMax);
    const defaultStep = displayUnit === "ms" ? 1 : 0.01;
    const step = parseMetaNumber(getMetaOption(meta, "step", defaultStep), defaultStep);
    const shiftMultiplierRaw = parseMetaNumber(
        getMetaOption(
            meta,
            "shift_increment_multiplier",
            getMetaOption(meta, "shift_step_multiplier", 0.1)
        ),
        0.1
    );
    const shiftMultiplier = (Number.isFinite(shiftMultiplierRaw) && shiftMultiplierRaw > 0)
        ? shiftMultiplierRaw
        : 0.1;
    const filepathParam = String(getMetaOption(meta, "filepath_param", "") || "");
    const enableZoom = parseMetaBool(getMetaOption(meta, "enable_zoom", false));
    const viewGroup = String(getMetaOption(meta, "view_group", "") || "");
    const markerLabel = String(getMetaOption(meta, "marker_label", "") || "");
    return {
        ...meta,
        type: "float",
        min,
        max,
        step,
        shift_increment_multiplier: shiftMultiplier,
        ui_type: "wav_position",
        display_unit: displayUnit,
        wav_mode: mode,
        filepath_param: filepathParam,
        enable_zoom: enableZoom,
        view_group: viewGroup,
        marker_label: markerLabel,
        expanded_type: "wav_position"
    };
}

function buildCanvasParamMeta(meta) {
    const displayTypeRaw = String(getMetaOption(meta, "display_value_type", "string")).toLowerCase();
    const displayValueType = (displayTypeRaw === "percent" || displayTypeRaw === "float" ||
        displayTypeRaw === "int") ? displayTypeRaw : "string";
    const showFooter = parseMetaBool(
        getMetaOption(meta, "show_footer", getMetaOption(meta, "showfooter", true))
    );
    const showValue = parseMetaBool(
        getMetaOption(meta, "show_value", getMetaOption(meta, "showvalue", true))
    );
    return {
        ...meta,
        type: "canvas",
        display_value_type: displayValueType,
        show_footer: showFooter,
        show_value: showValue,
        expanded_type: "canvas"
    };
}

function normalizeExpandedParamMeta(key, meta) {
    const resolved = getDynamicPickerMeta(key, meta);
    if (!resolved || typeof resolved !== "object") return resolved;
    const type = normalizeParamType(resolved.type);
    if (type === "note") return buildNoteParamMeta(resolved);
    if (type === "rate") return buildRateParamMeta(resolved);
    if (type === "wav_position") return buildWavPositionParamMeta(resolved);
    if (type === "canvas") return buildCanvasParamMeta(resolved);
    return resolved;
}

function formatMetaOptionValue(meta, rawValue) {
    if (!meta) return rawValue;
    const lookup = String(rawValue);
    /* Try option_labels map first (key → display label) */
    if (meta.option_labels && typeof meta.option_labels === "object") {
        if (Object.prototype.hasOwnProperty.call(meta.option_labels, lookup)) {
            return meta.option_labels[lookup];
        }
    }
    if (Array.isArray(meta.options)) {
        /* If the plugin returned an option label as-is (e.g. "1/8"), keep it.
         * parseInt would otherwise stop at the first non-digit and resolve
         * fraction strings like "1/4" / "1/8" / "1/16" all to index 1. */
        if (meta.options.indexOf(lookup) >= 0) {
            return lookup;
        }
        /* Plugin returned a numeric index like "7". Use Number() rather than
         * parseInt() so trailing non-numeric characters disqualify the value. */
        const num = Number(lookup);
        if (Number.isFinite(num)) {
            const idx = Math.round(num);
            if (idx >= 0 && idx < meta.options.length) {
                return meta.options[idx];
            }
        }
    }
    return rawValue;
}

function formatWavPositionDisplayValue(rawValue, meta) {
    const num = Number(rawValue);
    if (!Number.isFinite(num)) return rawValue;
    const unit = String(meta && meta.display_unit || "percent").toLowerCase();
    if (unit === "ms") return `${Math.round(num)} ms`;
    if (unit === "sec" || unit === "s") return `${num.toFixed(3)} s`;

    const min = parseMetaNumber(meta && meta.min, 0);
    const max = parseMetaNumber(meta && meta.max, 1);
    const span = max - min;
    if (span <= 0) return "0%";
    const pct = Math.max(0, Math.min(100, Math.round(((num - min) / span) * 100)));
    return `${pct}%`;
}

function formatCanvasDisplayValue(rawValue, meta) {
    const mode = String(meta && meta.display_value_type || "string").toLowerCase();
    if (mode === "string") return String(rawValue || "");
    const num = Number(rawValue);
    if (!Number.isFinite(num)) return String(rawValue || "");
    if (mode === "int") return String(Math.round(num));
    if (mode === "percent") {
        const min = parseMetaNumber(meta && meta.min, 0);
        const max = parseMetaNumber(meta && meta.max, 1);
        const span = max - min;
        if (span <= 0) return "0%";
        return `${Math.max(0, Math.min(100, Math.round(((num - min) / span) * 100)))}%`;
    }
    return num.toFixed(2);
}

function normalizeVisibilityConditionKey(componentPrefix, levelDef, childIndex, rawKey) {
    if (!rawKey) return "";
    if (rawKey.includes(":")) return rawKey;
    if (!componentPrefix) return rawKey;
    if (levelDef && levelDef.child_prefix && childIndex >= 0) {
        if (rawKey.startsWith(levelDef.child_prefix)) {
            return `${componentPrefix}:${rawKey}`;
        }
        return `${componentPrefix}:${levelDef.child_prefix}${childIndex}_${rawKey}`;
    }
    return `${componentPrefix}:${rawKey}`;
}

function compareConditionValue(actualRaw, expectedRaw) {
    if (typeof expectedRaw === "boolean") {
        return parseMetaBool(actualRaw) === expectedRaw;
    }
    if (typeof expectedRaw === "number") {
        const num = Number(actualRaw);
        return Number.isFinite(num) && num === expectedRaw;
    }
    return String(actualRaw) === String(expectedRaw);
}

function evaluateVisibilityConditionForContext(slot, componentPrefix, condition, levelDef, childIndex) {
    if (!condition || typeof condition !== "object") return true;
    const conditionParam = condition.param || condition.key || condition.param_key;
    if (!conditionParam) return true;

    const fullKey = normalizeVisibilityConditionKey(componentPrefix, levelDef, childIndex, String(conditionParam));
    const rawValue = getSlotParam(slot, fullKey);
    if (rawValue === null || rawValue === undefined) return true; // fail-open

    if (condition.equals !== undefined) {
        return compareConditionValue(rawValue, condition.equals);
    }
    if (condition.not_equals !== undefined) {
        return !compareConditionValue(rawValue, condition.not_equals);
    }
    if (condition.gt !== undefined || condition.greater_than !== undefined || condition.greater !== undefined) {
        const threshold = parseMetaNumber(
            condition.gt !== undefined ? condition.gt :
                (condition.greater_than !== undefined ? condition.greater_than : condition.greater),
            null
        );
        const current = Number(rawValue);
        return Number.isFinite(current) && Number.isFinite(threshold) && current > threshold;
    }
    if (condition.lt !== undefined || condition.smaller_than !== undefined || condition.smaller !== undefined) {
        const threshold = parseMetaNumber(
            condition.lt !== undefined ? condition.lt :
                (condition.smaller_than !== undefined ? condition.smaller_than : condition.smaller),
            null
        );
        const current = Number(rawValue);
        return Number.isFinite(current) && Number.isFinite(threshold) && current < threshold;
    }
    if (condition.truthy !== undefined) {
        return parseMetaBool(condition.truthy) ? parseMetaBool(rawValue) : !parseMetaBool(rawValue);
    }
    if (condition.falsey !== undefined || condition.falsy !== undefined) {
        const flag = condition.falsey !== undefined ? condition.falsey : condition.falsy;
        return parseMetaBool(flag) ? !parseMetaBool(rawValue) : parseMetaBool(rawValue);
    }

    return parseMetaBool(rawValue);
}

function evaluateVisibilityCondition(condition, levelDef) {
    const prefix = getComponentParamPrefix(hierEditorComponent);
    return evaluateVisibilityConditionForContext(
        hierEditorSlot,
        prefix,
        condition,
        levelDef,
        hierEditorChildIndex
    );
}

function extractHierarchyParamKey(param) {
    if (typeof param === "string") return param;
    if (param && typeof param === "object" && param.key) return param.key;
    return "";
}

function filterHierarchyParamsByVisibility(levelDef, params) {
    if (!Array.isArray(params)) return [];
    if (!levelDef || hierEditorSlot < 0) return [...params];

    const levels = hierEditorHierarchy && hierEditorHierarchy.levels ? hierEditorHierarchy.levels : {};
    return params.filter((param) => {
        if (!param || typeof param !== "object") return true;
        if (param.visible_if && !evaluateVisibilityCondition(param.visible_if, levelDef)) return false;
        if (param.level && levels && levels[param.level] && levels[param.level].visible_if) {
            return evaluateVisibilityCondition(levels[param.level].visible_if, levels[param.level]);
        }
        return true;
    });
}

function applyHierarchyVisibilityFilters(levelDef) {
    if (levelDef && levelDef.visible_if && !evaluateVisibilityCondition(levelDef.visible_if, levelDef)) {
        hierEditorParams = [];
        hierEditorKnobs = [];
        hierEditorSelectedIdx = 0;
        return;
    }

    if (Array.isArray(hierEditorAllParams) && hierEditorAllParams.length > 0 && levelDef) {
        hierEditorParams = filterHierarchyParamsByVisibility(levelDef, hierEditorAllParams);
    } else if (Array.isArray(hierEditorAllParams)) {
        hierEditorParams = [...hierEditorAllParams];
    } else {
        hierEditorParams = [];
    }

    if (Array.isArray(hierEditorAllKnobs) && hierEditorAllKnobs.length > 0) {
        const visibleKeys = new Set(
            hierEditorParams
                .map(extractHierarchyParamKey)
                .filter(k => k && k !== SWAP_MODULE_ACTION)
        );
        if (visibleKeys.size === 0) {
            /* Root/page-select level: no editable params visible (only nav links)
             * → keep all knobs so they control the first page's params */
            hierEditorKnobs = [...hierEditorAllKnobs];
        } else {
            hierEditorKnobs = hierEditorAllKnobs.filter(k => visibleKeys.has(k));
        }
    } else {
        hierEditorKnobs = [];
    }

    if (hierEditorParams.length === 0) {
        hierEditorSelectedIdx = 0;
    } else if (hierEditorSelectedIdx >= hierEditorParams.length) {
        hierEditorSelectedIdx = hierEditorParams.length - 1;
    } else if (hierEditorSelectedIdx < 0) {
        hierEditorSelectedIdx = 0;
    }
}

function refreshHierarchyVisibility() {
    const levelDef = getHierarchyLevelDef();
    applyHierarchyVisibilityFilters(levelDef);
    invalidateKnobContextCache();
}

function normalizeFilepathHookActions(rawActions, prefix) {
    if (!Array.isArray(rawActions)) return [];
    const out = [];
    for (const action of rawActions) {
        if (!action || typeof action !== "object") continue;
        const rawKey = typeof action.key === "string" ? action.key.trim() : "";
        if (!rawKey) continue;
        const fullKey = rawKey.includes(":") ? rawKey : (prefix ? `${prefix}:${rawKey}` : rawKey);
        const value = action.value === undefined || action.value === null ? "" : String(action.value);
        out.push({
            key: fullKey,
            value,
            restore: parseMetaBool(action.restore)
        });
    }
    return out;
}

function buildFilepathBrowserHooks(meta, prefix) {
    const hooksRaw = (meta && meta.browser_hooks && typeof meta.browser_hooks === "object")
        ? meta.browser_hooks
        : {};
    return {
        onOpen: normalizeFilepathHookActions(hooksRaw.on_open, prefix),
        onPreview: normalizeFilepathHookActions(hooksRaw.on_preview, prefix),
        onCancel: normalizeFilepathHookActions(hooksRaw.on_cancel, prefix),
        onCommit: normalizeFilepathHookActions(hooksRaw.on_commit, prefix)
    };
}

function resolveFilepathHookValue(rawValue, context) {
    const value = rawValue === undefined || rawValue === null ? "" : String(rawValue);
    if (value === "$path" || value === "$selected_path") {
        return context && context.path ? String(context.path) : "";
    }
    if (value === "$filename" || value === "$selected_filename") {
        if (context && context.path) {
            const path = String(context.path);
            const idx = path.lastIndexOf("/");
            return idx >= 0 ? path.slice(idx + 1) : path;
        }
        return "";
    }
    return value;
}

function applyFilepathHookActions(state, actions, context) {
    if (!state || !Array.isArray(actions) || actions.length === 0) return;
    for (const action of actions) {
        if (!action || !action.key) continue;

        if (action.restore && state.hookRestoreValues && !Object.prototype.hasOwnProperty.call(state.hookRestoreValues, action.key)) {
            const prev = getSlotParam(hierEditorSlot, action.key);
            if (prev !== null && prev !== undefined) {
                state.hookRestoreValues[action.key] = String(prev);
            }
        }

        const nextVal = resolveFilepathHookValue(action.value, context);
        setSlotParam(hierEditorSlot, action.key, nextVal);
    }
}

function restoreFilepathHookActions(state) {
    if (!state || !state.hookRestoreValues) return;
    for (const [key, prevValue] of Object.entries(state.hookRestoreValues)) {
        setSlotParam(hierEditorSlot, key, prevValue);
    }
}

function applyLivePreview(state, selected) {
    if (!state || !state.livePreviewEnabled || !selected || selected.kind !== "file" || !selected.path) return;
    if (selected.path === state.previewCurrentValue || !state.previewParamFullKey) return;
    if (setSlotParam(hierEditorSlot, state.previewParamFullKey, selected.path)) {
        state.previewCurrentValue = selected.path;
        applyFilepathHookActions(state, state.hooksOnPreview, { path: selected.path });
    }
}

/* Loaded module UI state */
let loadedModuleUi = null;       // The chain_ui object from loaded module
let loadedModuleSlot = -1;       // Which slot the module UI is for
let loadedModuleComponent = "";  // a component key: "synth", "midiFx", or "fxN"
let moduleUiLoadError = false;   // True if load failed

/* Asset warning overlay state */
let warningActive = false;  // True when showing warning overlay
let warningTitle = "";      // Warning overlay title
let warningLines = [];      // Wrapped warning message lines
let warningShownForSlots = new Set();  // Track which chain slots have shown warnings
let warningShownForMidiFxSlots = new Set();  // Track which slots have shown MIDI FX warnings
let warningShownForMasterFx = new Set();  // Track which Master FX slots have shown warnings

const MODULES_ROOT = "/data/UserData/schwung/modules";

/* Find UI path for a module - tries ui_chain.js first, then ui.js */
function getModuleUiPath(moduleId) {
    if (!moduleId) return null;

    /* Helper to check a directory for UI files */
    function checkDir(moduleDir) {
        /* First try ui_chain.js (preferred - uses chain_ui pattern) */
        let uiPath = `${moduleDir}/ui_chain.js`;

        /* Try to read module.json for custom ui_chain path */
        try {
            const moduleJsonStr = std.loadFile(`${moduleDir}/module.json`);
            if (moduleJsonStr) {
                const match = moduleJsonStr.match(/"ui_chain"\s*:\s*"([^"]+)"/);
                if (match && match[1]) {
                    uiPath = `${moduleDir}/${match[1]}`;
                }
            }
        } catch (e) {
            /* No module.json or can't read it */
        }

        /* Check if ui_chain.js exists */
        try {
            const stat = os.stat(uiPath);
            if (stat && stat[1] === 0) {
                return uiPath;
            }
        } catch (e) {
            /* File doesn't exist */
        }

        /* Fall back to ui.js (standard module UI) */
        uiPath = `${moduleDir}/ui.js`;
        try {
            const stat = os.stat(uiPath);
            if (stat && stat[1] === 0) {
                return uiPath;
            }
        } catch (e) {
            /* File doesn't exist */
        }

        return null;
    }

    /* Check locations in order */
    const searchDirs = [
        `${MODULES_ROOT}/${moduleId}`,                      /* Top-level modules */
        `${MODULES_ROOT}/sound_generators/${moduleId}`,     /* Sound generators */
        `${MODULES_ROOT}/audio_fx/${moduleId}`,             /* Audio FX */
        `${MODULES_ROOT}/midi_fx/${moduleId}`,              /* MIDI FX */
        `${MODULES_ROOT}/utilities/${moduleId}`,            /* Utilities */
        `${MODULES_ROOT}/other/${moduleId}`                 /* Other/unspecified */
    ];

    for (const dir of searchDirs) {
        const result = checkDir(dir);
        if (result) return result;
    }

    return null;
}

/* Convert component key to DSP param prefix (midiFx -> midi_fx1). The prefix
 * IS the position's id in the chain model — one definition, above. */
function getComponentParamPrefix(componentKey) {
    return chainComponentId(componentKey);
}

/* Set up shims for host_module_get_param and host_module_set_param
 * These route to the correct slot and component in shadow mode */
function setupModuleParamShims(slot, componentKey) {
    /* Cache the real host APIs on first install so co-run can swap them
     * back around active-tool callbacks. */
    if (!paramShimsInstalled) {
        originalHostGetParam = globalThis.host_module_get_param;
        originalHostSetParam = globalThis.host_module_set_param;
        paramShimsInstalled = true;
    }
    const prefix = getComponentParamPrefix(componentKey);

    globalThis.host_module_get_param = function(key) {
        return getSlotParam(slot, `${prefix}:${key}`);
    };

    globalThis.host_module_set_param = function(key, value) {
        return setSlotParam(slot, `${prefix}:${key}`, value);
    };

    globalThis.host_swap_module = function() {
        const compIndex = slotChainComponentIndex(slot, componentKey);
        if (compIndex >= 0) {
            unloadModuleUi();
            enterComponentSelect(slot, compIndex);
        }
    };

    globalThis.host_open_file_in_tool = function(filePath, toolId) {
        if (!filePath || !toolId) return false;
        if (!toolModules || !toolModules.length) {
            toolModules = scanForToolModules();
        }
        const tool = toolModules.find(t => t.id === toolId);
        if (!tool) {
            debugLog("host_open_file_in_tool: tool not found: " + toolId);
            return false;
        }
        debugLog("host_open_file_in_tool: opening " + filePath + " in " + toolId);
        unloadModuleUi();
        startInteractiveTool(tool, filePath);
        return true;
    };
}

/* Clear the param shims */
function clearModuleParamShims() {
    /* Restore the real host APIs we cached on first shim install. */
    if (paramShimsInstalled) {
        if (originalHostGetParam) globalThis.host_module_get_param = originalHostGetParam;
        else delete globalThis.host_module_get_param;
        if (originalHostSetParam) globalThis.host_module_set_param = originalHostSetParam;
        else delete globalThis.host_module_set_param;
        paramShimsInstalled = false;
    } else {
        delete globalThis.host_module_get_param;
        delete globalThis.host_module_set_param;
    }
    delete globalThis.host_module_set_param_blocking;
    delete globalThis.host_exit_module;
    delete globalThis.host_suspend_overtake;
    delete globalThis.host_swap_module;
    delete globalThis.host_open_file_in_tool;
}

/* Run a tool callback (tick / onMidiMessageInternal) with the real host
 * APIs restored. While chain-editor shims are installed, every direct
 * reference to globalThis.host_module_get_param / _set_param goes to
 * the chain slot DSP — wrong for the active tool. This swap-and-restore
 * keeps the tool talking to its own DSP. */
function runToolCallback(fn) {
    if (!paramShimsInstalled) {
        return fn();
    }
    const shimGet = globalThis.host_module_get_param;
    const shimSet = globalThis.host_module_set_param;
    if (originalHostGetParam) globalThis.host_module_get_param = originalHostGetParam;
    if (originalHostSetParam) globalThis.host_module_set_param = originalHostSetParam;
    try {
        return fn();
    } finally {
        globalThis.host_module_get_param = shimGet;
        globalThis.host_module_set_param = shimSet;
    }
}

/* Load a module's UI for editing */
function loadModuleUi(slot, componentKey, moduleId) {
    /* CO-RUN refuse: loading a chain module's UI overwrites globalThis.tick
     * (and onMidiMessageInternal), which silences the active tool. The caller
     * (enterComponentEditFallback) falls back to the simple preset browser
     * when this returns false — that path doesn't touch globals. */
    if (coRunUiActive()) {
        moduleUiLoadError = true;
        return false;
    }
    const uiPath = getModuleUiPath(moduleId);
    if (!uiPath) {
        moduleUiLoadError = true;
        return false;
    }

    /* Clear any previous chain_ui */
    globalThis.chain_ui = null;

    /* Set up param shims before loading */
    setupModuleParamShims(slot, componentKey);

    /* Load the UI module */
    if (typeof shadow_load_ui_module !== "function") {
        moduleUiLoadError = true;
        clearModuleParamShims();
        return false;
    }

    /* Save current globals before loading - module may overwrite them */
    const savedInit = globalThis.init;
    const savedTick = globalThis.tick;
    const savedMidi = globalThis.onMidiMessageInternal;

    const ok = shadow_load_ui_module(uiPath);
    if (!ok) {
        moduleUiLoadError = true;
        clearModuleParamShims();
        /* Restore globals in case partial load modified them */
        globalThis.init = savedInit;
        globalThis.tick = savedTick;
        globalThis.onMidiMessageInternal = savedMidi;
        return false;
    }

    /* Check if module used chain_ui pattern (preferred) */
    if (globalThis.chain_ui) {
        loadedModuleUi = globalThis.chain_ui;
    } else {
        /* Module used standard globals - wrap them in chain_ui object */
        loadedModuleUi = {
            init: (globalThis.init !== savedInit) ? globalThis.init : null,
            tick: (globalThis.tick !== savedTick) ? globalThis.tick : null,
            onMidiMessageInternal: (globalThis.onMidiMessageInternal !== savedMidi) ? globalThis.onMidiMessageInternal : null
        };

        /* Restore shadow UI's globals */
        globalThis.init = savedInit;
        globalThis.tick = savedTick;
        globalThis.onMidiMessageInternal = savedMidi;
    }

    /* Verify we got something useful */
    if (!loadedModuleUi || (!loadedModuleUi.tick && !loadedModuleUi.init && !loadedModuleUi.onMidiMessageInternal)) {
        moduleUiLoadError = true;
        clearModuleParamShims();
        loadedModuleUi = null;
        return false;
    }

    loadedModuleSlot = slot;
    loadedModuleComponent = componentKey;
    moduleUiLoadError = false;

    /* Call init if available */
    if (loadedModuleUi.init) {
        loadedModuleUi.init();
    }

    return true;
}

/* Unload the current module UI */
function unloadModuleUi() {
    loadedModuleUi = null;
    loadedModuleSlot = -1;
    loadedModuleComponent = "";
    moduleUiLoadError = false;
    globalThis.chain_ui = null;
    clearModuleParamShims();
}

/* Check for synth error in a slot and show warning if found */
function checkAndShowSynthError(slotIndex) {
    /* NOT chainComponentParamKey: the synth serves its error at "synth_error",
     * a flat key, where every other position uses "<id>:error". */
    const synthError = getSlotParam(slotIndex, "synth_error");
    if (synthError && synthError.length > 0) {
        const synthName = getSlotParam(slotIndex, "synth:name") || "Synth";
        showWarning(`${synthName} Warning`, synthError);
        return true;
    }
    return false;
}

/*
 * A component's asset warning, raised on screen — for EITHER chain.
 *
 * The three copies of this differed only in how the key was spelled and what
 * the box is called when the module serves no name. `fallbackName` is that,
 * and it is the POSITION ("MIDI FX", "FX 3"), because a module with no name
 * cannot supply one.
 */
function checkAndShowComponentError(target, componentKey, fallbackName) {
    const err = chainTargetGetParam(target, componentKey, "error");
    if (err && err.length > 0) {
        const name = chainTargetGetParam(target, componentKey, "name") || fallbackName;
        showWarning(`${name} Warning`, err);
        return true;
    }
    return false;
}

/* Check for MIDI FX warning in a slot and show warning if found */
function checkAndShowMidiFxError(slotIndex) {
    return checkAndShowComponentError(slotChainTarget(slotIndex), "midiFx", "MIDI FX");
}

/* Check for Master FX error in a slot and show warning if found */
function checkAndShowMasterFxError(fxSlot) {
    /* fxSlot is 0-based: 0 .. MASTER_FX_SLOTS-1 */
    return checkAndShowComponentError(MASTER_CHAIN_TARGET, masterFxComponentKey(fxSlot),
                                      `FX ${fxSlot + 1}`);
}

/* Show the warning overlay with a title and wrapped message */
function showWarning(title, message) {
    warningTitle = title;
    warningLines = wrapText(message, 18);
    warningActive = true;
    announce(`${title}: ${message}`);
    needsRedraw = true;
}

/*
 * Answer the message overlay, if one is up.
 *
 * The overlay is drawn OVER whatever view is on screen (see the draw path: it
 * is outside the view switch), so it has to be ANSWERABLE from whatever view is
 * on screen. It used to be dismissed from one site far down
 * onMidiMessageInternal, below the early-out that hands input to the page
 * chrome — so a warning raised by a write ON a page (Schwung Mix, Link Audio,
 * File Browser: all three now come from the Global Settings contract) drew
 * itself over a grid that went on consuming every button, and there was no
 * press that could clear it.
 *
 * Extracted rather than moved: hoisting the one site above the early-out would
 * also hoist it above splash, the analytics prompt, the feedback gate and text
 * entry, each of which deliberately outranks it today.
 *
 * A knob turn does not dismiss — it is how the value that raised the warning is
 * being changed, so the message would vanish on the same detent that produced
 * it.
 */
function maybeDismissWarningFromInput(status, d1, d2) {
    if (!warningActive) return false;
    if ((status & 0xF0) !== 0xB0 || d2 <= 0) return false;
    if (d1 === MoveMainKnob) return false;
    if (d1 >= KNOB_CC_START && d1 <= KNOB_CC_END) return false;
    dismissWarning();
    return true;
}

/* Dismiss asset warning overlay */
function dismissWarning() {
    warningActive = false;
    warningTitle = "";
    warningLines = [];
    needsRedraw = true;
}

/* Initialize chain configs for all slots */
function initChainConfigs() {
    chainConfigs = [];
    chainConfigFresh = [];
    lastSlotModuleSignatures = [];
    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
        chainConfigs.push(createEmptyChainConfig());
        chainConfigFresh.push(false);
        lastSlotModuleSignatures.push("");
    }
}

/*
 * Whether a slot's cached chain config is known to still describe the DSP.
 *
 * drawChainEdit used to call loadChainConfigFromSlot on EVERY FRAME, which is
 * `3 + <chain length>` IPC round trips at ~2.8ms each — 10 reads (~28ms) on a
 * two-FX chain and 25 (~70ms) on a full one, against a 16.67ms frame. It grew
 * with the chain, which is exactly what a variable-length chain makes possible,
 * so the longest chains drew the slowest. A whole page render is 1.68ms: the
 * reload cost more than everything it was feeding.
 *
 * A frame is the wrong cadence for a question whose answer only changes when
 * somebody edits the chain. Two mechanisms keep the cache honest, and they are
 * deliberately not the same one:
 *
 * - The user's own edits mark it stale HERE, at every point that writes the
 *   model or the DSP. That is a short list and it is enumerable
 *   (writeChainShape, the picker, the `+` box) because there are only four
 *   places in the file that assign `chainConfigs[i]`.
 * - Everything else — a patch restore, a set load, the shim loading a slot
 *   underneath us — is caught by the periodic refreshSlotModuleSignature
 *   (every 30 ticks), which already reloads on a changed signature. That is
 *   the pre-existing self-heal; this does not compete with it, it just stops
 *   asking the same question sixty times a second in between.
 *
 * A signature CANNOT be the invalidator for the first kind: a pure reorder
 * changes which module is at which position without changing WHICH modules the
 * slot holds, so the signature is byte-identical across it. Nor can object
 * identity: a picker swap mutates the config IN PLACE.
 */
let chainConfigFresh = [];

/** The cached chain config is no longer known to match the DSP. */
function invalidateChainConfig(slotIndex) {
    if (slotIndex === undefined) chainConfigFresh = [];
    else chainConfigFresh[slotIndex] = false;
}

/** The slot's chain config, reloading from the DSP only if it went stale. */
function ensureChainConfigFresh(slotIndex) {
    if (chainConfigFresh[slotIndex]) {
        return chainConfigs[slotIndex] || createEmptyChainConfig();
    }
    return loadChainConfigFromSlot(slotIndex);
}

/* Load chain config from current patch info */
function loadChainConfigFromSlot(slotIndex) {
    const cfg = chainConfigs[slotIndex] || createEmptyChainConfig();

    /* Read current patch configuration from DSP
     * Note: get_param uses underscores (synth_module), set_param uses colons (synth:module) */
    /* An unserved key answers "" rather than null, which reads the same as an
     * empty position — both mean "nothing loaded here". */
    const readPosition = (id) => {
        const moduleId = getSlotParam(slotIndex, `${id}_module`);
        return moduleId && moduleId !== ""
            ? { module: moduleId.toLowerCase(), params: {} } : null;
    };
    /*
     * How long the section is, ASKED rather than probed.
     *
     * The DSP publishes fx_count / midi_fx_count (chain_host.c), so finding out
     * that a chain has no FX costs ONE read. Probing fx1..fx8 to discover the
     * same thing would cost eight — and this function runs from drawChainEdit
     * on every frame at ~2.8ms per IPC round trip, which is more per frame than
     * rendering the entire page. An unserved key answers "", and Number("") is
     * 0, so the parse is explicit about its fallback.
     */
    const readCount = (key, cap) => {
        const raw = getSlotParam(slotIndex, key);
        const n = parseInt(raw, 10);
        return (isNaN(n) || n < 0) ? 0 : Math.min(n, cap);
    };

    /*
     * Trailing empties are dropped so the list says what is actually loaded; a
     * hole in FRONT of a loaded module is KEPT.
     *
     * That is deliberate and it is the one place the chain model does not get
     * the last word. Position i of this list IS `fx(i+1)` in the DSP — that is
     * what makes `getComponentParamPrefix` correct — so compacting a hole away
     * on READ would leave the editor addressing fx1's params while the audio
     * ran through fx2. The model compacts on the user's own edit
     * (removeAt + the `remove` verb), which renumbers the DSP at the same time.
     * A legacy patch with a hole therefore draws its hole, once, and closes it
     * the first time the user changes the order.
     */
    const readSection = (idAt, count) => {
        const list = [];
        for (let i = 0; i < count; i++) list.push(readPosition(idAt(i)));
        while (list.length && !list[list.length - 1]) list.pop();
        return list;
    };

    const oldFx = cfg.fx;

    cfg.synth = readPosition("synth");
    cfg.midiFx = readSection((i) => `midi_fx${i + 1}`,
                             readCount("midi_fx_count", CHAIN_CAP.midiFx));
    cfg.fx = readSection((i) => `fx${i + 1}`, readCount("fx_count", CHAIN_CAP.fx));

    /* Clear display_name cache when FX modules change (prevents stale
     * announcement on swap). Also drop the poll backoff: a different module may
     * well implement display_name even though the last one didn't. */
    const forgetFxName = (ck) => {
        delete fxDisplayNameCache[ck];
        delete fxDisplayNameSkip[ck];
        delete fxDisplayNameBackoff[ck];
    };
    for (let i = 0; i < Math.max(oldFx.length, cfg.fx.length); i++) {
        const before = oldFx[i] ? oldFx[i].module : null;
        const after = cfg.fx[i] ? cfg.fx[i].module : null;
        if (before !== after) forgetFxName(`${slotIndex}:fx${i + 1}`);
    }

    chainConfigs[slotIndex] = cfg;
    /* This IS the reload every other path invalidates towards, so the slot is
     * clean by definition once it returns. */
    chainConfigFresh[slotIndex] = true;
    return cfg;
}

/* The DSP's name for a section, which is the prefix its position ids and its
 * reorder verbs both carry: "midi_fx" or "fx". */
function chainSectionPrefix(section) {
    return section === "midiFx" ? "midi_fx" : "fx";
}

/*
 * Change a section's SHAPE in the DSP: one write, and nothing is reloaded.
 *
 * This replaced writeChainOrder, which pushed a whole section as a run of
 * `<id>:module` writes. Every one of those unloads the position and dlopen()s a
 * fresh instance (v2_load_audio_fx_slot / v2_load_midi_fx_slot), so renumbering
 * a chain destroyed and rebuilt every module the renumber touched. Inserting a
 * MIDI FX at the head rebuilt every MIDI FX behind it; removing a mid-chain
 * reverb rebuilt everything downstream. writeChainOrder tried to make that
 * survivable by carrying each module's opaque `<id>:state`, its modulation base
 * and its LFO routing across the shift, keyed by object identity — a large
 * amount of careful machinery for something that should not have been happening
 * at all. A state blob is not an instance: it cannot carry a running
 * arpeggiator's phase or the tail ringing in a delay. Reported on hardware as
 * "we can't change phase of a running module by just adding a new one".
 *
 * The DSP now PERMUTES its per-position arrays instead (chain_permute.h), so
 * the instances keep running and only their index changes. It also re-aims
 * everything that names a position by string — modulation targets, both LFOs,
 * the knob mappings — which is why none of that is carried from here any more:
 * the modulation entry is never destroyed, so its base never needs restoring.
 *
 * `op` is `{ kind, section, index, to }` with 0-based indices; the wire is
 * 1-based to match the ids ("fx2"), so the conversion happens here, once.
 *
 *   insert   open an empty position at `index`, shifting the rest along. The
 *            caller follows with the ordinary `<id>:module` write; for the one
 *            audio frame in between the chain has a hole in it, which both the
 *            audio and the MIDI walk skip per position.
 *   remove   unload `index` and close the gap.
 *   move     `index` -> `to`, rotating the span between.
 *
 * `target` says WHICH chain, and it is the only difference between the two
 * editors here: the verb is spelled `fx:insert` for a slot and
 * `master_fx:fx:insert` for the master bus, which is exactly what
 * `target.chainKey` was introduced for. One emitter, so a shape edit cannot
 * mean two different things depending on which screen the user is standing on.
 */
function writeChainShape(target, op) {
    if (!op || !op.section) return;
    const prefix = chainSectionPrefix(op.section);
    const verb = (name) => target.chainKey(`${prefix}:${name}`);
    if (op.kind === "insert") {
        setSlotParam(target.slot, verb("insert"), String(op.index + 1));
    } else if (op.kind === "remove") {
        setSlotParam(target.slot, verb("remove"), String(op.index + 1));
    } else if (op.kind === "move") {
        setSlotParam(target.slot, verb("move"), `${op.index + 1}>${op.to + 1}`);
    } else {
        return;
    }
    /* The chain the editor is drawing from has just been renumbered underneath
     * it. Here rather than in each caller so a fourth one cannot forget — and
     * note this matters even though no `<id>:module` write went out, because
     * nothing keyed on the module signature would notice a pure reorder. */
    target.invalidate();
}

/*
 * Drop every LFO routing aimed at one component position.
 *
 * A position keeps its NAME across a module change — "fx1" is "fx1" whatever
 * is loaded there — so nothing about swapping the module invalidates a routing
 * that names it. The DSP does clear the position's modulation ENTRIES when it
 * unloads (chain_mod_clear_target_entries, chain_host.c), but the LFO itself
 * still holds `target:"fx1", target_param:"room_size"` and keeps emitting. Two
 * consequences, both seen on hardware:
 *
 *   - the LFO editor shows the routing as live, naming a param of a module
 *     that is no longer in the chain; and
 *   - if the NEW module happens to declare a param of the same name — `mix`,
 *     `gain`, `feedback` are not rare — the LFO silently starts modulating it.
 *     That is the dangerous half: audible, and attributable to nothing the
 *     user did.
 *
 * So a module leaving a position takes the routings aimed at that position
 * with it. Both keys: `target` alone would leave `target_param` naming the
 * dead module's param, ready to be revived by the next `target` write.
 *
 * Bounded by LFO_COUNT (2) reads, on a user gesture. Only the routings that
 * actually name this component are written.
 */
/*
 * Did a picker choice REPLACE what was at a position, as opposed to leaving it
 * or removing it?
 *
 * `replaced` is what applyPickerChoiceToChain saw there before. Three answers,
 * and each of them is a decision:
 *
 *   null       a REMOVAL. It hands back a whole section to rewrite, and
 *              the DSP owns the routing there because it alone can tell a
 *              module that left from a module that only moved down.
 *   same id    a reload, not a replacement. The routing still names the module
 *              the user routed, so it stays.
 *   anything   a replacement, INCLUDING filling a position that was empty: a
 *   else       routing left over from that position previous occupant would
 *              otherwise land on the module arriving now.
 *
 * Case-insensitive because module ids reach here from both the picker and the
 * DSP, and the DSP answers lowercase.
 */
function pickerReplacedModule(replaced, moduleId) {
    if (replaced === null || replaced === undefined) return false;
    return String(replaced).toLowerCase() !== String(moduleId || "").toLowerCase();
}

function clearLfoRoutingForComponent(target, componentId) {
    if (!componentId) return;
    for (let li = 1; li <= 2; li++) {
        const aim = target.chainKey(`lfo${li}:target`);
        if ((getSlotParam(target.slot, aim) || "") !== componentId) continue;
        setSlotParam(target.slot, aim, "");
        setSlotParam(target.slot, target.chainKey(`lfo${li}:target_param`), "");
    }
}

/*
 * Move one module one place along its own section, model and DSP together.
 *
 * Returns whether anything moved, so the caller can say "at the end" rather
 * than announce a move that did not happen.
 *
 * The bounds are the MODEL's — chainMoveBy refuses to cross a section boundary
 * (a MIDI FX passing the synth would be a type change, not a reorder) and
 * stops at the ends rather than wrapping. Rather than restate those rules here
 * and risk the two drifting, this asks the model for the result and compares:
 * a refused move returns the list it was given. The one rule that IS here is
 * that an EMPTY position does not move — the pending entry a `+` box
 * materialises is exactly that, and dragging a hole around is not a gesture.
 *
 * The model edit and the DSP write happen in the SAME call, so the editor's
 * list and the chain the audio runs through can never disagree about the order.
 * The DSP write is ONE verb and reloads nothing — a module that moves keeps
 * running, which is the difference between reordering a chain and rebuilding it
 * (see writeChainShape).
 */
function moveChainComponent(target, componentKey, delta) {
    const cfg = target.config();
    if (!cfg) return false;
    const id = chainComponentId(componentKey);
    const at = parseChainId(id);
    /* The synth is not a list position; Settings and the `+` boxes are not
     * modules at all. None of them move. */
    if (!at) return false;
    const before = cfg[at.section] || [];
    if (!before[at.index]) return false;
    const next = chainMoveBy(cfg, id, delta);
    const after = next[at.section];
    if (after.length === before.length && after.every((m, i) => m === before[i])) return false;
    target.setConfig(next);
    writeChainShape(target, { kind: "move", section: at.section,
                             index: at.index, to: at.index + delta });
    /* An LFO label names a module by the position it was routed to, and both
     * just changed. The knob context self-heals within ~30 ticks via
     * applySlotModuleSignature; this cache does not, so a stale label would sit
     * there naming a module that has moved on. */
    resetLfoTargetLabels();
    invalidateKnobContextCache();
    return true;
}

/*
 * The picker's Move rows for a component position.
 *
 * They exist so Shift+jog is not the ONLY way to reorder a chain: a modifier
 * gesture with no discoverable equivalent is a feature only the person who
 * wrote it knows about.
 *
 * Offered only where they would DO something — an occupied list position with
 * somewhere to go — so the list never carries a row that answers a click by
 * doing nothing. That is also why the synth has neither: it is not a list
 * position, and the sections either side of it are not the same kind of thing.
 * Neither `+` nor Settings ever reaches a picker at all.
 */
function chainMoveEntries(cfg, componentKey) {
    const at = parseChainId(chainComponentId(componentKey));
    if (!cfg || !at) return [];
    const list = cfg[at.section] || [];
    if (!list[at.index]) return [];
    const out = [];
    if (at.index > 0) out.push({ id: "__move_left__", name: "  Move Left" });
    if (at.index < list.length - 1) out.push({ id: "__move_right__", name: "  Move Right" });
    return out;
}

/*
 * What a picker choice does to the chain, IN THE MODEL, and what the DSP owes
 * for it.
 *
 * The swap/remove distinction lives here because the two sit one entry apart
 * in the same list: `None` is the first row, every module below it is a swap.
 *
 * A swap REPLACES the occupant and moves nothing — resequencing a patch the
 * user only meant to retouch would change the signal path behind their back —
 * so it asks for no shape change, only the one `<id>:module` write the caller
 * already makes.
 *
 * `None` on a list position REMOVES and CLOSES THE GAP, which renumbers every
 * module downstream of it. That is a SHAPE change, and it goes to the DSP as
 * one `remove` verb: the module leaving is unloaded, and everything behind it
 * is renumbered by permuting the arrays rather than by being rebuilt (see
 * writeChainShape). `None` on the synth is a clear, not a removal: the synth
 * has no neighbours to renumber.
 *
 * `replaced` is the module id that was there BEFORE, for the two branches that
 * write a single position. The caller needs it to decide whether any LFO aimed
 * at that position is still meaningful — see clearLfoRoutingForComponent. The
 * removal branch does not report one: the DSP re-aims the routings itself
 * across a permutation, and it alone can tell a module that LEFT from one that
 * only moved along, which one position in isolation cannot.
 */
function applyPickerChoiceToChain(cfg, componentKey, moduleId) {
    const id = chainComponentId(componentKey);
    const at = parseChainId(id);
    const before = getChainComponentModule(cfg, componentKey);
    const replaced = (before && before.module) ? String(before.module) : "";
    if (!moduleId) {
        if (at) {
            return { cfg: chainRemoveAt(cfg, id), replaced: null,
                     shape: { kind: "remove", section: at.section, index: at.index } };
        }
        setChainComponentModule(cfg, componentKey, null);
        return { cfg, shape: null, replaced };
    }
    setChainComponentModule(cfg, componentKey, { module: moduleId, params: {} });
    return { cfg, shape: null, replaced };
}

/*
 * The position a `+` box opened, which exists ONLY IN THE MODEL.
 *
 * Clicking `+` materialises an empty entry and opens the picker on it; nothing
 * is loaded and NOTHING IS WRITTEN until the picker resolves, so until then the
 * DSP has never heard of it. Two things have to know it is there, and they are
 * different problems:
 *
 *  - THE CONFIRM PATH, because where the entry went decides what the DSP is
 *    asked for. The audio `+` appends and renumbers nothing, so the single
 *    `<id>:module` write an ordinary pick makes is right. The MIDI `+` is the
 *    LEFTMOST box on screen and inserts at the head, which pushes midi_fx1 to
 *    midi_fx2 and every other MIDI FX along with it — that single write would
 *    land on midi_fx1 and CLOBBER the module already there, leaving the rest
 *    stale. See withPendingChainInsert.
 *
 *  - EVERY WAY OUT of the picker, because an entry that is not dropped leaves
 *    the editor drawing a box the user cancelled. See cancelPendingChainInsert.
 *
 * `count` is how long the section was BEFORE the entry, which is the whole
 * question the confirm path asks of it: an entry at the end is an append and
 * needs nothing, an entry anywhere else renumbers.
 */
let pendingChainInsert = null;

/** Remember a `+` box's new position until the picker resolves it. `target.id`
 *  rather than the target object: slotChainTarget builds a fresh one per call,
 *  so identity would never match on the way back out. */
function beginPendingChainInsert(target, section, index, count) {
    pendingChainInsert = { target, id: target.id, section, index, count,
                           key: chainEditorKeyAt(section, index) };
}

/*
 * A `+` box was clicked: open a NEW position WHERE THE BOX IS DRAWN, and hand
 * back its index in the (now longer) component list so the caller can raise its
 * picker on it. -1 when the section is full, which is announced here.
 *
 * The two `+` boxes sit at opposite ends of the diagram — the MIDI one is the
 * LEFTMOST box, ahead of every MIDI FX, and the audio one comes after the last
 * FX — so the audio `+` appends and the MIDI `+` inserts at the head. Sharing
 * one append made the MIDI side put the new module at the far end of the
 * section from the button that was pressed. The asymmetry is the rule, not an
 * exception to it. Master FX has only the audio end, so its `+` appends.
 *
 * The position is materialised in the MODEL only. NOTHING IS WRITTEN here: an
 * insert with anything to its right renumbers the section, and it is the
 * picker's confirm that owes the DSP that (withPendingChainInsert). Backing out
 * writes nothing at all, which is why the record of the pending position is
 * kept rather than inferred.
 *
 * Written once for both chains — a second copy is how "Master FX can append but
 * cannot move" happens.
 */
function beginChainInsertFromAddBox(target, comp) {
    const cfg = target.config();
    const list = cfg[comp.section] || [];
    if (list.length >= target.cap(comp.section)) {
        announce(`${comp.label} full`);
        return -1;
    }
    const at = comp.section === "midiFx" ? 0 : list.length;
    beginPendingChainInsert(target, comp.section, at, list.length);
    target.setConfig(chainInsertAt(cfg, comp.section, at, null));
    /* The pending entry exists only in the model, so the cached view is stale
     * the moment it is added — and stale in BOTH directions: backing out of the
     * picker is supposed to drop it, and it is a reload that drops it (the DSP
     * never held it). Without this the editor would come back still drawing a
     * `+` that was cancelled. */
    target.invalidate();
    const want = chainEditorKeyAt(comp.section, at);
    return target.components().findIndex((c) => c.key === want);
}

/*
 * The pending insert a picker choice belongs to, or null.
 *
 * Keyed on the slot AND the position, so a pick made somewhere else can never
 * adopt it — adopting one would turn an ordinary swap into a renumber of an
 * edit that never happened.
 *
 * The emptiness check is what makes the record SELF-EXPIRING, and it is the
 * only forgetting mechanism on the confirm path deliberately: once the pick has
 * landed the position is occupied, so the record cannot be claimed again, and a
 * second "clear it here too" would be a rule that has to agree with this one
 * forever. It also covers the case nothing else could — anything that reloaded
 * the slot underneath the picker (a set load, the shim loading the slot) has
 * already dropped the hole, and the record left behind would be a lie. It costs
 * no IPC: the config it reads is the cached one.
 *
 * The CANCEL paths still clear explicitly, because backing out leaves the
 * position empty and there is nothing for this to notice.
 */
function pendingChainInsertFor(target, componentKey) {
    const p = pendingChainInsert;
    if (!p || p.id !== target.id || p.key !== componentKey) return null;
    if (getChainComponentModule(target.config(), componentKey)) return null;
    return p;
}

/*
 * Drop a pending position WITHOUT WRITING ANYTHING.
 *
 * The model is discarded rather than repaired: loadChainConfigFromSlot reads the
 * section back from the DSP, which never heard of the entry, so the reload IS
 * the drop — wherever in the list it sat. That matters now that it can sit at
 * the HEAD: the old "trailing empties are dropped on read" reasoning only ever
 * covered the appending `+`, and a leading hole is deliberately KEPT by the
 * reader (see loadChainConfigFromSlot) because a hole in front of a loaded
 * module is a real thing a legacy patch can hold.
 *
 * The selection goes back to the `+` the user pressed: it is where they were
 * standing, and it is the one position guaranteed to still exist afterwards.
 */
function cancelPendingChainInsert() {
    const p = pendingChainInsert;
    pendingChainInsert = null;
    if (!p) return;
    /* The reload IS the drop: the DSP never heard of the entry, so reading the
     * chain back from it is what removes the hole — wherever in the list it
     * sat. NOW rather than lazily, because the selection below has to name a
     * position in the chain that is left. */
    p.target.reload();
    /* The editor may be pointing at a DIFFERENT chain by now — there are four
     * slot chains and the shim can switch between them — and putting the
     * selection back would move it on a chain the user is not looking at. Asked
     * of the target: there is only one master bus, so it answers yes. */
    if (!p.target.isSelectedChain()) return;
    const at = p.target.components().findIndex(
        (c) => c.key === (p.section === "midiFx" ? "add_midi" : "add_fx"));
    if (at >= 0) p.target.setSelection(at);
}

/*
 * Fold a pending `+` insert into what the picker choice asks the DSP for.
 *
 * An insert with anything to its RIGHT shifts those positions along, so the DSP
 * is asked to OPEN A HOLE there first (`<section>:insert`) and the module write
 * then lands in it. Without that, the single `<id>:module` write would land on
 * the position the shift was supposed to vacate and overwrite the module
 * already there.
 *
 * The hole is opened by permuting the section's arrays, so the modules being
 * pushed along are not reloaded — they keep their instance, and with it their
 * phase, their tails, their live modulation entries and the base values inside
 * them. That is why nothing is carried from here.
 *
 * `replaced` is dropped for the same reason a removal drops it: the DSP re-aims
 * position routings across the permutation itself, and it alone can tell a
 * module that left from one that only moved along.
 *
 * An APPEND is deliberately left alone. Nothing is to its right, so nothing
 * shifts, and the module write on its own already grows the section.
 */
function withPendingChainInsert(choice, pending) {
    if (!choice || !pending) return choice;
    if (pending.index >= pending.count) return choice;
    return { cfg: choice.cfg, replaced: null,
             shape: { kind: "insert", section: pending.section, index: pending.index } };
}

/*
 * A signature of a slot's loaded module ids, read from the DSP.
 *
 * It has to come from the DSP, not from the cached config: this is precisely
 * the thing that NOTICES the DSP changing underneath the UI (a patch restore,
 * the shim's own slot load), and a signature derived from the cache could
 * never differ from itself.
 *
 * Length comes from the published counts, so a slot holding nothing is three
 * reads rather than a walk of the cap.
 */
function getSlotModuleSignature(slotIndex) {
    const read = (id) => getSlotParam(slotIndex, `${id}_module`) || "";
    const count = (key, cap) => {
        const n = parseInt(getSlotParam(slotIndex, key), 10);
        return (isNaN(n) || n < 0) ? 0 : Math.min(n, cap);
    };
    const parts = [read("synth")];
    const nMidi = count("midi_fx_count", CHAIN_CAP.midiFx);
    for (let i = 0; i < nMidi; i++) parts.push(read(`midi_fx${i + 1}`));
    /* A separator, so [a] + [] and [] + [a] are different signatures rather
     * than the same "a" with the sections' lengths silently swapped. */
    parts.push("/");
    const nFx = count("fx_count", CHAIN_CAP.fx);
    for (let i = 0; i < nFx; i++) parts.push(read(`fx${i + 1}`));
    return parts.join("|");
}

/* Refresh module signature for a slot and invalidate knob cache on changes */
function refreshSlotModuleSignature(slotIndex) {
    if (slotIndex < 0 || slotIndex >= SHADOW_UI_SLOTS) return false;
    return applySlotModuleSignature(slotIndex, getSlotModuleSignature(slotIndex));
}

/*
 * The half of refreshSlotModuleSignature that costs nothing, split out so a
 * caller that already has the signature does not pay for it twice.
 *
 * getSlotModuleSignature is FOUR synchronous IPC round trips (~2.8ms each,
 * ~11ms), and autosave was calling it twice per slot per pass — once through
 * refreshSlotModuleSignature at the top of the loop and again below as
 * `currentSig` — i.e. 32 round trips across four slots to compute 16 values,
 * half of them a second time. That is ~90ms of the ~200ms autosave stall, for
 * strings that only change when the user swaps a module.
 */
function applySlotModuleSignature(slotIndex, signature) {
    if (slotIndex < 0 || slotIndex >= SHADOW_UI_SLOTS) return false;
    if (signature !== lastSlotModuleSignatures[slotIndex]) {
        lastSlotModuleSignatures[slotIndex] = signature;
        loadChainConfigFromSlot(slotIndex);
        invalidateKnobContextCache();
        needsRedraw = true;
        return true;
    }
    return false;
}

/*
 * A module's DISPLAY NAME, from the same module.json the abbreviation comes
 * from. Populated by cacheModuleAbbrev because it already reads and parses the
 * file at both call sites -- this costs no extra I/O.
 */
const moduleNameCache = {};

/* Cache a module's abbreviation and display name from its module.json */
function cacheModuleAbbrev(json) {
    if (json && json.id && json.abbrev) {
        moduleAbbrevCache[json.id.toLowerCase()] = json.abbrev;
    }
    if (json && json.id && json.name) {
        moduleNameCache[json.id.toLowerCase()] = json.name;
    }
}

/*
 * The name to show for a module, preferring the real one.
 *
 * The param-pages header used to show only the abbreviation, so Master FX read
 * "MFX > CS" permanently for a module with no presets -- and an abbreviation
 * is a placeholder, not an identity. "MFX > CLOUDSEED" is 70px in a 70px
 * budget and "MFX > CAPICOLA" is 62px, so the names that matter here fit; the
 * long ones truncate, and a truncated name still says more than two letters.
 *
 * Falls back to the abbreviation when the module.json has not been read yet,
 * so the header never goes blank waiting for a name.
 */
function getModuleDisplayName(moduleId) {
    if (!moduleId) return "--";
    let id = String(moduleId);
    if (id.indexOf("/") >= 0) {
        const parts = id.split("/").filter(Boolean);
        if (!parts.length) return "--";
        const last = parts[parts.length - 1];
        id = (/\.[A-Za-z0-9]+$/.test(last) && parts.length >= 2)
            ? parts[parts.length - 2]
            : last;
    }
    if (!id) return "--";
    return moduleNameCache[id.toLowerCase()] || getModuleAbbrev(id);
}

/*
 * Get abbreviation for a module.
 *
 * Takes a module ID -- and defends against being handed a filesystem PATH,
 * which is how the Master FX header came to read "MFX > /D" for every module:
 * the chrome asked for a key that serves the plugin path, and the fallback
 * below happily returned the first two characters of "/data/UserData/...".
 * A wrong KEY would have been survivable, since an unserved read comes back
 * as "" and the header just loses its name; a served key with the wrong KIND
 * of value produced a confident, plausible-looking answer instead.
 *
 * The path handling is INLINE, not a helper. Several tests lift this function
 * out of the file with `new Function` and an explicit dependency list -- see
 * tests/host/test_chain_editor_snapshot.sh -- so a new free identifier here
 * is a ReferenceError there, which is the same trap drawChainEdit documents.
 *
 * Note the last path segment is NOT the id: a module path names the plugin
 * FILE (".../cloudseed/dsp.so" -- shadow_chain_mgmt.c stores dsp_path), so a
 * plain basename gives "dsp.so" and an abbreviation of "DS" for every module
 * in the fleet. The id is the directory holding the plugin.
 */
function getModuleAbbrev(moduleId) {
    if (!moduleId) return "--";
    let id = String(moduleId);
    if (id.indexOf("/") >= 0) {
        const parts = id.split("/").filter(Boolean);
        if (!parts.length) return "--";
        const last = parts[parts.length - 1];
        id = (/\.[A-Za-z0-9]+$/.test(last) && parts.length >= 2)
            ? parts[parts.length - 2]
            : last;
    }
    if (!id) return "--";
    const lower = id.toLowerCase();
    return moduleAbbrevCache[lower] || id.substring(0, 2).toUpperCase();
}


/* Param API helper functions */
function getSlotParam(slot, key) {
    if (typeof shadow_get_param !== "function") return null;
    try {
        return shadow_get_param(slot, key);
    } catch (e) {
        return null;
    }
}

/* Blocking set_param for DISCRETE multi-field commits (e.g. LFO target+param,
 * MPE recv/fwd/enable). In co-run (overtake mode) shadow_set_param is
 * fire-and-forget and shares ONE shadow_param SHM slot, so two back-to-back
 * writes race — the 2nd clobbers the 1st before the host drains it, losing the
 * earlier write (e.g. LFO target reverts to "none"). The blocking variant
 * (shadow_set_param_timeout, force_blocking) waits for each write to be consumed,
 * serializing the pair. Knob streams keep the non-blocking path. */
const SHADOW_SET_BLOCKING_TIMEOUT_MS = 200;
function shadowSetParamBlocking(slot, key, value) {
    if (typeof shadow_set_param_timeout === "function") {
        return shadow_set_param_timeout(slot, key, String(value), SHADOW_SET_BLOCKING_TIMEOUT_MS);
    }
    if (typeof shadow_set_param === "function") {
        return shadow_set_param(slot, key, String(value));
    }
    return false;
}

function setSlotParam(slot, key, value) {
    if (typeof shadow_set_param !== "function") return false;
    try {
        const ok = shadow_set_param(slot, key, String(value));
        if (!ok) return false;

        /* Re-check MIDI FX warnings immediately after sync/module changes. */
        if (key === "midi_fx1:module") {
            warningShownForMidiFxSlots.delete(slot);
        }
        if (key === "midi_fx1:sync") {
            warningShownForMidiFxSlots.delete(slot);
            if (String(value) === "clock") {
                checkAndShowMidiFxError(slot);
            }
        }

        return true;
    } catch (e) {
        return false;
    }
}

function setSlotParamWithTimeout(slot, key, value, timeoutMs) {
    const timeout = Number.isFinite(timeoutMs) ? Math.max(1, Math.floor(timeoutMs)) : 100;
    if (typeof shadow_set_param_timeout === "function") {
        try {
            return shadow_set_param_timeout(slot, key, String(value), timeout);
        } catch (e) {
            return false;
        }
    }
    return setSlotParam(slot, key, value);
}

function setSlotParamWithRetry(slot, key, value, timeoutMs, retryTimeoutMs, logLabel) {
    let ok = setSlotParamWithTimeout(slot, key, value, timeoutMs);
    if (!ok) {
        debugLog(`${logLabel} timeout slot ${slot + 1} key ${key} (retry)`);
        ok = setSlotParamWithTimeout(slot, key, value, retryTimeoutMs);
    }
    if (!ok) {
        debugLog(`${logLabel} timeout slot ${slot + 1} key ${key} (final)`);
    }
    return ok;
}

/* ---- Continuous feedback guard (boot-feedback fix, 2026-06-25) ----
 * A line-input slot (e.g. Line In) producing audio while the built-in speakers
 * are active and no cable is plugged feeds the mic straight back to the speakers.
 * The shim BYPASSES such a slot at boot (visible "B" glyph + slot:feedback_hold
 * marker), since it can't read jack state there. Here in JS — where jack state is
 * reliable — we keep guarding continuously:
 *   - risk present (speakers on, no line-in cable) -> bypass any Line In slot,
 *       at boot AND if the user unplugs headphones mid-session; announce + (when
 *       the shadow UI is on screen) raise the "Speaker Feedback Risk" modal
 *   - safe (headphones or line-in cable) -> un-bypass the guard's bypass
 *   - user un-bypasses manually (Mute+JogClick) or chooses "enable anyway" in the
 *       modal -> treat as an override; don't re-bypass until it's safe again or a
 *       reboot
 * feedback_hold marks bypasses the guard owns (vs. a user's own bypass). The
 * modal's draw/input are gated on shadow_get_display_mode()==1 so the gate can
 * never steal Move's native jog/back while the shadow UI is hidden. */
let feedbackOverride = {};       /* slot -> user enabled despite risk (until safe/reboot) */
let feedbackEpisode = {};        /* slot -> currently guard-bypassing due to risk */
let feedbackModalPending = {};   /* slot -> show the visual modal next time the UI is up */
let feedbackSafeSince = {};      /* slot -> Date.now() when the safe reading began, 0 while at risk */
let feedbackGuardModalRaised = false;  /* the active feedback modal is the guard's (auto-dismissable) */
let lineInConsumerCache = {};    /* moduleId -> bool (consumesLineInput) */

/* Safe must hold this long before the guard undoes a bypass / clears the user's
 * override, so a brief jack-sense blip (e.g. USB-C audio renegotiating) can't
 * wipe the override and storm the modal. The protective direction (risk -> bypass)
 * is never debounced. */
const FEEDBACK_SAFE_DEBOUNCE_MS = 2000;

function bootFeedbackRisk() {
    if (typeof host_speaker_active !== "function") return false;
    if (typeof host_line_in_connected !== "function") return false;
    return host_speaker_active() && !host_line_in_connected();
}

function isLineInConsumerModule(moduleId) {
    if (!moduleId) return false;
    if (moduleId in lineInConsumerCache) return lineInConsumerCache[moduleId];
    let meta = null;
    try {
        if (typeof host_get_module_metadata === "function") meta = host_get_module_metadata(moduleId);
    } catch (e) { meta = null; }
    const v = consumesLineInput(meta);
    lineInConsumerCache[moduleId] = v;
    return v;
}

/* Rotates the no-risk scan over the slots; see reconcileFeedbackHolds. */
let _feedbackScanCursor = 0;

function reconcileFeedbackHolds() {
    const risk = bootFeedbackRisk();

    /*
     * Which slots to examine THIS pass.
     *
     * Every slot needs `synth_module`, and that is a synchronous IPC round
     * trip at ~2.8ms. Four of them landed on one tick, six times a second:
     * ~11ms of blocking on top of the ~7ms the knob grid already spends, over
     * the 16.67ms period, six times a second. Measured on device via the
     * js.feedback_guard span — mean 10.84ms, and 34% of ALL js.tick time —
     * which is the whole of the unexplained "60 should be 60, is 54-56".
     * Nothing else here costs anything: bootFeedbackRisk() is two plain SHM
     * reads and isLineInConsumerModule() is memoised by module id.
     *
     * So when there is no risk, scan ONE slot per pass and rotate: same total
     * coverage, a quarter of the reads, and no tick ever takes more than one
     * of them. Each slot is still visited about every 670ms, which is well
     * inside the FEEDBACK_SAFE_DEBOUNCE_MS the safe path waits out anyway.
     *
     * Under risk, scan ALL of them, every pass. The protective direction is
     * never rate-limited — that is the same rule the bypass path itself
     * follows, and risk is a rare, transient state (someone unplugged the
     * headphones), so its cost does not sit on the steady-state frame rate.
     */
    const scan = [];
    if (risk) {
        for (let i = 0; i < SHADOW_UI_SLOTS; i++) scan.push(i);
    } else {
        scan.push(_feedbackScanCursor % SHADOW_UI_SLOTS);
        _feedbackScanCursor = (_feedbackScanCursor + 1) % SHADOW_UI_SLOTS;
    }

    for (const slot of scan) {
        const moduleId = getSlotParam(slot, "synth_module");
        if (!isLineInConsumerModule(moduleId)) {
            /* Not a line-in slot — drop any stale guard state. */
            feedbackEpisode[slot] = false;
            feedbackModalPending[slot] = false;
            feedbackOverride[slot] = false;
            feedbackSafeSince[slot] = 0;
            continue;
        }

        const bypassed = getSlotParam(slot, "synth:bypassed") === "1";
        const hold = getSlotParam(slot, "slot:feedback_hold") === "1";

        /* User manually un-bypassed a guard bypass (Mute+JogClick) → override. */
        if (hold && !bypassed) {
            setSlotParam(slot, "slot:feedback_hold", "0");
            feedbackOverride[slot] = true;
            feedbackEpisode[slot] = false;
            feedbackModalPending[slot] = false;
            debugLog(`feedback guard: slot ${slot} user-enabled (override)`);
            continue;
        }

        if (risk) {
            /* Any risk reading resets the safe-stability timer, so a brief safe
             * blip below can't undo the guard. */
            feedbackSafeSince[slot] = 0;
            if (!feedbackOverride[slot]) {
                if (!bypassed) {
                    /* Entering risk (e.g. headphones unplugged) on a live Line In.
                     * Bypass immediately — the protective direction is never
                     * debounced. */
                    setSlotParam(slot, "synth:bypassed", "1");
                    setSlotParam(slot, "slot:feedback_hold", "1");
                    debugLog(`feedback guard: slot ${slot} bypassed (feedback risk)`);
                }
                if (!feedbackEpisode[slot]) {
                    feedbackEpisode[slot] = true;
                    feedbackModalPending[slot] = true;
                    announce("Feedback risk. Audio monitoring disabled. Plug in headphones.");
                    debugLog(`feedback guard: slot ${slot} alert raised`);
                }
            }
            /* risk && override: the user chose to keep it live — leave it alone. */
        } else {
            /* Safe — but require it to hold for FEEDBACK_SAFE_DEBOUNCE_MS before
             * undoing the guard, so a single jack-sense blip can't wipe the
             * user's override and re-raise the modal every few seconds. */
            if (!feedbackSafeSince[slot]) feedbackSafeSince[slot] = Date.now();
            if (Date.now() - feedbackSafeSince[slot] >= FEEDBACK_SAFE_DEBOUNCE_MS) {
                feedbackOverride[slot] = false;
                feedbackEpisode[slot] = false;
                feedbackModalPending[slot] = false;
                if (bypassed && hold) {
                    setSlotParam(slot, "synth:bypassed", "0");
                    setSlotParam(slot, "slot:feedback_hold", "0");
                    debugLog(`feedback guard: slot ${slot} un-bypassed (safe)`);
                }
                /* If the risk cleared while the modal was up (e.g. headphones
                 * plugged back in), dismiss it — the slot is already enabled. */
                if (feedbackGuardModalRaised && feedbackGateActive() && feedbackGateCancel()) {
                    feedbackGuardModalRaised = false;
                    announce("Headphones connected. Line In enabled.");
                    debugLog(`feedback guard: modal dismissed (safe)`);
                }
            }
        }
    }
    pumpFeedbackAlertModal();
}

/* Raise the "Speaker Feedback Risk" modal for a pending slot, but only while the
 * shadow UI is actually on screen (display_mode==1) — otherwise the gate's input
 * handler would intercept Move's native jog/back. The active modal swallows
 * input, so the user can't dismiss the UI without answering. */
function pumpFeedbackAlertModal() {
    if (typeof shadow_get_display_mode !== "function") return;
    if (shadow_get_display_mode() !== 1) return;
    if (feedbackGateActive()) return;
    for (let slot = 0; slot < SHADOW_UI_SLOTS; slot++) {
        if (!feedbackModalPending[slot]) continue;
        feedbackModalPending[slot] = false;
        feedbackGuardModalRaised = true;
        confirmLineInput("Line In", (ok) => {
            feedbackGuardModalRaised = false;
            if (ok) {
                /* Enable anyway → un-bypass + override (no re-bypass until safe/reboot). */
                setSlotParam(slot, "synth:bypassed", "0");
                setSlotParam(slot, "slot:feedback_hold", "0");
                feedbackOverride[slot] = true;
                feedbackEpisode[slot] = false;
                debugLog(`feedback guard: slot ${slot} enabled by user (modal)`);
            } else {
                /* Keep bypassed — the "B" glyph remains; Mute+JogClick enables later. */
                debugLog(`feedback guard: slot ${slot} kept bypassed (modal)`);
            }
        }, {
            title: "Feedback Risk",
            lines: ["Audio monitoring", "disabled.", "Plug in headphones."],
            footer: "Back:No  Jog:Override",
            announceText: "Feedback risk. Audio monitoring disabled. Plug in headphones.",
        });
        return;  /* one modal at a time */
    }
}

/* Tear down a raised feedback-guard modal before an overtake surface takes over.
 * The gate's input handler only runs at display_mode==1, so under an overtake
 * tool the modal can't be answered — Back leaks through and exits the tool
 * (issue #158). Cancel the modal but keep the slot bypassed and re-arm it
 * pending, so pumpFeedbackAlertModal re-raises it once the shadow UI is back on
 * screen. */
function deferFeedbackModalForOvertake() {
    if (!feedbackGuardModalRaised) return;
    if (!(feedbackGateActive() && feedbackGateCancel())) return;
    feedbackGuardModalRaised = false;
    /* Re-arm on every line-in slot still under the guard (bypassed + held), so
     * the modal returns when the user is back in the shadow UI. */
    for (let slot = 0; slot < SHADOW_UI_SLOTS; slot++) {
        if (!isLineInConsumerModule(getSlotParam(slot, "synth_module"))) continue;
        if (getSlotParam(slot, "synth:bypassed") === "1" &&
            getSlotParam(slot, "slot:feedback_hold") === "1") {
            feedbackModalPending[slot] = true;
        }
    }
    debugLog("feedback guard: modal deferred for overtake entry");
}

/* Scan modules directory for audio_fx modules */
function scanForAudioFxModules() {
    const MODULES_DIR = "/data/UserData/schwung/modules";
    const AUDIO_FX_DIR = `${MODULES_DIR}/audio_fx`;
    const result = [{ id: "", name: "None", dspPath: "" }];

    /* Helper to scan a directory for audio_fx modules */
    function scanDir(dirPath) {
        try {
            const entries = os.readdir(dirPath) || [];
            const dirList = entries[0];
            if (!Array.isArray(dirList)) return;

            for (const entry of dirList) {
                if (entry === "." || entry === "..") continue;

                const modulePath = `${dirPath}/${entry}/module.json`;
                try {
                    const content = std.loadFile(modulePath);
                    if (!content) continue;

                    const json = JSON.parse(content);
                    cacheModuleAbbrev(json);
                    /* Check if this is an audio_fx module */
                    if (json.component_type === "audio_fx" ||
                        (json.capabilities && json.capabilities.component_type === "audio_fx")) {
                        const dspFile = json.dsp || "dsp.so";
                        const dspPath = `${dirPath}/${entry}/${dspFile}`;
                        result.push({
                            id: json.id || entry,
                            name: json.name || entry,
                            dspPath: dspPath
                        });
                    }
                } catch (e) {
                    /* Skip modules without readable module.json */
                }
            }
        } catch (e) {
            /* Failed to read directory */
        }
    }

    /* Scan audio_fx directory for all audio effects */
    scanDir(AUDIO_FX_DIR);

    /* Sort modules alphabetically by name, keeping "None" at the top */
    const noneItem = result[0];
    const modules = result.slice(1);
    modules.sort((a, b) => a.name.localeCompare(b.name));
    /* Add option to get more modules from store at the end */
    return [noneItem, ...modules, { id: "__get_more__", name: "[Get more...]" }];
}

/* Scan modules directory for overtake modules */
function scanForOvertakeModules() {
    const MODULES_DIR = "/data/UserData/schwung/modules";
    const result = [];

    debugLog("scanForOvertakeModules starting");

    /* Helper to check a directory for an overtake module */
    function checkDir(dirPath, name) {
        const modulePath = `${dirPath}/module.json`;
        try {
            const content = std.loadFile(modulePath);
            if (!content) return;

            const json = JSON.parse(content);
            debugLog(name + ": component_type=" + json.component_type);
            /* Check if this is an overtake module */
            if (json.component_type === "overtake" ||
                (json.capabilities && json.capabilities.component_type === "overtake")) {
                /* Skip modules whose required path doesn't exist on device */
                if (json.requires_path && !host_file_exists(json.requires_path)) {
                    debugLog("SKIP overtake (requires_path missing): " + json.name + " needs " + json.requires_path);
                    return;
                }
                debugLog("FOUND overtake: " + json.name);
                result.push({
                    id: json.id || name,
                    name: json.name || name,
                    path: dirPath,
                    uiPath: `${dirPath}/${json.ui || 'ui.js'}`,
                    dsp: json.dsp || null,
                    basePath: dirPath,
                    capabilities: json.capabilities || null
                });
            }
        } catch (e) {
            /* Skip directories without readable module.json */
        }
    }

    /* Scan the modules directory for overtake modules */
    try {
        const entries = os.readdir(MODULES_DIR) || [];
        debugLog("readdir result: " + JSON.stringify(entries));
        const dirList = entries[0];
        if (!Array.isArray(dirList)) {
            debugLog("dirList not an array, returning empty");
            return result;
        }
        debugLog("found entries: " + dirList.join(", "));

        for (const entry of dirList) {
            if (entry === "." || entry === "..") continue;

            const entryPath = `${MODULES_DIR}/${entry}`;

            /* Check if this entry itself is a module */
            checkDir(entryPath, entry);

            /* Also scan subdirectories (utilities/, sound_generators/, etc.) */
            try {
                const subEntries = os.readdir(entryPath) || [];
                const subDirList = subEntries[0];
                if (Array.isArray(subDirList)) {
                    for (const subEntry of subDirList) {
                        if (subEntry === "." || subEntry === "..") continue;
                        checkDir(`${entryPath}/${subEntry}`, subEntry);
                    }
                }
            } catch (e) {
                /* Not a directory or can't read, skip */
            }
        }
    } catch (e) {
        debugLog("scan error: " + e);
        /* Failed to read modules directory */
    }

    result.sort((a, b) => a.name.localeCompare(b.name));
    debugLog("returning " + result.length + " modules");
    return result;
}

/* Invoke a module's optional onUnload() callback before we clear callbacks.
 * Wrapped in try/catch so a buggy module can't block the exit path. As a safety
 * net, we also emit "all notes off" on every MIDI channel so any hanging note
 * from a sequencer module gets released even if the module's onUnload missed it. */
function invokeModuleOnUnload(callbacks, moduleId) {
    if (callbacks && typeof callbacks.onUnload === "function") {
        try {
            debugLog("invokeModuleOnUnload: " + moduleId);
            callbacks.onUnload();
        } catch (e) {
            debugLog("invokeModuleOnUnload(" + moduleId + ") threw: " + e);
        }
    }
    if (typeof shadow_send_midi_to_dsp === "function") {
        for (let ch = 0; ch < 16; ch++) {
            shadow_send_midi_to_dsp([0xB0 | ch, 123, 0]);  // CC 123 = All Notes Off
        }
    }
}

/* Invoke a module's optional onResume() callback once per suspend→resume.
 *
 * Module-facing contract: onResume() runs each time the user returns to
 * an already-loaded overtake module that was suspended (another module or
 * MoveOriginal had been brought to the front). It is NOT a second init()
 * — init() ran once at load and is not repeated — it is the signal to
 * re-establish whatever state does not survive being backgrounded. Notably
 * the hardware LEDs are cleared while suspended, so a module that paints
 * LEDs should force a full repaint here. Optional: a module that needs
 * nothing on resume simply omits it.
 *
 * Wrapped in try/catch so a buggy module can't break the resume path. */
/* Every gesture that starts or resumes an overtake module holds Shift
 * (Shift+Vol+jog click to launch, Shift+Vol+Step13 / Shift+long-press to
 * resume). The release lands during the blackout when the module is not yet
 * receiving MIDI, so it is discarded and both the host and the module stay
 * latched in shift-mode for the whole session — issue #191.
 *
 * It does not present as one bug. In timncox's module it showed up as a knob
 * editing a base value instead of writing a parameter lock, a pad loading
 * machine +21, and the slot pad opening the sample browser: three unrelated
 * hardware reports, one cause.
 *
 * loadOvertakeModule already clears hostShiftHeld, but too early — the stale
 * state arrives afterwards, once MIDI starts flowing. So repair it at the
 * point the blackout actually ends: clear the host flag and synthesise the
 * release the module never got.
 *
 * Width of the window is the module's own init(): ~300ms for a small module,
 * measured at ~6s for a moderate one, which is why this reproduces reliably
 * for some modules and never for others.
 *
 * Unconditional by design. A Shift-up delivered to a module that already
 * thinks Shift is up is a no-op, whereas guessing from the shim's own
 * shift_held is not safe here — shadow_ui tracks Shift locally precisely
 * because the shim's tracking does not hold up in overtake mode. The cost of
 * being wrong is that someone still physically holding Shift through init has
 * to re-press it; the cost of not doing it is a latched session. */
function repairSwallowedShiftRelease(reason) {
    hostShiftHeld = false;
    hostVolumeKnobTouched = false;
    if (!overtakeModuleCallbacks || !overtakeModuleCallbacks.onMidiMessageInternal) return;
    try {
        runToolCallback(function() {
            overtakeModuleCallbacks.onMidiMessageInternal([0xB0, 49, 0]);  /* CC 49 = Shift, released */
        });
        debugLog("repairSwallowedShiftRelease(" + reason + "): synthesised Shift-up");
    } catch (e) {
        debugLog("repairSwallowedShiftRelease(" + reason + ") threw: " + e);
    }
}

function invokeModuleOnResume(callbacks, moduleId) {
    if (callbacks && typeof callbacks.onResume === "function") {
        try {
            debugLog("invokeModuleOnResume: " + moduleId);
            callbacks.onResume();
        } catch (e) {
            debugLog("invokeModuleOnResume(" + moduleId + ") threw: " + e);
        }
    }
}

/* Enter the overtake module selection menu */
function enterOvertakeMenu() {
    /* A raised feedback-guard modal can't be answered once we leave the shadow
     * UI — defer it so Back doesn't leak into the menu/tool (issue #158). */
    deferFeedbackModalForOvertake();
    /* Flush set state before entering overtake — periodic autosave is gated
     * on !isOvertakeActive, so without this a slot change made seconds before
     * Shift+Vol+Jog would be lost on reboot. */
    autosaveAllSlots();
    saveMasterFxChainConfig();
    saveChainConfigToDir(activeSlotStateDir);
    debugLog("enterOvertakeMenu: flushed set state before overtake");

    /* Reset overtake state — but preserve it if a tool has a hidden session
     * (DSP still running, waiting for reconnect). */
    if (!toolHiddenFile) {
        overtakeModuleLoaded = false;
        overtakeModulePath = "";
        overtakeModuleId = "";
    }
    overtakeModuleCallbacks = null;
    overtakeExitPending = false;
    overtakeInitPending = false;
    overtakeInitTicks = 0;
    ledClearIndex = 0;

    /* Enable overtake mode 1 (menu) - only UI events forwarded, not all MIDI */
    if (typeof shadow_set_overtake_mode === "function") {
        shadow_set_overtake_mode(1);  /* 1 = menu mode (UI events only) */
        debugLog("enterOvertakeMenu: overtake_mode=1 (menu)");
    }

    overtakeModules = scanForOvertakeModules();
    /* Add [Get more...] option at the end */
    overtakeModules.push({ id: "__get_more__", name: "[Get more...]", path: null, uiPath: null });
    overtakeModules.push({ id: "__back_to_move__", name: "[Back to Move]", path: null, uiPath: null });
    selectedOvertakeModule = 0;
    previousView = view;
    setView(VIEWS.OVERTAKE_MENU);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = overtakeModules[0]?.name || "None";
    announce(`Overtake Menu, ${moduleName}`);
}

/* Overtake exit state - clear LEDs before returning to Move */
let overtakeExitPending = false;

/* Exit overtake mode back to Move */
function exitOvertakeMode() {
    corunTeardown();
    /* Flush set state on the way out — defensive, since chain state is not
     * edited during overtake, but keeps the invariant "all transitions
     * persist state" honest. */
    autosaveAllSlots();
    saveMasterFxChainConfig();
    saveChainConfigToDir(activeSlotStateDir);
    debugLog("exitOvertakeMode: flushed set state on exit");

    /* Let the module clean up (send note-offs, etc.) before we tear callbacks down. */
    invokeModuleOnUnload(overtakeModuleCallbacks, overtakeModuleId);

    /* Deactivate LED queue before cleanup - restores original move_midi_internal_send */
    deactivateLedQueue();

    /* Unload overtake DSP if loaded */
    unloadOvertakeDsp();
    delete globalThis.host_module_set_param;
    delete globalThis.host_module_set_param_blocking;
    delete globalThis.host_module_get_param;

    /* Write exiting module ID so shim runs the correct per-module hook */
    debugLog("exitOvertakeMode: overtakeModuleId=" + overtakeModuleId + " host_write_file=" + (typeof host_write_file));
    if (overtakeModuleId && typeof host_write_file === "function") {
        var writeResult = host_write_file("/data/UserData/schwung/hooks/.exiting-module-id", overtakeModuleId);
        debugLog("exitOvertakeMode: wrote module ID file, result=" + writeResult);
    } else {
        debugLog("exitOvertakeMode: SKIPPED file write - id=" + overtakeModuleId + " fn=" + (typeof host_write_file));
    }

    /* If this module is in the suspended map (shouldn't be normally — active
     * means not-suspended — but exit-from-paused flows can land here), drop it. */
    if (overtakeModuleId && suspendedOvertakes[overtakeModuleId]) {
        delete suspendedOvertakes[overtakeModuleId];
    }

    overtakeModuleLoaded = false;
    overtakeModulePath = "";
    overtakeModuleId = "";
    overtakeModuleCallbacks = null;
    overtakeModuleCaps = null;
    overtakeSuspendKeepsJs = false;
    overtakeSuspendSelfManaged = false;
    overtakePassthroughCCs = [];
    if (typeof shadow_set_param_timeout === "function") {
        shadow_set_param_timeout(0, "passthrough", "", 100);  /* clear list */
    } else if (typeof shadow_set_param === "function") {
        shadow_set_param(0, "passthrough", "");
    }

    /* Reset encoder accumulation */
    for (let k = 0; k < NUM_KNOBS; k++) overtakeKnobDelta[k] = 0;
    overtakeJogDelta = 0;

    /* NOTE: skip_led_clear is consumed by the C-side when it observes the
     * overtake_mode transition. JS must not clear it in the same tick. */

    /* Signal exit — C-side LED cache will restore Move's LEDs
     * when overtake_mode transitions back to 0 */
    overtakeExitPending = true;
    needsRedraw = true;
}

/* Suspend overtake mode — leave background processes running */
function suspendOvertakeMode() {
    corunTeardown();
    /* Capabilities of the module being parked, for the LED-handoff decision. */
    const parkedCaps = (overtakeModuleCallbacks && overtakeModuleCaps) ? overtakeModuleCaps : null;
    if (overtakeSuspendKeepsJs && overtakeModuleCallbacks && overtakeModuleId) {
        debugLog("suspendOvertakeMode: suspend_keeps_js — parking " + overtakeModuleId + " in background");

        /* Snapshot LED state BEFORE deactivation clears the queues. Modules that
         * only write LEDs on-change rely on this snapshot to reappear on resume. */
        const ledNotesSnapshot = Object.assign({}, ledQueueNotes);
        const ledCCsSnapshot = Object.assign({}, ledQueueCCs);

        deactivateLedQueue();

        /* Pending init? Call it now — the module expects init() before its first tick. */
        if (overtakeInitPending && overtakeModuleCallbacks.init) {
            overtakeInitPending = false;
            ledClearIndex = 0;
            try { overtakeModuleCallbacks.init(); } catch (e) {
                debugLog("suspendOvertakeMode: init() threw " + e);
            }
        }

        /* Reset encoder accumulation — any pending deltas belong to the pre-suspend UI. */
        for (let k = 0; k < NUM_KNOBS; k++) overtakeKnobDelta[k] = 0;
        overtakeJogDelta = 0;

        /* Park this module in the suspended map. Callbacks stay alive via closure.
         * Stash dspPath so resume can detect "another tool clobbered slot 0" and
         * reload the DSP (Bug C, 2026-05-04).
         * Snapshot the param-shim handles too: chain-component editors mutate
         * globalThis.host_module_get_param/_set_param/_set_param_blocking, which
         * would otherwise leave the parked tick talking to the wrong shim
         * (or to a deleted global → SHIM MISS). The parked-tick loop swaps
         * these in for the duration of each tick(); resume restores them
         * back to globalThis. (Bug D, 2026-05-06.) */
        suspendedOvertakes[overtakeModuleId] = {
            id: overtakeModuleId,
            path: overtakeModulePath,
            callbacks: overtakeModuleCallbacks,
            ledNotes: ledNotesSnapshot,
            ledCCs: ledCCsSnapshot,
            dspPath: currentSlot0DspPath,
            caps: overtakeModuleCaps,
            selfManaged: overtakeSuspendSelfManaged,
            shimGet: globalThis.host_module_get_param,
            shimSet: globalThis.host_module_set_param,
            shimSetBlocking: globalThis.host_module_set_param_blocking
        };
        lastSuspendedToolId = overtakeModuleId;

        /* Clear active-module state so a different module can be loaded next. */
        overtakeModuleLoaded = false;
        overtakeModuleCallbacks = null;
        overtakeSuspendKeepsJs = false;
        overtakeSuspendSelfManaged = false;

        /* Tell shim to skip exit hook (module stays loaded). */
        if (typeof shadow_set_suspend_overtake === "function") {
            shadow_set_suspend_overtake(1);
        }

        /* Ask the audio-side transition to leave Move's fresh native LED output
         * authoritative. Mono's entry snapshot can be incomplete for dynamic
         * scale colors and the Shift row, so replaying it here leaves the grid
         * dark or stale. The C-side consumes skip_led_clear after mode reaches 0.
         *
         * Opt-in per module. This used to fire for EVERY suspend_keeps_js
         * module, which regressed the ones it was not written for: "let Move
         * repaint" only clears the module's LEDs if Move actually has a reason
         * to repaint those surfaces. Performance FX suspends with 32 lit pads
         * that Move never touches, so its colors just stayed on the hardware.
         * Without the flag we fall back to the ordinary snapshot restore, which
         * puts Move's pre-overtake LEDs back and turns unknowns off — correct
         * for anything that does not own a dynamic native layout. */
        const wantsNativeRepaint = !!(parkedCaps && parkedCaps.native_led_repaint_on_suspend);
        if (typeof shadow_set_overtake_mode === "function") {
            if (wantsNativeRepaint && typeof shadow_set_skip_led_clear === "function") {
                debugLog("suspendOvertakeMode: module wants native LED repaint");
                shadow_set_skip_led_clear(1);
            }
            shadow_set_overtake_mode(0);
        }
        /* Clear the opt-in sysex suppression so it never leaks to the next tool. */
        if (typeof shadow_set_overtake_suppress_sysex === "function") {
            shadow_set_overtake_suppress_sysex(0);
        }

        /* Dismiss shadow UI entirely so Move's native UI returns. */
        setView(VIEWS.SLOTS);
        if (typeof shadow_request_exit === "function") {
            shadow_request_exit();
        }
        needsRedraw = true;
        return;
    }

    debugLog("suspendOvertakeMode: suspending overtake, JACK keeps running");

    /* Deactivate LED queue */
    deactivateLedQueue();

    /* Do NOT unload overtake DSP — JACK stays running */

    /* Clean up JS state */
    delete globalThis.host_module_set_param;
    delete globalThis.host_module_set_param_blocking;
    delete globalThis.host_module_get_param;

    overtakeModuleLoaded = false;
    overtakeModulePath = "";
    overtakeModuleId = "";
    overtakeModuleCallbacks = null;

    /* Reset encoder accumulation */
    for (let k = 0; k < NUM_KNOBS; k++) overtakeKnobDelta[k] = 0;
    overtakeJogDelta = 0;

    /* Tell shim to skip exit hook on overtake_mode transition.
     * Must write directly to shadow_control (not via param interface)
     * to guarantee the flag is set before overtake_mode drops to 0. */
    if (typeof shadow_set_suspend_overtake === "function") {
        shadow_set_suspend_overtake(1);
    }

    /* JACK display override is cleared by the shim on overtake_mode transition.
     * No need to set jack:display here — the shim always clears it on exit. */

    /* Begin LED clearing ceremony, then return to Move */
    overtakeExitPending = true;
    needsRedraw = true;
}

/* Resume a previously suspended overtake module by id.
 * Called when the user re-selects a suspended module in the overtake menu. */
function resumeOvertakeModule(moduleId) {
    const parked = suspendedOvertakes[moduleId];
    if (!parked || !parked.callbacks) return false;
    debugLog("resumeOvertakeModule: resuming " + moduleId);

    /* Restore active-module state from the parked entry. */
    overtakeModuleCallbacks = parked.callbacks;
    overtakeModuleCaps = parked.caps || overtakeModuleCaps;
    overtakeModulePath = parked.path;
    overtakeModuleId = parked.id;
    overtakeModuleLoaded = true;
    overtakeSuspendKeepsJs = true;
    overtakeSuspendSelfManaged = !!parked.selfManaged;
    overtakeInitPending = false;  /* Already ran */
    overtakeExitPending = false;

    /* Re-activate LED queue and restore the snapshot so LEDs the module had set
     * before suspend reappear. Needed for modules that write LEDs on-change. */
    activateLedQueue();
    if (parked.ledNotes) Object.assign(ledQueueNotes, parked.ledNotes);
    if (parked.ledCCs) Object.assign(ledQueueCCs, parked.ledCCs);

    /* Bug C fix: slot-0 overtake DSP is single-tenant. If another tool loaded
     * its DSP since we suspended, ours got destroyed — reload before the JS
     * module starts polling host_module_get_param against a dead/wrong DSP. */
    if (parked.dspPath && parked.dspPath !== currentSlot0DspPath) {
        debugLog("resumeOvertakeModule: slot 0 DSP mismatch (current=" +
                 (currentSlot0DspPath || "(empty)") + " parked=" + parked.dspPath +
                 ") — reloading DSP");
        loadOvertakeDsp(parked.dspPath);
    }

    /* Bug D fix: chain-component editor may have overwritten or deleted the
     * overtake param-shim globals while we were parked. Restore the snapshot
     * captured at suspend so the resumed module's tick/UI sees its own shims
     * regardless of what's currently on globalThis. */
    if (parked.shimGet) globalThis.host_module_get_param = parked.shimGet;
    if (parked.shimSet) globalThis.host_module_set_param = parked.shimSet;
    if (parked.shimSetBlocking) globalThis.host_module_set_param_blocking = parked.shimSetBlocking;

    delete suspendedOvertakes[moduleId];
    if (lastSuspendedToolId === moduleId) lastSuspendedToolId = "";

    if (typeof shadow_set_suspend_overtake === "function") {
        shadow_set_suspend_overtake(0);
    }
    if (typeof shadow_set_overtake_mode === "function") {
        shadow_set_overtake_mode(2);
    }

    setView(VIEWS.OVERTAKE_MODULE);
    needsRedraw = true;

    /* Sent before onResume() so a module that tracks its own modifiers can
     * still override in the hook. */
    repairSwallowedShiftRelease("resume");

    /* Fire the module's onResume() hook (init() is NOT re-run on resume).
     * Called after the full callback / LED-queue / shim restore above so
     * the module sees its own globals in place. See invokeModuleOnResume
     * for the module-facing contract. */
    invokeModuleOnResume(overtakeModuleCallbacks, overtakeModuleId);
    /* Flush restored LEDs to SHM so they render this frame. */
    flushLedQueue();
    return true;
}

/* Direct exit for interactive tools - skip LED clearing ceremony */
function exitToolOvertake() {
    corunTeardown();
    debugLog("exitToolOvertake: direct tool exit, nonOvertake=" + toolNonOvertake);

    /* Let the module clean up before teardown. */
    invokeModuleOnUnload(overtakeModuleCallbacks, overtakeModuleId);

    /* Deactivate LED queue (no-op if never activated for non-overtake tools) */
    deactivateLedQueue();

    /* Unload overtake DSP */
    unloadOvertakeDsp();

    /* Evict any parked suspend_keeps_js modules. Their DSP was already
     * unloaded when *this* foreground module loaded its own (the shim
     * has only one overtake_dsp slot — see schwung_shim.c:1284-1296),
     * so they're talking to nothing. Worse, we're about to delete the
     * host_module_set_param/get_param shims below; if we leave parked
     * entries in place the parked-tick loop (see ~line 13745) keeps
     * calling their tick() into deleted globals — silent set-failures
     * and 100% null get-reads, indefinitely. Fully unload them now. */
    var parkedIds = Object.keys(suspendedOvertakes);
    for (var pi = 0; pi < parkedIds.length; pi++) {
        var pid = parkedIds[pi];
        var parked = suspendedOvertakes[pid];
        if (parked && parked.callbacks) {
            invokeModuleOnUnload(parked.callbacks, pid);
        }
        delete suspendedOvertakes[pid];
    }
    if (parkedIds.length > 0) {
        debugLog("exitToolOvertake: evicted " + parkedIds.length + " parked module(s): " + parkedIds.join(","));
        if (lastSuspendedToolId && parkedIds.indexOf(lastSuspendedToolId) >= 0) {
            lastSuspendedToolId = "";
        }
    }

    /* Clean up shims */
    delete globalThis.host_module_set_param;
    delete globalThis.host_module_set_param_blocking;
    delete globalThis.host_module_get_param;
    delete globalThis.host_exit_module;
    delete globalThis.host_suspend_overtake;
    delete globalThis.host_hide_module;

    /* Write exiting module ID so shim runs the correct per-module hook */
    if (overtakeModuleId && typeof host_write_file === "function") {
        host_write_file("/data/UserData/schwung/hooks/.exiting-module-id", overtakeModuleId);
    }

    /* Reset overtake state */
    overtakeModuleLoaded = false;
    overtakeModulePath = "";
    overtakeModuleId = "";
    overtakeModuleCallbacks = null;
    overtakeExitPending = false;
    overtakeInitPending = false;

    /* Reset encoder accumulation */
    for (let k = 0; k < NUM_KNOBS; k++) overtakeKnobDelta[k] = 0;
    overtakeJogDelta = 0;

    /* Disable overtake mode. The C-side consumes skip_led_clear when it
     * observes this transition; clearing it here would race the audio thread. */
    if (!toolNonOvertake && typeof shadow_set_overtake_mode === "function") {
        shadow_set_overtake_mode(0);
    }

    /* Return to tools menu — preserve hidden session state if one exists
     * (this tool exit may be from a *different* tool, e.g. Song Mode) */
    toolOvertakeActive = false;
    toolNonOvertake = false;
    if (!toolHiddenFile) {
        /* No hidden session — clean slate */
    } else {
        /* Hidden session exists from a different tool — don't clear it */
    }
    enterToolsMenu();
}

/* Hide an interactive tool - exit overtake but keep DSP loaded */
function hideToolOvertake() {
    corunTeardown();
    debugLog("hideToolOvertake: hiding tool, keeping DSP");

    /* Deactivate LED queue */
    deactivateLedQueue();

    /* Do NOT unload overtake DSP — it stays running */
    /* Do NOT delete host_module_set_param/get_param shims — they'll be reused */

    /* Clean up module callbacks only (JS UI is being torn down) */
    overtakeModuleCallbacks = null;

    /* Reset encoder accumulation */
    for (let k = 0; k < NUM_KNOBS; k++) overtakeKnobDelta[k] = 0;
    overtakeJogDelta = 0;

    /* Exit overtake mode — restore Move's LEDs and input */
    if (!toolNonOvertake && typeof shadow_set_overtake_mode === "function") {
        shadow_set_overtake_mode(0);
    }

    /* Mark as hidden, not fully exited */
    toolOvertakeActive = false;
    toolNonOvertake = false;
    /* Use sentinel "_hidden_" when no file was selected (e.g. fileless tools like
     * Wave Edit that removed input_extensions). Without this, toolHiddenFile stays
     * empty, dspAlreadyLoaded = "" && ... = false, and reconnect is never detected. */
    toolHiddenFile = toolSelectedFile || "_hidden_";
    toolHiddenModulePath = overtakeModulePath;  /* Remember module path independently */
    debugLog("hideToolOvertake: toolHiddenFile set to '" + toolHiddenFile + "' overtakeModuleLoaded=" + overtakeModuleLoaded + " overtakeModulePath=" + overtakeModulePath);

    /* Keep these set so re-launch can detect existing session:
     * overtakeModuleLoaded stays true
     * overtakeModulePath stays set */

    enterToolsMenu();
}

/* Complete the exit after LEDs are cleared.
 * skipNavigation: tear down only, leave the view alone. Used by callers that
 * are about to navigate somewhere themselves (the Tools shortcut) — without
 * it the non-tool tail below would fire shadow_request_exit() and bounce the
 * user all the way out to Move. */
function completeOvertakeExit(skipNavigation) {
    overtakeExitPending = false;

    /* Disable overtake mode to allow MIDI to reach Move again */
    if (typeof shadow_set_overtake_mode === "function") {
        shadow_set_overtake_mode(0);
    }

    /* The C-side consumes skip_led_clear when it observes the transition.
     * Do not clear it here: JS and the audio thread run independently. */

    /* Drop end-of-chain FX placement — the next module must not inherit it. */
    if (typeof shadow_set_overtake_fx_end_of_chain === "function") {
        shadow_set_overtake_fx_end_of_chain(0);
    }

    /* If exiting an interactive tool, return to tools menu instead of Move */
    if (toolOvertakeActive) {
        toolOvertakeActive = false;
        if (!skipNavigation) enterToolsMenu();
        return;
    }

    if (skipNavigation) return;

    /* Return to slots view */
    setView(VIEWS.SLOTS);
    needsRedraw = true;
    /* Request exit from shadow UI to return to Move */
    if (typeof shadow_request_exit === "function") {
        shadow_request_exit();
    }
}

/* Load and run an overtake module */
function loadOvertakeModule(moduleInfo, skipOvertake) {
    debugLog("loadOvertakeModule called with: " + JSON.stringify(moduleInfo) + " skipOvertake=" + !!skipOvertake);
    if (!moduleInfo || !moduleInfo.uiPath) {
        debugLog("loadOvertakeModule: no moduleInfo or uiPath");
        return false;
    }
    /* A raised feedback-guard modal can't be answered under an overtake tool —
     * defer it so Back doesn't leak into the tool (issue #158). */
    deferFeedbackModalForOvertake();

    /* Flush set state before entering overtake — covers the Tools-menu path
     * (enterToolsMenu → pick tool) which bypasses enterOvertakeMenu, and the
     * suspended-module resume path below. Periodic autosave is suppressed
     * once view == OVERTAKE_MODULE, so recent slot edits would otherwise be
     * lost on reboot. */
    autosaveAllSlots();
    saveMasterFxChainConfig();
    saveChainConfigToDir(activeSlotStateDir);
    debugLog("loadOvertakeModule: flushed set state before overtake");

    /* If this module is already running in the background (suspended), just resume it
     * instead of reloading. User re-picking a suspended module from the overtake menu
     * should pop them back into it with state intact. */
    if (moduleInfo.id && suspendedOvertakes[moduleInfo.id]) {
        debugLog("loadOvertakeModule: " + moduleInfo.id + " is suspended — resuming");
        return resumeOvertakeModule(moduleInfo.id);
    }

    try {
        /* Step 1: Set overtake mode.
         * Non-overtake tools stay at mode 0 — jog/click/back already forwarded to shadow,
         * and everything else (pads, steps, knobs) passes through to Move normally.
         * If skip_led_clear, tell C-side to preserve LED state on overtake entry. */
        if (!skipOvertake && typeof shadow_set_overtake_mode === "function") {
            const wantSkipLed = !!(moduleInfo.capabilities && moduleInfo.capabilities.skip_led_clear);
            if (typeof shadow_set_skip_led_clear === "function") {
                /* Set both branches explicitly so an interrupted prior tool
                 * cannot leak native-LED ownership into this module. */
                shadow_set_skip_led_clear(wantSkipLed ? 1 : 0);
                if (wantSkipLed) debugLog("loadOvertakeModule: skip_led_clear set");
            }
            shadow_set_overtake_mode(2);  /* 2 = module mode (all events) */
            debugLog("loadOvertakeModule: overtake_mode=2 (module)");
        }

        /* Reset escape state variables for clean state */
        hostShiftHeld = false;
        hostVolumeKnobTouched = false;
        debugLog("loadOvertakeModule: escape state reset");

        /* Audio-FX overtake modules that declare `end_of_chain` process the
         * final Move+ME mix rather than the ME bus alone, so a whole-mix effect
         * hears Move's own tracks without Link Audio routing. Cleared on exit
         * in completeOvertakeExit(). */
        overtakeModuleCaps = moduleInfo.capabilities || null;

        if (typeof shadow_set_overtake_fx_end_of_chain === "function") {
            const eoc = !!(moduleInfo.capabilities && moduleInfo.capabilities.end_of_chain);
            shadow_set_overtake_fx_end_of_chain(eoc ? 1 : 0);
            if (eoc) debugLog("loadOvertakeModule: end_of_chain FX placement enabled");
        }

        /* A pending exit from a *previous* module must never survive into this
         * load. The exit branch in the OVERTAKE_MODULE tick is checked before
         * the init branch, so a stale flag tears the module down on its first
         * tick and init() never runs — the module looks like it silently
         * refuses to load. Every arm-then-leave-the-view path is supposed to
         * drain the flag itself; this is the backstop. */
        overtakeExitPending = false;

        /* Activate LED queue before loading module - intercepts move_midi_internal_send
         * to prevent SHM buffer flooding from modules that send many LEDs per tick.
         * Non-overtake tools don't own LEDs, so skip this.
         * skip_led_clear modules also don't own LEDs — Move's LEDs pass through. */
        const wantLedQueue = !skipOvertake && !(moduleInfo.capabilities && moduleInfo.capabilities.skip_led_clear);
        if (wantLedQueue) activateLedQueue();

        /* Save current globals before loading - module may overwrite them */
        const savedInit = globalThis.init;
        const savedTick = globalThis.tick;
        const savedMidi = globalThis.onMidiMessageInternal;

        overtakeModulePath = moduleInfo.uiPath;
        overtakeModuleId = moduleInfo.id || "";
        setView(VIEWS.OVERTAKE_MODULE);
        needsRedraw = true;

        /* Step 2: Load DSP plugin if the module has one (before JS load so params work) */
        if (moduleInfo.dsp && typeof shadow_set_param === "function") {
            const dspPath = moduleInfo.basePath + "/" + moduleInfo.dsp;
            debugLog("loadOvertakeModule: loading DSP from " + dspPath);
            loadOvertakeDsp(dspPath);
        }

        /* Step 3: Install host_module_set_param / host_module_get_param shims BEFORE
         * loading the module JS. QuickJS ES modules resolve bare global identifiers at
         * compile time — if the identifier doesn't exist on globalThis when the module
         * is evaluated, it won't be found later even if added afterwards. */
        globalThis.host_module_set_param = function(key, value) {
            if (typeof shadow_set_param === "function") {
                return shadow_set_param(0, "overtake_dsp:" + key, String(value));
            }
        };
        /* Blocking set_param that waits for the shim to process the request.
         * Use for critical params (e.g. file_path) that must be delivered
         * before a subsequent get_param reads the result. In overtake mode
         * the normal set_param is fire-and-forget, which can lose params
         * when multiple rapid writes hit the single shared-memory slot. */
        globalThis.host_module_set_param_blocking = function(key, value, timeoutMs) {
            var timeout = (typeof timeoutMs === "number" && timeoutMs > 0) ? timeoutMs : 500;
            if (typeof shadow_set_param_timeout === "function") {
                return shadow_set_param_timeout(0, "overtake_dsp:" + key, String(value), timeout);
            }
            if (typeof shadow_set_param === "function") {
                return shadow_set_param(0, "overtake_dsp:" + key, String(value));
            }
        };
        globalThis.host_module_get_param = function(key) {
            if (typeof shadow_get_param === "function") {
                return shadow_get_param(0, "overtake_dsp:" + key);
            }
            return null;
        };
        /* Bulk get/set: ONE round-trip for many keys (the per-key wait is the
         * cost, not the in-module get/set). `blob` is the module's
         * length-prefixed payload; key is just the "overtake_dsp:" routing
         * marker. Returns the response blob (get) / true (set), null if the
         * host lacks the binding (module falls back to single calls). */
        globalThis.host_module_get_params = function(blob) {
            if (typeof shadow_get_params === "function") {
                return shadow_get_params(0, "overtake_dsp:", blob);
            }
            return null;
        };
        globalThis.host_module_set_params = function(blob) {
            if (typeof shadow_set_params === "function") {
                return shadow_set_params(0, "overtake_dsp:", blob);
            }
            return false;
        };
        globalThis.host_exit_module = function() {
            debugLog("host_exit_module called by overtake module");
            if (toolOvertakeActive) {
                exitToolOvertake();
            } else {
                exitOvertakeMode();
            }
        };
        globalThis.host_suspend_overtake = function() {
            debugLog("host_suspend_overtake called by overtake module");
            suspendOvertakeMode();
        };
        globalThis.host_hide_module = function() {
            debugLog("host_hide_module called by overtake module");
            if (toolOvertakeActive) {
                hideToolOvertake();
            }
        };
        /* Expose file I/O to overtake modules */
        if (typeof host_write_file === "function") {
            globalThis.host_write_file = host_write_file;
        }
        if (typeof host_ensure_dir === "function") {
            globalThis.host_ensure_dir = host_ensure_dir;
        }
        if (typeof host_file_exists === "function") {
            globalThis.host_file_exists = host_file_exists;
        }
        if (typeof host_read_file === "function") {
            globalThis.host_read_file = host_read_file;
        }
        if (typeof host_system_cmd === "function") {
            globalThis.host_system_cmd = host_system_cmd;
        }
        /* Expose text entry to overtake modules */
        globalThis.host_open_text_entry = function(opts) {
            openTextEntry({
                title: opts.title || "Text Entry",
                initialText: opts.initialText || "",
                onAnnounce: announce,
                onConfirm: opts.onConfirm || function() {},
                onCancel: opts.onCancel || function() {}
            });
        };
        debugLog("loadOvertakeModule: param shims installed");

        /* Step 4: Load the module's UI script (after DSP + shims so module can use them) */
        debugLog("loadOvertakeModule: loading " + moduleInfo.uiPath);
        if (typeof shadow_load_ui_module === "function") {
            const result = shadow_load_ui_module(moduleInfo.uiPath);
            debugLog("loadOvertakeModule: shadow_load_ui_module returned " + result);
            if (!result) {
                deactivateLedQueue();
                overtakeModuleLoaded = false;
                overtakeModuleCallbacks = null;
                delete globalThis.host_module_set_param;
                delete globalThis.host_module_get_param;
                unloadOvertakeDsp();
                if (typeof shadow_set_overtake_mode === "function") {
                    shadow_set_overtake_mode(0);
                }
                return false;
            }
        } else {
            debugLog("loadOvertakeModule: shadow_load_ui_module not available");
            deactivateLedQueue();
            delete globalThis.host_module_set_param;
            delete globalThis.host_module_get_param;
            return false;
        }

        /* Step 5: Capture the module's callbacks */
        overtakeModuleCallbacks = {
            init: (globalThis.init !== savedInit) ? globalThis.init : null,
            tick: (globalThis.tick !== savedTick) ? globalThis.tick : null,
            onMidiMessageInternal: (globalThis.onMidiMessageInternal !== savedMidi) ? globalThis.onMidiMessageInternal : null,
            onUnload: (typeof globalThis.onUnload === "function") ? globalThis.onUnload : null,
            /* onResume(): optional. Called once each time the module is
             * resumed from suspend (init() is NOT re-run). See
             * invokeModuleOnResume for the module-facing contract. */
            onResume: (typeof globalThis.onResume === "function") ? globalThis.onResume : null
        };

        /* Restore shadow UI's globals */
        globalThis.init = savedInit;
        globalThis.tick = savedTick;
        globalThis.onMidiMessageInternal = savedMidi;
        if (typeof globalThis.onUnload === "function") delete globalThis.onUnload;
        if (typeof globalThis.onResume === "function") delete globalThis.onResume;

        debugLog("loadOvertakeModule: callbacks captured - init:" + !!overtakeModuleCallbacks.init +
                 " tick:" + !!overtakeModuleCallbacks.tick +
                 " midi:" + !!overtakeModuleCallbacks.onMidiMessageInternal);

        overtakeModuleLoaded = true;
        overtakeSuspendKeepsJs = !!(moduleInfo.capabilities && moduleInfo.capabilities.suspend_keeps_js);
        /* suspend_self_managed: the module uses Back for its own navigation and
         * calls host_suspend_overtake() when it decides to park. It implies
         * keeps-JS — the closure must stay alive to keep deciding + ticking. An
         * older host that predates this capability simply ignores it, so the
         * module degrades to a plain exit on Back. */
        overtakeSuspendSelfManaged = !!(moduleInfo.capabilities && moduleInfo.capabilities.suspend_self_managed);
        if (overtakeSuspendSelfManaged) overtakeSuspendKeepsJs = true;

        /* button_passthrough: array of CC numbers for buttons the module yields
         * to Move firmware (press events reach Move, LEDs stay Move-driven).
         * We push as a single CSV write because overtake_mode=2 makes
         * shadow_set_param fire-and-forget; multiple consecutive writes would
         * overwrite the shared buffer before the shim reads them. */
        const bp = moduleInfo.capabilities && moduleInfo.capabilities.button_passthrough;
        overtakePassthroughCCs = Array.isArray(bp) ? bp.slice().filter((c) => typeof c === "number" && c >= 0 && c < 128) : [];
        /* Blocking write: the caller (loadTool) fires more shadow_set_param
         * calls immediately after this returns. In overtake_mode=2 plain
         * shadow_set_param is fire-and-forget, which clobbers the request
         * before the shim reads it. */
        if (typeof shadow_set_param_timeout === "function") {
            shadow_set_param_timeout(0, "passthrough", overtakePassthroughCCs.join(","), 100);
        } else if (typeof shadow_set_param === "function") {
            shadow_set_param(0, "passthrough", overtakePassthroughCCs.join(","));
        }

        /* Track module load for analytics */
        if (typeof host_track_event === "function" && moduleInfo.id) {
            host_track_event('module_loaded', '"module_id":"' + moduleInfo.id + '","source":"overtake"');
        }

        /* Step 6: Defer init() call - LEDs will be cleared progressively during loading screen.
         * Non-overtake tools don't own LEDs, so call init() immediately.
         * Modules with skip_led_clear capability skip LED clearing and init immediately
         * (e.g. song-mode wants Move's pad colors to stay visible). */
        const skipLedClear = moduleInfo.capabilities && moduleInfo.capabilities.skip_led_clear;
        if (skipOvertake || skipLedClear) {
            overtakeInitPending = false;
            debugLog("loadOvertakeModule: " + (skipOvertake ? "non-overtake" : "skip_led_clear") + ", calling init() immediately");
            if (overtakeModuleCallbacks && overtakeModuleCallbacks.init) {
                overtakeModuleCallbacks.init();
            }
        } else {
            overtakeInitPending = true;
            overtakeInitTicks = 0;
            ledClearIndex = 0;  /* Start LED clearing from beginning */
            debugLog("loadOvertakeModule: init deferred, LEDs will clear progressively");
        }

        /* Remember this launch for the Tools-shortcut relaunch gesture. Every
         * load path (overtake menu, Tools menu, startInteractiveTool) funnels
         * through here, so this one write covers them all. Interactive tools
         * overwrite it with a richer descriptor once their file is known. */
        lastLaunchedTool = {
            kind: "overtake",
            module: moduleInfo,
            skipOvertake: !!skipOvertake,
            filePath: ""
        };

        return true;
    } catch (e) {
        debugLog("loadOvertakeModule error: " + e);
        deactivateLedQueue();
        overtakeModuleLoaded = false;
        overtakeModuleCallbacks = null;
        /* Clean up DSP and param shims on error */
        unloadOvertakeDsp();
        delete globalThis.host_module_set_param;
        delete globalThis.host_module_get_param;
        delete globalThis.host_exit_module;
        delete globalThis.host_suspend_overtake;
        if (typeof shadow_set_overtake_mode === "function") {
            shadow_set_overtake_mode(0);
        }
        return false;
    }
}

/* Draw the overtake module selection menu */
function drawOvertakeMenu() {
    clear_screen();

    drawHeader("Overtake Modules");

    if (overtakeModules.length === 0) {
        print(4, LIST_TOP_Y + 10, "No overtake modules found", 1);
        print(4, LIST_TOP_Y + 26, "Install modules with", 1);
        print(4, LIST_TOP_Y + 42, "component_type: \"overtake\"", 1);
    } else {
        const items = overtakeModules.map(m => ({
            label: m.name,
            value: ""
        }));
        drawMenuList({
            items,
            selectedIndex: selectedOvertakeModule,
            listArea: {
                topY: menuLayoutDefaults.listTopY,
                bottomY: menuLayoutDefaults.listBottomWithFooter
            },
            getLabel: (item) => item.label,
            getValue: (item) => item.value
        });
    }

    drawFooter(["Back: exit", "Jog: select"]);
}

/*
 * A `<prefix>:<suffix>` device key for a component, or null when the key does
 * not address a module position (e.g. "settings").
 *
 * WIDER THAN WHAT IT REPLACED, and the difference is a cost rather than a bug.
 * The ternary ladders here before answered null for anything but synth / fx1 /
 * fx2 / midiFx, so an unknown component cost NOTHING. This accepts any valid
 * position id, so `fx3` now produces a real key and a real IPC round trip
 * (~2.8ms) — which comes back "" from the DSP, not null, i.e. indistinguishable
 * from "this module serves no chain_params".
 *
 * Not reachable by accident: the editor's list is built from the positions the
 * DSP says it holds (fx_count), so a caller can only name fx3 when there IS an
 * fx3. What is NOT protected is a caller inventing an id — every such call is
 * one wasted round trip, on a draw path that is a frame's worth of budget.
 */
function chainComponentParamKey(componentKey, suffix) {
    if (!isChainModuleKey(componentKey)) return null;
    return `${chainComponentId(componentKey)}:${suffix}`;
}

/* Fetch chain_params metadata from a component.
 * Chain params are typically in module.json, but we query via get_param. */
function getComponentChainParams(slot, componentKey) {
    return chainTargetChainParams(slotChainTarget(slot), componentKey);
}

/* Synthesize a minimal one-level ui_hierarchy from a component's chain_params
 * so a module lacking a real ui_hierarchy still gets the full hierarchy param
 * editor. Used in co-run (loadModuleUi is refused there, so a preset-less
 * module would otherwise dead-end on the bare "No presets" browser). The level
 * only needs each param's `key` — getParamMetadata() merges the real
 * min/max/type/options from hierEditorChainParams at edit time. Returns null
 * when there are no usable params (caller then keeps the preset-browser path). */
function buildSynthHierarchyFromChainParams(chainParams) {
    if (!Array.isArray(chainParams) || chainParams.length === 0) return null;
    const params = [];
    const knobs = [];
    for (const p of chainParams) {
        if (!p || !p.key) continue;
        params.push({ key: p.key, label: p.name || p.label || p.key });
        if (knobs.length < NUM_KNOBS) knobs.push(p.key);
    }
    if (params.length === 0) return null;
    return { levels: { root: { label: "Parameters", params: params, knobs: knobs } } };
}

/* Fetch ui_hierarchy from a component */
function getComponentHierarchy(slot, componentKey) {
    /* Addressed through the chain target, like the Master FX equivalent. The
     * parse is spelled out here rather than deferred to chainTargetHierarchy
     * only because these two debugLogs want the key and the raw JSON, and
     * re-reading to get them would cost a second ~2.8ms IPC round trip. */
    const target = slotChainTarget(slot);
    const key = target.key(componentKey, "ui_hierarchy");
    if (!key) {
        debugLog(`getComponentHierarchy: no key for componentKey=${componentKey}`);
        return null;
    }

    const json = getSlotParam(slot, key);
    debugLog(`getComponentHierarchy: slot=${slot}, key=${key}, json=${json ? json.substring(0, 100) + '...' : 'null'}`);
    if (!json) return null;

    try {
        return JSON.parse(json);
    } catch (e) {
        return null;
    }
}

/* Fetch chain_params metadata from a Master FX slot.
 * Index-taking wrapper over the shared chain-target accessor — the bounds
 * guard and the JSON handling now live in ONE place for both editors. */
function getMasterFxChainParams(fxSlot) {
    return chainTargetChainParams(MASTER_CHAIN_TARGET, masterFxComponentKey(fxSlot));
}

/* Fetch ui_hierarchy from a Master FX slot */
function getMasterFxHierarchy(fxSlot) {
    return chainTargetHierarchy(MASTER_CHAIN_TARGET, masterFxComponentKey(fxSlot));
}

/* fetchPatchDetail -> shadow_ui_patches.mjs */

/* Fetch knob mappings for the selected slot */
function fetchKnobMappings(slot) {
    knobMappings = [];
    for (let i = 1; i <= NUM_KNOBS; i++) {
        const name = getSlotParam(slot, `knob_${i}_name`) || `Knob ${i}`;
        const value = getSlotParam(slot, `knob_${i}_value`) || "-";
        knobMappings.push({ cc: 70 + i, name, value });
    }
    lastKnobSlot = slot;
}

/* getDetailItems -> shadow_ui_patches.mjs */

/* SYNTH_PARAMS, FX_PARAMS -> shadow_ui_patches.mjs */

/* fetchComponentParams, enterComponentParams, formatParamValue,
 * adjustParamValue -> shadow_ui_patches.mjs */

function safeLoadJson(path) {
    try {
        const raw = std.loadFile(path);
        if (!raw) return null;
        return JSON.parse(raw);
    } catch (e) {
        return null;
    }
}

function loadSlotsFromConfig() {
    const data = safeLoadJson(CONFIG_PATH);
    if (!data || !Array.isArray(data.patches)) {
        return DEFAULT_SLOTS.map((slot) => ({ ...slot }));
    }
    /* Load saved slots, preserving both channel and name */
    const slotsFromConfig = data.patches.map((entry, idx) => {
        const channel = (typeof entry.channel === "number") ? entry.channel : (DEFAULT_SLOTS[idx]?.channel ?? 1 + idx);
        return {
            channel: channel,
            name: (typeof entry.name === "string") ? entry.name : (DEFAULT_SLOTS[idx]?.name ?? "Unknown")
        };
    });
    return slotsFromConfig;
}

function loadMasterFxFromConfig() {
    const data = safeLoadJson(CONFIG_PATH);
    return {
        id: (data && typeof data.master_fx === "string") ? data.master_fx : "",
        path: (data && typeof data.master_fx_path === "string") ? data.master_fx_path : ""
    };
}

function saveSlotsToConfig(nextSlots) {
    /* Read existing config to preserve fields written by C-side shadow_save_state()
     * (slot_volumes, slot_muted, slot_soloed, slot_forward_channels, etc.) */
    const existing = safeLoadJson(CONFIG_PATH) || {};
    existing.patches = nextSlots.map((slot, idx) => ({
        name: slot.name,
        channel: slot.channel,
        forward_channel: parseInt(getSlotParam(idx, "slot:forward_channel") || "-1")
    }));
    existing.master_fx = currentMasterFxId || "";
    try {
        host_write_file(CONFIG_PATH, JSON.stringify(existing, null, 2) + "\n");
    } catch (e) {
        /* ignore */
    }
}

/* Save chain config (volumes, channels, mute/solo) to a per-set directory.
 * Mirrors what shadow_save_config_to_dir() did on the C side, but runs
 * on the UI thread to avoid blocking the audio thread. */
function saveChainConfigToDir(dir) {
    if (!dir) return;
    const path = dir + "/shadow_chain_config.json";
    try {
        const cfgSlots = [];
        for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
            const vol = parseFloat(getSlotParam(i, "slot:volume") || "1");
            const ch = parseInt(getSlotParam(i, "slot:receive_channel") || "0");
            const fwd = parseInt(getSlotParam(i, "slot:forward_channel") || "-1");
            const muted = parseInt(getSlotParam(i, "slot:muted") || "0");
            const soloed = parseInt(getSlotParam(i, "slot:soloed") || "0");
            cfgSlots.push({ name: slots[i] ? slots[i].name : "", channel: ch, volume: vol, forward_channel: fwd, muted: muted, soloed: soloed });
        }
        host_write_file(path, JSON.stringify({ slots: cfgSlots }, null, 2) + "\n");
    } catch (e) {
        debugLog("saveChainConfigToDir error: " + e);
    }
}

/* Load chain config (volumes, channels, mute/solo) from a per-set directory.
 * Mirrors what shadow_load_config_from_dir() did on the C side, but runs
 * on the UI thread to avoid blocking the audio thread. */
/* Detect if a new set is a copy of an existing tracked set.
 * Compares Song.abl file sizes between the new set UUID and all
 * existing set_state directories. Returns source dir path or null. */
function detectCopySource(newUuid) {
    const SETS_DIR = "/data/UserData/UserLibrary/Sets";
    const STATE_DIR = "/data/UserData/schwung/set_state";

    /* Get new set's Song.abl size */
    function getSongAblSize(uuid) {
        try {
            const uuidPath = SETS_DIR + "/" + uuid;
            const entries = os.readdir(uuidPath);
            const dirList = entries[0];
            if (!Array.isArray(dirList)) return -1;
            for (const sub of dirList) {
                if (sub === "." || sub === "..") continue;
                const songPath = uuidPath + "/" + sub + "/Song.abl";
                try {
                    const st = os.stat(songPath);
                    if (st[0] && st[0].size > 0) return st[0].size;
                } catch (e) {}
            }
        } catch (e) {}
        return -1;
    }

    const newSize = getSongAblSize(newUuid);
    if (newSize <= 0) return null;

    /* Scan set_state/ for tracked sets with matching Song.abl size */
    try {
        const entries = os.readdir(STATE_DIR);
        const dirList = entries[0];
        if (!Array.isArray(dirList)) return null;
        let matchUuid = null;
        let matchCount = 0;
        for (const entry of dirList) {
            if (entry === "." || entry === ".." || entry === newUuid) continue;
            if (!host_file_exists(STATE_DIR + "/" + entry + "/slot_0.json")) continue;
            const existingSize = getSongAblSize(entry);
            if (existingSize === newSize) {
                matchUuid = entry;
                matchCount++;
            }
        }
        if (matchCount === 1 && matchUuid) {
            return STATE_DIR + "/" + matchUuid;
        }
    } catch (e) {
        debugLog("detectCopySource error: " + e);
    }
    return null;
}

/* Save current RNBO graph name to a per-set directory.
 * Only writes if RNBO is running and returns a graph name. */
/* Helper: query an RNBO OSCQuery endpoint and return parsed VALUE, or null. */
function rnboGetValue(urlPath) {
    const tmpFile = "/data/UserData/schwung/tmp_rnbo_query.json";
    host_system_cmd('sh -c "wget -q -O ' + tmpFile + ' http://localhost:5678' + urlPath + ' 2>/dev/null"');
    const raw = host_read_file(tmpFile);
    if (!raw) return null;
    try {
        const data = JSON.parse(raw);
        if (data && data.VALUE !== undefined && data.VALUE !== null) return data.VALUE;
    } catch (e) {}
    return null;
}

/* Helper: send an OSC message with a string argument via UDP.
 * async=true (default) runs in background, async=false blocks until sent. */
function rnboSendOsc(path, value, async) {
    const script = "/data/UserData/schwung/modules/overtake/rnbo-runner/osc_send.py";
    const safeValue = value.replace(/"/g, "").replace(/'/g, "");
    const bg = (async !== false) ? " &" : "";
    host_system_cmd('sh -c "python3 ' + script + ' \'' + path + '\' \'' + safeValue + '\'' + bg + '"');
}

/* Save current RNBO graph name and state to a per-set directory.
 * Auto-saves the current RNBO state as a set preset named _schwung_{uuid}
 * so all parameter tweaks are captured without manual preset saves.
 * Only writes if RNBO is running and returns valid values. */
function saveRnboGraphToDir(dir) {
    if (!dir || typeof host_system_cmd !== "function") return;
    const graphName = rnboGetValue("/rnbo/inst/control/sets/current/name");
    if (!graphName || typeof graphName !== "string") return;
    /* Extract UUID from dir path (last path component) */
    const parts = dir.split("/");
    const uuid = parts[parts.length - 1];
    if (!uuid || uuid.length < 8) return;
    /* Auto-save current RNBO state as a set preset tied to this Schwung set.
     * Synchronous — must complete before we load the next set's preset. */
    const autoPresetName = "_schwung_" + uuid;
    rnboSendOsc("/rnbo/inst/control/sets/presets/save", autoPresetName, false);
    /* Brief pause to let RNBO persist the preset before we proceed */
    host_system_cmd('sh -c "sleep 0.2"');
    const state = { graph: graphName, auto_preset: autoPresetName };
    /* Also record the user's last loaded preset (if any) for reference */
    const userPreset = rnboGetValue("/rnbo/inst/control/sets/presets/loaded");
    if (userPreset && typeof userPreset === "string" && userPreset.length > 0 && !userPreset.startsWith("_schwung_")) {
        state.user_preset = userPreset;
    }
    host_write_file(dir + "/rnbo_state.json", JSON.stringify(state) + "\n");
    debugLog("SET_CHANGED: saved RNBO state: " + graphName + " auto_preset=" + autoPresetName);
}

/* Load RNBO graph and state from a per-set directory via OSC.
 * Loads the auto-saved preset (_schwung_{uuid}) to restore full state.
 * Only sends if there's saved state and RNBO is running. */
function loadRnboGraphFromDir(dir) {
    if (!dir || typeof host_system_cmd !== "function") return;
    /* Support both new format (rnbo_state.json) and old format (rnbo_graph.txt) */
    let graphName = null;
    let presetName = null;
    const stateRaw = host_read_file(dir + "/rnbo_state.json");
    if (stateRaw) {
        try {
            const state = JSON.parse(stateRaw);
            graphName = state.graph || null;
            /* Prefer auto_preset (full state), fall back to user_preset */
            presetName = state.auto_preset || state.user_preset || state.preset || null;
        } catch (e) {}
    }
    if (!graphName) {
        /* Fallback to old format */
        const old = host_read_file(dir + "/rnbo_graph.txt");
        if (old) graphName = old.trim();
    }
    if (!graphName) return;
    /* Check if RNBO is running */
    const currentGraph = rnboGetValue("/rnbo/inst/control/sets/current/name");
    if (currentGraph === null) return;  /* RNBO not running */
    /* Load graph (skip if already loaded) */
    if (currentGraph !== graphName) {
        rnboSendOsc("/rnbo/inst/control/sets/load", graphName);
        debugLog("SET_CHANGED: loading RNBO graph: " + graphName);
        /* Wait for graph to load before loading preset */
        if (presetName) {
            const script = "/data/UserData/schwung/modules/overtake/rnbo-runner/osc_send.py";
            const safeName = presetName.replace(/"/g, "").replace(/'/g, "");
            host_system_cmd('sh -c "sleep 3 && python3 ' + script + ' /rnbo/inst/control/sets/presets/load \'' + safeName + '\' &"');
            debugLog("SET_CHANGED: queued RNBO preset: " + presetName + " (3s delay)");
        }
    } else if (presetName) {
        /* Same graph, just load preset immediately */
        rnboSendOsc("/rnbo/inst/control/sets/presets/load", presetName);
        debugLog("SET_CHANGED: loading RNBO preset: " + presetName);
    }
}

function loadChainConfigFromDir(dir) {
    if (!dir) return;
    const path = dir + "/shadow_chain_config.json";
    try {
        const raw = host_read_file(path);
        if (!raw) return;
        const data = JSON.parse(raw);
        if (!data || !Array.isArray(data.slots)) return;
        for (let i = 0; i < SHADOW_UI_SLOTS && i < data.slots.length; i++) {
            const s = data.slots[i];
            if (typeof s.volume === "number") setSlotParamWithTimeout(i, "slot:volume", String(s.volume), 500);
            /* Always write receive_channel: use saved value if present, else
             * default to slot index + 1. Chain configs written before
             * 072d3fd3 (or saved by older host code) can lack the field —
             * silently skipping leaves shim.channel stale from the prior set. */
            const recvCh = (typeof s.channel === "number") ? s.channel : (i + 1);
            setSlotParamWithTimeout(i, "slot:receive_channel", String(recvCh), 500);
            if (typeof s.forward_channel === "number") setSlotParamWithTimeout(i, "slot:forward_channel", String(s.forward_channel), 500);
            if (typeof s.muted === "number") setSlotParamWithTimeout(i, "slot:muted", String(s.muted), 500);
            if (typeof s.soloed === "number") setSlotParamWithTimeout(i, "slot:soloed", String(s.soloed), 500);
        }
        debugLog("SET_CHANGED: loaded chain config from " + path);
    } catch (e) {
        debugLog("loadChainConfigFromDir error: " + e);
    }
}

function refreshSlots() {
    let hostSlots = null;
    try {
        if (typeof shadow_get_slots === "function") {
            hostSlots = shadow_get_slots();
        }
    } catch (e) {
        hostSlots = null;
    }
    /* Always load config to get authoritative slot names */
    const configSlots = loadSlotsFromConfig();
    let newSlots;
    if (Array.isArray(hostSlots) && hostSlots.length) {
        newSlots = hostSlots.map((slot, idx) => ({
            channel: (typeof slot.channel === "number") ? slot.channel : (DEFAULT_SLOTS[idx] ? DEFAULT_SLOTS[idx].channel : 1 + idx),
            /* Prefer config name (set by save), fall back to shim name, then default */
            name: (configSlots[idx] && configSlots[idx].name) || slot.name || (DEFAULT_SLOTS[idx] ? DEFAULT_SLOTS[idx].name : "Unknown Patch")
        }));
    } else {
        newSlots = configSlots;
    }
    /* Only redraw if slot data actually changed */
    let changed = (newSlots.length !== slots.length);
    if (!changed) {
        for (let i = 0; i < newSlots.length; i++) {
            if (newSlots[i].name !== slots[i].name || newSlots[i].channel !== slots[i].channel) {
                changed = true;
                break;
            }
        }
    }
    slots = newSlots;
    if (selectedSlot >= slots.length) {
        selectedSlot = Math.max(0, slots.length - 1);
    }
    if (changed) {
        needsRedraw = true;
    }
}

/* parsePatchName, loadPatchList, findPatchIndexByName,
 * enterPatchBrowser, enterPatchDetail, PATCH_INDEX_NONE,
 * applyPatchSelection -> shadow_ui_patches.mjs */

/* ========== Slot Preset Save/Delete Functions ========== */

/* Check if current slot has an existing preset (vs "Untitled" or empty) */
function isExistingPreset(slotIndex) {
    const name = slots[slotIndex] ? slots[slotIndex].name : null;
    return name && name !== "" && name !== "Untitled";
}

/* Get dynamic settings items (excludes Delete for new presets) */
function getChainSettingsItems(slotIndex) {
    if (isExistingPreset(slotIndex)) {
        /* Existing preset: show all items (Save, Save As, Delete) */
        return CHAIN_SETTINGS_ITEMS;
    }
    /* New preset: hide DELETE, but keep Save As — see the Master FX twin of
     * this filter. Save suggests a name, Save As asks for one; both are
     * meaningful before anything is saved, and the two chains must offer the
     * same entries or their settings screens drift again. */
    return CHAIN_SETTINGS_ITEMS.filter(function(item) {
        return item.key !== "delete";
    });
}

/* findPatchByName -> shadow_ui_patches.mjs (imported) */

/* Generate default name from chain components */
function generateSlotPresetName(slotIndex) {
    const cfg = chainConfigs[slotIndex];
    if (!cfg) return "Untitled";

    const parts = [];
    if (cfg.synth && cfg.synth.module) {
        const abbrev = moduleAbbrevCache[cfg.synth.module] || cfg.synth.module.toUpperCase().slice(0, 3);
        parts.push(abbrev);
    }
    for (const fx of cfg.fx) {
        if (!fx || !fx.module) continue;
        parts.push(moduleAbbrevCache[fx.module] || fx.module.toUpperCase().slice(0, 2));
    }

    return parts.length > 0 ? parts.join(" + ") : "Untitled";
}

/* Query a slot:component state via shadow_get_param, retrying briefly if
 * the first call returns empty. The shim audio thread can be momentarily
 * busy during set-change / heavy SPI activity, which makes a single 100ms
 * round-trip race and return "" — silently dropping the save and losing
 * recent edits (diagnosed 2026-05-12). 3 retries adds up to ~400ms worst
 * case which is well under typical set-change duration. */
/*
 * `retries` defaults to the historical 3. Periodic autosave passes 1.
 *
 * Every attempt is a synchronous IPC round trip (~2.8ms), and the loop fires
 * on any FALSY result — which includes a component whose state is genuinely
 * empty, so such a component paid four round trips (~11ms) on every autosave,
 * forever, to be told the same thing four times. Across a populated slot set
 * that is a large share of the ~200ms autosave stall.
 *
 * Retrying at all is still right for an explicit save: the retries exist
 * because a state query can come back empty transiently while a module is
 * still loading, and an explicit save has no second chance. Periodic autosave
 * does: the bail-if-empty guard in buildSlotPatchJson preserves the existing
 * slot_N.json, and the next pass is five seconds away. So it takes one extra
 * attempt, not three.
 */
/*
 * A state read has THREE answers, and only retrying is right for one of them.
 *
 *   JSON/text  the component serialised its state
 *   ""         the channel served us and the module declares no `state` key
 *   null       the read did not complete (claim refused, timeout, stolen)
 *
 * `if (state)` collapsed the last two, so a module that legitimately
 * implements no `state` looked identical to a shim round-trip that timed out —
 * and the caller's bail-to-protect-a-good-file then abandoned the WHOLE slot's
 * autosave, including the other components in it. `denis` and `branchage`
 * implement no `state`; a slot containing either never autosaved anything,
 * ever, and neither did the FX behind it. Found in the 2026-08 fleet audit.
 *
 * This is the same rule as the contract reads in page_controller.mjs, one
 * layer up: branch on the RAW value before testing it for truthiness, because
 * `""` and `null` are both falsy and by then the distinction is gone.
 */
function getSlotStateWithRetry(slotIndex, key, retries) {
    const limit = (typeof retries === "number") ? retries : 3;
    let state = getSlotParam(slotIndex, key);
    if (state) return state;
    /* Served, and the module has nothing here. Retrying cannot change that. */
    if (state === "") return "";
    for (let attempt = 1; attempt <= limit; attempt++) {
        state = getSlotParam(slotIndex, key);
        if (state) {
            debugLog("getSlotStateWithRetry: slot " + slotIndex + " " +
                     key + " succeeded on retry " + attempt);
            return state;
        }
        if (state === "") return "";
    }
    return null;
}

/* Build patch JSON for saving
 * Note: save_patch expects raw chain content (synth, audio_fx at root)
 * with "custom_name" for the name. It wraps it with name/version/chain.
 */
function buildSlotPatchJson(slotIndex, name, forAutosave, moduleChanged) {
    const cfg = chainConfigs[slotIndex];
    if (!cfg) return null;

    /* Reason for moduleChanged: the "state query returned empty" guard
     * below exists to avoid clobbering a good file when a shim round-trip
     * times out. But it also blocks legitimate saves when the user swaps
     * to a module that doesn't implement get_param("state") at all. When
     * the signature differs from the last-saved one, the old file's state
     * is inapplicable anyway — save with whatever we have (possibly empty)
     * so the module selection survives a reboot. */
    const bailIfEmpty = forAutosave && !moduleChanged;
    /* Periodic autosave gets one extra attempt, not three — see
     * getSlotStateWithRetry. It has the bail-if-empty guard and another pass
     * in five seconds; an explicit save has neither. */
    const stateRetries = forAutosave ? 1 : 3;

    const patch = {
        custom_name: name,
        input: "both",
        synth: null,
        audio_fx: []
    };

    /*
     * One position's saved payload: its opaque state (or the in-memory params
     * when the module serves none) and its bypass flag. Returns BAIL when the
     * state query came back empty and bailing is on — the caller then abandons
     * the whole save rather than clobbering a good file.
     */
    const BAIL = {};
    const componentEntry = (id, moduleData) => {
        let config = moduleData.params || {};
        const stateJson = getSlotStateWithRetry(slotIndex, `${id}:state`, stateRetries);
        if (stateJson) {
            try {
                config = { state: JSON.parse(stateJson) };
            } catch (e) {
                /* State is not JSON (e.g. key=value pairs) — store as opaque string */
                config = { state: stateJson };
            }
        } else if (stateJson === null && bailIfEmpty) {
            /* The read FAILED (timeout / refused claim) and the module is
             * unchanged — skip autosave rather than clobber a good file, which
             * would revert it to defaults.
             *
             * `stateJson === null` is load-bearing: `""` means the module
             * declares no `state`, which is an answer, not a failure. Bailing
             * on that abandoned the whole slot forever for denis and branchage
             * — see getSlotStateWithRetry. */
            debugLog("buildSlotPatchJson: slot " + slotIndex + " " + id +
                     ":state read FAILED after retries — bailing (preserving existing slot_" +
                     slotIndex + ".json)");
            return BAIL;
        }
        const entry = {
            config,
            bypassed: parseInt(getSlotParam(slotIndex, `${id}:bypassed`) || "0", 10) === 1 ? 1 : 0
        };
        /* Only when there is one. An absent key is how a component that has
         * never loaded a preset is spelled, and how every patch written before
         * this existed still reads. */
        const record = getUserPresetRecord(slotIndex, id);
        if (record) entry.user_preset = { name: record.name, hash: record.hash };
        return entry;
    };

    if (cfg.synth && cfg.synth.module) {
        const entry = componentEntry("synth", cfg.synth);
        if (entry === BAIL) return null;
        patch.synth = {
            module: cfg.synth.module,
            config: entry.config,
            bypassed: entry.bypassed,
            user_preset: entry.user_preset
        };
    }

    for (let i = 0; i < cfg.midiFx.length; i++) {
        const moduleData = cfg.midiFx[i];
        if (!moduleData || !moduleData.module) continue;
        const entry = componentEntry(`midi_fx${i + 1}`, moduleData);
        if (entry === BAIL) return null;
        if (!patch.midi_fx) patch.midi_fx = [];
        patch.midi_fx.push({
            type: moduleData.module,
            params: entry.config,
            bypassed: entry.bypassed,
            user_preset: entry.user_preset
        });
    }

    for (let i = 0; i < cfg.fx.length; i++) {
        const moduleData = cfg.fx[i];
        if (!moduleData || !moduleData.module) continue;
        const entry = componentEntry(`fx${i + 1}`, moduleData);
        if (entry === BAIL) return null;
        patch.audio_fx.push({
            type: moduleData.module,
            params: entry.config,
            bypassed: entry.bypassed,
            user_preset: entry.user_preset
        });
    }

    /* Include slot channel settings */
    const recvCh = getSlotParam(slotIndex, "slot:receive_channel");
    const fwdCh = getSlotParam(slotIndex, "slot:forward_channel");
    if (recvCh !== null) patch.receive_channel = parseInt(recvCh);
    if (fwdCh !== null) patch.forward_channel = parseInt(fwdCh);

    /* Include MIDI FX placement (Pre/Post) */
    const preMode = getSlotParam(slotIndex, "midi_fx_pre_mode");
    if (preMode !== null) patch.midi_fx_pre_mode = parseInt(preMode) ? 1 : 0;

    /* Include knob mappings */
    const knobMappingsJson = getSlotParam(slotIndex, "knob_mappings");
    if (knobMappingsJson) {
        try {
            const mappings = JSON.parse(knobMappingsJson);
            if (mappings && mappings.length > 0) {
                patch.knob_mappings = mappings;
            }
        } catch (e) {
            /* Ignore parse errors */
        }
    }

    /* Include LFO config */
    const lfoConfigJson = getSlotParam(slotIndex, "lfo_config");
    if (lfoConfigJson) {
        try {
            const lfos = JSON.parse(lfoConfigJson);
            if (lfos) {
                patch.lfos = lfos;
            }
        } catch (e) {
            /* Ignore parse errors */
        }
    }

    return JSON.stringify(patch);
}

/* Autosave all slot states to slot_state/slot_N.json */
/*
 * Autosave ONE slot. Split out of autosaveAllSlots so the periodic pass can
 * spend a slot per tick instead of landing ~70 IPC reads (~200ms) on a
 * single frame every five seconds. Body is unchanged apart from the
 * loop's `continue`s becoming `return`s.
 */
function autosaveOneSlot(i) {
    /* Never persist an uncommitted preset audition. While the user scrolls
     * User Presets, the live <prefix>:state is the previewed sound, not a
     * committed choice — saving it would let a slot silently adopt a preview
     * (e.g. if a periodic autosave or overtake-suspend teardown lands
     * mid-audition). previewActive clears on Load (commit) or Back (revert),
     * after which autosave resumes normally. */
    if (isPresetPreviewActive()) return;
    /* Sync chainConfigs from DSP before checking - prevents clobbering
     * valid autosave files for slots we haven't navigated to yet.
     * Read ONCE and reused as `currentSig` below — it used to be read
     * again a few lines down, at four IPC round trips a time. */
    const currentSig = getSlotModuleSignature(i);
    applySlotModuleSignature(i, currentSig);
    const cfg = chainConfigs[i];
    if (!chainHasAnyModule(cfg)) {
        /* Cross-check before clobbering: if the slot has a preset name
         * but the shim is reporting "no modules" AND the user did not
         * explicitly clear the slot via the picker, it's a transient
         * shim-side glitch (e.g. boot-time patch load failure
         * diagnosed 2026-04-18). Preserve the existing slot_N.json so
         * the next boot has a chance to reload it.
         *
         * slotUserCleared[i] = true means the user picked None for
         * every component in the slot, so the empty state is real and
         * must be persisted (otherwise removals never save — diagnosed
         * 2026-04-29). */
        const slotName = (slots[i] && slots[i].name) || "";
        if (!slotUserCleared[i] && slotName !== "") {
            /* Only guard when there's actually content on disk to protect.
             * If slot_N.json is missing/empty, the chain-config name has
             * drifted from the saved state — writing the empty marker is
             * safe and stops the every-autosave log spam. */
            const existing = host_read_file(
                activeSlotStateDir + "/slot_" + i + ".json");
            let hasSavedChain = false;
            if (existing) {
                try {
                    const parsed = JSON.parse(existing);
                    hasSavedChain = !!(parsed && parsed.chain &&
                        ((parsed.chain.synth && parsed.chain.synth.module) ||
                         (parsed.chain.audio_fx && parsed.chain.audio_fx.length) ||
                         (parsed.chain.midi_fx && parsed.chain.midi_fx.length)));
                } catch (e) { /* malformed → treat as no content */ }
            }
            if (hasSavedChain) {
                debugLog("autosave: slot " + i + " shim reports empty but " +
                         "preset name=\"" + slotName + "\" and slot_" + i +
                         ".json has chain — preserving (likely shim glitch)");
                slotDirtyCache[i] = false;
                return;
            }
            /* No saved chain on disk — chain-config name is stale. Fall
             * through to write the empty marker so future autosaves stop
             * tripping this branch. */
        }
        /* Empty slot - write empty marker to clear autosave.
         * Same skip-if-unchanged as the populated path below: an empty slot
         * was rewriting "{}" to eMMC every five seconds forever. */
        if (lastWrittenSlotJson[i] === "{}\n") {
            slotDirtyCache[i] = false;
            slotUserCleared[i] = false;
            return;
        }
        if (host_write_file(
            activeSlotStateDir + "/slot_" + i + ".json",
            "{}\n"
        )) {
            slotDirtyCache[i] = false;
            slotUserCleared[i] = false;
            lastWrittenSlotJson[i] = "{}\n";
        } else {
            lastWrittenSlotJson[i] = null;
            debugLog("autosave: failed to write empty marker for slot " + i +
                     " — will retry next autosave");
        }
        return;
    }

    const dirty = getSlotParam(i, "dirty");
    slotDirtyCache[i] = (dirty === "1");

    const moduleChanged = currentSig !== lastSavedSlotSignature[i];
    const patchJson = buildSlotPatchJson(i, slots[i].name || "Untitled", true, moduleChanged);
    if (!patchJson) return;

    /* Wrap with name, version, modified flag */
    const wrapper = {
        name: slots[i].name || "Untitled",
        version: 1,
        modified: slotDirtyCache[i],
        chain: JSON.parse(patchJson)
    };

    const slotPath = activeSlotStateDir + "/slot_" + i + ".json";
    const payload = JSON.stringify(wrapper, null, 2) + "\n";

    /*
     * Skip the write when the bytes are identical to what we last wrote.
     *
     * Measured on device: an autosave tick spent 187ms of which only 66ms was
     * IPC — the other ~120ms was this host_write_file. eMMC on this device is
     * slow enough that writing a few KB of JSON is comfortably the most
     * expensive thing the UI thread does all second, and on an idle set it was
     * rewriting byte-for-byte identical files every five seconds forever.
     *
     * Comparing against the last payload we wrote (not against the file — that
     * would be a read, and reads are what we are trying to avoid) makes the
     * idle case free. `lastWrittenSlotJson` is per-process, so the first pass
     * after a restart still writes, which is what we want: it re-establishes
     * the file even if something else changed it underneath us.
     */
    if (lastWrittenSlotJson[i] === payload) {
        lastSavedSlotSignature[i] = currentSig;
        return;
    }

    if (host_write_file(slotPath, payload)) {
        lastSavedSlotSignature[i] = currentSig;
        lastWrittenSlotJson[i] = payload;
    } else {
        lastWrittenSlotJson[i] = null;   /* force a retry next pass */
        debugLog("autosave: failed to write slot_" + i + ".json — " +
                 "keeping stale signature so the next autosave retries");
    }
}

/* Every slot, right now. Used by the explicit save paths (shutdown, set
 * switch, overtake suspend) where the whole set must land before we
 * proceed. The periodic timer uses autosaveOneSlot per tick instead. */
function autosaveAllSlots() {
    for (let i = 0; i < SHADOW_UI_SLOTS; i++) autosaveOneSlot(i);
}

/* Actually save the preset */
function doSavePreset(slotIndex, name) {
    const json = buildSlotPatchJson(slotIndex, name);
    if (!json) {
        debugLog("doSavePreset: buildSlotPatchJson returned null for slot " + slotIndex);
        showWarning("Save Failed", "Could not read chain state. Try again.");
        return;
    }

    if (overwriteTargetIndex >= 0) {
        setSlotParam(slotIndex, "update_patch", overwriteTargetIndex + ":" + json);
    } else {
        setSlotParam(slotIndex, "save_patch", json);
    }

    slots[slotIndex].name = name;
    saveSlotsToConfig(slots);

    confirmingOverwrite = false;
    overwriteFromKeyboard = false;
    showingNamePreview = false;
    pendingSaveName = "";
    overwriteTargetIndex = -1;

    loadPatchList();
    announce("Chain Settings");
    /* Save complete */
    needsRedraw = true;
}

/* Actually delete the preset */
function doDeletePreset(slotIndex) {
    const name = slots[slotIndex] ? slots[slotIndex].name : null;
    const patchIndex = findPatchByName(name);

    if (patchIndex >= 0) {
        setSlotParam(slotIndex, "delete_patch", String(patchIndex));
    }

    /* Clear the slot to "Untitled" state */
    slots[slotIndex].name = "Untitled";
    saveSlotsToConfig(slots);

    /* Request slot clear (load PATCH_INDEX_NONE) */
    if (typeof shadow_request_patch === "function") {
        try {
            shadow_request_patch(slotIndex, PATCH_INDEX_NONE);
        } catch (e) {
            /* ignore */
        }
    }

    /* Refresh knob mappings (patch detail refreshed on re-entry) */
    fetchKnobMappings(slotIndex);
    invalidateKnobContextCache();  /* Clear stale knob contexts after slot clear */

    confirmingDelete = false;
    loadPatchList();
    setView(VIEWS.CHAIN_EDIT);
    /* Delete complete */
    needsRedraw = true;
}

function getSavePreviewText(name) {
    if (!name || name === "") return "Untitled";
    return name;
}

function announceSavePreview(name, selectedIndex, full = true) {
    const selected = selectedIndex === 0 ? "Edit" : "OK";
    if (!full) {
        announce(selected);
        return;
    }
    announce(`Save As, current text: ${getSavePreviewText(name)}. ${selected} selected`);
}

/* enterSlotSettings() -> shadow_ui_slots.mjs */

/* ========== Master Preset Picker Functions ========== */

function loadMasterPresetList() {
    masterPresets = [];
    const countStr = getSlotParam(0, "master_preset_count");
    const count = parseInt(countStr, 10) || 0;
    debugLog(`loadMasterPresetList: countStr='${countStr}' count=${count}`);

    for (let i = 0; i < count; i++) {
        const name = getSlotParam(0, `master_preset_name_${i}`) || `Preset ${i + 1}`;
        /* Hex dump first 20 chars to debug garbage issue */
        let hex = "";
        for (let j = 0; j < Math.min(20, name.length); j++) {
            hex += name.charCodeAt(j).toString(16).padStart(2, '0') + " ";
        }
        debugLog(`loadMasterPresetList: preset ${i} name='${name}' len=${name.length} hex=[${hex.trim()}]`);
        masterPresets.push({ name: name, index: i });
    }
}

function enterMasterPresetPicker() {
    loadMasterPresetList();
    inMasterPresetPicker = true;
    selectedMasterPresetIndex = 0;  /* Start at [New] */
    needsRedraw = true;

    /* Announce menu title + initial selection */
    announce("Master Presets, [New]");
}

function exitMasterPresetPicker() {
    inMasterPresetPicker = false;
    needsRedraw = true;
}

/* drawMasterPresetPicker -> shadow_ui_master_fx.mjs */

function findMasterPresetByName(name) {
    for (let i = 0; i < masterPresets.length; i++) {
        if (masterPresets[i].name === name) {
            return i;
        }
    }
    return -1;
}

function generateMasterPresetName() {
    const parts = [];
    for (let i = 0; i < MASTER_FX_SLOTS; i++) {
        const key = `fx${i + 1}`;
        const moduleId = masterFxConfig[key]?.module;
        if (moduleId) {
            const abbrev = moduleAbbrevCache[moduleId] || moduleId.toUpperCase().slice(0, 3);
            parts.push(abbrev);
        }
    }
    return parts.length > 0 ? parts.join(" + ") : "Master FX";
}

function clearMasterFx() {
    /* Clear every FX slot */
    for (let i = 0; i < MASTER_FX_SLOTS; i++) {
        setMasterFxSlotModule(i, "");
        masterFxConfig[`fx${i + 1}`].module = "";
        /* Different module — it may implement display_name even if the
         * last one didn't, so poll it at full rate again. */
        delete fxDisplayNameCache[`master:fx${i + 1}`];
        delete fxDisplayNameSkip[`master:fx${i + 1}`];
        delete fxDisplayNameBackoff[`master:fx${i + 1}`];
    }
    saveMasterFxChainConfig();
    currentMasterPresetName = "";
    needsRedraw = true;
}

function loadMasterPreset(index, presetName) {
    /* Get preset JSON from DSP */
    const json = getSlotParam(0, `master_preset_json_${index}`);
    if (!json) return;

    try {
        const preset = JSON.parse(json);
        const fx = preset.master_fx || {};

        /* Apply each FX slot */
        for (let i = 0; i < MASTER_FX_SLOTS; i++) {
            const key = `fx${i + 1}`;
            const fxConfig = fx[key];
            if (fxConfig && fxConfig.type) {
                /* Find module path from type */
                const opt = MASTER_FX_OPTIONS.find(o => o.id === fxConfig.type);
                if (opt) {
                    setMasterFxSlotModule(i, opt.dspPath || "");
                    masterFxConfig[key].module = opt.id;
                    /* Different module — it may implement display_name even if the
                     * last one didn't, so poll it at full rate again. */
                    delete fxDisplayNameCache[`master:${key}`];
                    delete fxDisplayNameSkip[`master:${key}`];
                    delete fxDisplayNameBackoff[`master:${key}`];

                    /* Restore plugin_id first (CLAP sub-plugin selection) */
                    if (fxConfig.params && typeof shadow_set_param === "function") {
                        if (fxConfig.params.plugin_id) {
                            shadow_set_param(0, `master_fx:${key}:plugin_id`, fxConfig.params.plugin_id);
                        }
                        /* Restore remaining params */
                        for (const [pkey, pval] of Object.entries(fxConfig.params)) {
                            if (pkey !== "plugin_id") {
                                shadow_set_param(0, `master_fx:${key}:${pkey}`, String(pval));
                            }
                        }
                    }
                } else {
                    /* Module not found - clear slot */
                    setMasterFxSlotModule(i, "");
                    masterFxConfig[key].module = "";
                    /* Different module — it may implement display_name even if the
                     * last one didn't, so poll it at full rate again. */
                    delete fxDisplayNameCache[`master:${key}`];
                    delete fxDisplayNameSkip[`master:${key}`];
                    delete fxDisplayNameBackoff[`master:${key}`];
                }
            } else {
                setMasterFxSlotModule(i, "");
                masterFxConfig[key].module = "";
                /* Different module — it may implement display_name even if the
                 * last one didn't, so poll it at full rate again. */
                delete fxDisplayNameCache[`master:${key}`];
                delete fxDisplayNameSkip[`master:${key}`];
                delete fxDisplayNameBackoff[`master:${key}`];
            }
        }

        /* Restore master FX LFO configs */
        for (let li = 1; li <= 2; li++) {
            const lfoConfig = preset["lfo" + li];
            if (lfoConfig && typeof shadow_set_param === "function") {
                restoreMasterFxLfo(li, lfoConfig);
            } else {
                /* No LFO in preset — disable */
                if (typeof shadow_set_param === "function") {
                    shadow_set_param(0, "master_fx:lfo" + li + ":enabled", "0");
                    shadow_set_param(0, "master_fx:lfo" + li + ":target", "");
                    shadow_set_param(0, "master_fx:lfo" + li + ":target_param", "");
                }
            }
        }

        /* Set preset name before saving so it persists */
        if (presetName) {
            currentMasterPresetName = presetName;
        }
        saveMasterFxChainConfig();
    } catch (e) {
        /* Parse error - ignore */
    }
    needsRedraw = true;
}

/* Build JSON for saving master preset */
function buildMasterPresetJson(name) {
    const preset = { custom_name: name };
    for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
        preset[`fx${i}`] = null;
    }

    for (let i = 0; i < MASTER_FX_SLOTS; i++) {
        const key = `fx${i + 1}`;
        const moduleId = masterFxConfig[key]?.module;
        if (moduleId) {
            const slotPreset = {
                type: moduleId,
                params: {}
            };

            /* Capture plugin_id (for CLAP sub-plugin selection) */
            if (typeof shadow_get_param === "function") {
                try {
                    const pluginId = shadow_get_param(0, `master_fx:${key}:plugin_id`);
                    if (pluginId) {
                        slotPreset.params["plugin_id"] = pluginId;
                    }
                } catch (e) {}

                /* Capture individual params from chain_params */
                try {
                    const chainParams = getMasterFxChainParams(i);
                    if (chainParams && chainParams.length > 0) {
                        for (const p of chainParams) {
                            const val = shadow_get_param(0, `master_fx:${key}:${p.key}`);
                            if (val !== null && val !== undefined && val !== "") {
                                slotPreset.params[p.key] = val;
                            }
                        }
                    }
                } catch (e) {}
            }

            preset[key] = slotPreset;
        }
    }

    /* Include master FX LFO configs */
    for (let li = 1; li <= 2; li++) {
        try {
            const configJson = shadow_get_param(0, "master_fx:lfo" + li + ":config");
            if (configJson) {
                preset["lfo" + li] = JSON.parse(configJson);
            }
        } catch (e) {}
    }

    return JSON.stringify(preset);
}

/* Actually save the master preset */
function doSaveMasterPreset(name) {
    const json = buildMasterPresetJson(name);
    if (!json) {
        debugLog("doSaveMasterPreset: buildMasterPresetJson returned null");
        showWarning("Save Failed", "Could not read FX state. Try again.");
        return;
    }

    if (masterOverwriteTargetIndex >= 0) {
        /* Overwriting existing preset */
        setSlotParam(0, "update_master_preset", masterOverwriteTargetIndex + ":" + json);
    } else {
        /* Creating new preset */
        setSlotParam(0, "save_master_preset", json);
    }

    currentMasterPresetName = name;

    /* Reset state */
    masterShowingNamePreview = false;
    masterConfirmingOverwrite = false;
    masterPendingSaveName = "";
    masterOverwriteTargetIndex = -1;
    inMasterFxSettingsMenu = false;

    loadMasterPresetList();
    needsRedraw = true;
}

/* Handle master FX settings menu actions */
function handleMasterFxSettingsAction(key) {
    if (key === "mfx_lfo1" || key === "mfx_lfo2") {
        const lfoIdx = (key === "mfx_lfo1") ? 0 : 1;
        lfoCtx = makeMfxLfoCtx(lfoIdx);
        selectedLfoItem = 0;
        editingLfoValue = false;
        setView(VIEWS.LFO_EDIT);
        const enabled = lfoCtx.getParam("enabled");
        if (enabled === "1") {
            const targetDesc = describeCurrentLfoTarget();
            if (targetDesc && !targetDesc.empty) {
                announce(lfoCtx.title + ", " + targetDesc.long);
            } else {
                announce(lfoCtx.title + ", no target");
            }
        } else {
            announce(lfoCtx.title + ", Off");
        }
        return;
    }
    if (key === "help") {
        if (!helpContent) {
            try {
                const raw = host_read_file("/data/UserData/schwung/shared/help_content.json");
                if (raw) {
                    helpContent = JSON.parse(raw);
                    /* Append core version to Schwung title */
                    const coreVersion = getHostVersion();
                    const meSection = helpContent.sections && helpContent.sections.find(s => s.title === "Schwung");
                    if (meSection) meSection.title = `Schwung v${coreVersion}`;
                }
            } catch (e) {
                debugLog("Failed to load help content: " + e);
            }
        }
        /* Try to load Move Manual (from bundled or cache — never HTTP) */
        if (helpContent && !helpContent._manualLoaded) {
            try {
                /* Pick up any completed background download first */
                processDownloadedHtml();
                const sections = fetchAndParseManual();
                if (sections && sections.length > 0) {
                    /* Find the Move Manual section and replace its children */
                    for (let i = 0; i < helpContent.sections.length; i++) {
                        if (helpContent.sections[i].title === "Move Manual") {
                            helpContent.sections[i].children = sections;
                            break;
                        }
                    }
                    helpContent._manualLoaded = true;
                    debugLog("Loaded Move Manual: " + sections.length + " chapters");
                }
            } catch (e) {
                debugLog("Move Manual not available: " + e);
            }
        }
        /* Build Modules section from all installed modules with versions */
        if (helpContent && helpContent.sections) {
            try {
                /* Remove previous Modules section if present */
                const oldIdx = helpContent.sections.findIndex(s => s.title === "Modules");
                if (oldIdx >= 0) helpContent.sections.splice(oldIdx, 1);

                /* Build a map of help.json content keyed by module directory */
                const MODULES_DIR = "/data/UserData/schwung/modules";
                const helpMap = {};
                const entries = os.readdir(MODULES_DIR) || [];
                const dirList = entries[0];
                if (Array.isArray(dirList)) {
                    for (const entry of dirList) {
                        if (entry === "." || entry === "..") continue;
                        const entryPath = `${MODULES_DIR}/${entry}`;
                        const loadHelp = function(dirPath, id) {
                            try {
                                const helpRaw = std.loadFile(`${dirPath}/help.json`);
                                if (!helpRaw) return;
                                const helpData = JSON.parse(helpRaw);
                                if (helpData.children) helpMap[id] = helpData.children;
                            } catch (e) { /* skip */ }
                        };
                        loadHelp(entryPath, entry);
                        /* Scan category subdirectories */
                        try {
                            const subEntries = os.readdir(entryPath) || [];
                            const subDirList = subEntries[0];
                            if (Array.isArray(subDirList)) {
                                for (const subEntry of subDirList) {
                                    if (subEntry === "." || subEntry === "..") continue;
                                    loadHelp(`${entryPath}/${subEntry}`, subEntry);
                                }
                            }
                        } catch (e) { /* not a directory */ }
                    }
                }

                /* List all installed modules with versions, merging help content */
                const allModules = host_list_modules();
                const moduleHelpChildren = [];
                for (const mod of allModules) {
                    const title = `${mod.name} v${mod.version || '?'}`;
                    const children = helpMap[mod.id] || [
                        { title: "Info", lines: ["No help content", "available for this", "module."] }
                    ];
                    moduleHelpChildren.push({ title, children });
                }

                if (moduleHelpChildren.length > 0) {
                    moduleHelpChildren.sort((a, b) => a.title.localeCompare(b.title));
                    const modulesSection = { title: "Modules", children: moduleHelpChildren };
                    const meIdx = helpContent.sections.findIndex(s => s.title.startsWith("Schwung"));
                    helpContent.sections.splice(meIdx >= 0 ? meIdx + 1 : 1, 0, modulesSection);
                    debugLog("Loaded module help: " + moduleHelpChildren.length + " modules");
                }
            } catch (e) {
                debugLog("Module help scan failed: " + e);
            }
        }
        /* Ensure Notice section is always present */
        if (helpContent && helpContent.sections &&
            !helpContent.sections.find(s => s.title === "Notice")) {
            helpContent.sections.push({
                title: "Notice",
                children: [{
                    title: "Copyright",
                    lines: [
                        "Ableton Move Manual",
                        "",
                        "Copyright 2024",
                        "Ableton AG.",
                        "All rights reserved.",
                        "Made in Germany.",
                        "",
                        "Manual content",
                        "displayed with",
                        "permission from",
                        "Ableton AG."
                    ]
                }, {
                    title: "Trademark Notice",
                    lines: [
                        "Ableton and Move are",
                        "trademarks of",
                        "Ableton AG.",
                        "",
                        "Schwung is",
                        "an independent",
                        "product and has not",
                        "been authorized,",
                        "sponsored, or",
                        "otherwise approved",
                        "by Ableton AG."
                    ]
                }]
            });
        }
        if (helpContent && helpContent.sections && helpContent.sections.length > 0) {
            helpNavStack = [{ items: helpContent.sections, selectedIndex: 0, title: "Help" }];
            needsRedraw = true;
            announce("Help, " + helpContent.sections[0].title);
        }
        return;
    }
    if (key === "save") {
        if (currentMasterPresetName) {
            /* Existing preset - confirm overwrite */
            masterPendingSaveName = currentMasterPresetName;
            masterOverwriteTargetIndex = findMasterPresetByName(currentMasterPresetName);
            masterConfirmingOverwrite = true;
            masterConfirmIndex = 0;
            masterOverwriteFromKeyboard = false;
            announce(`Overwrite ${currentMasterPresetName}?`);
        } else {
            /* New preset - show name preview */
            masterPendingSaveName = generateMasterPresetName();
            masterShowingNamePreview = true;
            masterNamePreviewIndex = 1;  /* Default to OK */
            masterOverwriteFromKeyboard = true;
            announceSavePreview(masterPendingSaveName, masterNamePreviewIndex);
            needsRedraw = true;
        }
    } else if (key === "save_as") {
        /* Save As - show name preview with current name */
        masterPendingSaveName = currentMasterPresetName || generateMasterPresetName();
        masterShowingNamePreview = true;
        masterNamePreviewIndex = 1;
        masterOverwriteFromKeyboard = true;
        masterOverwriteTargetIndex = -1;  /* Force create new */
        announceSavePreview(masterPendingSaveName, masterNamePreviewIndex);
        needsRedraw = true;
    } else if (key === "check_updates") {
        /* Detection only — no install actions on-device. */
        showUpdatesAvailableScreen();
    } else if (key === "module_store") {
        /* Browsing on-device is disabled — point at the web manager. */
        showModuleStorePointer();
    } else if (key === "delete") {
        /* Delete - confirm */
        masterConfirmingDelete = true;
        masterConfirmIndex = 0;
        announce(`Delete ${currentMasterPresetName}?`);
        needsRedraw = true;
    }
}

/* Delete the current master preset */
function doDeleteMasterPreset() {
    const index = findMasterPresetByName(currentMasterPresetName);
    if (index >= 0) {
        setSlotParam(0, "delete_master_preset", String(index));
    }

    /* Clear current preset and return to picker */
    clearMasterFx();
    currentMasterPresetName = "";
    masterConfirmingDelete = false;
    inMasterFxSettingsMenu = false;
    loadMasterPresetList();
    needsRedraw = true;
}

/* ========== End Master Preset Picker Functions ========== */

/* ========== Tools Menu Functions ========== */

/* scanForToolModules(), enterToolsMenu() -> shadow_ui_tools.mjs */

/* drawToolsMenu() -> shadow_ui_tools.mjs */

/* ========== Tool File Browser Functions (shared filepath_browser) ========== */

/**
 * Check if the active tool supports creating new files.
 */
function toolAllowsNewFile() {
    return toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.allow_new_file;
}

/**
 * Inject a "+ New File" action item into the tool browser after refresh.
 * Only added when the active tool declares allow_new_file in tool_config.
 */
function injectNewFileItem() {
    if (!toolAllowsNewFile()) return;
    if (!toolBrowserState || !toolBrowserState.items) return;
    /* Insert after ".." (up) entry if present, otherwise at top */
    let insertIdx = 0;
    if (toolBrowserState.items.length > 0 && toolBrowserState.items[0].kind === "up") {
        insertIdx = 1;
    }
    toolBrowserState.items.splice(insertIdx, 0, {
        kind: "new_file",
        label: "+ New File",
        path: ""
    });
    /* Adjust selectedIndex if it was at or after the insert point */
    if (toolBrowserState.selectedIndex >= insertIdx) {
        toolBrowserState.selectedIndex++;
    }
}

/**
 * Generate a timestamped file path for a new file in the given directory.
 * Uses the first input extension from the tool config.
 */
function generateNewFilePath(dir) {
    const d = new Date();
    const pad2 = (n) => n < 10 ? "0" + n : "" + n;
    const stamp = d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate())
        + "_" + pad2(d.getHours()) + "-" + pad2(d.getMinutes()) + "-" + pad2(d.getSeconds());
    const exts = (toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.input_extensions) || [".wav"];
    const ext = Array.isArray(exts) ? exts[0] : exts;
    return dir + "/New_" + stamp + ext;
}

/* Launch a tool from the tools menu after the feedback gate has run (or
 * been bypassed when the tool has no id). Mirrors the original VIEWS.TOOLS
 * select dispatch — every launch path (overtake / set_picker /
 * skip_file_browser+interactive / file browser / standalone / fallback)
 * must be preserved here. */
function launchToolConfirmed(tool) {
    /* Track tool selection. Overtake tools are tracked inside
     * loadOvertakeModule → skip here to avoid a double event. */
    if (tool.kind !== 'overtake' && typeof host_track_event === "function" && tool.id) {
        host_track_event('module_loaded', '"module_id":"' + tool.id + '","source":"tools"');
    }
    if (tool.kind === 'overtake') {
        debugLog("TOOLS SELECT overtake: " + tool.id);
        announce(`Loading ${tool.name || tool.id}`);
        loadOvertakeModule(tool);
        return;
    }
    debugLog("TOOLS SELECT tool: " + tool.id + " config=" + JSON.stringify(tool.tool_config));
    if (tool.tool_config && tool.tool_config.set_picker) {
        debugLog("TOOLS SELECT: entering set picker");
        enterToolSetPicker(tool);
    } else if (tool.tool_config && tool.tool_config.skip_file_browser && tool.tool_config.interactive) {
        debugLog("TOOLS SELECT: skip_file_browser, launching interactive directly");
        startInteractiveTool(tool, "");
    } else if (tool.tool_config && (tool.tool_config.command || tool.tool_config.interactive || tool.tool_config.engines)) {
        debugLog("TOOLS SELECT: entering file browser");
        enterToolFileBrowser(tool);
    } else if (tool.standalone) {
        debugLog("TOOLS SELECT: launching standalone binary");
        announce(`Launching ${tool.name}`);
        const binaryPath = tool.path + "/standalone";
        host_system_cmd("sh /data/UserData/schwung/launch-standalone.sh " + binaryPath);
    } else {
        debugLog("TOOLS SELECT: tool not available");
        announce("Tool not available");
    }
}

function enterToolFileBrowser(toolModule) {
    debugLog("enterToolFileBrowser: " + toolModule.id);
    toolActiveTool = toolModule;
    const exts = (toolModule.tool_config && toolModule.tool_config.input_extensions) || [".wav"];
    const filter = Array.isArray(exts) ? exts : [exts];
    toolBrowserState = buildFilepathBrowserState(
        { root: "/data/UserData/UserLibrary", filter: filter, name: toolModule.name },
        ""
    );
    refreshFilepathBrowser(toolBrowserState, FILEPATH_BROWSER_FS);
    injectNewFileItem();
    /* Inject "Resume" item at top if a hidden session exists for this tool */
    debugLog("enterToolFileBrowser resume check: hiddenModulePath=" + toolHiddenModulePath +
             " id=" + toolModule.id + " hiddenFile=" + toolHiddenFile);
    if (toolHiddenFile && toolHiddenFile !== "_hidden_" && toolHiddenModulePath.indexOf("/" + toolModule.id + "/") !== -1) {
        const resumeLabel = "Resume: " + toolHiddenFile.substring(toolHiddenFile.lastIndexOf("/") + 1);
        toolBrowserState.items.splice(0, 0, { label: resumeLabel, kind: "resume", path: toolHiddenFile });
        toolBrowserState.selectedIndex = 0;
    }
    setView(VIEWS.TOOL_FILE_BROWSER);
    needsRedraw = true;
    const firstEntry = toolBrowserState.items.length > 0 ? toolBrowserState.items[0].label : "empty";
    announce(toolModule.name + " - Browse files, " + firstEntry);
    debugLog("enterToolFileBrowser: view now=" + view + " items=" + toolBrowserState.items.length);
}

function toolBrowserNavigate(delta) {
    if (!toolBrowserState || toolBrowserState.items.length === 0) return;
    moveFilepathBrowserSelection(toolBrowserState, delta);
    const item = toolBrowserState.items[toolBrowserState.selectedIndex];
    announce(item.label + (item.kind === "dir" ? " folder" : ""));
    /* Trigger preview for audio files */
    if (previewEnabled && item.kind === "file" && item.path && isPreviewableFile(item.path)) {
        previewPendingPath = item.path;
        previewPendingTime = Date.now();
    } else {
        previewStopIfPlaying();
    }
}

function toolBrowserSelect() {
    previewStopIfPlaying();
    if (!toolBrowserState || toolBrowserState.items.length === 0) return;
    const selItem = toolBrowserState.items[toolBrowserState.selectedIndex];
    /* Handle "Resume" session item */
    if (selItem && selItem.kind === "resume") {
        debugLog("toolBrowserSelect: resuming hidden session");
        startInteractiveTool(toolActiveTool, selItem.path);
        return;
    }
    /* Handle "+ New File" action item */
    if (selItem && selItem.kind === "new_file") {
        const newPath = generateNewFilePath(toolBrowserState.currentDir);
        toolSelectedFile = newPath;
        debugLog("toolBrowserSelect: new file -> " + newPath);
        if (toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.interactive) {
            startInteractiveTool(toolActiveTool, newPath);
        } else {
            toolSelectedEngine = null;
            enterToolConfirm();
        }
        return;
    }
    const result = activateFilepathBrowserItem(toolBrowserState);
    if (result.action === "open") {
        refreshFilepathBrowser(toolBrowserState, FILEPATH_BROWSER_FS);
        injectNewFileItem();
        needsRedraw = true;
        if (toolBrowserState.items.length > 0) {
            announce(toolBrowserState.items[0].label);
        } else {
            announce("empty");
        }
    } else if (result.action === "select") {
        toolSelectedFile = result.value;
        if (toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.interactive) {
            startInteractiveTool(toolActiveTool, result.value);
        } else if (toolActiveTool && toolActiveTool.tool_config &&
                   toolActiveTool.tool_config.engines) {
            enterToolEngineSelect();
        } else {
            toolSelectedEngine = null;
            enterToolConfirm();
        }
    }
}

function toolBrowserBack() {
    previewStopIfPlaying();
    if (!toolBrowserState) { enterToolsMenu(); return; }
    const root = toolBrowserState.root;
    if (toolBrowserState.currentDir === root) {
        enterToolsMenu();
        return;
    }
    /* Navigate up by setting currentDir to parent */
    const lastSlash = toolBrowserState.currentDir.lastIndexOf("/");
    if (lastSlash > 0) {
        toolBrowserState.currentDir = toolBrowserState.currentDir.substring(0, lastSlash);
        if (toolBrowserState.currentDir.length < root.length) {
            toolBrowserState.currentDir = root;
        }
    } else {
        toolBrowserState.currentDir = root;
    }
    toolBrowserState.selectedIndex = 0;
    toolBrowserState.selectedPath = "";
    refreshFilepathBrowser(toolBrowserState, FILEPATH_BROWSER_FS);
    injectNewFileItem();
    needsRedraw = true;
    const dirName = toolBrowserState.currentDir.substring(toolBrowserState.currentDir.lastIndexOf("/") + 1);
    announce(dirName);
}

/* drawToolFileBrowser() -> shadow_ui_tools.mjs */

/* ========== Tool Set Picker Functions ========== */

function scanSetsForPicker() {
    const SETS_DIR = "/data/UserData/UserLibrary/Sets";
    const result = [];
    try {
        const entries = os.readdir(SETS_DIR) || [];
        const dirList = entries[0];
        if (!Array.isArray(dirList)) return result;
        for (const uuid of dirList) {
            if (uuid.startsWith(".")) continue;
            const subEntries = os.readdir(SETS_DIR + "/" + uuid) || [];
            const subList = subEntries[0];
            if (!Array.isArray(subList)) continue;
            for (const name of subList) {
                if (name.startsWith(".")) continue;
                result.push({ uuid, name });
                break; /* only one subdirectory per UUID */
            }
        }
    } catch (e) {
        debugLog("scanSetsForPicker error: " + e);
    }
    result.sort((a, b) => a.name.localeCompare(b.name));
    return result;
}

function enterToolSetPicker(toolModule) {
    debugLog("enterToolSetPicker: " + toolModule.id);
    toolActiveTool = toolModule;
    toolSetList = scanSetsForPicker();
    toolSetPickerIndex = 0;
    setView(VIEWS.TOOL_SET_PICKER);
    needsRedraw = true;
    if (toolSetList.length > 0) {
        announce(toolModule.name + " - Choose set, " + toolSetList[0].name);
    } else {
        announce(toolModule.name + " - No sets found");
    }
}

function toolSetPickerNavigate(delta) {
    if (toolSetList.length === 0) return;
    toolSetPickerIndex = Math.max(0, Math.min(toolSetList.length - 1, toolSetPickerIndex + delta));
    announce(toolSetList[toolSetPickerIndex].name);
}

function toolSetPickerSelect() {
    if (toolSetList.length === 0) return;
    const chosen = toolSetList[toolSetPickerIndex];
    toolSelectedSetUuid = chosen.uuid;
    toolSelectedSetName = chosen.name;
    /* Use the set name as the "selected file" for display in confirm/processing */
    toolSelectedFile = chosen.name;
    toolSelectedEngine = null;
    enterToolConfirmForSet();
}

function enterToolConfirmForSet() {
    toolFileDurationSec = 0;  /* Unknown duration for sets */
    setView(VIEWS.TOOL_CONFIRM);
    needsRedraw = true;
    announce("Render " + toolSelectedSetName + "? Jog to confirm, Back to cancel");
}

/* drawToolSetPicker() -> shadow_ui_tools.mjs */

/* ========== Tool Engine Selection View ========== */

/* Filter engines to only those whose command script exists on disk */
function getAvailableEngines() {
    const engines = toolActiveTool.tool_config.engines;
    if (!engines) return [];
    return engines.filter(e => {
        const cmdPath = toolActiveTool.path + "/" + e.command;
        try {
            const st = os.stat(cmdPath);
            return st && st[1] === 0;
        } catch (err) { return false; }
    });
}

function enterToolEngineSelect() {
    toolEngineIndex = 0;
    toolSelectedEngine = null;
    toolAvailableEngines = getAvailableEngines();
    if (toolAvailableEngines.length === 0) {
        announce("No engines installed");
        return;
    }
    if (toolAvailableEngines.length === 1) {
        /* Only one engine available — skip selection */
        toolSelectedEngine = toolAvailableEngines[0];
        enterToolConfirm();
        return;
    }
    setView(VIEWS.TOOL_ENGINE_SELECT);
    needsRedraw = true;
    announce("Choose engine, " + toolAvailableEngines[0].name);
}

function toolEngineNavigate(delta) {
    if (toolAvailableEngines.length === 0) return;
    toolEngineIndex = Math.max(0, Math.min(toolAvailableEngines.length - 1, toolEngineIndex + delta));
    announce(toolAvailableEngines[toolEngineIndex].name);
}

function toolEngineConfirm() {
    toolSelectedEngine = toolAvailableEngines[toolEngineIndex];
    enterToolConfirm();
}

/* drawToolEngineSelect() -> shadow_ui_tools.mjs */

/* ========== Tool Confirm View ========== */

/* Read WAV file duration in seconds from header.
 * WAV format: bytes 24-27 = sample rate (uint32 LE),
 *             bytes 28-31 = byte rate (uint32 LE),
 *             bytes 32-33 = block align (uint16 LE).
 * File size minus header (~44 bytes) divided by byte rate = duration. */
function getWavDurationSec(filePath) {
    try {
        const st = os.stat(filePath);
        if (!st || st[1] !== 0) return 0;
        const fileSize = st[0].size;

        const fd = os.open(filePath, os.O_RDONLY);
        if (fd < 0) return 0;
        const buf = new ArrayBuffer(44);
        os.read(fd, buf, 0, 44);
        os.close(fd);

        const view = new DataView(buf);
        const byteRate = view.getUint32(28, true);  /* little-endian */
        if (byteRate <= 0) return 0;
        return (fileSize - 44) / byteRate;
    } catch (e) {
        debugLog("getWavDurationSec error: " + e);
        return 0;
    }
}

/* Estimate processing time based on selected engine.
 * SpleeterRT (3-stem): ~0.5x realtime on Move's Cortex-A72.
 * Spleeter TFLite (4-stem): ~3.0x realtime. */
function getToolProcessingRatio() {
    if (toolSelectedEngine && toolSelectedEngine.processing_ratio) {
        return toolSelectedEngine.processing_ratio;
    }
    return 0.5;  /* default for legacy/unknown engines */
}

function formatTime(seconds) {
    seconds = Math.round(seconds);
    if (seconds < 60) return seconds + "s";
    const m = Math.floor(seconds / 60);
    const s = seconds % 60;
    return m + "m " + (s < 10 ? "0" : "") + s + "s";
}

function enterToolConfirm() {
    /* Read file duration for time estimate */
    toolFileDurationSec = getWavDurationSec(toolSelectedFile);
    /* Auto-preview the selected file */
    if (toolSelectedFile.toLowerCase().endsWith(".wav")) {
        wavPlayerPlay(toolSelectedFile);
    }
    setView(VIEWS.TOOL_CONFIRM);
    needsRedraw = true;
    const fileName = toolSelectedFile.substring(toolSelectedFile.lastIndexOf("/") + 1);
    const estSec = Math.round(toolFileDurationSec * getToolProcessingRatio());
    const estStr = estSec > 0 ? (", about " + formatTime(estSec)) : "";
    announce("Separate " + fileName + "? Jog to confirm, Back to cancel" + estStr);
}

/* drawToolConfirm() -> shadow_ui_tools.mjs */

/* ========== Tool Processing View ========== */

function startToolProcess() {
    if (!toolActiveTool || !toolSelectedFile) return;

    const isSetPicker = toolActiveTool.tool_config && toolActiveTool.tool_config.set_picker;

    /* Compute output directory */
    if (isSetPicker) {
        toolOutputDir = "/data/UserData/UserLibrary/Recordings/Renders";
    } else {
        const fileName = toolSelectedFile.substring(toolSelectedFile.lastIndexOf("/") + 1);
        const baseName = fileName.replace(/\.[^.]+$/, "");  /* Remove extension */
        toolOutputDir = "/data/UserData/UserLibrary/Samples/Schwung/Stems/" + baseName;
    }

    /* Create output directory hierarchy */
    if (isSetPicker) {
        try { os.mkdir("/data/UserData/UserLibrary/Recordings", 0o755); } catch (e) {}
        try { os.mkdir("/data/UserData/UserLibrary/Recordings/Renders", 0o755); } catch (e) {}
    } else {
        try { os.mkdir("/data/UserData/UserLibrary/Samples/Schwung", 0o755); } catch (e) {}
        try { os.mkdir("/data/UserData/UserLibrary/Samples/Schwung/Stems", 0o755); } catch (e) {}
        try { os.mkdir(toolOutputDir, 0o755); } catch (e) {}
    }

    /* Remove stale marker files from previous runs */
    try { os.remove(toolOutputDir + "/.done"); } catch (e) {}
    try { os.remove(toolOutputDir + "/.error"); } catch (e) {}

    /* Spawn the tool process — use selected engine's command if available */
    const engineCmd = toolSelectedEngine ? toolSelectedEngine.command
        : (toolActiveTool.tool_config.command || "separate");
    const command = toolActiveTool.path + "/" + engineCmd;

    let execArgs;
    if (isSetPicker) {
        execArgs = [command, toolSelectedSetUuid, toolSelectedSetName, toolOutputDir];
        debugLog("startToolProcess (set): " + command + " " + toolSelectedSetUuid + " " + toolSelectedSetName);
    } else {
        execArgs = [command, toolSelectedFile, toolOutputDir];
        debugLog("startToolProcess: " + command + " " + toolSelectedFile + " " + toolOutputDir);
    }

    try {
        toolProcessPid = os.exec(execArgs, { block: false });
        debugLog("startToolProcess: pid=" + toolProcessPid);
    } catch (e) {
        debugLog("startToolProcess error: " + e);
        toolProcessPid = -1;
        toolResultSuccess = false;
        toolResultMessage = "Failed to start: " + e;
        setView(VIEWS.TOOL_RESULT);
        needsRedraw = true;
        announce("Error: " + toolResultMessage);
        return;
    }

    toolProcessingDots = 0;
    toolProcessStartTime = Date.now();
    toolStemsFound = 0;
    toolExpectedStems = (toolSelectedEngine && toolSelectedEngine.stems)
        ? toolSelectedEngine.stems.length
        : (toolActiveTool.tool_config && toolActiveTool.tool_config.stems)
            ? toolActiveTool.tool_config.stems.length : 4;
    setView(VIEWS.TOOL_PROCESSING);
    needsRedraw = true;
    const estSec = Math.round(toolFileDurationSec * getToolProcessingRatio());
    const estStr = estSec > 0 ? (", about " + formatTime(estSec) + " remaining") : "";
    announce("Processing" + estStr);
}

function startInteractiveTool(toolModule, filePath) {
    toolActiveTool = toolModule;
    toolSelectedFile = filePath;
    toolOvertakeActive = true;

    /* Non-overtake tools keep pads/buttons working for Move */
    const skipOvertake = (toolModule.tool_config && toolModule.tool_config.overtake === false);
    toolNonOvertake = skipOvertake;

    /* Check if this tool's DSP is already running (hidden session).
     * Use toolHiddenModulePath because overtakeModuleLoaded/Path get
     * overwritten when other tools (e.g. Song Mode) load and unload. */
    const dspAlreadyLoaded = toolHiddenFile &&
        toolHiddenModulePath.indexOf("/" + toolModule.id + "/") !== -1;

    if (dspAlreadyLoaded) {
        /* If a different file was selected, discard hidden session and start fresh */
        if (filePath && filePath !== toolHiddenFile) {
            debugLog("startInteractiveTool: different file, discarding hidden session");
            /* DSP may already be unloaded if another tool ran since hide */
            if (overtakeModuleLoaded) {
                unloadOvertakeDsp();
            }
            overtakeModuleLoaded = false;
            overtakeModulePath = "";
            overtakeModuleId = "";
            toolHiddenFile = "";
            toolHiddenModulePath = "";
            /* Fall through to normal fresh load below */
        } else if (!overtakeModuleLoaded) {
            /* Hidden session exists but DSP was replaced by another tool.
             * Do a fresh load with the hidden file — DSP will be reloaded.
             * Set host_tool_reconnect so Wave Edit checks sampler state. */
            debugLog("startInteractiveTool: hidden session DSP was replaced, doing fresh load with " + toolHiddenFile);
            filePath = (toolHiddenFile === "_hidden_") ? "" : toolHiddenFile;
            toolSelectedFile = filePath;
            toolHiddenFile = "";
            toolHiddenModulePath = "";
            globalThis.host_tool_reconnect = true;
            /* Fall through to normal fresh load below */
        } else {
            debugLog("startInteractiveTool: reconnecting to existing DSP session for " + toolModule.id);

            /* Re-enter overtake mode */
            if (!skipOvertake && typeof shadow_set_overtake_mode === "function") {
                shadow_set_overtake_mode(2);
            }

            /* Reset escape state variables for clean state */
            hostShiftHeld = false;
            hostVolumeKnobTouched = false;

            /* Re-activate LED queue */
            const wantLedQueue = !skipOvertake && !(toolModule.capabilities && toolModule.capabilities.skip_led_clear);
            if (wantLedQueue) activateLedQueue();

            /* Re-load the UI JS — DSP is already running */
            const uiPath = toolModule.path + "/ui.js";
            setView(VIEWS.OVERTAKE_MODULE);
            needsRedraw = true;

            /* Save current globals before loading - module may overwrite them */
            const savedInit = globalThis.init;
            const savedTick = globalThis.tick;
            const savedMidi = globalThis.onMidiMessageInternal;

            /* Reinstall shims before loading the ES module.
             * QuickJS resolves bare global identifiers at compile time,
             * so they must exist on globalThis when shadow_load_ui_module
             * evaluates the module JS — even if they were set before. */
            globalThis.host_module_set_param = function(key, value) {
                if (typeof shadow_set_param === "function") {
                    return shadow_set_param(0, "overtake_dsp:" + key, String(value));
                }
            };
            globalThis.host_module_set_param_blocking = function(key, value, timeoutMs) {
                var timeout = (typeof timeoutMs === "number" && timeoutMs > 0) ? timeoutMs : 500;
                if (typeof shadow_set_param_timeout === "function") {
                    return shadow_set_param_timeout(0, "overtake_dsp:" + key, String(value), timeout);
                } else if (typeof shadow_set_param === "function") {
                    return shadow_set_param(0, "overtake_dsp:" + key, String(value));
                }
            };
            globalThis.host_module_get_param = function(key) {
                if (typeof shadow_get_param === "function") {
                    return shadow_get_param(0, "overtake_dsp:" + key);
                }
            };
            globalThis.host_exit_module = function() {
                debugLog("host_exit_module called by overtake module (reconnect)");
                if (toolOvertakeActive) {
                    exitToolOvertake();
                } else {
                    exitOvertakeMode();
                }
            };
            globalThis.host_suspend_overtake = function() {
                debugLog("host_suspend_overtake called by overtake module (reconnect)");
                suspendOvertakeMode();
            };
            globalThis.host_hide_module = function() {
                debugLog("host_hide_module called by overtake module (reconnect)");
                if (toolOvertakeActive) {
                    hideToolOvertake();
                }
            };
            globalThis.host_open_text_entry = function(opts) {
                openTextEntry({
                    title: opts.title || "Text Entry",
                    initialText: opts.initialText || "",
                    onAnnounce: announce,
                    onConfirm: opts.onConfirm || function() {},
                    onCancel: opts.onCancel || function() {}
                });
            };
            globalThis.host_tool_file_path = filePath || "";

            if (typeof shadow_load_ui_module === "function") {
                const result = shadow_load_ui_module(uiPath);
                debugLog("startInteractiveTool reconnect: shadow_load_ui_module returned " + result);
                if (!result) {
                    debugLog("startInteractiveTool reconnect: failed to load UI");
                    toolOvertakeActive = false;
                    setView(VIEWS.TOOL_RESULT);
                    toolResultMessage = "Failed to reload UI";
                    toolResultSuccess = false;
                    needsRedraw = true;
                    return;
                }
            } else {
                debugLog("startInteractiveTool reconnect: shadow_load_ui_module not available");
                toolOvertakeActive = false;
                setView(VIEWS.TOOL_RESULT);
                toolResultMessage = "Failed to reload UI";
                toolResultSuccess = false;
                needsRedraw = true;
                return;
            }

            /* Capture callbacks (same keys as loadOvertakeModule) */
            overtakeModuleCallbacks = {
                init: (globalThis.init !== savedInit) ? globalThis.init : null,
                tick: (globalThis.tick !== savedTick) ? globalThis.tick : null,
                onMidiMessageInternal: (globalThis.onMidiMessageInternal !== savedMidi) ? globalThis.onMidiMessageInternal : null,
                onUnload: (typeof globalThis.onUnload === "function") ? globalThis.onUnload : null,
                /* onResume(): optional. Called once each time the module is
                 * resumed from suspend (init() is NOT re-run). See
                 * invokeModuleOnResume for the module-facing contract. */
                onResume: (typeof globalThis.onResume === "function") ? globalThis.onResume : null
            };
            globalThis.init = savedInit;
            globalThis.tick = savedTick;
            globalThis.onMidiMessageInternal = savedMidi;
            if (typeof globalThis.onUnload === "function") delete globalThis.onUnload;
            if (typeof globalThis.onResume === "function") delete globalThis.onResume;
            debugLog("startInteractiveTool reconnect: callbacks captured - init:" + !!overtakeModuleCallbacks.init +
                     " tick:" + !!overtakeModuleCallbacks.tick +
                     " midi:" + !!overtakeModuleCallbacks.onMidiMessageInternal);

            /* Signal reconnect to the UI via a global flag */
            globalThis.host_tool_reconnect = true;

            /* Defer init — clear LEDs progressively first, same as fresh load.
             * The overtake init phase will call init() after LEDs are cleared. */
            overtakeInitPending = true;
            overtakeInitTicks = 0;
            ledClearIndex = 0;
            debugLog("startInteractiveTool reconnect: init deferred, LEDs will clear progressively");

            /* Clear reconnect flag after init is called (in the deferred path) */
            /* Note: host_tool_reconnect stays set until init() runs */

            return;
        }
    }

    /* Build a moduleInfo descriptor compatible with loadOvertakeModule */
    const uiPath = toolModule.path + "/ui.js";
    const uiStat = os.stat(uiPath);
    if (!uiStat || uiStat[0] === -1) {
        announce("Tool UI not found");
        setView(VIEWS.TOOL_RESULT);
        toolResultSuccess = false;
        toolResultMessage = "Missing ui.js";
        toolOvertakeActive = false;
        toolNonOvertake = false;
        needsRedraw = true;
        return;
    }

    /* Check for DSP plugin */
    const dspPath = toolModule.path + "/dsp.so";
    const dspStat = os.stat(dspPath);
    const hasDsp = (dspStat && dspStat[0] !== -1) ? "dsp.so" : null;

    /* Construct moduleInfo in the same format as scanForOvertakeModules */
    const moduleInfo = {
        id: toolModule.id,
        name: toolModule.name,
        path: toolModule.path,
        uiPath: uiPath,
        dsp: hasDsp,
        basePath: toolModule.path,
        capabilities: toolModule.capabilities || null
    };

    /* Reuse the existing overtake module loading infrastructure */
    announce("Loading " + toolModule.name);
    const success = loadOvertakeModule(moduleInfo, skipOvertake);
    if (success) {
        /* Replace the plain-overtake record loadOvertakeModule just wrote:
         * relaunching an interactive tool must go back through this function
         * so toolOvertakeActive / toolNonOvertake / file_path are all set. */
        lastLaunchedTool = {
            kind: "interactive",
            module: toolModule,
            skipOvertake: skipOvertake,
            filePath: filePath || ""
        };
        /* DSP is now loaded — pass the selected file path */
        globalThis.host_tool_file_path = filePath || "";
        if (typeof shadow_set_param === "function") {
            shadow_set_param(0, "overtake_dsp:file_path", filePath);
            /* Pass project BPM for tempo-aware tools */
            if (overlayState && overlayState.samplerBpm > 0) {
                shadow_set_param(0, "overtake_dsp:project_bpm", String(overlayState.samplerBpm));
            }
        }
    } else {
        toolOvertakeActive = false;
        setView(VIEWS.TOOL_RESULT);
        toolResultSuccess = false;
        toolResultMessage = "Failed to load tool";
        needsRedraw = true;
        announce("Error: Failed to load tool");
    }
}

function countStemFiles() {
    /* Count .wav files in the output directory */
    try {
        const entries = os.readdir(toolOutputDir) || [];
        const dirList = entries[0];
        if (!Array.isArray(dirList)) return 0;
        let count = 0;
        for (const name of dirList) {
            if (name.toLowerCase().endsWith(".wav")) count++;
        }
        return count;
    } catch (e) { return 0; }
}

function getStemFileNames() {
    try {
        const entries = os.readdir(toolOutputDir) || [];
        const dirList = entries[0];
        if (!Array.isArray(dirList)) return [];
        const wavFiles = [];
        for (const name of dirList) {
            if (name.toLowerCase().endsWith(".wav")) wavFiles.push(name);
        }
        wavFiles.sort();
        return wavFiles;
    } catch (e) { return []; }
}

function pollToolProcess() {
    /* Count completed stem files for progress */
    toolStemsFound = countStemFiles();

    /* Check for .done or .error marker files */
    try {
        const doneStat = os.stat(toolOutputDir + "/.done");
        if (doneStat && doneStat[1] === 0) {
            /* Success! */
            toolProcessPid = -1;
            toolResultSuccess = true;
            const isSetPicker = toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.set_picker;
            if (isSetPicker) {
                /* Set picker tools (e.g. render): skip stem review, show simple result */
                toolResultMessage = "Saved to\nrecordings/";
                setView(VIEWS.TOOL_RESULT);
                needsRedraw = true;
                announce("Complete! Saved to recordings");
            } else {
                toolStemFiles = getStemFileNames();
                if (toolStemFiles.length > 0) {
                    toolStemReviewIndex = 0;
                    toolStemKept = new Array(toolStemFiles.length).fill(true);
                    setView(VIEWS.TOOL_STEM_REVIEW);
                    needsRedraw = true;
                    announce("Complete! " + toolStemFiles.length + " stems. Push to save all, or jog to choose.");
                } else {
                    /* Fallback if no WAVs found */
                    const baseName = toolOutputDir.substring(toolOutputDir.lastIndexOf("/") + 1);
                    toolResultMessage = "Stems saved to\nStems/" + baseName + "/";
                    setView(VIEWS.TOOL_RESULT);
                    needsRedraw = true;
                    announce("Complete! Stems saved to Stems " + baseName);
                }
            }
            return;
        }
    } catch (e) { /* .done not found yet */ }

    try {
        const errStat = os.stat(toolOutputDir + "/.error");
        if (errStat && errStat[1] === 0) {
            /* Error */
            toolProcessPid = -1;
            toolResultSuccess = false;
            try {
                const errContent = std.loadFile(toolOutputDir + "/.error");
                toolResultMessage = "Error: " + (errContent || "unknown").trim();
            } catch (e2) {
                toolResultMessage = "Error: separation failed";
            }
            setView(VIEWS.TOOL_RESULT);
            needsRedraw = true;
            announce(toolResultMessage);
            return;
        }
    } catch (e) { /* .error not found yet */ }

    /* Also check if process exited via waitpid (non-blocking) */
    if (toolProcessPid > 0) {
        try {
            const wp = os.waitpid(toolProcessPid, os.WNOHANG);
            if (wp && wp[0] === toolProcessPid) {
                /* Process exited but no marker file — check exit status */
                const status = wp[1];
                const exitCode = (status >> 8) & 0xff;
                if (exitCode !== 0) {
                    toolProcessPid = -1;
                    toolResultSuccess = false;
                    toolResultMessage = "Process exited with code " + exitCode;
                    setView(VIEWS.TOOL_RESULT);
                    needsRedraw = true;
                    announce(toolResultMessage);
                }
                /* If exit code 0 but no .done yet, keep polling briefly */
            }
        } catch (e) { /* waitpid not available or error, keep polling */ }
    }
}

function cancelToolProcess() {
    if (toolProcessPid > 0) {
        try {
            os.kill(toolProcessPid, os.SIGTERM);
            debugLog("cancelToolProcess: killed pid " + toolProcessPid);
        } catch (e) {
            debugLog("cancelToolProcess error: " + e);
        }
        toolProcessPid = -1;
    }
    toolResultSuccess = false;
    toolResultMessage = "Cancelled";
    setView(VIEWS.TOOL_RESULT);
    needsRedraw = true;
    announce("Cancelled");
}

/* drawToolProcessing(), drawToolResult(), drawToolStemReview() -> shadow_ui_tools.mjs */

/* ========== Global Settings Functions ========== */

/*
 * GLOBAL SETTINGS, as the page chrome — the same engine slot settings and
 * Master FX settings run on.
 *
 * The component name is not a module and not "slot"; it names the SYNTHESISED
 * contract, so headerTitle() has something to say and the io can tell which
 * contract is loaded. The declaration is shadow_ui_global_grid.mjs and the
 * backends are globalGridIoFor(); nothing about which params exist lives here.
 *
 * NOT gated on paramPagesEnabled(). Every other consumer of this engine has the
 * hierarchy editor to fall back to when the screen reader is on; Global Settings
 * has nothing — its bespoke list is deleted — and it is the screen you go to in
 * order to turn the screen reader off. paramPagesLayout() forces the LIST for
 * TTS instead, which is the arrangement that has a selected row to announce.
 */
const GLOBAL_SETTINGS_COMPONENT = "global_settings";

function enterGlobalSettingsGrid(restorePageName) {
    /* A fresh entry is not a return from a modal. Whatever hand-off was
     * outstanding has been served by getting here. */
    globalModalFromGrid = false;
    enterParamPages(0, GLOBAL_SETTINGS_COMPONENT, GLOBAL_SETTINGS_COMPONENT,
                    restorePageName || null, globalGridIoFor(),
                    /* No moduleKey: there is no module behind this contract to
                     * abbreviate. Back leaves shadow mode, which is not a view,
                     * so it is an onExit rather than a returnView. */
                    { label: "Global", name: "Settings",
                      /* PINNED TO THE LIST, whatever Param View says.
                       *
                       * Param View is a preference about MODULE parameters,
                       * where eight cells you can grab at once is the point.
                       * Every one of these 25 is a set-once toggle, and several
                       * are destructive to brush past: link_audio_routing
                       * re-routes Move's audio, resample_bridge replaces the
                       * sampler's input, and param_view changes the screen you
                       * are standing on. A knob has no detent to tell you it
                       * moved. Slot and Master FX settings deliberately do not
                       * pin — their Volume, Mute and Solo really are
                       * performance controls. */
                      layout: LAYOUT_LIST,
                      onExit: () => {
                          if (typeof shadow_request_exit === "function") shadow_request_exit();
                      } });
    needsRedraw = true;
}

function enterGlobalSettings() {
    enterGlobalSettingsGrid(null);
}

/*
 * The page name enterGlobalSettingsScreenReader lands on.
 *
 * DERIVED from the section it names rather than spelled again. The engine
 * restores a page by NAME (restorePageName; see enterParamPages) and the name
 * planPages gives a section is its root nav entry's LABEL — so a hardcoded
 * "Screen Reader" here would silently stop matching the day the label is
 * reworded, and the jump would land on page 1 with nothing to say it had.
 * `accessibility` is the level ID, which is the stable half.
 */
const GLOBAL_SCREEN_READER_PAGE =
    (GLOBAL_SECTIONS.find((s) => s.id === "accessibility") || {}).label || null;

function enterGlobalSettingsScreenReader() {
    enterGlobalSettingsGrid(GLOBAL_SCREEN_READER_PAGE);
    /*
     * ...and SAY so, which the restore itself will not.
     *
     * controller.restorePage is deliberately silent: its other caller is
     * "return to the page you were already on" after an editor closes, where an
     * announcement is noise. This is the opposite — a jump the user asked for
     * from a shortcut — and load() has already announced "Display, 1 of 7" on
     * the way past. Leaving it at that names the wrong page out loud to the one
     * user who reached this screen by ear.
     */
    if (GLOBAL_SCREEN_READER_PAGE) announce(GLOBAL_SCREEN_READER_PAGE + " Settings");
}

function handleGlobalSettingsAction(key) {
    if (key === "help") {
        helpReturnView = VIEWS.GLOBAL_SETTINGS;
        handleMasterFxSettingsAction("help");
        return;
    }
    if (key === "check_updates") {
        storeReturnView = VIEWS.GLOBAL_SETTINGS;
        showUpdatesAvailableScreen();
        return;
    }
    if (key === "module_store") {
        showModuleStorePointer();
        return;
    }
}

/* ========== End Global Settings Functions ========== */

/* enterMasterFxSettings() -> shadow_ui_master_fx.mjs */

/*
 * Whether masterFxConfig is known to still describe the DSP.
 *
 * The exact counterpart of chainConfigFresh, and it exists for the same two
 * reasons: a shape edit renumbers the chain underneath the editor without
 * changing WHICH modules it holds (so nothing keyed on a module id would
 * notice), and a `+` box materialises a position that only exists in the model
 * (so the reload must not happen until the picker has resolved it).
 */
let masterFxConfigFresh = false;

function invalidateMasterFxConfig() { masterFxConfigFresh = false; }

/** Reload the Master FX positions from the DSP only if they went stale. */
function ensureMasterFxConfigFresh() {
    if (!masterFxConfigFresh) loadMasterFxChainConfig();
    return masterFxConfig;
}

/* Load master FX chain configuration from DSP */
function loadMasterFxChainConfig() {
    masterFxConfig = makeEmptyMasterFxConfig();
    /* This IS the reload every other path invalidates towards, and the DSP is
     * the authority on the length again. */
    masterFxConfigFresh = true;
    masterFxChainLength = -1;

    /* Query each slot's module from DSP */
    for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
        const key = `fx${i}`;
        const moduleId = getMasterFxSlotModule(i - 1);
        masterFxConfig[key].module = moduleId || "";
        /* Different module — it may implement display_name even if the
         * last one didn't, so poll it at full rate again. */
        delete fxDisplayNameCache[`master:${key}`];
        delete fxDisplayNameSkip[`master:${key}`];
        delete fxDisplayNameBackoff[`master:${key}`];
    }
}

/* Get a parameter from a master FX slot (0..MASTER_FX_SLOTS-1).
 * Index-taking wrapper over the shared chain-target accessor. The "" rather
 * than null is this caller's convention and is preserved here. */
function getMasterFxParam(slotIndex, key) {
    return chainTargetGetParam(MASTER_CHAIN_TARGET, masterFxComponentKey(slotIndex), key) || "";
}

/* Get module ID loaded in a master FX slot (0..MASTER_FX_SLOTS-1).
 * The shim answers ":name" with the module_id. */
function getMasterFxSlotModule(slotIndex) {
    return getMasterFxParam(slotIndex, "name");
}

/* Set module for a master FX slot */
function setMasterFxSlotModule(slotIndex, dspPath) {
    /* Clear warning tracking for this slot so warning can show again for new module */
    warningShownForMasterFx.delete(slotIndex);
    /*
     * A position was written, so the chain's length is derivable again.
     *
     * masterFxChainLength is only ever explicitly set to cover a TRAILING HOLE
     * — the position a `+` box opens, which is empty and would otherwise look
     * like the end of the chain. Once a module write lands there the hole is
     * gone and the derivation is correct, so dropping the override here is both
     * safe and the one place that catches every bulk path (preset load, set
     * load, clear) without each of them having to remember.
     */
    masterFxChainLength = -1;
    return chainTargetSetParam(MASTER_CHAIN_TARGET, masterFxComponentKey(slotIndex),
                               "module", dspPath || "");
}

/*
 * The rows the Master FX picker shows: the modules, plus this position's Move
 * Left / Move Right.
 *
 * Held rather than recomputed because the confirm has to resolve the SAME index
 * the draw and the jog were addressing, and the move rows only exist while the
 * position has somewhere to go — which the confirm itself changes.
 */
let masterFxPickerItems = [];

/* Enter module selection for a Master FX position */
function enterMasterFxModuleSelect(componentIndex) {
    const comp = masterFxChainComponents()[componentIndex];
    if (!comp || comp.kind !== "module") return;

    /*
     * Move Left / Move Right, tucked under the loaded module — the same rows
     * the slot chain's picker carries, built by the same chainMoveEntries, so
     * Shift+jog is not the only way to reorder a Master FX chain either. A
     * modifier gesture with no discoverable equivalent is a feature only the
     * person who wrote it knows about. Offered only where they would DO
     * something, so the list never carries a row that answers a click by doing
     * nothing.
     */
    const currentModule = masterFxConfig[comp.key]?.module || "";
    masterFxPickerItems = MASTER_FX_OPTIONS.slice();
    const loadedIdx = currentModule
        ? masterFxPickerItems.findIndex(o => o.id === currentModule) : -1;
    masterFxPickerItems.splice(loadedIdx >= 0 ? loadedIdx + 1 : 0, 0,
        ...chainMoveEntries(masterFxChainConfig(), comp.key));

    /* Set selection index to current module if any */
    selectedMasterFxModuleIndex = masterFxPickerItems.findIndex(o => o.id === currentModule);
    if (selectedMasterFxModuleIndex < 0) selectedMasterFxModuleIndex = 0;

    selectingMasterFxModule = true;
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = masterFxPickerItems[selectedMasterFxModuleIndex]?.name || "None";
    announce(`Select ${comp.label}, ${moduleName}`);
}

/*
 * Apply a Master FX picker choice.
 *
 * The shape half of this is the slot chain's, function for function:
 * applyPickerChoiceToChain turns `None` on a list position into a `remove`,
 * withPendingChainInsert turns a `+` that is not an append into an `insert`,
 * and writeChainShape sends whichever verb resulted. The only Master-FX-shaped
 * thing left is that the module write takes a DSP PATH rather than a module id
 * — the shim loads master positions by path.
 */
function applyMasterFxModuleSelection() {
    const comps = masterFxChainComponents();
    const comp = comps[selectedMasterFxComponent];
    if (!comp || comp.kind !== "module") {
        cancelPendingChainInsert();
        selectingMasterFxModule = false;
        needsRedraw = true;
        return;
    }

    /* Was this picker opened from a `+` box? Read BEFORE the choice is applied,
     * because applying it fills the very hole this recognises. */
    const pending = pendingChainInsertFor(MASTER_CHAIN_TARGET, comp.key);
    const selected = masterFxPickerItems[selectedMasterFxModuleIndex];

    if (selected && selected.id === "__get_more__") {
        /* Open store picker for audio FX modules */
        selectingMasterFxModule = false;
        enterStorePicker('master_fx');
        return;
    }

    /* Move Left / Move Right — the same reorder the Shift+jog gesture performs,
     * through the same moveChainComponent, so the bounds are the model's in
     * both. */
    if (selected && (selected.id === "__move_left__" || selected.id === "__move_right__")) {
        const delta = selected.id === "__move_left__" ? -1 : 1;
        if (moveChainComponent(MASTER_CHAIN_TARGET, comp.key, delta)) {
            const after = masterFxChainComponents();
            const at = after.findIndex(
                (c) => c.key === chainEditorKeyAt(comp.section, comp.index + delta));
            if (at >= 0) selectedMasterFxComponent = at;
            const moved = after[selectedMasterFxComponent];
            announce(`${moved ? moved.label : comp.label} moved ${delta < 0 ? "left" : "right"}`);
            saveMasterFxChainConfig();
        }
        selectingMasterFxModule = false;
        needsRedraw = true;
        return;
    }

    const picked = selected && selected.id ? selected.id : "";

    /*
     * `None` from a `+` box is a CANCEL, not a removal. The position it would
     * remove was never written, so there is nothing to renumber — and running
     * it through the removal path would send `<section>:remove` for a position
     * the DSP does not have, compacting away a module the user never touched.
     */
    if (pending && !picked) {
        cancelPendingChainInsert();
        selectingMasterFxModule = false;
        needsRedraw = true;
        return;
    }

    const choice = withPendingChainInsert(
        applyPickerChoiceToChain(masterFxChainConfig(), comp.key, picked), pending);
    MASTER_CHAIN_TARGET.setConfig(choice.cfg);

    /* A shape change first, if the choice asked for one: a removal compacts the
     * list and an insert opens a hole in it, and either way every position past
     * the edit is renumbered — which one `<id>:module` write cannot say. One
     * verb, and the DSP permutes rather than reloading. A removal is COMPLETE
     * here; an insert only opens the hole and the module write below fills it. */
    if (choice.shape) writeChainShape(MASTER_CHAIN_TARGET, choice.shape);
    if (!(choice.shape && choice.shape.kind === "remove")) {
        if (pickerReplacedModule(choice.replaced, picked)) {
            clearLfoRoutingForComponent(MASTER_CHAIN_TARGET, comp.key);
        }
        setMasterFxSlotModule(selectedMasterFxComponent, (selected && selected.dspPath) || "");
    }

    resetLfoTargetLabels();
    saveMasterFxChainConfig();

    /* Exit module selection mode */
    selectingMasterFxModule = false;
    needsRedraw = true;
}

/* Save master FX chain configuration */
function saveMasterFxChainConfig() {
    /* The shim persists the state, but we also save to shadow config */
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        let config = {};
        try {
            const content = host_read_file(configPath);
            if (content) config = JSON.parse(content);
        } catch (e) {}

        /* Save master FX chain - store dspPaths and plugin state for each slot */
        config.master_fx_chain = {
            preset_name: currentMasterPresetName || ""
        };
        let masterFxLfoConfig = null;
        if (typeof shadow_get_param === "function") {
            const lfos = {};
            for (let li = 1; li <= 2; li++) {
                try {
                    const configJson = shadow_get_param(0, "master_fx:lfo" + li + ":config");
                    if (configJson) {
                        lfos["lfo" + li] = JSON.parse(configJson);
                    }
                } catch (e) {}
            }
            if (Object.keys(lfos).length > 0) {
                masterFxLfoConfig = lfos;
            }
        }
        for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
            const key = `fx${i}`;
            const slotIdx = i - 1;
            const moduleId = masterFxConfig[key]?.module || "";
            const stateFilePath = activeSlotStateDir + "/master_fx_" + slotIdx + ".json";

            if (!moduleId) {
                /* Empty slot - keep LFO snapshot with fx1 state for per-set restore */
                if (slotIdx === 0 && masterFxLfoConfig) {
                    host_write_file(stateFilePath, JSON.stringify({ lfos: masterFxLfoConfig }, null, 2) + "\n");
                } else {
                    host_write_file(stateFilePath, "{}\n");
                }
                continue;
            }

            const opt = MASTER_FX_OPTIONS.find(o => o.id === moduleId);
            const dspPath = opt?.dspPath || "";
            const slotConfig = {
                id: moduleId,
                path: dspPath
            };

            /* Snapshot plugin state if available */
            let stateObj = null;
            let paramsObj = null;
            if (typeof shadow_get_param === "function") {
                try {
                    const stateJson = shadow_get_param(0, `master_fx:${key}:state`);
                    if (stateJson) {
                        try {
                            stateObj = JSON.parse(stateJson);
                        } catch (parseErr) {
                            /* State is not JSON — store as opaque string */
                            stateObj = stateJson;
                        }
                        slotConfig.state = stateObj;
                    }
                } catch (e) {
                    /* state not supported - fall back to chain_params */
                }

                /* If no state, save individual params from chain_params */
                if (!stateObj) {
                    try {
                        paramsObj = {};
                        /* Query plugin_id first (needed by CLAP and other host plugins) */
                        try {
                            const pluginId = shadow_get_param(0, `master_fx:${key}:plugin_id`);
                            if (pluginId) {
                                paramsObj["plugin_id"] = pluginId;
                            }
                        } catch (e2) {}
                        const chainParams = getMasterFxChainParams(slotIdx);
                        if (chainParams && chainParams.length > 0) {
                            for (const p of chainParams) {
                                const val = shadow_get_param(0, `master_fx:${key}:${p.key}`);
                                if (val !== null && val !== undefined && val !== "") {
                                    paramsObj[p.key] = val;
                                }
                            }
                        }
                        if (Object.keys(paramsObj).length > 0) {
                            slotConfig.params = paramsObj;
                        }
                    } catch (e) {}
                }
            }

            config.master_fx_chain[key] = slotConfig;

            /* Guard against clobbering a good state file with empty data.
             * If every state/chain_params query came back empty (shim stalled,
             * teardown in progress, module not yet fully loaded), preserve the
             * existing file — same pattern as slot autosave (see ~line 3615). */
            let snapshotOk = false;
            if (stateObj) {
                snapshotOk = true;
            } else if (paramsObj) {
                const realKeys = Object.keys(paramsObj).filter(k => k !== "plugin_id");
                if (realKeys.length > 0) snapshotOk = true;
            }

            if (!snapshotOk) {
                debugLog(`MFX save: skipping ${key} write — no state/params from shim (preserving existing file)`);
                continue;
            }

            /* Write per-slot state file for shim-side restore at boot */
            const stateFile = {
                module_path: dspPath,
                module_id: moduleId
            };
            if (stateObj) {
                stateFile.state = stateObj;
            } else if (paramsObj) {
                stateFile.params = paramsObj;
            }
            if (slotIdx === 0 && masterFxLfoConfig) {
                stateFile.lfos = masterFxLfoConfig;
            }
            /* Persist bypass — restored by loadMasterFxChainFromConfig at boot
             * via setSlotParam, since the shim's MFX load path doesn't carry
             * this flag through. */
            const bypassedVal = (typeof shadow_get_param === "function")
                ? parseInt(shadow_get_param(0, `master_fx:${key}:bypassed`) || "0", 10)
                : 0;
            if (bypassedVal === 1) {
                stateFile.bypassed = 1;
            }
            host_write_file(stateFilePath, JSON.stringify(stateFile, null, 2) + "\n");
        }

        /* Save overlay knobs mode */
        if (typeof overlay_knobs_get_mode === "function") {
            config.overlay_knobs_mode = overlay_knobs_get_mode();
        }
        /* Save TTS debounce */
        if (typeof tts_get_debounce === "function") {
            config.tts_debounce_ms = tts_get_debounce();
        }
        /* Use JS-cached values instead of reading from shim to avoid
         * race condition where periodic autosave reads shim defaults
         * before loadMasterFxChainFromConfig() has restored them. */
        config.resample_bridge_mode = cachedResampleBridgeMode;
        config.link_audio_routing = cachedLinkAudioRouting;
        config.link_audio_publish = cachedLinkAudioPublish;
        config.latency_comp_enabled = cachedLatencyCompEnabled;
        config.usbc_out_persist = cachedUsbcOutPersist;
        config.master_fx_midi_channel = cachedMasterFxMidiChannel;

        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {
        /* Ignore errors */
    }
}

/* Save auto-update setting to shadow config */
function saveAutoUpdateConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        let config = {};
        try {
            const content = host_read_file(configPath);
            if (content) config = JSON.parse(content);
        } catch (e) {}
        config.auto_update_check = autoUpdateCheckEnabled;
        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {
        /* Ignore errors */
    }
}

/* Load auto-update setting from config */
function loadAutoUpdateConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        const content = host_read_file(configPath);
        if (!content) return;
        const config = JSON.parse(content);
        if (config.auto_update_check !== undefined) {
            autoUpdateCheckEnabled = config.auto_update_check;
        }
    } catch (e) {
        /* Ignore errors - default to enabled */
    }
}

function saveBrowserPreviewConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        let config = {};
        try {
            const content = host_read_file(configPath);
            if (content) config = JSON.parse(content);
        } catch (e) {}
        config.browser_preview = previewEnabled;
        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {}
}

function loadBrowserPreviewConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        const content = host_read_file(configPath);
        if (!content) return;
        const config = JSON.parse(content);
        if (config.browser_preview !== undefined) {
            previewEnabled = config.browser_preview;
        }
    } catch (e) {}
}

function saveFilebrowserConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        let config = {};
        try {
            const content = host_read_file(configPath);
            if (content) config = JSON.parse(content);
        } catch (e) {}
        config.filebrowser_enabled = filebrowserEnabled;
        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {}
}

function loadFilebrowserConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        const content = host_read_file(configPath);
        if (!content) return;
        const config = JSON.parse(content);
        if (config.filebrowser_enabled !== undefined) {
            filebrowserEnabled = config.filebrowser_enabled;
            /* Sync flag file with config */
            const flagPath = "/data/UserData/schwung/filebrowser_enabled";
            if (filebrowserEnabled) {
                host_write_file(flagPath, "1");
            } else {
                host_remove_dir(flagPath);
            }
        }
    } catch (e) {}
}

function savePadTypingConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        let config = {};
        try {
            const content = host_read_file(configPath);
            if (content) config = JSON.parse(content);
        } catch (e) {}
        config.pad_typing = padSelectGlobal;
        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {}
}

function loadPadTypingConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        const content = host_read_file(configPath);
        if (!content) return;
        const config = JSON.parse(content);
        if (config.pad_typing !== undefined) {
            setPadSelectGlobal(config.pad_typing);
        }
    } catch (e) {}
}

function saveTextPreviewConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        let config = {};
        try {
            const content = host_read_file(configPath);
            if (content) config = JSON.parse(content);
        } catch (e) {}
        config.text_preview = textPreviewGlobal;
        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {}
}

/* Periodic sync: re-read shadow_config.json and apply changes made by web UI.
 * Called from tick() every ~2 seconds. Only updates settings that differ from
 * cached state to avoid unnecessary writes to shared memory.
 *
 * Note: host_read_file() does fopen/fread on the UI thread, which is safe.
 * The SIGABRT was from display_mirror_set/set_pages_set which did fopen+fwrite
 * to features.json — we now use _shm variants that only write shared memory. */
let _configSyncTickCounter = 0;
const CONFIG_SYNC_INTERVAL = 88; /* ~2 seconds at 44 ticks/sec */

let _feedbackHoldTickCounter = 0;
const FEEDBACK_HOLD_CHECK_INTERVAL = 10; /* ~4x/sec — run the continuous feedback guard */
let _upgradeOverlayText = null; /* Web-initiated upgrade status for OLED display */

/* Sync JS-only variables from shadow_config.json. Called from tick() every ~2s.
 * IMPORTANT: No C function calls here — only pure JS variable updates.
 * Shared-memory settings are handled by the Go web server via mmap. */
function syncJsOnlySettings() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        const content = host_read_file(configPath);
        if (!content) return;
        const c = JSON.parse(content);

        if (c.pad_typing !== undefined && c.pad_typing !== padSelectGlobal) {
            setPadSelectGlobal(c.pad_typing);
        }
        if (c.text_preview !== undefined && c.text_preview !== textPreviewGlobal) {
            setTextPreviewGlobal(c.text_preview);
        }
        if (c.browser_preview !== undefined && c.browser_preview !== previewEnabled) {
            previewEnabled = c.browser_preview;
        }
        if (c.auto_update_check !== undefined) {
            autoUpdateCheckEnabled = c.auto_update_check;
        }
        if (c.filebrowser_enabled !== undefined && c.filebrowser_enabled !== filebrowserEnabled) {
            filebrowserEnabled = c.filebrowser_enabled;
        }
    } catch (e) {
        /* Ignore errors — file may be mid-write */
    }
}

function loadTextPreviewConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        const content = host_read_file(configPath);
        if (!content) return;
        const config = JSON.parse(content);
        if (config.text_preview !== undefined) {
            setTextPreviewGlobal(config.text_preview);
        }
    } catch (e) {}
}

/* Load master FX chain from config at startup.
 * The shim handles actual module loading + state restore from
 * slot_state/master_fx_N.json files at boot. This function just
 * syncs the JS-side masterFxConfig to reflect what the shim loaded. */
function loadMasterFxChainFromConfig() {
    try {
        const configPath = "/data/UserData/schwung/shadow_config.json";
        const content = host_read_file(configPath);
        const config = content ? JSON.parse(content) : {};

        /* Restore overlay knobs mode */
        if (typeof config.overlay_knobs_mode === "number" && typeof overlay_knobs_set_mode === "function") {
            overlay_knobs_set_mode(config.overlay_knobs_mode);
        }
        /* Restore TTS debounce */
        if (typeof config.tts_debounce_ms === "number" && typeof tts_set_debounce === "function") {
            tts_set_debounce(config.tts_debounce_ms);
        }
        if (config.resample_bridge_mode !== undefined && typeof shadow_set_param === "function") {
            const mode = parseResampleBridgeMode(config.resample_bridge_mode);
            shadow_set_param(0, "master_fx:resample_bridge", String(mode));
            cachedResampleBridgeMode = mode;
        }
        if (config.link_audio_routing !== undefined && typeof shadow_set_param === "function") {
            shadow_set_param(0, "master_fx:link_audio_routing", config.link_audio_routing ? "1" : "0");
            cachedLinkAudioRouting = !!config.link_audio_routing;
        }
        if (config.link_audio_publish !== undefined && typeof shadow_set_param === "function") {
            shadow_set_param(0, "master_fx:link_audio_publish", config.link_audio_publish ? "1" : "0");
            cachedLinkAudioPublish = !!config.link_audio_publish;
        }
        if (config.latency_comp_enabled !== undefined && typeof shadow_set_param === "function") {
            shadow_set_param(0, "master_fx:latency_comp_enabled", config.latency_comp_enabled ? "1" : "0");
            cachedLatencyCompEnabled = !!config.latency_comp_enabled;
        }
        if (config.usbc_out_persist !== undefined && typeof shadow_set_param === "function") {
            /* The shim also reads this key straight from shadow_config.json at
             * init, so the boot replay is already correctly gated before we get
             * here. This push only keeps the two in sync for the UI. */
            shadow_set_param(0, "master_fx:usbc_out_persist", config.usbc_out_persist ? "1" : "0");
            cachedUsbcOutPersist = !!config.usbc_out_persist;
        }
        if (config.master_fx_midi_channel !== undefined && typeof shadow_set_param === "function") {
            /* Like usbc_out_persist, the shim reads this key straight from
             * shadow_config.json at init, so the filter is already in force
             * before the first frame. This push only keeps the two in sync. */
            /* Round-tripped through the index so a garbage stored value lands
             * on All rather than being written straight back to the shim. */
            const val = mfxMidiChannelFromIndex(
                mfxMidiChannelToIndex(config.master_fx_midi_channel));
            shadow_set_param(0, "master_fx:midi_channel", String(val));
            cachedMasterFxMidiChannel = val;
        }

        /* Restore loaded preset name */
        if (config.master_fx_chain && config.master_fx_chain.preset_name) {
            currentMasterPresetName = config.master_fx_chain.preset_name;
        }

        /* Sync masterFxConfig from state files (shim already loaded the modules) */
        for (let i = 0; i < MASTER_FX_SLOTS; i++) {
            const key = `fx${i + 1}`;
            const stateFilePath = activeSlotStateDir + "/master_fx_" + i + ".json";
            try {
                const raw = host_read_file(stateFilePath);
                if (raw) {
                    const stateFile = JSON.parse(raw);
                    if (stateFile.module_id) {
                        masterFxConfig[key].module = stateFile.module_id;
                        /* Different module — it may implement display_name even if the
                         * last one didn't, so poll it at full rate again. */
                        delete fxDisplayNameCache[`master:${key}`];
                        delete fxDisplayNameSkip[`master:${key}`];
                        delete fxDisplayNameBackoff[`master:${key}`];
                        debugLog(`MFX sync ${key}: module=${stateFile.module_id} (loaded by shim)`);
                    }
                    /* Restore bypass via setSlotParam — the shim doesn't carry
                     * this flag through its load_file path, so we apply it
                     * after autosave has settled. */
                    if (stateFile.bypassed && typeof shadow_set_param === "function") {
                        shadow_set_param(0, `master_fx:${key}:bypassed`, "1");
                    }
                    if (i === 0 && stateFile.lfos && typeof shadow_set_param === "function") {
                        for (let li = 1; li <= 2; li++) {
                            const lfoConfig = stateFile.lfos["lfo" + li];
                            if (!lfoConfig) continue;
                            restoreMasterFxLfo(li, lfoConfig);
                            debugLog(`MFX LFO ${li}: restored target=${lfoConfig.target || "none"}`);
                        }
                    }
                }
            } catch (e) {}
        }
    } catch (e) {
        /* Ignore errors */
    }
}

/* Enter chain editing view for a slot */
function enterChainEdit(slotIndex) {
    selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    /* Loads the config, then anchors the selection to something that exists. */
    restoreChainComponent(slotIndex);
    setView(VIEWS.CHAIN_EDIT);
    needsRedraw = true;

    /* Announce menu title + initial selection. The remembered selection can be
     * -1 (the patch), which is not a component — it was always able to be, and
     * reading `.key` off it threw. */
    const comp = slotChainComponents(selectedSlot)[selectedChainComponent];
    if (!comp) {
        announce(`Slot ${slotIndex + 1}, Patch Selection`);
        return;
    }
    const moduleData = getChainComponentModule(chainConfigs[selectedSlot], comp.key);

    let info = "(empty)";
    if (moduleData && moduleData.module) {
        const prefix = getComponentParamPrefix(comp.key);
        const displayName = getSlotParam(selectedSlot, `${prefix}:name`) || moduleData.module;
        info = displayName;
    }

    announce(`Slot ${slotIndex + 1}, ${comp.label} ${info}`);
}

/* Scan modules directory for modules of a specific component type */
function scanModulesForType(componentType) {
    const MODULES_DIR = "/data/UserData/schwung/modules";
    const result = [{ id: "", name: "None" }];

    /* Map component type to directory and expected component_type */
    let searchDirs = [];
    let expectedTypes = [];

    if (componentType === "synth") {
        searchDirs = [`${MODULES_DIR}/sound_generators`];
        expectedTypes = ["sound_generator"];
    } else if (componentType === "midiFx" ||
               parseChainId(componentType)?.section === "midiFx") {
        /* "midiFx" is the first position's legacy key; "midi_fx2".. are the
         * rest, and they take the same directory. */
        searchDirs = [`${MODULES_DIR}/midi_fx`];
        expectedTypes = ["midi_fx"];
    } else if (parseChainId(componentType)?.section === "fx") {
        /* Any FX position — "fx1", "fx2", ... — takes the same directory. */
        searchDirs = [`${MODULES_DIR}/audio_fx`];
        expectedTypes = ["audio_fx"];
    }

    function scanDir(dirPath) {
        try {
            const entries = os.readdir(dirPath) || [];
            const dirList = entries[0];
            if (!Array.isArray(dirList)) return;

            for (const entry of dirList) {
                if (entry === "." || entry === "..") continue;

                const modulePath = `${dirPath}/${entry}/module.json`;
                try {
                    const content = std.loadFile(modulePath);
                    if (!content) continue;

                    const json = JSON.parse(content);
                    cacheModuleAbbrev(json);
                    const modType = json.component_type ||
                                   (json.capabilities && json.capabilities.component_type);

                    if (expectedTypes.includes(modType)) {
                        const scanPacks = json.scan_packs;
                        if (scanPacks) {
                            /* Expand packs: scan subdirectory for extracted
                             * pack directories with info.json */
                            const packsDir = `${dirPath}/${entry}/${scanPacks}`;
                            /* Auto-extract any .rnbopack tarballs */
                            try {
                                const rawEntries = os.readdir(packsDir) || [];
                                const rawList = rawEntries[0];
                                if (Array.isArray(rawList)) {
                                    for (const fn of rawList) {
                                        if (!fn.endsWith('.rnbopack')) continue;
                                        const stem = fn.slice(0, -9);
                                        const infoCheck = `${packsDir}/${stem}/info.json`;
                                        /* Skip if already extracted */
                                        if (std.loadFile(infoCheck)) continue;
                                        host_system_cmd(`mkdir -p '${packsDir}/${stem}' && tar -xf '${packsDir}/${fn}' -C '${packsDir}/${stem}' --strip-components=1 2>/dev/null`);
                                    }
                                }
                            } catch (e2) { /* packs dir may not exist yet */ }
                            try {
                                const packEntries = os.readdir(packsDir) || [];
                                const packList = packEntries[0];
                                if (Array.isArray(packList)) {
                                    for (const pe of packList) {
                                        if (pe === '.' || pe === '..') continue;
                                        const infoPath = `${packsDir}/${pe}/info.json`;
                                        try {
                                            const infoContent = std.loadFile(infoPath);
                                            if (!infoContent) continue;
                                            const info = JSON.parse(infoContent);
                                            const packId = (json.id || entry) + '-' + pe;
                                            const packName = info.name || pe;
                                            if (!result.find(m => m.id === packId)) {
                                                result.push({ id: packId, name: packName });
                                            }
                                        } catch (e2) { /* skip */ }
                                    }
                                }
                            } catch (e2) { /* packs dir not found */ }
                        } else {
                            /* Regular module — add directly */
                            const id = json.id || entry;
                            if (!result.find(m => m.id === id)) {
                                result.push({
                                    id: id,
                                    name: json.name || entry
                                });
                            }
                        }
                    }
                } catch (e) {
                    /* Skip modules without readable module.json */
                }
            }
        } catch (e) {
            /* Failed to read directory */
        }
    }

    for (const dir of searchDirs) {
        scanDir(dir);
    }

    /* Sort modules alphabetically by name, keeping "None" at the top */
    const noneItem = result[0];
    const modules = result.slice(1);
    modules.sort((a, b) => a.name.localeCompare(b.name));

    /* Add option to get more modules from store at the end */
    return [noneItem, ...modules, { id: "__get_more__", name: "[Get more...]" }];
}

/* Map component key to catalog category ID */
function componentKeyToCategoryId(componentKey) {
    switch (componentKey) {
        case 'synth': return 'sound_generator';
        case 'master_fx': return 'audio_fx';
        case 'midiFx': return 'midi_fx';
        case 'overtake': return 'overtake';
        default: {
            /* Any FX position — "fx1", "fx2", ... */
            const at = parseChainId(componentKey);
            return at && at.section === "fx" ? 'audio_fx' : null;
        }
    }
}

/* Enter the store picker for a specific component type — disabled.
 *
 * Was the gateway from "[Get more...]" entries in the overtake / master FX
 * / chain component module pickers into the on-device store browser. The
 * browser flow ended in installs/updates that silently failed for users
 * without root, so we redirect every "Get more" tap straight at the web
 * manager pointer screen — same destination as Settings → Module Store.
 *
 * Preserves the entry-context flags (storePickerFromOvertake /
 * storePickerFromMasterFx / storePickerCategory) so the result-screen
 * jog-click can return to wherever the user came from instead of dumping
 * them on Global Settings. */
function enterStorePicker(componentKey) {
    const categoryId = componentKeyToCategoryId(componentKey);
    if (!categoryId) return;

    storePickerCategory = categoryId;
    storePickerCurrentModule = null;
    storePickerFromOvertake = (componentKey === 'overtake');
    storePickerFromMasterFx = (componentKey === 'master_fx');
    storePickerFromSettings = false;

    storePickerResultTitle = 'Module Store';
    storePickerMessage = 'Module store available at\nhttp://move.local:7700';
    setView(VIEWS.STORE_PICKER_RESULT);
    needsRedraw = true;
    announce(storePickerMessage);
}

/* Handle selection in store picker result */
function handleStorePickerResultSelect() {
    /* Honor the entry-context flags so dismissing the pointer screen
     * sends the user back to wherever they came from. These mirror
     * handleStorePickerBack — the only
     * reason the back-button and click-dismiss diverged historically
     * is that the result screen used to terminate install flows that
     * could only sensibly return to the module list. With the install
     * paths gone, every result screen is informational, and dismiss
     * should round-trip to the entry context. */
    storePickerCurrentModule = null;

    if (storeReturnView === VIEWS.GLOBAL_SETTINGS) {
        storeReturnView = null;
        enterGlobalSettings();
        return;
    }
    if (storePickerFromOvertake) {
        overtakeModules = scanForOvertakeModules();
        setView(VIEWS.OVERTAKE_MENU);
        storePickerFromOvertake = false;
        storeCatalog = null;
        storePickerCategory = null;
        storePickerModules = [];
        return;
    }
    if (storePickerFromMasterFx) {
        MASTER_FX_OPTIONS = scanForAudioFxModules();
        enterMasterFxModuleSelect(selectedMasterFxComponent);
        setView(VIEWS.MASTER_FX);
        storePickerFromMasterFx = false;
        storeCatalog = null;
        storePickerCategory = null;
        storePickerModules = [];
        return;
    }
    if (storePickerCategory) {
        /* Came from the chain component picker. */
        availableModules = scanModulesForType(
            slotChainComponents(selectedSlot)[selectedChainComponent].key);
        setView(VIEWS.COMPONENT_SELECT);
        storeCatalog = null;
        storePickerCategory = null;
        storePickerModules = [];
        return;
    }
    /* Last-resort fallback (legacy callers): back to the slots view. */
    setView(VIEWS.SLOTS);
    needsRedraw = true;
}

/* Handle back in store picker result — the only store view left. Back
 * and click-dismiss route identically to the entry context. */
function handleStorePickerBack() {
    handleStorePickerResultSelect();
    needsRedraw = true;
}

/* Enter component module selection view */
function enterComponentSelect(slotIndex, componentIndex) {
    const comp = slotChainComponents(slotIndex)[componentIndex];
    /* Only a real module position has modules to pick from. Settings never
     * did; the `+` boxes are resolved to a position by their caller before
     * they get here, so one arriving unresolved is a bug, not a gesture. */
    if (!comp || !isChainModuleKey(comp.key)) return;

    selectedSlot = slotIndex;
    selectedChainComponent = componentIndex;

    /* Scan for available modules of this type */
    availableModules = scanModulesForType(comp.key);

    /* Surface this component's User Presets manager (see shadow_ui_presets.mjs)
     * as an indented row tucked directly beneath the loaded module — for any
     * loaded chain component (synth, audio FX, MIDI FX). It rides alongside the
     * module it belongs to (rather than at the top of the swap list) so the
     * picker reads "<loaded module> / its presets" in place. Only shown when a
     * module is loaded, since a preset snapshots its <component>:state. */
    let presetsRowIndex = -1;
    {
        const loaded = getChainComponentModule(chainConfigs[slotIndex], comp.key);
        const loadedId = loaded && loaded.module;
        if (loadedId) {
            const presetsRow = {
                id: "__user_presets__",
                /* No module abbrev needed — the indented row sits directly
                 * under the module it belongs to, so context is implicit. */
                name: "  [User Presets]"
            };
            /* Slot it right under the loaded module's entry; if that module
             * isn't in the scan list (e.g. uninstalled), fall back to the top. */
            const loadedIdx = availableModules.findIndex(m => m.id === loadedId);
            presetsRowIndex = loadedIdx >= 0 ? loadedIdx + 1 : 0;
            availableModules.splice(presetsRowIndex, 0, presetsRow);
        }
    }

    /*
     * Move Left / Move Right, tucked under the loaded module beside its
     * presets.
     *
     * They exist so Shift+jog is not the ONLY way to reorder a chain: a
     * modifier gesture with no discoverable equivalent is a feature only the
     * person who wrote it knows about. Offered only where they would do
     * something — an occupied list position with somewhere to go — so the list
     * never carries a row that answers a click by doing nothing. That is also
     * why the synth has neither: it is not a list position, and the sections
     * either side of it are not the same kind of thing.
     */
    availableModules.splice(presetsRowIndex >= 0 ? presetsRowIndex + 1 : 0, 0,
        ...chainMoveEntries(chainConfigs[slotIndex], comp.key));

    selectedModuleIndex = 0;

    if (presetsRowIndex >= 0) {
        /* A module is loaded — default the cursor to its presets row (entering
         * here is usually to reach presets, not to swap modules). */
        selectedModuleIndex = presetsRowIndex;
    } else {
        /* Nothing loaded — default the cursor to the current module if any. */
        const current = getChainComponentModule(chainConfigs[slotIndex], comp.key);
        if (current && current.module) {
            const idx = availableModules.findIndex(m => m.id === current.module);
            if (idx >= 0) selectedModuleIndex = idx;
        }
    }

    setView(VIEWS.COMPONENT_SELECT);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = availableModules[selectedModuleIndex]?.name || "None";
    announce(`Select ${comp.label}, ${moduleName}`);
}

/* Apply the selected module to the component - updates DSP in realtime */
function applyComponentSelection() {
    const comp = slotChainComponents(selectedSlot)[selectedChainComponent];
    const selected = availableModules[selectedModuleIndex];

    if (!comp || !isChainModuleKey(comp.key)) {
        cancelPendingChainInsert();
        setView(VIEWS.CHAIN_EDIT);
        return;
    }

    /* Was this picker opened from a `+` box? Read BEFORE the choice is applied,
     * because applying it fills the very hole this recognises. */
    const pending = pendingChainInsertFor(slotChainTarget(selectedSlot), comp.key);

    /* Check if user selected this component's User Presets manager */
    if (selected && selected.id === "__user_presets__") {
        const loaded = getChainComponentModule(chainConfigs[selectedSlot], comp.key);
        enterPresetBrowser(selectedSlot, comp.key, loaded && loaded.module,
                           getComponentParamPrefix(comp.key));
        return;
    }

    /* Check if user selected "[Get more...]" - enter store picker */
    if (selected && selected.id === "__get_more__") {
        enterStorePicker(comp.key);
        return;
    }

    /* Move Left / Move Right — the same reorder the Shift+jog gesture performs,
     * offered here so the gesture is not the only way in. Both go through
     * moveChainComponent, so the bounds are the model's in both. */
    if (selected && (selected.id === "__move_left__" || selected.id === "__move_right__")) {
        const delta = selected.id === "__move_left__" ? -1 : 1;
        if (moveChainComponent(slotChainTarget(selectedSlot), comp.key, delta)) {
            selectedChainComponent = slotChainComponentIndex(selectedSlot,
                chainEditorKeyAt(comp.section, comp.index + delta));
            lastChainComponent[selectedSlot] = selectedChainComponent;
            const moved = slotChainComponents(selectedSlot)[selectedChainComponent];
            announce(`${moved ? moved.label : comp.label} moved ${delta < 0 ? "left" : "right"}`);
        }
        setView(VIEWS.CHAIN_EDIT);
        needsRedraw = true;
        return;
    }

    /* Update in-memory config. `None` on a list position REMOVES and compacts,
     * which renumbers everything downstream and so cannot be expressed as the
     * single `<id>:module` write below — applyPickerChoiceToChain says which
     * section, if any, has to be rewritten whole. */
    const picked = selected && selected.id ? selected.id : "";

    /*
     * `None` from a `+` box is a CANCEL, not a removal.
     *
     * The position it would remove was never written, so there is nothing to
     * renumber — and running it through the removal path would be actively
     * wrong: that path sends `<section>:remove` for a position the DSP does not
     * have, which would unload and compact away a module the user never
     * touched. The cheapest correct answer is the same one backing out gives:
     * write nothing.
     */
    if (pending && !picked) {
        cancelPendingChainInsert();
        setView(VIEWS.CHAIN_EDIT);
        needsRedraw = true;
        return;
    }

    applyChainComponentPick(selectedSlot, comp.key, picked, pending);
}

/*
 * Apply a chain-component pick: the whole sequence, from one place.
 *
 * `picked` is the module id, or "" for None. None on a LIST position is a
 * removal that closes the gap and renumbers everything downstream — a SHAPE
 * change carried by one `remove` verb — while None on the synth is a clear
 * with no neighbours to renumber. applyPickerChoiceToChain knows which; this
 * function is everything that must happen either way, and it is extracted so
 * the Module page's "Remove Module" IS this path rather than a copy of it.
 */
function applyChainComponentPick(slotIndex, componentKey, picked, pending) {
    const at = slotChainComponentIndex(slotIndex, componentKey);
    const comp = at >= 0 ? slotChainComponents(slotIndex)[at] : null;
    if (!comp) return;

    const cfg = chainConfigs[slotIndex] || createEmptyChainConfig();
    const choice = withPendingChainInsert(
        applyPickerChoiceToChain(cfg, componentKey, picked), pending);
    chainConfigs[slotIndex] = choice.cfg;
    /* A swap MUTATES `cfg` in place and `None` hands back a different object,
     * so neither identity nor the module signature can be what notices this.
     * The confirm path reloads (and the declined-gate path reloads too), but
     * the model and the DSP disagree from here until one of them runs. */
    invalidateChainConfig(slotIndex);

    /* Track explicit user-removal so autosave can bypass the boot-glitch
     * guard. Set when the slot is now fully empty; reset on any non-empty
     * pick (the user is rebuilding the slot). */
    slotUserCleared[slotIndex] = !chainHasAnyModule(choice.cfg);

    /* A component changed, so any LFO label naming that component by module
     * is now wrong. The label cache keys on the stored ROUTING, which a swap
     * does not touch — "Freeverb: Room Size" would survive the Freeverb
     * leaving. Cheap to drop: it re-resolves on the next draw that needs it. */
    resetLfoTargetLabels();

    /* Apply to DSP - map component key to param key */
    const moduleId = picked;
    const paramKey = chainComponentParamKey(componentKey, "module") || "";

    /* Feedback gate: if the picked module pulls line-in, warn about speakers.
     * Callback-based — schwung's QuickJS doesn't pump pending jobs so
     * Promise.then never fires. */
    if (paramKey && moduleId) {
        let meta = null;
        try {
            if (typeof host_get_module_metadata === 'function') {
                meta = host_get_module_metadata(moduleId);
            }
        } catch (err) {
            if (typeof host_log === 'function') {
                host_log(`applyComponentSelection: feedback gate metadata error for ${moduleId}: ${err}`);
            }
        }
        maybeConfirmForModule(meta, (ok) => {
            if (!ok) {
                if (typeof host_log === 'function') {
                    host_log(`applyComponentSelection: declined feedback gate for ${moduleId}`);
                }
                loadChainConfigFromSlot(slotIndex);
                slotUserCleared[slotIndex] = false;
                setView(VIEWS.CHAIN_EDIT);
                needsRedraw = true;
                return;
            }
            applyComponentSelectionConfirmed(slotIndex, paramKey, moduleId, comp, choice);
        });
        return;
    }

    /* Clearing a slot (empty moduleId) — no feedback risk, run directly. */
    applyComponentSelectionConfirmed(slotIndex, paramKey, moduleId, comp, choice);
}

function applyComponentSelectionConfirmed(slotIndex, paramKey, moduleId, comp, choice) {
    /*
     * A SHAPE change first, if the choice asked for one.
     *
     * A removal compacts the list and an insert opens a hole in it, and either
     * way every position past the edit is renumbered — which one `<id>:module`
     * write cannot say, and which used to be expressed as a rewrite of the
     * whole section that reloaded each of those modules in passing. It is now
     * one verb that permutes the DSP's arrays and reloads nothing
     * (writeChainShape).
     *
     * A removal is COMPLETE here: the verb unloads the position itself, so
     * there is no module write to follow. An insert only opens the hole, and
     * the module write below fills it — hence `insert` falling through rather
     * than returning.
     */
    const shape = choice && choice.shape;
    if (shape) writeChainShape(slotChainTarget(slotIndex), shape);
    if (shape && shape.kind === "remove") {
        /* nothing more to write: the verb did the unload */
    } else if (paramKey) {
        if (typeof host_log === "function") host_log(`applyComponentSelection: slot=${slotIndex} param=${paramKey} module=${moduleId}`);
        /*
         * BEFORE the module write, because the write reloads the position and
         * takes its modulation entries with it — after it, there is nothing
         * left to say the routing was ever valid, and the LFO keeps its aim.
         * Only when the module actually CHANGES: re-picking the same module is
         * a reload, not a replacement, and the routing still names the module
         * the user routed. See clearLfoRoutingForComponent for what a stale
         * routing does to the module that lands here next.
         */
        if (pickerReplacedModule(choice ? choice.replaced : null, moduleId)) {
            clearLfoRoutingForComponent(slotChainTarget(slotIndex),
                                        getComponentParamPrefix(comp.key));
        }
        const success = setSlotParam(slotIndex, paramKey, moduleId);
        if (typeof host_log === "function") host_log(`applyComponentSelection: setSlotParam returned ${success}`);
        if (!success) {
            print(2, 50, "Failed to apply", 1);
        }
    }

    /* Track component selection for analytics. Outside the branches, because a
     * `+` that INSERTS loads a module through the section rewrite and would
     * otherwise be the one way of adding one that reports nothing. `moduleId`
     * is empty for a removal, which is what still keeps this to loads. */
    if (moduleId && typeof host_track_event === "function") {
        host_track_event('module_loaded', '"module_id":"' + moduleId + '","source":"picker","component":"' + comp.key + '"');
    }

    /* Force sync chainConfigs from DSP and reset caches after module change.
     * Without this, the knob overlay can show the old module's name and params
     * because the periodic refreshSlotModuleSignature (every 30 ticks) hasn't
     * run yet to sync the in-memory state with DSP. */
    loadChainConfigFromSlot(slotIndex);
    lastSlotModuleSignatures[slotIndex] = getSlotModuleSignature(slotIndex);
    invalidateKnobContextCache();
    /* A removal shortened the list, so the remembered index can now point past
     * its end — and the CHAIN_EDIT handlers read `comps[selection].key` without
     * checking. One removal can only ever overshoot by one, but the clamp is
     * here rather than in each of them. */
    if (slotIndex === selectedSlot && shape) {
        const len = slotChainComponents(slotIndex).length;
        if (selectedChainComponent >= len) selectedChainComponent = len - 1;
        lastChainComponent[slotIndex] = selectedChainComponent;
    }
    setView(VIEWS.CHAIN_EDIT);
    needsRedraw = true;
}

/*
 * Perform a slot-settings ACTION, by key.
 *
 * Lifted verbatim out of the CHAIN_SETTINGS jog-click handler, which is the
 * only place it lived. It is a function so a second surface — the knob grid,
 * whose menu page returns an { action } and performs nothing itself — can run
 * the SAME Save rather than growing its own.
 *
 * `slot` is what the handler called `selectedSlot`, passed explicitly rather
 * than read from the module global, so the caller cannot get the two out of
 * step. Everything else it touches (pendingSaveName, confirmingOverwrite,
 * lfoCtx, ...) is module state it always mutated and still does.
 */
function runChainSettingAction(slot, key) {
    if (key === "knobs") {
        enterKnobEditor(slot);
        return;
    }

    if (key === "save") {
        /* Start save flow */
        const currentName = slots[slot] ? slots[slot].name : "";
        if (!currentName || currentName === "" || currentName === "Untitled") {
            /* New - show name preview with Edit/OK */
            pendingSaveName = generateSlotPresetName(slot);
            showingNamePreview = true;
            namePreviewIndex = 1;  /* Default to OK */
            overwriteFromKeyboard = true;  /* Will use keyboard if Edit is selected */
            announceSavePreview(pendingSaveName, namePreviewIndex);
            needsRedraw = true;
        } else {
            /* Existing - confirm overwrite (no keyboard needed) */
            pendingSaveName = currentName;
            overwriteTargetIndex = findPatchByName(currentName);
            confirmingOverwrite = true;
            overwriteFromKeyboard = false;  /* Direct save, no keyboard */
            confirmIndex = 0;
            needsRedraw = true;
        }
        return;
    }

    if (key === "save_as") {
        /* Save As - show name preview with Edit/OK */
        const currentName = slots[slot] ? slots[slot].name : "";
        pendingSaveName = currentName && currentName !== "" && currentName !== "Untitled"
            ? currentName
            : generateSlotPresetName(slot);
        showingNamePreview = true;
        namePreviewIndex = 1;  /* Default to OK */
        overwriteFromKeyboard = true;  /* Will use keyboard if Edit is selected */
        announceSavePreview(pendingSaveName, namePreviewIndex);
        needsRedraw = true;
        return;
    }

    if (key === "lfo1" || key === "lfo2") {
        const lfoIdx = (key === "lfo1") ? 0 : 1;
        lfoCtx = makeSlotLfoCtx(slot, lfoIdx);
        selectedLfoItem = 0;
        editingLfoValue = false;
        setView(VIEWS.LFO_EDIT);
        const enabled = lfoCtx.getParam("enabled");
        if (enabled === "1") {
            const targetDesc = describeCurrentLfoTarget();
            if (targetDesc && !targetDesc.empty) {
                announce(lfoCtx.title + ", " + targetDesc.long);
            } else {
                announce(lfoCtx.title + ", no target");
            }
        } else {
            announce(lfoCtx.title + ", Off");
        }
        return;
    }

    if (key === "delete") {
        if (isExistingPreset(slot)) {
            confirmingDelete = true;
            confirmIndex = 0;
            const patchName = slots[slot]?.name || "patch";
            announce(`Delete ${patchName}?`);
            needsRedraw = true;
        }
        return;
    }
}

/*
 * The param accessors the slot grid drives this slot through.
 *
 * All the mapping lives in shadow_ui_slot_grid.mjs, which is pure and tested on
 * its own; this only supplies the four host functions it needs. Built fresh per
 * entry so it always closes over the slot actually being edited.
 */
/*
 * A slot action chosen from the KNOB GRID, with a hand-off for the ones that
 * open a modal.
 *
 * Save / Save As / Delete do not act immediately: they set showingNamePreview
 * or confirmingOverwrite / confirmingDelete and wait for a confirmation. Both
 * the DRAWING of those (drawChainSettings, in shadow_ui_settings.mjs) and the
 * jog/click that answer them live under `case VIEWS.CHAIN_SETTINGS` -- the LIST
 * view. Slot settings now open as the grid, so from there the flag flipped and
 * nothing rendered it and nothing could answer it.
 *
 * On device that read as: pressing Save did nothing whatsoever, and jogging
 * afterwards announced no "Edit"/"OK" -- because that handler was never
 * reached. The action HAD run. It ran into a view with no way to show or
 * finish it.
 *
 * So hand off to the list, exactly as an opaque param does (see
 * enterHierarchyEditorFromParamPages). Teaching the grid to draw and drive
 * three confirm flows would be a second implementation of screens that already
 * work.
 *
 * The hand-off asks WHETHER A MODAL IS NOW OPEN rather than listing which keys
 * are modal ones. A fifth action that opens a confirm would otherwise be
 * silently broken in precisely the same way, and that is the failure mode worth
 * designing out rather than re-fixing.
 */
function runSlotActionFromGrid(slot, key) {
    runChainSettingAction(slot, key);
    if (!(showingNamePreview || confirmingOverwrite || confirmingDelete)) return false;
    exitParamPages();
    /* The list must STAY the list while the modal is up -- re-entering the grid
     * would drop the confirmation on the floor again. */
    suppressSlotGridOnce = true;
    slotModalFromGrid = true;
    setView(VIEWS.CHAIN_SETTINGS);
    needsRedraw = true;
    return true;
}

/*
 * ...and back to the grid once the modal is done with.
 *
 * You opened the knob grid, so that is where you should still be afterwards;
 * being dropped into the list is a view you did not ask for and did not
 * navigate to.
 *
 * This RECONCILES rather than firing at the end of each flow, because the modal
 * has many ways to finish -- confirm, decline, Back, and for Save a decline
 * that returns to the name preview instead of exiting. Hooking each one means
 * being wrong about exactly one of them, which is how the original bug got
 * here. Instead: while a hand-off is outstanding and no modal is open any more,
 * go back. Cheap (flags only, no IPC) and correct for exits nobody enumerated.
 */
function maybeReturnToSlotGrid() {
    if (!slotModalFromGrid) return false;
    if (showingNamePreview || confirmingOverwrite || confirmingDelete) return false;
    /* The name preview's "Edit" CLEARS showingNamePreview and opens the on-screen
     * keyboard, so for the length of that keyboard none of the three flags is
     * set and the reconcile would fire — pulling the grid up over a text entry
     * the user is halfway through. The keyboard is part of the flow, not the
     * end of it. */
    if (isTextEntryActive()) return false;
    slotModalFromGrid = false;
    /* Consume the suppression that kept the list up for the modal, or
     * enterChainSettings would spend it and hand back the list again. */
    suppressSlotGridOnce = false;
    enterChainSettings(selectedSlot);
    return true;
}

function slotGridIoFor(slotIndex) {
    const io = createSlotGridIo({
        readSlotParam: (key) => getSlotParam(slotIndex, key),
        writeSlotParam: (key, value) => setSlotParam(slotIndex, key, value),
        isMpeMode: () => isSlotMpeMode(slotIndex),
        /* The compound handler: recv + fwd + the synth flag, with the pre-MPE
         * channels stashed for the way back. adjustChainSetting takes a DELTA,
         * and only acts when the state actually differs. */
        setMpeMode: (on) => adjustChainSetting(slotIndex, { key: "mpe_mode" }, on ? 1 : -1),
        hasPreset: () => isExistingPreset(slotIndex),
        /* An LFO's target reads as a name, not as "fx1" — see
         * shared/lfo_target_label.mjs. Resolved through the same ctx the LFO
         * editor uses, so the grid and the list can never describe the same
         * routing differently, and cached per scope because a miss is a dozen
         * IPC round trips inside a draw. */
        describeTarget: (lfoIndex) => describeLfoTargetFor(makeSlotLfoCtx(slotIndex, lfoIndex)),
        /* Only the LFO params reach this — see createSlotGridIo.isModulated. */
        isModulated: (realKey) => isHierarchyParamModulated(slotIndex, realKey),
    });
    /*
     * Visibility, bound to THIS slot. The default evaluator reads
     * hierEditorSlot/hierEditorComponent, which belong to the list editor and
     * are stale while the grid is up — it would resolve every condition against
     * the wrong slot, and a condition that reads empty compares false, so BOTH
     * rate cells would vanish rather than one. The condition keys carry their
     * own "lfoN:" prefix, so the component prefix is unused (see
     * normalizeVisibilityConditionKey: a key containing ":" passes through).
     */
    io.visible = (condition, levelDef) =>
        evaluateVisibilityConditionForContext(slotIndex, "slot", condition, levelDef, -1);
    return io;
}

/*
 * MASTER FX SETTINGS, as the knob grid — the same four pages a slot gets.
 *
 * The component name is not a module and not "slot"; it names the SYNTHESISED
 * contract so headerTitle() and openParamEditorFromGrid can tell the two
 * settings screens apart without asking which chain they are on.
 */
const MASTER_SETTINGS_COMPONENT = "master_settings";

function masterGridIoFor() {
    const io = createMasterGridIo({
        /* IPC slot 0 by CONVENTION — Master FX is not instrument slot 0. Every
         * declared key already carries its "master_fx:" prefix, so these are
         * pass-throughs rather than a mapping. */
        readParam: (key) => getSlotParam(0, key),
        writeParam: (key, value) => setSlotParam(0, key, value),
        hasPreset: () => !!currentMasterPresetName,
        /* The SAME resolver the MFX LFO list editor uses, so the grid and the
         * list can never describe one routing differently, and cached per scope
         * because a miss is a dozen IPC round trips inside a draw. */
        describeTarget: (lfoIndex) => describeLfoTargetFor(makeMfxLfoCtx(lfoIndex)),
        isModulated: (realKey) => isHierarchyParamModulated(0, realKey),
        runAction: (action) => runMasterFxActionFromGrid(action),
    });
    /*
     * Visibility, bound to the master bus. Same trap as the slot grid: the
     * default evaluator reads the LIST editor's slot/component, which are stale
     * while the grid is up. The condition keys carry the full "master_fx:lfoN:"
     * prefix, so the component prefix passed here is unused (see
     * normalizeVisibilityConditionKey) — but the SLOT is not, and it must be 0.
     */
    io.visible = (condition, levelDef) =>
        evaluateVisibilityConditionForContext(0, "master_fx", condition, levelDef, -1);
    return io;
}

function enterMasterFxSettingsGrid() {
    enterParamPages(0, MASTER_SETTINGS_COMPONENT, MASTER_SETTINGS_COMPONENT, null,
                    masterGridIoFor(),
                    /* No moduleKey: there is no module behind this contract to
                     * abbreviate. `name` is what the header shows instead. Back
                     * goes where the settings LIST's Back went — the Master FX
                     * chain editor. */
                    { label: MASTER_CHAIN_TARGET.label, name: "Settings",
                      returnView: VIEWS.MASTER_FX });
}

/*
 * GLOBAL SETTINGS, as the knob grid — the host half of the contract.
 *
 * The routing table lives in shadow_ui_global_grid.mjs and is tested with no
 * device; what cannot leave this file is here and nothing else: the concrete
 * backends, and the module-level cache vars a write must keep in step.
 *
 * WHY THE CACHE VARS AND THE SAVE MATTER MORE THAN THEY LOOK.
 *
 * This replaces adjustMasterFxSetting, which is delta-based and side-effectful:
 * six of its branches call saveMasterFxChainConfig() and four assign a
 * `cached*` var alongside the shadow_set_param. A converted write that drops
 * either sets the param, reads back correctly, draws correctly — and is gone on
 * reboot, with no error anywhere. That is the failure this shape exists to
 * prevent, so the SHARED save is declared once (`persist`, applied by
 * writeGlobalParam for exactly the keys the table marks) while the key-specific
 * savers stay welded to the assignment they save.
 */
function globalGridIoFor() {
    const mfx = (key) => shadow_get_param(0, "master_fx:" + key);
    const mfxSet = (key, value) => shadow_set_param(0, "master_fx:" + key, value);
    const bit = (on) => (on ? "1" : "0");

    const io = {
        readParam(key) {
            switch (key) {
            /* ---- display */
            case "display_mirror":
                return bit(typeof display_mirror_get === "function" && display_mirror_get());
            case "overlay_knobs":
                return String(typeof overlay_knobs_get_mode === "function" ? overlay_knobs_get_mode() : 0);
            case "pad_typing":         return bit(padSelectGlobal);
            case "text_preview":       return bit(textPreviewGlobal);
            case "midi_indicator_enabled": return bit(midiIndicatorEnabled);
            case "param_view":         return String(paramViewGlobal === 1 ? 1 : 0);

            /* ---- audio. These four are the ONLY reads that cost IPC. */
            case "link_audio_routing":
            case "link_audio_publish":
            case "latency_comp_enabled":
            case "usbc_out_persist":
            case "usbc_out_source":
                return mfx(key);
            case "resample_bridge": {
                /* Normalised through the same parser the old path used, so an
                 * unset or malformed value lands on a mode that exists. A read
                 * that did not complete is passed through untouched — null is
                 * not news about the setting. */
                const raw = mfx("resample_bridge");
                if (raw === null || raw === undefined || raw === "") return raw;
                return String(parseResampleBridgeMode(raw));
            }
            case "skipback_shortcut":
                return bit(typeof skipback_shortcut_get === "function" && skipback_shortcut_get());
            case "skipback_seconds":
                return String(typeof skipback_seconds_get === "function" ? skipback_seconds_get() : 30);
            case "browser_preview":    return bit(previewEnabled);

            /* ---- screen reader */
            case "screen_reader_enabled":
                return bit(typeof tts_get_enabled === "function" && tts_get_enabled());
            case "screen_reader_engine":
                return (typeof tts_get_engine === "function" && tts_get_engine() === "flite") ? "flite" : "espeak";
            case "screen_reader_speed":
                return String(typeof tts_get_speed === "function" ? tts_get_speed() : 1.0);
            case "screen_reader_pitch":
                return String(typeof tts_get_pitch === "function" ? Math.round(tts_get_pitch()) : 110);
            case "screen_reader_volume":
                return String(typeof tts_get_volume === "function" ? tts_get_volume() : 70);
            case "screen_reader_debounce":
                return String(typeof tts_get_debounce === "function" ? tts_get_debounce() : 300);

            /* ---- set pages / shortcuts / services */
            case "set_pages_enabled":
                return bit(typeof set_pages_get === "function" && set_pages_get());
            case "shadow_ui_trigger":
                return String(typeof shadow_ui_trigger_get === "function" ? shadow_ui_trigger_get() : 2);
            case "filebrowser_enabled": return bit(filebrowserEnabled);
            case "auto_update_check":   return bit(autoUpdateCheckEnabled);
            case "analytics_enabled":
                return bit(typeof host_get_analytics_enabled === "function" && host_get_analytics_enabled());
            }
            return "";
        },

        /*
         * ABSOLUTE, unlike the delta-based path this replaces. `value` is the
         * STORED value as a string — writeGlobalParam has already turned an
         * enum index into it, which is what keeps resample_bridge writing 2
         * rather than the index 1, a mode that does not exist.
         */
        writeParam(key, value) {
            const on = (value === "1");
            switch (key) {
            /* ---- display */
            case "display_mirror":
                if (typeof display_mirror_set === "function") display_mirror_set(on ? 1 : 0);
                return;
            case "overlay_knobs":
                if (typeof overlay_knobs_set_mode === "function") overlay_knobs_set_mode(parseInt(value, 10) || 0);
                return;
            case "pad_typing":
                setPadSelectGlobal(on);
                savePadTypingConfig();
                return;
            case "text_preview":
                setTextPreviewGlobal(on);
                saveTextPreviewConfig();
                return;
            case "midi_indicator_enabled":
                midiIndicatorEnabled = on;
                if (typeof midi_indicator_set === "function") midi_indicator_set(on ? 1 : 0);
                return;
            case "param_view":
                paramViewGlobal = on ? 1 : 0;
                saveParamViewConfig();
                announce(paramViewGlobal === 1 ? "Param View Knobs" : "Param View List");
                return;

            /* ---- audio. The cache var is not a cache of the write; it is what
             * saveMasterFxChainConfig serialises, so skipping it writes the OLD
             * value to disk and the setting reverts on reboot. */
            case "link_audio_routing":
                mfxSet(key, on ? "1" : "0");
                cachedLinkAudioRouting = on;
                /* Turning it ON with Link off in Move's System Settings does
                 * nothing at all, silently — that is the whole reason for the
                 * warning, so it rides with the write rather than with the
                 * screen that used to perform it. */
                if (on) warnIfLinkDisabled("Move->Schwung");
                return;
            case "link_audio_publish":
                mfxSet(key, on ? "1" : "0");
                cachedLinkAudioPublish = on;
                if (on) warnIfLinkDisabled("Schwung->Link");
                return;
            case "latency_comp_enabled":
                mfxSet(key, on ? "1" : "0");
                cachedLatencyCompEnabled = on;
                return;
            case "usbc_out_persist":
                /* Both On indexes store 1 — the third option carries only the
                 * wire annotation, and the source is Move's to choose. */
                mfxSet(key, on ? "1" : "0");
                cachedUsbcOutPersist = on;
                return;
            case "resample_bridge": {
                const mode = parseResampleBridgeMode(value);
                mfxSet(key, String(mode));
                cachedResampleBridgeMode = mode;
                /* Schwung Mix takes over Mic and Line-in; a user who does not
                 * know that hears their input vanish. */
                if (mode === 2) {
                    showWarning("Schwung Mix",
                                "Replaces Mic and Line-in with ME + Move Audio");
                }
                return;
            }
            case "skipback_shortcut":
                if (typeof skipback_shortcut_set === "function") skipback_shortcut_set(on ? 1 : 0);
                return;
            case "skipback_seconds":
                if (typeof skipback_seconds_set === "function") skipback_seconds_set(parseInt(value, 10) || 30);
                return;
            case "browser_preview":
                previewEnabled = on;
                if (!previewEnabled) previewStopIfPlaying();
                saveBrowserPreviewConfig();
                return;

            /* ---- screen reader. These persist themselves. */
            case "screen_reader_enabled":
                if (typeof tts_set_enabled === "function") tts_set_enabled(on);
                return;
            case "screen_reader_engine":
                if (typeof tts_set_engine === "function") tts_set_engine(value === "flite" ? "flite" : "espeak");
                return;
            case "screen_reader_speed":
                if (typeof tts_set_speed === "function") tts_set_speed(parseFloat(value));
                return;
            case "screen_reader_pitch":
                if (typeof tts_set_pitch === "function") tts_set_pitch(Math.round(parseFloat(value)));
                return;
            case "screen_reader_volume":
                if (typeof tts_set_volume === "function") tts_set_volume(Math.round(parseFloat(value)));
                return;
            case "screen_reader_debounce":
                if (typeof tts_set_debounce === "function") tts_set_debounce(Math.round(parseFloat(value)));
                return;

            /* ---- set pages / shortcuts / services */
            case "set_pages_enabled":
                if (typeof set_pages_set === "function") set_pages_set(on ? 1 : 0);
                return;
            case "shadow_ui_trigger":
                if (typeof shadow_ui_trigger_set === "function") shadow_ui_trigger_set(parseInt(value, 10) || 0);
                return;
            case "filebrowser_enabled":
                filebrowserEnabled = on;
                setFilebrowserRunning(on);
                saveFilebrowserConfig();
                /* The URL is the entire point of the setting and there is
                 * nowhere else on the device it is written down. */
                showWarning("File Browser",
                            on ? "On. Access at http://move.local:404" : "Off.");
                return;
            case "auto_update_check":
                autoUpdateCheckEnabled = on;
                saveAutoUpdateConfig();
                return;
            case "analytics_enabled":
                if (typeof host_set_analytics_enabled === "function") host_set_analytics_enabled(on ? 1 : 0);
                return;
            }
        },

        /* The SHARED sink, applied by writeGlobalParam for exactly the keys
         * GLOBAL_ROUTING marks `persist: "save"`. */
        persist: () => saveMasterFxChainConfig(),

        /* The Updates menu page's entries, plus [Help...]. Own runner rather
         * than the host's generic one for the same reason Master FX has one:
         * the generic runner takes the IPC slot and would run a SLOT action. */
        runAction: (action) => runGlobalActionFromGrid(action),
    };

    return createGlobalGridIo(io);
}

/*
 * Start or stop the file browser to match the flag.
 *
 * Named rather than inlined into the write, because the flag file and the
 * process must move together: a flag written without the process started reads
 * as On with nothing listening on :404. The old adjustMasterFxSetting branch
 * still inlines its own copy; it goes when that path does (Task 9), and until
 * then this is the only caller.
 */
function setFilebrowserRunning(on) {
    const flagPath = "/data/UserData/schwung/filebrowser_enabled";
    if (on) {
        host_write_file(flagPath, "1");
        if (typeof host_system_cmd === "function") {
            host_system_cmd("sh -c '/data/UserData/schwung/bin/filebrowser --noauth --address 0.0.0.0 --port 404 --root /data/UserData --database /data/UserData/schwung/filebrowser.db --disableThumbnails --disablePreviewResize --disableExec --disableTypeDetectionByHeader >/dev/null 2>&1 &'");
        }
    } else {
        host_remove_dir(flagPath);
        if (typeof host_system_cmd === "function") {
            host_system_cmd("sh -c 'killall filebrowser 2>/dev/null'");
        }
    }
}

/*
 * A Master FX action chosen from the KNOB GRID.
 *
 * Exactly the hand-off runSlotActionFromGrid performs, and for exactly the same
 * reason: Save / Save As / Delete do not act, they raise a modal, and both the
 * drawing of those modals and the jog/click that answer them live under
 * `case VIEWS.MASTER_FX`. Left in the grid, pressing Save would appear to do
 * nothing at all.
 *
 * It asks WHETHER A MODAL IS NOW OPEN rather than listing which keys are modal
 * ones, so a fourth action that opens a confirm is not silently broken in the
 * same way.
 */
function runMasterFxActionFromGrid(key) {
    handleMasterFxSettingsAction(key);
    if (!(masterShowingNamePreview || masterConfirmingOverwrite || masterConfirmingDelete)) {
        return false;
    }
    exitParamPages();
    /* The Master FX view must stay the LIST/modal surface while the modal is
     * up — re-entering the grid would drop the confirmation on the floor. */
    suppressMasterGridOnce = true;
    masterModalFromGrid = true;
    setView(VIEWS.MASTER_FX);
    needsRedraw = true;
    return true;
}

/* ...and back to the grid once the modal is done with. RECONCILES rather than
 * firing at the end of each flow — see maybeReturnToSlotGrid for why hooking
 * each exit is how the original bug got there. */
function maybeReturnToMasterGrid() {
    if (!masterModalFromGrid) return false;
    if (masterShowingNamePreview || masterConfirmingOverwrite || masterConfirmingDelete) return false;
    /* ...and while the on-screen keyboard is up. The name preview's "Edit"
     * clears masterShowingNamePreview before opening it, so all three flags are
     * down for the whole of the text entry. See maybeReturnToSlotGrid. */
    if (isTextEntryActive()) return false;
    /* Only once the Master FX surface is actually idle. The preset picker is
     * the one other screen a finished modal can leave up, and re-entering the
     * grid over it would take a screen the user is looking at. */
    if (inMasterPresetPicker) return false;
    masterModalFromGrid = false;
    suppressMasterGridOnce = false;
    enterMasterFxSettingsGrid();
    return true;
}

/*
 * A Global Settings action chosen from the page chrome.
 *
 * Third instance of runSlotActionFromGrid / runMasterFxActionFromGrid, and the
 * two properties that make those work are kept:
 *
 * It asks WHETHER SOMETHING ELSE IS NOW ON SCREEN rather than listing which
 * actions leave. All three of today's actions do leave — Help pushes the help
 * stack, [Check Updates] and [Module Store] set a view of their own — so a test
 * on the key would be right today and silently wrong for the fourth action.
 *
 * The two store screens set their own view and route back through
 * storeReturnView; Help does not, so this is also where VIEWS.GLOBAL_SETTINGS
 * gets set for it. That view no longer draws a settings list — it is the help
 * viewer's host and nothing else.
 */
function runGlobalActionFromGrid(action) {
    handleGlobalSettingsAction(action);
    const opened = helpNavStack.length > 0 || !!helpDetailScrollState
                   || view !== VIEWS.PARAM_PAGES;
    if (!opened) return false;
    exitParamPages();
    globalModalFromGrid = true;
    if (view === VIEWS.PARAM_PAGES) setView(VIEWS.GLOBAL_SETTINGS);
    needsRedraw = true;
    return true;
}

/* ...and back to the page once the help stack is done with.
 *
 * RECONCILES from the draw path rather than firing at the end of each flow —
 * see maybeReturnToSlotGrid for why hooking each exit is how the original bug
 * got there. Help has three ways out (Back off the last frame, Back out of a
 * detail and then the frame, and the detail's own "Back" action row) and only
 * one of them is a single obvious site.
 *
 * The store screens are NOT reconciled here: they leave VIEWS.GLOBAL_SETTINGS
 * entirely and come back through storeReturnView -> enterGlobalSettings(),
 * which clears the flag itself. */
function maybeReturnToGlobalGrid() {
    if (!globalModalFromGrid) return false;
    if (helpNavStack.length > 0 || helpDetailScrollState) return false;
    if (isTextEntryActive()) return false;
    globalModalFromGrid = false;
    /* Consumed: it is the MASTER FX back handler that reads this, and a stale
     * GLOBAL_SETTINGS left in it would send a help session opened from Master FX
     * to the settings page instead. */
    if (helpReturnView === VIEWS.GLOBAL_SETTINGS) helpReturnView = null;
    enterGlobalSettings();
    return true;
}

/* Enter chain settings view */
function enterChainSettings(slotIndex) {
    selectedSlot = slotIndex;
    selectedChainSetting = 0;
    editingChainSettingValue = false;

    /* Knob grid instead of the list, when the user has opted in. Same gate the
     * component editor uses, so the screen reader still gets the list — a grid
     * has nothing selected to read out. */
    if (paramPagesEnabled() && !suppressSlotGridOnce) {
        enterParamPages(slotIndex, "slot", "slot", null, slotGridIoFor(slotIndex));
        return;
    }
    suppressSlotGridOnce = false;

    setView(VIEWS.CHAIN_SETTINGS);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const setting = SLOT_SETTINGS[0];
    const val = getSlotSettingValue(slotIndex, setting);
    announce(`Chain Settings, ${setting.label}: ${val}`);
}

/* Get current value for a chain setting */
/* Check if a slot is in MPE mode (Recv=All + Fwd=THRU) */
function isSlotMpeMode(slot) {
    const recv = parseInt(getSlotParam(slot, "slot:receive_channel")) || 0;
    const fwd = parseInt(getSlotParam(slot, "slot:forward_channel"));
    return recv === 0 && fwd === -2;
}

/* State to restore when MPE mode is turned off */
const chainMpePreState = [null, null, null, null];

function getChainSettingValue(slot, setting) {
    if (setting.key === "mpe_mode") {
        return isSlotMpeMode(slot) ? "On" : "Off";
    }
    const val = getSlotParam(slot, setting.key);
    if (val === null) return "-";

    if (setting.key === "slot:volume") {
        const pct = Math.round(parseFloat(val) * 100);
        return `${pct}%`;
    }
    if (setting.key === "slot:muted") {
        return parseInt(val) ? "Yes" : "No";
    }
    if (setting.key === "slot:soloed") {
        return parseInt(val) ? "Yes" : "No";
    }
    if (setting.key === "slot:forward_channel") {
        const ch = parseInt(val);
        if (ch === -2) return "Thru";
        if (ch === -1) return "Auto";
        return `Ch ${ch + 1}`;  // Internal 0-15 → display 1-16
    }
    if (setting.key === "slot:receive_channel") {
        const ch = parseInt(val);
        return ch === 0 ? "All" : `Ch ${val}`;
    }
    if (setting.key === "slot:transpose") {
        const n = parseInt(val) || 0;
        if (n === 0) return "0 st";
        return `${n > 0 ? "+" : ""}${n} st`;
    }
    if (setting.key === "midi_fx_pre_mode") {
        return parseInt(val) ? "Schw+Move" : "Schw";
    }
    return String(val);
}

/* Adjust a chain setting value */
function adjustChainSetting(slot, setting, delta) {
    if (setting.type === "action") return;

    /* MPE Mode toggle: sets recv/fwd/synth MPE in one action */
    if (setting.key === "mpe_mode") {
        const mpeOn = isSlotMpeMode(slot);
        if (delta > 0 && !mpeOn) {
            chainMpePreState[slot] = {
                recv: getSlotParam(slot, "slot:receive_channel"),
                fwd: getSlotParam(slot, "slot:forward_channel"),
            };
            shadowSetParamBlocking(slot, "slot:receive_channel", "0");    /* All */
            shadowSetParamBlocking(slot, "slot:forward_channel", "-2");   /* THRU */
            shadowSetParamBlocking(slot, "synth:mpe_enabled", "1");
        } else if (delta < 0 && mpeOn) {
            const prev = chainMpePreState[slot];
            shadowSetParamBlocking(slot, "slot:receive_channel", prev?.recv || String(slot + 1));
            shadowSetParamBlocking(slot, "slot:forward_channel", prev?.fwd || "-1");
            shadowSetParamBlocking(slot, "synth:mpe_enabled", "0");
            chainMpePreState[slot] = null;
        }
        return;
    }

    const currentVal = getSlotParam(slot, setting.key);
    let newVal;

    if (setting.type === "float") {
        const parsed = parseFloat(currentVal);
        const current = isNaN(parsed) ? setting.min : parsed;
        newVal = Math.max(setting.min, Math.min(setting.max, current + delta * setting.step));
        newVal = newVal.toFixed(2);
    } else if (setting.type === "int") {
        const parsed = parseInt(currentVal);
        const current = isNaN(parsed) ? setting.min : parsed;
        newVal = Math.max(setting.min, Math.min(setting.max, current + delta * setting.step));
        newVal = String(newVal);
    }

    if (newVal !== undefined) {
        setSlotParam(slot, setting.key, newVal);
    }
}

/* ========== Knob Editor Functions ========== */

/* Enter knob editor for a slot */
function enterKnobEditor(slot) {
    knobEditorSlot = slot;
    knobEditorIndex = 0;
    loadKnobAssignments(slot);
    setView(VIEWS.KNOB_EDITOR);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const assignLabel = getKnobAssignmentLabel(knobEditorAssignments[0]);
    announce(`Knob Editor, Knob 1: ${assignLabel}`);
}

/* Load current knob assignments from DSP */
function loadKnobAssignments(slot) {
    knobEditorAssignments = [];
    for (let i = 0; i < NUM_KNOBS; i++) {
        const target = getSlotParam(slot, `knob_${i + 1}_target`) || "";
        const param = getSlotParam(slot, `knob_${i + 1}_param`) || "";
        knobEditorAssignments.push({ target, param });
    }
}

/* Get available targets for knob assignment (components with modules loaded) */
function getKnobTargets(slot) {
    const targets = [{ id: "", name: "(None)" }];
    const cfg = chainConfigs[slot];
    if (!cfg) return targets;

    /* In signal order, so the list reads the way the chain does. The prefix is
     * spelled per kind ("FX1", not "FX 1") — these strings are what the knob
     * editor has always shown. */
    const push = (id, prefix, moduleData) => {
        if (!moduleData || !moduleData.module) return;
        const name = getSlotParam(slot, `${id}:name`) || moduleData.module;
        targets.push({ id, name: `${prefix}: ${name}` });
    };
    cfg.midiFx.forEach((m, i) => push(`midi_fx${i + 1}`, "MIDI FX", m));
    push("synth", "Synth", cfg.synth);
    cfg.fx.forEach((m, i) => push(`fx${i + 1}`, `FX${i + 1}`, m));

    return targets;
}

/* Get available params for a target via ui_hierarchy or known params */
function getKnobParamsForTarget(slot, target) {
    const params = [];

    /* Try to get params from ui_hierarchy */
    const hierarchy = getSlotParam(slot, `${target}:ui_hierarchy`);
    if (hierarchy) {
        try {
            const hier = JSON.parse(hierarchy);
            /* Collect all knob-mappable params from the hierarchy */
            if (hier.levels) {
                for (const levelName in hier.levels) {
                    const level = hier.levels[levelName];
                    if (level.knobs && Array.isArray(level.knobs)) {
                        for (const knob of level.knobs) {
                            /* knob can be just a param name or a {key, label} object */
                            if (typeof knob === "string") {
                                if (!params.find(p => p.key === knob)) {
                                    params.push({ key: knob, label: knob });
                                }
                            } else if (knob.key) {
                                if (!params.find(p => p.key === knob.key)) {
                                    params.push({ key: knob.key, label: knob.label || knob.key });
                                }
                            }
                        }
                    }
                    /* Also check params array */
                    if (level.params && Array.isArray(level.params)) {
                        for (const p of level.params) {
                            if (typeof p === "string") {
                                if (!params.find(pp => pp.key === p)) {
                                    params.push({ key: p, label: p });
                                }
                            } else if (p.key) {
                                if (!params.find(pp => pp.key === p.key)) {
                                    params.push({ key: p.key, label: p.label || p.key });
                                }
                            }
                        }
                    }
                }
            }
        } catch (e) {
            /* Parse error, fall back to known params */
        }
    }

    /* Fall back to chain_params metadata before using hardcoded defaults */
    if (params.length === 0) {
        const chainParamsJson = getSlotParam(slot, `${target}:chain_params`);
        if (chainParamsJson) {
            try {
                const chainParams = JSON.parse(chainParamsJson);
                if (Array.isArray(chainParams)) {
                    for (const p of chainParams) {
                        if (!p || !p.key) continue;
                        if (!params.find(pp => pp.key === p.key)) {
                            params.push({ key: p.key, label: p.name || p.label || p.key });
                        }
                    }
                }
            } catch (e) {
                /* Parse error, continue to legacy hardcoded fallback */
            }
        }
    }

    /* If no params from hierarchy, use known defaults */
    if (params.length === 0) {
        if (target === "synth") {
            params.push({ key: "preset", label: "Preset" });
            params.push({ key: "volume", label: "Volume" });
        } else {
            /* FX params */
            params.push({ key: "wet", label: "Wet" });
            params.push({ key: "dry", label: "Dry" });
            params.push({ key: "room_size", label: "Room Size" });
            params.push({ key: "damping", label: "Damping" });
        }
    }

    return params;
}

function getNumericParamsForTarget(slot, target, numericOnly = true) {
    const params = [];
    const seen = new Set();
    const chainMetaByKey = new Map();
    const chainParamsJson = getSlotParam(slot, `${target}:chain_params`);
    if (!chainParamsJson) return params;

    try {
        const chainParams = JSON.parse(chainParamsJson);
        if (!Array.isArray(chainParams)) return params;
        for (const p of chainParams) {
            if (!p || !p.key) continue;
            chainMetaByKey.set(p.key, p);
        }

        const hierarchyJson = getSlotParam(slot, `${target}:ui_hierarchy`);
        if (hierarchyJson) {
            try {
                const hierarchy = JSON.parse(hierarchyJson);
                const levels = hierarchy && hierarchy.levels && typeof hierarchy.levels === "object"
                    ? hierarchy.levels : null;
                if (levels) {
                    for (const levelName of Object.keys(levels)) {
                        const level = levels[levelName];
                        if (!level || !Array.isArray(level.params)) continue;

                        const childPrefix = (typeof level.child_prefix === "string") ? level.child_prefix : "";
                        const rawChildCount = parseInt(level.child_count, 10);
                        const childCount = Number.isFinite(rawChildCount) ? Math.max(0, rawChildCount) : 0;
                        const childLabel = (typeof level.child_label === "string" && level.child_label)
                            ? level.child_label : "Item";

                        for (const entry of level.params) {
                            const key = (typeof entry === "string") ? entry : (entry && entry.key ? entry.key : "");
                            if (!key) continue;

                            const meta = chainMetaByKey.get(key);
                            if (!meta) continue;
                            const type = (meta.type || "").toLowerCase();
                            const isNumeric = type === "float" || type === "int" ||
                                (!type && (meta.min !== undefined || meta.max !== undefined));
                            if (numericOnly && !isNumeric) continue;

                            if (childPrefix && childCount > 0) {
                                for (let i = 0; i < childCount; i++) {
                                    const fullKey = `${childPrefix}${i}_${key}`;
                                    if (seen.has(fullKey)) continue;
                                    const baseLabel = meta.name || meta.label || key;
                                    params.push({ key: fullKey, label: `${childLabel} ${i + 1} ${baseLabel}` });
                                    seen.add(fullKey);
                                }
                            } else {
                                if (seen.has(key)) continue;
                                params.push({ key, label: meta.name || meta.label || key });
                                seen.add(key);
                            }
                        }
                    }
                }
            } catch (e) {
                /* Ignore ui_hierarchy parse errors and fall back to chain_params list. */
            }
        }

        if (params.length > 0) return params;

        for (const p of chainParams) {
            if (!p || !p.key) continue;
            const type = (p.type || "").toLowerCase();
            const isNumeric = type === "float" || type === "int" ||
                (!type && (p.min !== undefined || p.max !== undefined));
            if (numericOnly && !isNumeric) continue;
            if (!seen.has(p.key)) {
                params.push({ key: p.key, label: p.name || p.label || p.key });
                seen.add(p.key);
            }
        }
    } catch (e) {
        /* Ignore parse errors and return empty numeric set. */
    }

    return params;
}

function inferLinkedTargetComponentKey(paramKey, meta) {
    if (meta && typeof meta.target_key === "string" && meta.target_key.trim()) {
        return meta.target_key.trim();
    }
    return "";
}

function inferLinkedTargetParamKey(componentKey, meta) {
    if (meta && typeof meta.param_key === "string" && meta.param_key.trim()) {
        return meta.param_key.trim();
    }
    return "";
}

function parseMetaStringList(value) {
    if (Array.isArray(value)) {
        return value
            .map(v => String(v).trim())
            .filter(Boolean);
    }
    if (typeof value === "string") {
        return value
            .split(",")
            .map(v => v.trim())
            .filter(Boolean);
    }
    return [];
}

function buildModulePickerOptions(meta) {
    if (hierEditorSlot < 0 || !hierEditorComponent || hierEditorIsMasterFx) {
        return [];
    }

    const opts = [];
    const seen = new Set();
    const allowNone = meta && meta.allow_none !== undefined ? parseMetaBool(meta.allow_none) : true;
    const allowSelf = meta && meta.allow_self !== undefined ? parseMetaBool(meta.allow_self) : false;
    const allowedTargets = new Set(parseMetaStringList(meta && meta.allowed_targets));
    const selfTarget = getComponentParamPrefix(hierEditorComponent);

    if (allowNone) {
        opts.push("");
        seen.add("");
    }

    const targets = getKnobTargets(hierEditorSlot);
    for (const target of targets) {
        if (!target || !target.id) continue;
        if (!allowSelf && selfTarget && target.id === selfTarget) continue;
        if (allowedTargets.size > 0 && !allowedTargets.has(target.id)) continue;
        if (seen.has(target.id)) continue;
        opts.push(target.id);
        seen.add(target.id);
    }

    return opts;
}

function buildParameterPickerOptions(paramKey, meta) {
    if (hierEditorSlot < 0 || !hierEditorComponent || hierEditorIsMasterFx) {
        return [];
    }

    const opts = [];
    const seen = new Set();
    const allowNone = meta && meta.allow_none !== undefined ? parseMetaBool(meta.allow_none) : true;
    if (allowNone) {
        opts.push("");
        seen.add("");
    }

    const prefix = getComponentParamPrefix(hierEditorComponent);
    if (!prefix) return opts;

    const targetKey = inferLinkedTargetComponentKey(paramKey, meta);
    if (!targetKey) return opts;

    const selectedTarget = getSlotParam(hierEditorSlot, `${prefix}:${targetKey}`) || "";
    if (!selectedTarget) return opts;

    const numericOnly = meta && meta.numeric_only !== undefined ? parseMetaBool(meta.numeric_only) : true;
    const params = getNumericParamsForTarget(hierEditorSlot, selectedTarget, numericOnly);
    for (const param of params) {
        if (!param || !param.key || seen.has(param.key)) continue;
        opts.push(param.key);
        seen.add(param.key);
    }

    return opts;
}

function getDynamicPickerMeta(key, meta) {
    if (!meta || typeof meta !== "object") return meta;
    const rawType = String(meta.type || "").toLowerCase();
    if (rawType !== "module_picker" && rawType !== "parameter_picker") return meta;

    const dynamicOptions = rawType === "module_picker"
        ? buildModulePickerOptions(meta)
        : buildParameterPickerOptions(key, meta);

    return {
        ...meta,
        type: "enum",
        options: dynamicOptions,
        picker_type: rawType,
        none_label: meta.none_label || "(none)"
    };
}

function buildDynamicPickerTargetItems(meta) {
    const optionIds = buildModulePickerOptions(meta);
    const sourceTargets = getKnobTargets(hierEditorSlot);
    const nameById = new Map();
    for (const target of sourceTargets) {
        if (!target || !target.id) continue;
        nameById.set(target.id, target.name || target.label || target.id);
    }
    return optionIds.map(id => ({
        id,
        label: id ? (nameById.get(id) || id) : (meta.none_label || "(none)")
    }));
}

function buildDynamicPickerParamItemsForTarget(meta, targetId) {
    if (!targetId) return [];
    const numericOnly = meta && meta.numeric_only !== undefined ? parseMetaBool(meta.numeric_only) : true;
    const params = getNumericParamsForTarget(hierEditorSlot, targetId, numericOnly);
    return params.map(p => ({ key: p.key, label: p.label || p.key }));
}

function resetDynamicParamPickerState() {
    dynamicPickerMeta = null;
    dynamicPickerKey = "";
    dynamicPickerTargetKey = "";
    dynamicPickerMode = "target";
    dynamicPickerIndex = 0;
    dynamicPickerTargets = [];
    dynamicPickerParams = [];
    dynamicPickerSelectedTarget = "";
}

function closeDynamicParamPicker(announcement) {
    resetDynamicParamPickerState();
    refreshHierarchyChainParams();
    refreshHierarchyVisibility();
    setView(VIEWS.HIERARCHY_EDITOR);
    needsRedraw = true;
    if (announcement) {
        announce(announcement);
    }
}

function openDynamicParamPicker(key, meta) {
    if (!meta || !meta.picker_type) return false;
    if (hierEditorSlot < 0 || !hierEditorComponent || hierEditorIsMasterFx) return false;

    const prefix = getComponentParamPrefix(hierEditorComponent);
    if (!prefix) return false;

    resetDynamicParamPickerState();
    dynamicPickerMeta = meta;
    dynamicPickerKey = key;
    dynamicPickerTargetKey = inferLinkedTargetComponentKey(key, meta);
    dynamicPickerTargets = buildDynamicPickerTargetItems(meta);

    const currentValue = getSlotParam(hierEditorSlot, `${prefix}:${key}`) || "";
    if (meta.picker_type === "module_picker") {
        const idx = dynamicPickerTargets.findIndex(t => t.id === currentValue);
        dynamicPickerIndex = idx >= 0 ? idx : 0;
        dynamicPickerMode = "target";
        dynamicPickerSelectedTarget = currentValue;
    } else {
        const currentTarget = dynamicPickerTargetKey
            ? (getSlotParam(hierEditorSlot, `${prefix}:${dynamicPickerTargetKey}`) || "")
            : "";
        dynamicPickerSelectedTarget = currentTarget;
        dynamicPickerParams = buildDynamicPickerParamItemsForTarget(meta, currentTarget);

        if (currentTarget && dynamicPickerParams.length > 0) {
            dynamicPickerMode = "param";
            const idx = dynamicPickerParams.findIndex(p => p.key === currentValue);
            dynamicPickerIndex = idx >= 0 ? idx : 0;
        } else {
            dynamicPickerMode = "target";
            const idx = dynamicPickerTargets.findIndex(t => t.id === currentTarget);
            dynamicPickerIndex = idx >= 0 ? idx : 0;
        }
    }

    setView(VIEWS.DYNAMIC_PARAM_PICKER);
    needsRedraw = true;
    if (meta.picker_type === "parameter_picker" && dynamicPickerMode === "param") {
        announce("Select target parameter");
    } else {
        announce("Select target component");
    }
    return true;
}

function handleDynamicParamPickerSelect() {
    if (!dynamicPickerMeta || !dynamicPickerKey) {
        closeDynamicParamPicker("Hierarchy Editor");
        return;
    }

    const prefix = getComponentParamPrefix(hierEditorComponent);
    if (!prefix) {
        closeDynamicParamPicker("Hierarchy Editor");
        return;
    }

    if (dynamicPickerMode === "target") {
        const selectedTarget = dynamicPickerTargets[dynamicPickerIndex];
        if (!selectedTarget) return;

        if (dynamicPickerMeta.picker_type === "module_picker") {
            setSlotParam(hierEditorSlot, `${prefix}:${dynamicPickerKey}`, selectedTarget.id || "");
            const linkedParamKey = inferLinkedTargetParamKey(dynamicPickerKey, dynamicPickerMeta);
            if (linkedParamKey) {
                setSlotParam(hierEditorSlot, `${prefix}:${linkedParamKey}`, "");
            }
            closeDynamicParamPicker(`Target component ${selectedTarget.label || "(none)"}`);
            return;
        }

        /* parameter_picker target stage */
        if (!dynamicPickerTargetKey) {
            closeDynamicParamPicker("No target key");
            return;
        }

        setSlotParam(hierEditorSlot, `${prefix}:${dynamicPickerTargetKey}`, selectedTarget.id || "");
        dynamicPickerSelectedTarget = selectedTarget.id || "";

        if (!dynamicPickerSelectedTarget) {
            setSlotParam(hierEditorSlot, `${prefix}:${dynamicPickerKey}`, "");
            closeDynamicParamPicker("Target cleared");
            return;
        }

        dynamicPickerParams = buildDynamicPickerParamItemsForTarget(dynamicPickerMeta, dynamicPickerSelectedTarget);
        if (dynamicPickerParams.length === 0) {
            setSlotParam(hierEditorSlot, `${prefix}:${dynamicPickerKey}`, "");
            closeDynamicParamPicker("No matching params");
            return;
        }

        dynamicPickerMode = "param";
        dynamicPickerIndex = 0;
        announceMenuItem("Param", dynamicPickerParams[0].label || dynamicPickerParams[0].key || "");
        needsRedraw = true;
        return;
    }

    const selectedParam = dynamicPickerParams[dynamicPickerIndex];
    if (!selectedParam) return;

    if (dynamicPickerTargetKey) {
        setSlotParam(hierEditorSlot, `${prefix}:${dynamicPickerTargetKey}`, dynamicPickerSelectedTarget || "");
    }
    setSlotParam(hierEditorSlot, `${prefix}:${dynamicPickerKey}`, selectedParam.key || "");
    closeDynamicParamPicker(`Target ${dynamicPickerSelectedTarget}:${selectedParam.key}`);
}

/* Get display label for a knob assignment */
function getKnobAssignmentLabel(assignment) {
    if (!assignment || !assignment.target || !assignment.param) {
        return "(None)";
    }
    return `${assignment.target}: ${assignment.param}`;
}

/* Enter param picker for current knob */
function enterKnobParamPicker() {
    knobParamPickerFolder = null;
    knobParamPickerIndex = 0;
    knobParamPickerParams = [];
    knobParamPickerHierarchy = null;
    knobParamPickerLevel = null;
    knobParamPickerPath = [];
    setView(VIEWS.KNOB_PARAM_PICKER);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const targets = getKnobTargets(knobEditorSlot);
    const targetName = targets[0]?.name || "None";
    announce(`Knob ${knobEditorIndex + 1} Target, ${targetName}`);
}

/* Get items for current level in knob param picker hierarchy */
function getKnobPickerLevelItems(hierarchy, levelName) {
    const items = [];
    if (!hierarchy || !hierarchy.levels || !hierarchy.levels[levelName]) {
        return items;
    }
    const level = hierarchy.levels[levelName];
    const componentPrefix = knobParamPickerFolder || "";
    const isVisible = (entry) => {
        if (!entry || typeof entry !== "object") return true;
        if (entry.visible_if &&
            !evaluateVisibilityConditionForContext(knobEditorSlot, componentPrefix, entry.visible_if, level, -1)) {
            return false;
        }
        if (entry.level && hierarchy.levels && hierarchy.levels[entry.level] &&
            hierarchy.levels[entry.level].visible_if) {
            return evaluateVisibilityConditionForContext(
                knobEditorSlot,
                componentPrefix,
                hierarchy.levels[entry.level].visible_if,
                hierarchy.levels[entry.level],
                -1
            );
        }
        return true;
    };

    /* Add navigation items (levels) and param items */
    if (level.params && Array.isArray(level.params)) {
        for (const p of level.params) {
            if (!isVisible(p)) continue;
            if (typeof p === "string") {
                /* Simple param name */
                items.push({ type: "param", key: p, label: p });
            } else if (p && typeof p === "object") {
                if (p.level) {
                    /* Navigation item - drill into another level */
                    items.push({ type: "nav", level: p.level, label: p.label || p.level });
                } else if (p.key) {
                    /* Param with label */
                    items.push({ type: "param", key: p.key, label: p.label || p.key });
                }
            }
        }
    }

    /* If no params but has knobs, use knobs as params */
    if (items.length === 0 && level.knobs && Array.isArray(level.knobs)) {
        for (const k of level.knobs) {
            if (typeof k === "string") {
                items.push({ type: "param", key: k, label: k });
            } else if (k && k.key) {
                items.push({ type: "param", key: k.key, label: k.label || k.key });
            }
        }
    }

    return items;
}

/* Find first level with params in hierarchy (skip preset browsers) */
function findFirstParamLevel(hierarchy) {
    if (!hierarchy || !hierarchy.levels) return "root";

    /* Check if root has children, follow to first real params level */
    let level = hierarchy.levels.root;
    let levelName = "root";

    /* Skip preset browser levels (those with list_param) */
    while (level && level.list_param && level.children) {
        levelName = level.children;
        level = hierarchy.levels[levelName];
    }

    return levelName;
}

/* Apply knob assignment from picker */
function applyKnobAssignment(target, param) {
    const assignment = { target: target || "", param: param || "" };
    knobEditorAssignments[knobEditorIndex] = assignment;

    /* Save to DSP via set_param */
    const knobNum = knobEditorIndex + 1;
    if (target && param) {
        /* Set knob mapping - DSP uses "target:param" format internally */
        setSlotParam(knobEditorSlot, `knob_${knobNum}_set`, `${target}:${param}`);
    } else {
        /* Clear knob mapping */
        setSlotParam(knobEditorSlot, `knob_${knobNum}_clear`, "1");
    }

    /* Refresh knob mappings cache */
    fetchKnobMappings(knobEditorSlot);
    invalidateKnobContextCache();

    /* Announce and return to knob editor */
    if (target && param) {
        announce(`Knob ${knobNum} assigned to ${param}`);
    } else {
        announce(`Knob ${knobNum} cleared`);
    }
    setView(VIEWS.KNOB_EDITOR);
    needsRedraw = true;
}

/* ========== End Knob Editor Functions ========== */

/* Handle Shift+Click - enter component edit mode */
function handleShiftSelect() {
    const comp = slotChainComponents(selectedSlot)[selectedChainComponent];
    if (!comp) return;

    /*
     * The `+` IS the empty position, so Shift opens the picker on it too.
     *
     * enterComponentSelect refuses an unresolved `+` by design -- it wants a
     * real position -- so the box is resolved first, exactly as the plain
     * click does. Without this, shift+click did nothing on an empty cell in
     * the slot chain while working in Master FX, whose own handler already
     * resolved it. Reported from the device as precisely that asymmetry.
     */
    if (comp.kind === "add") {
        const at = beginChainInsertFromAddBox(slotChainTarget(selectedSlot), comp);
        if (at >= 0) {
            selectedChainComponent = at;
            enterComponentSelect(selectedSlot, at);
        }
        return;
    }

    if (!isChainModuleKey(comp.key)) return;

    /* Shift+click always goes to module chooser (for swapping) */
    enterComponentSelect(selectedSlot, selectedChainComponent);
}

/* Enter component edit mode - try hierarchy editor first, then module UI, then preset browser */
function enterComponentEdit(slotIndex, componentKey) {
    debugLog(`enterComponentEdit: slot=${slotIndex}, key=${componentKey}`);
    selectedSlot = slotIndex;
    editingComponentKey = componentKey;

    /* Check for synth load errors first to show a warning overlay */
    if (componentKey === "synth" && checkAndShowSynthError(slotIndex)) {
        return;
    }

    /* Try hierarchy editor first (for plugins with ui_hierarchy) */
    const hierarchy = getComponentHierarchy(slotIndex, componentKey);
    debugLog(`enterComponentEdit: hierarchy=${hierarchy ? 'found' : 'null'}`);
    if (hierarchy) {
        debugLog(`enterComponentEdit: calling enterHierarchyEditor`);
        enterHierarchyEditor(slotIndex, componentKey);
        return;
    }

    /* Fall back to simple preset browser */
    debugLog(`enterComponentEdit: falling back to simple preset browser`);
    enterComponentEditFallback(slotIndex, componentKey);
}

/* Fallback component edit - simple preset browser */
function enterComponentEditFallback(slotIndex, componentKey) {
    selectedSlot = slotIndex;
    editingComponentKey = componentKey;

    /* Get module ID from chain config */
    const moduleData = getChainComponentModule(chainConfigs[slotIndex], componentKey);
    const moduleId = moduleData ? moduleData.module : null;

    /* Try to load the module's UI */
    if (moduleId && loadModuleUi(slotIndex, componentKey, moduleId)) {
        /* Module UI loaded successfully */
        setView(VIEWS.COMPONENT_EDIT);
        needsRedraw = true;
        return;
    }

    /* CO-RUN: loadModuleUi is refused (it would overwrite globalThis.tick and
     * starve the running tool), so a module with no ui_hierarchy would dead-end
     * on the bare preset browser — and with NO presets there is nothing to show
     * or edit. Before that, give the module its real param menu by synthesizing
     * a hierarchy from its chain_params and entering the (co-run-dispatched)
     * hierarchy editor. Gated to the no-preset case so preset-having modules
     * keep the working preset browser; non-co-run paths are untouched. */
    if (coRunChainEditSlot >= 0) {
        const cPrefix = getComponentParamPrefix(componentKey);
        const cCountStr = getSlotParam(slotIndex, `${cPrefix}:preset_count`);
        const cPresetCount = cCountStr ? parseInt(cCountStr) : 0;
        if (cPresetCount <= 0) {
            const synthHier = buildSynthHierarchyFromChainParams(
                getComponentChainParams(slotIndex, componentKey));
            debugLog(`enterComponentEditFallback: co-run no-preset synth=${componentKey} ` +
                     `params=${synthHier ? synthHier.levels.root.params.length : 0}`);
            if (synthHier) {
                enterHierarchyEditorWith(slotIndex, componentKey, synthHier);
                return;
            }
        }
    }

    /* Fall back to simple preset browser */
    const prefix = getComponentParamPrefix(componentKey);

    /* Fetch preset count and current preset */
    const countStr = getSlotParam(slotIndex, `${prefix}:preset_count`);
    editComponentPresetCount = countStr ? parseInt(countStr) : 0;

    const presetStr = getSlotParam(slotIndex, `${prefix}:preset`);
    editComponentPreset = presetStr ? parseInt(presetStr) : 0;

    /* Fetch preset name */
    editComponentPresetName = getSlotParam(slotIndex, `${prefix}:preset_name`) || "";

    setView(VIEWS.COMPONENT_EDIT);
    needsRedraw = true;

    /* Announce menu title + initial selection - name first, then position */
    const moduleName = getSlotParam(slotIndex, `${prefix}:name`) || componentKey;
    const presetName = editComponentPresetName || `Preset ${editComponentPreset + 1}`;
    if (editComponentPresetCount > 0) {
        announce(`${moduleName}, ${presetName}, Preset ${editComponentPreset + 1} of ${editComponentPresetCount}`);
    } else {
        announce(`${moduleName}, No presets`);
    }
}

/* ============================================================
 * Hierarchy Editor - Generic parameter editing for plugins
 * ============================================================ */

/* Enter hierarchy-based parameter editor for a component. Fetches the
 * component's real ui_hierarchy; falls back to the preset browser if absent. */
function enterHierarchyEditor(slotIndex, componentKey) {
    /*
     * A Master FX position arrives here only from the knob grid's two hand-offs
     * (a non-grid page kind, and an opaque param), which carry the component key
     * the grid was opened with and nothing else. Route it to the master entry
     * point rather than letting it fall through: this path would set
     * hierEditorIsMasterFx = false, and that flag is what exitHierarchyEditor
     * reads to decide where Back goes — so a Master FX module opened from the
     * grid would eject into the SLOT chain editor. (Its param reads would have
     * been right, since "master_fx:fx2" is self-addressing; only the identity
     * would have been lost, which is the failure that looks like a UI glitch
     * and reads like nothing at all in review.)
     */
    const mfx = masterFxIndexFromComponentKey(componentKey);
    if (mfx >= 0) { enterMasterFxHierarchyEditor(mfx); return; }

    const hierarchy = getComponentHierarchy(slotIndex, componentKey);
    if (!hierarchy) {
        /* No hierarchy - fall back to simple preset browser */
        enterComponentEditFallback(slotIndex, componentKey);
        return;
    }
    enterHierarchyEditorWith(slotIndex, componentKey, hierarchy);
}

/*
 * The grid only draws PAGE_KNOBS (shadow_ui_param_pages.mjs's drawParamPages
 * returns false on anything else) — a preset browser, a runtime items list, a
 * mode select or a child selector is meant to hand off to the list editor
 * that already draws it. That hand-off never actually populated the list
 * editor's state though: enterParamPages() never touches hierEditorSlot /
 * hierEditorHierarchy / hierEditorLevel, so falling straight into
 * drawHierarchyEditor() drew whatever those globals happened to hold —
 * usually their reset defaults (hierEditorSlot=-1, hierEditorParams=[]),
 * i.e. "S0: no parameters", not the intended preset browser. This performs
 * the hand-off for real: entering the legacy editor exactly as if the user
 * had opened it directly, then jumping to the same *level* the grid was on
 * (not the hierarchy's root) so a "Presets" page mid-tree opens the preset
 * browser, not the top of the whole component.
 *
 * hierEditorPath is left empty rather than reconstructed, so Back exits the
 * component instead of stepping up to the level's real parent. Good enough
 * to make the page work at all; a true breadcrumb would need walking the
 * hierarchy's own level graph backward from `page.level`, which none of the
 * existing entry points do either.
 */
function enterHierarchyEditorFromParamPages() {
    const page = currentParamPage();
    const slotIndex = paramPagesSlot();
    const componentKey = paramPagesComponent();
    exitParamPages();
    suppressParamPagesOnce = true;
    enterHierarchyEditor(slotIndex, componentKey);
    cameFromParamPages = true;
    if (page && page.level && hierEditorHierarchy &&
        hierEditorHierarchy.levels && hierEditorHierarchy.levels[page.level] &&
        page.level !== hierEditorLevel) {
        const levelDef = hierEditorHierarchy.levels[page.level];
        hierEditorLevel = page.level;
        hierEditorPath = [];
        hierEditorChildIndex = -1;
        /*
         * The CHILD COUNT travels with the level, and this path used to drop
         * it.
         *
         * enterHierarchyEditor -> resetHierarchyEditorFor has just zeroed it,
         * and loadHierarchyLevel gates its child selector on
         * `child_prefix && hierEditorChildCount > 0`. So handing off a child
         * level from the grid produced neither the selector nor an error: the
         * level fell through to the generic param list, and
         * buildHierarchyParamKey -- which needs hierEditorChildIndex >= 0 to
         * add the prefix -- emitted the unprefixed template keys. Eleven rows
         * of parameters addressing keys the DSP does not serve.
         *
         * Reported from the device as minijv's "Edit Parts doesn't do
         * anything". The other two navigation sites set these; only the
         * hand-off from the grid did not.
         */
        hierEditorChildCount = levelDef.child_count || 0;
        hierEditorChildLabel = levelDef.child_label || "Child";
        loadHierarchyLevel();
    }
}

/* Enter the hierarchy editor with an explicit hierarchy object. Lets callers
 * inject a SYNTHESIZED hierarchy (see buildSynthHierarchyFromChainParams) for a
 * component that lacks a real ui_hierarchy — used in co-run, where loadModuleUi
 * is refused so a preset-less module would otherwise dead-end on "No presets".
 * Editing resolves real min/max/type/options from hierEditorChainParams via
 * getParamMetadata(), so the injected level only needs each param's `key`.
 * NOTE for future edits: a synthesized hierarchy has no preset level, so
 * changeHierPreset() early-returns and the loading-transition re-fetch is
 * guarded by `if (newHier)` — don't "fix" those guards to re-pull the
 * hierarchy unconditionally or it would clobber the synthesized one with null. */
/*
 * Dismiss whatever is over the editor and drop the knob state left pointing at
 * the last component.
 *
 * Called by both chain editors, but NOT folded into resetHierarchyEditorFor:
 * the slot path runs it BEFORE the paramPages early return, so opening the
 * knob grid clears the overlay too. Order is the behaviour here.
 */
function dismissOverlayForHierarchyEntry() {
    hideOverlay();
    invalidateKnobContextCache();
    pendingHierKnobIndex = -1;
    pendingHierKnobDelta = 0;
}

/*
 * The hierarchy editor's per-entry state, aimed at one component.
 *
 * Shared by BOTH chain editors. `componentKey` is what hierEditorComponent
 * holds, which is the editor key for a slot chain ("midiFx", "fx2") and the
 * prefixed form for Master FX ("master_fx:fx2") — the one place the two
 * genuinely disagree, so it is a parameter rather than something derived.
 *
 * `isMasterFx` / `masterFxSlot` are the legacy pair the rest of the file still
 * asks (`hierEditorIsMasterFx`) to know which chain it is editing. They will
 * be a chain target once the exit paths follow.
 *
 * NOT in 4b, and the reason is worth recording so the next reader does not
 * re-derive it. The knob CONTEXT carried a dead copy of the same pair and that
 * is gone — nothing read it. This pair has FOURTEEN read sites across the
 * hierarchy editor's refresh, exit, preset and LFO paths, and two obstacles a
 * knob-card change cannot absorb:
 *
 *   - `hierEditorComponent` holds the PREFIXED key on Master FX
 *     ("master_fx:fx2") while a chain target's key() takes the bare position
 *     ("fx2"), so every site needs a conversion, not a substitution.
 *   - Half the sites ask it "which chain am I in / where do I go back to",
 *     which a target can only answer with `target.kind` — the kind test §1b
 *     forbids. Those sites need a return-destination of their own first.
 *
 * That is a refactor of the hierarchy editor's chain identity, in the largest
 * switch in this file, with no pixel baseline over it. Its own step.
 */
function resetHierarchyEditorFor(slotIndex, componentKey, hierarchy, isMasterFx, masterFxSlot) {
    hierEditorSlot = slotIndex;
    hierEditorComponent = componentKey;
    hierEditorHierarchy = hierarchy;
    hierEditorLevel = hierarchy.modes ? null : "root";  // Start at mode select if modes exist
    hierEditorPath = [];
    hierEditorChildIndex = -1;
    hierEditorChildCount = 0;
    hierEditorChildLabel = "";
    hierEditorSelectedIdx = 0;
    hierEditorEditMode = false;
    resetHierarchyEditState();
    hierEditorIsMasterFx = isMasterFx;
    hierEditorMasterFxSlot = masterFxSlot;
    resetDynamicParamPickerState();
}

/*
 * What the screen reader says on arrival, for BOTH chain editors: the preset
 * name and its position when the level is a preset browser, the first param
 * otherwise, and an explicit "No parameters" rather than silence.
 */
function announceHierarchyEditorEntry(moduleName) {
    if (hierEditorIsPresetLevel && hierEditorPresetCount > 0) {
        /* Preset browser level - announce preset name first, then position */
        const presetName = hierEditorPresetName || `Preset ${hierEditorPresetIndex + 1}`;
        announce(`${moduleName}, ${presetName}, Preset ${hierEditorPresetIndex + 1} of ${hierEditorPresetCount}`);
    } else if (hierEditorParams.length > 0) {
        const param = hierEditorParams[0];
        const label = param.label || param.key;
        const value = param.value || "";
        announce(`${moduleName}, ${label}: ${value}`);
    } else {
        announce(`${moduleName}, No parameters`);
    }
}

function enterHierarchyEditorWith(slotIndex, componentKey, hierarchy) {
    if (!hierarchy) {
        /* No hierarchy - fall back to simple preset browser */
        enterComponentEditFallback(slotIndex, componentKey);
        return;
    }

    dismissOverlayForHierarchyEntry();

    /* Preview: the knob grid replaces the list for this component when the
     * user has opted in. It plans from the same declared contract, so nothing
     * about entry differs — and paramPagesEnabled() forces the list whenever the
     * screen reader is on, since a grid has nothing selected to read out. */
    if (paramPagesEnabled() && !suppressParamPagesOnce) {
        enterParamPages(slotIndex, componentKey, getComponentParamPrefix(componentKey),
                        null, null, paramPagesChromeFor(componentKey));
        return;
    }
    suppressParamPagesOnce = false;

    resetHierarchyEditorFor(slotIndex, componentKey, hierarchy, false, -1);
    /* Only the slot path clears the file browser. Master FX never has, which is
     * an asymmetry rather than a decision — left alone here because 4a-2 is a
     * refactor with no behaviour change. */
    filepathBrowserState = null;
    filepathBrowserParamKey = "";

    /* Fetch chain_params metadata for this component */
    hierEditorChainParams = getComponentChainParams(slotIndex, componentKey);

    /* Set up param shims for this component */
    setupModuleParamShims(slotIndex, componentKey);

    /* Load current level's params and knobs */
    loadHierarchyLevel();

    /* Check for synth errors (missing assets) when entering synth editor */
    if (componentKey === "synth") {
        checkAndShowSynthError(slotIndex);
    }

    setView(VIEWS.HIERARCHY_EDITOR);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const prefix = getComponentParamPrefix(componentKey);
    const moduleName = getSlotParam(slotIndex, `${prefix}:name`) || componentKey;
    announceHierarchyEditorEntry(moduleName);
}

/* Enter hierarchy-based parameter editor for a Master FX slot */
function enterMasterFxHierarchyEditor(fxSlot) {
    if (fxSlot < 0 || fxSlot >= MASTER_FX_SLOTS) return;

    const hierarchy = getMasterFxHierarchy(fxSlot);
    if (!hierarchy) {
        /* No hierarchy - just return, module selection is available */
        return;
    }

    dismissOverlayForHierarchyEntry();

    /* Master FX params are addressed at IPC slot 0 (a convention, NOT
     * instrument slot 0), and hierEditorComponent carries the prefixed form
     * "master_fx:fxN" so params become "master_fx:fxN:param". */
    const fxKey = masterFxComponentKey(fxSlot);
    const componentKey = `master_fx:${fxKey}`;

    /*
     * Param View = Knobs opens the grid HERE TOO.
     *
     * The gate is a copy of enterHierarchyEditorWith's, and it is a copy on
     * purpose: the two entry points differ in how they resolve the hierarchy
     * and in nothing else, and this is the smaller half of converging them.
     * Without it the setting was silently slot-chain-only — the same module
     * opened the labelled knob grid in a slot and the scrolling list in Master
     * FX, which is the drift §1b of the variable-length design exists to end,
     * and it was reported from the device within a day.
     *
     * The chrome is what makes the grid say "MFX", read the master spelling of
     * the module key, and send Back to the Master FX editor rather than to the
     * slot chain. See paramPagesChromeFor.
     */
    if (paramPagesEnabled() && !suppressParamPagesOnce) {
        enterParamPages(MASTER_CHAIN_TARGET.slot, componentKey,
                        getComponentParamPrefix(componentKey), null, null,
                        paramPagesChromeFor(componentKey));
        return;
    }
    suppressParamPagesOnce = false;

    resetHierarchyEditorFor(0, componentKey, hierarchy, true, fxSlot);

    /* Fetch chain_params metadata for this Master FX slot */
    hierEditorChainParams = getMasterFxChainParams(fxSlot);

    /* Set up param shims for Master FX component */
    setupModuleParamShims(0, componentKey);

    /* Load current level's params and knobs */
    loadHierarchyLevel();

    setView(VIEWS.HIERARCHY_EDITOR);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = getMasterFxParam(fxSlot, "name") || `FX ${fxSlot + 1}`;
    announceHierarchyEditorEntry(moduleName);
}

/* Load params and knobs for current hierarchy level */
function loadHierarchyLevel() {
    if (!hierEditorHierarchy) return;

    const levels = hierEditorHierarchy.levels;
    const levelDef = hierEditorLevel ? levels[hierEditorLevel] : null;

    if (!levelDef) {
        /* At mode selection level - include swap module here */
        hierEditorAllParams = [...(hierEditorHierarchy.modes || []), SWAP_MODULE_ACTION];
        hierEditorAllKnobs = [];
        hierEditorParams = [...hierEditorAllParams];
        hierEditorKnobs = [];
        hierEditorIsPresetLevel = false;
        hierEditorIsDynamicItems = false;
        return;
    }

    /* Determine if this is the top level (swap module only at top) */
    /* Also treat the direct child of root as top level when root has children,
       since root's edit mode (where swap is injected) is never shown in that case */
    const rootDef = levels["root"];
    const isTopLevel = !hierEditorHierarchy.modes && (
        hierEditorLevel === "root" ||
        (rootDef && rootDef.children === hierEditorLevel)
    );

    /* Child selector for levels that require child_prefix */
    if (levelDef.child_prefix && hierEditorChildCount > 0 && hierEditorChildIndex < 0) {
        hierEditorIsPresetLevel = false;
        hierEditorIsDynamicItems = false;
        hierEditorPresetEditMode = false;
        hierEditorAllKnobs = [];
        hierEditorAllParams = [];
        const label = hierEditorChildLabel || "Child";
        for (let i = 0; i < hierEditorChildCount; i++) {
            hierEditorAllParams.push({
                isChild: true,
                childIndex: i,
                label: `${label} ${i + 1}`
            });
        }
        hierEditorParams = [...hierEditorAllParams];
        hierEditorKnobs = [];
        return;
    }

    /* Check if this is a preset browser level */
    if (levelDef.list_param && levelDef.count_param) {
        hierEditorIsPresetLevel = true;
        hierEditorIsDynamicItems = false;
        hierEditorPresetEditMode = false;  /* Reset edit mode when entering preset level */
        hierEditorAllKnobs = levelDef.knobs || [];

        /* Fetch preset count and current preset */
        const prefix = getComponentParamPrefix(hierEditorComponent);
        const countStr = getSlotParam(hierEditorSlot, `${prefix}:${levelDef.count_param}`);
        hierEditorPresetCount = countStr ? parseInt(countStr) : 0;

        const presetStr = getSlotParam(hierEditorSlot, `${prefix}:${levelDef.list_param}`);
        hierEditorPresetIndex = presetStr ? parseInt(presetStr) : 0;

        /* Fetch preset name */
        const nameParam = levelDef.name_param || "preset_name";
        hierEditorPresetName = getSlotParam(hierEditorSlot, `${prefix}:${nameParam}`) || "";

        /* Also load params for preset edit mode (swap only at top level) */
        hierEditorAllParams = isTopLevel
            ? [...(levelDef.params || []), SWAP_MODULE_ACTION]
            : (levelDef.params || []);
    } else if (levelDef.items_param) {
        /* Dynamic items level - fetch items from plugin */
        hierEditorIsPresetLevel = false;
        hierEditorIsDynamicItems = true;
        hierEditorPresetEditMode = false;
        hierEditorSelectParam = levelDef.select_param || "";
        hierEditorNavigateTo = levelDef.navigate_to || "";
        hierEditorAllKnobs = levelDef.knobs || [];

        /* Fetch items list from plugin */
        const prefix = getComponentParamPrefix(hierEditorComponent);
        const itemsJson = getSlotParam(hierEditorSlot, `${prefix}:${levelDef.items_param}`);
        let items = [];
        if (itemsJson) {
            try {
                items = JSON.parse(itemsJson);
            } catch (e) {
                console.log(`Failed to parse items_param: ${e}`);
            }
        }

        /* Convert items to params format with isDynamicItem flag */
        hierEditorAllParams = items.map(item => ({
            isDynamicItem: true,
            label: item.label || item.name || `Item ${item.index}`,
            index: item.index
        }));
        hierEditorParams = [...hierEditorAllParams];
        hierEditorKnobs = [...hierEditorAllKnobs];
    } else {
        hierEditorIsPresetLevel = false;
        hierEditorIsDynamicItems = false;
        hierEditorPresetEditMode = false;
        /* Use hierarchy params for scrollable list, knobs for physical mapping */
        hierEditorAllParams = isTopLevel
            ? [...(levelDef.params || []), SWAP_MODULE_ACTION]
            : (levelDef.params || []);
        hierEditorAllKnobs = levelDef.knobs || [];
    }

    applyHierarchyVisibilityFilters(levelDef);
}

/* Change preset in hierarchy editor preset browser */
function changeHierPreset(delta) {
    if (hierEditorPresetCount <= 0) return;

    /* Get level definition to find param names */
    const levelDef = hierEditorHierarchy.levels[hierEditorLevel];
    if (!levelDef) return;

    /* Calculate new preset with wrapping */
    let newPreset = hierEditorPresetIndex + delta;
    if (newPreset < 0) newPreset = hierEditorPresetCount - 1;
    if (newPreset >= hierEditorPresetCount) newPreset = 0;

    /* Apply the preset change */
    const prefix = getComponentParamPrefix(hierEditorComponent);
    setSlotParam(hierEditorSlot, `${prefix}:${levelDef.list_param}`, String(newPreset));

    /* Update local state */
    hierEditorPresetIndex = newPreset;

    /* Fetch new preset name */
    const nameParam = levelDef.name_param || "preset_name";
    hierEditorPresetName = getSlotParam(hierEditorSlot, `${prefix}:${nameParam}`) || "";

    /* Announce preset change - name first for easier scrolling */
    const presetName = hierEditorPresetName || `Preset ${hierEditorPresetIndex + 1}`;
    announce(`${presetName}, Preset ${hierEditorPresetIndex + 1} of ${hierEditorPresetCount}`);

    /* Re-fetch chain_params for new preset/plugin and invalidate knob cache */
    if (hierEditorIsMasterFx) {
        hierEditorChainParams = getMasterFxChainParams(hierEditorMasterFxSlot);
    } else {
        hierEditorChainParams = getComponentChainParams(hierEditorSlot, hierEditorComponent);
    }
    /* Re-fetch ui_hierarchy too — plugins (e.g. schwung-sfz xsynth fork)
     * emit a different param/knob set per preset. Without this the menu
     * keeps the previous preset's slot list with stale labels until the
     * user exits and re-enters. */
    let newHierarchy = null;
    if (hierEditorIsMasterFx) {
        newHierarchy = getMasterFxHierarchy(hierEditorMasterFxSlot);
    } else {
        newHierarchy = getComponentHierarchy(hierEditorSlot, hierEditorComponent);
    }
    if (newHierarchy) {
        hierEditorHierarchy = newHierarchy;
        /* Rebuild the visible param list from the new hierarchy. */
        loadHierarchyLevel();
    }
    invalidateKnobContextCache();
    /* Plugin set_param("preset", N) may have overwritten knob-mapped values
     * internally (e.g. ambiotica mode change rewrites all 8 knobs). The
     * cached values are now stale, so force a re-read on next knob touch. */
    invalidateKnobValueCache();
    /* ...and book a second look for after any module-side debounce, because
     * the refetch above may have read the contract of the preset we just
     * LEFT. See armHierEditorContractSettle. */
    armHierEditorContractSettle();
}

/* Exit hierarchy editor */
function exitHierarchyEditor() {
    hierEditorContractDueMs = 0;
    /* Clear pending knob state to prevent stale overlays */
    pendingHierKnobIndex = -1;
    pendingHierKnobDelta = 0;
    cameFromParamPages = false;
    /* Cleared on EVERY exit, not just the return-to-grid one: leaving the
     * editor any other way (Back at root, a slot swap, the shortcut out) must
     * not leave the flag armed for a later list-originated session, or the
     * next Back out of an unrelated param edit would teleport to the grid. */
    paramEditorOpenedFromGrid = false;
    paramEditorReturnPage = "";

    clearModuleParamShims();
    clearWavZoomStates();

    /* Determine return view based on whether we're editing Master FX */
    const returnToMasterFx = hierEditorIsMasterFx;

    hierEditorSlot = -1;
    hierEditorComponent = "";
    hierEditorHierarchy = null;
    hierEditorChainParams = [];
    hierEditorAllParams = [];
    hierEditorAllKnobs = [];
    hierEditorChildIndex = -1;
    hierEditorChildCount = 0;
    hierEditorChildLabel = "";
    hierEditorIsPresetLevel = false;
    hierEditorPresetEditMode = false;
    resetHierarchyEditState();
    hierEditorIsMasterFx = false;
    hierEditorMasterFxSlot = -1;
    filepathBrowserState = null;
    filepathBrowserParamKey = "";
    resetDynamicParamPickerState();

    view = returnToMasterFx ? VIEWS.MASTER_FX : VIEWS.CHAIN_EDIT;
    needsRedraw = true;
}

/* Refresh chain_params metadata for dynamic filepath fields (e.g. start_path). */
function refreshHierarchyChainParams() {
    if (hierEditorIsMasterFx) {
        if (hierEditorMasterFxSlot >= 0) {
            hierEditorChainParams = getMasterFxChainParams(hierEditorMasterFxSlot);
        }
        return;
    }

    if (hierEditorSlot >= 0 && hierEditorComponent) {
        hierEditorChainParams = getComponentChainParams(hierEditorSlot, hierEditorComponent);
    }
}

/* Open generic file browser for a filepath parameter */
function openHierarchyFilepathBrowser(key, meta) {
    refreshHierarchyChainParams();
    const effectiveMeta = getParamMetadata(key) || meta;
    if (!effectiveMeta || effectiveMeta.type !== "filepath") return false;

    const fullKey = buildHierarchyParamKey(key);
    const currentVal = getSlotParam(hierEditorSlot, fullKey) || "";
    const prefix = getComponentParamPrefix(hierEditorComponent);

    filepathBrowserParamKey = key;
    filepathBrowserState = buildFilepathBrowserState(effectiveMeta, currentVal);
    filepathBrowserState.livePreviewEnabled = parseMetaBool(effectiveMeta.live_preview);
    filepathBrowserState.previewOriginalValue = currentVal;
    filepathBrowserState.previewCurrentValue = currentVal;
    filepathBrowserState.previewCommitted = false;
    filepathBrowserState.previewParamFullKey = fullKey;
    filepathBrowserState.previewPendingPath = "";
    filepathBrowserState.previewPendingTime = 0;
    filepathBrowserState.previewSelectedPath = "";
    const hooks = buildFilepathBrowserHooks(effectiveMeta, prefix);
    filepathBrowserState.hooksOnOpen = hooks.onOpen;
    filepathBrowserState.hooksOnPreview = hooks.onPreview;
    filepathBrowserState.hooksOnCancel = hooks.onCancel;
    filepathBrowserState.hooksOnCommit = hooks.onCommit;
    filepathBrowserState.hookRestoreValues = {};
    applyFilepathHookActions(filepathBrowserState, filepathBrowserState.hooksOnOpen, { path: currentVal });

    refreshFilepathBrowser(filepathBrowserState, FILEPATH_BROWSER_FS);

    if (filepathBrowserState.livePreviewEnabled) {
        const selected = filepathBrowserState.items[filepathBrowserState.selectedIndex];
        applyLivePreview(filepathBrowserState, selected);
    }

    setView(VIEWS.FILEPATH_BROWSER);

    if (filepathBrowserState.items.length > 0) {
        const selected = filepathBrowserState.items[filepathBrowserState.selectedIndex];
        announceMenuItem(selected.label || "File", "");
    } else {
        announce("No files found");
    }

    return true;
}

function closeHierarchyFilepathBrowser() {
    const state = filepathBrowserState;
    if (state) {
        const committed = !!state.previewCommitted;
        if (state.livePreviewEnabled &&
            !committed &&
            state.previewParamFullKey &&
            state.previewCurrentValue !== state.previewOriginalValue) {
            setSlotParam(
                hierEditorSlot,
                state.previewParamFullKey,
                state.previewOriginalValue || ""
            );
        }

        if (committed) {
            applyFilepathHookActions(state, state.hooksOnCommit, { path: state.previewSelectedPath || state.previewCurrentValue || "" });
        } else {
            applyFilepathHookActions(state, state.hooksOnCancel, { path: state.previewOriginalValue || "" });
        }

        restoreFilepathHookActions(state);
    }

    filepathBrowserState = null;
    filepathBrowserParamKey = "";
    resetDynamicParamPickerState();
    /* Back where the user came from. All three call sites — committing a
     * selection, Back, and the no-state guard — funnel through here, so the
     * grid return lives here rather than being repeated (and forgotten) at
     * each one. Committing a sample used to drop you in the hierarchy list. */
    if (paramEditorOpenedFromGrid) {
        returnToParamPagesFromEditor();
        return;
    }
    setView(VIEWS.HIERARCHY_EDITOR);
}

/* Get param metadata from chain_params */
function getParamMetadata(key) {
    let chainMeta = null;
    let hierarchyMeta = null;

    if (Array.isArray(hierEditorChainParams)) {
        chainMeta = hierEditorChainParams.find(p => p && p.key === key) || null;
    }
    if (Array.isArray(hierEditorAllParams)) {
        hierarchyMeta = hierEditorAllParams.find(p => p && typeof p === "object" && p.key === key) || null;
    }

    const merged = chainMeta && hierarchyMeta
        ? { ...hierarchyMeta, ...chainMeta }
        : (chainMeta || hierarchyMeta);

    return normalizeExpandedParamMeta(key, merged);
}

function getStepPrecision(step, fallback) {
    const num = Number(step);
    if (!Number.isFinite(num) || num <= 0) return fallback;
    const text = String(num).toLowerCase();
    const expIdx = text.indexOf("e-");
    if (expIdx >= 0) {
        const exp = parseInt(text.slice(expIdx + 2), 10);
        if (Number.isFinite(exp)) return Math.max(0, Math.min(6, exp));
        return fallback;
    }
    const dotIdx = text.indexOf(".");
    if (dotIdx < 0) return 0;
    return Math.max(0, Math.min(6, text.length - dotIdx - 1));
}

function getWavPositionSetPrecision(meta) {
    const unit = String(meta && meta.display_unit || "percent").toLowerCase();
    if (unit === "ms") return 0;

    const baseStep = Math.abs(parseMetaNumber(meta && meta.step, 0.01));
    const shiftMult = getWavPositionShiftMultiplier(meta);
    const fineStep = baseStep > 0 ? Math.abs(baseStep * shiftMult) : 0;
    let effectiveStep = fineStep > 0
        ? Math.min(baseStep || fineStep, fineStep)
        : baseStep;
    /* When the knob can zoom, step is dynamically divided by up to 2^8 (256x).
     * Account for the smallest possible step or formatParamForSet will round
     * away sub-thousandth changes and the marker won't move at high zoom. */
    if (meta && meta.enable_zoom) {
        effectiveStep = effectiveStep / 256;
    }
    const fallback = (unit === "sec" || unit === "s") ? 3 : 2;
    const precision = getStepPrecision(effectiveStep, fallback);
    return Math.max(fallback, precision);
}

/* Format a param value for setting (respects type) */
function formatParamForSet(val, meta) {
    if (meta && meta.ui_type === "wav_position") {
        if (meta.display_unit === "ms") return Math.round(val).toString();
        const precision = getWavPositionSetPrecision(meta);
        return Number(val).toFixed(precision);
    }
    return ufFormatParamForSet(val, meta);
}

function formatParamForOverlay(val, meta) {
    /* Local-only special cases that param_format.mjs intentionally doesn't know about */
    if (meta && meta.ui_type === "wav_position") {
        return formatWavPositionDisplayValue(val, meta);
    }
    if (meta && meta.type === "canvas" && meta.show_value === false) {
        return "";
    }
    if (meta && meta.type === "canvas") {
        return formatCanvasDisplayValue(val, meta);
    }
    /* picker enum with empty value -> none_label */
    if (meta && (meta.type === "enum" || meta.type === "bool") &&
        meta.picker_type && (val === "" || val === null || val === undefined)) {
        return meta.none_label || "(none)";
    }
    /* enum/bool — go through formatMetaOptionValue (handles index↔label dual format) */
    if (meta && (meta.type === "enum" || meta.type === "bool")) {
        return String(formatMetaOptionValue(meta, val));
    }
    /* If the param has a unit or display_format, hand off to the shared formatter */
    if (meta && (meta.unit || meta.display_format)) {
        return ufFormatParamValue(val, meta);
    }
    /* Legacy auto-percent fallback for un-tagged floats in 0..(1..4] range —
     * preserves visual behavior of un-migrated modules until they declare unit:"%". */
    if (meta && meta.type === "int") {
        return Math.round(val).toString();
    }
    const min = meta && typeof meta.min === "number" ? meta.min : 0;
    const max = meta && typeof meta.max === "number" ? meta.max : 1;
    if (min === 0 && max >= 1 && max <= 4) {
        return Math.round(val * 100) + "%";
    }
    return Number(val).toFixed(2);
}

function getHierarchyLevelDef() {
    if (!hierEditorHierarchy || !hierEditorLevel) return null;
    return hierEditorHierarchy.levels ? hierEditorHierarchy.levels[hierEditorLevel] : null;
}

function buildHierarchyParamKey(key) {
    const prefix = getComponentParamPrefix(hierEditorComponent);
    const levelDef = getHierarchyLevelDef();
    if (levelDef && levelDef.child_prefix && hierEditorChildIndex >= 0) {
        return `${prefix}:${levelDef.child_prefix}${hierEditorChildIndex}_${key}`;
    }
    return `${prefix}:${key}`;
}

function getHierarchyDisplayRawValue(slot, fullKey) {
    const baseVal = getSlotParam(slot, `${fullKey}:base`);
    if (baseVal !== null && baseVal !== undefined) return baseVal;
    return getSlotParam(slot, fullKey);
}

function isHierarchyParamModulated(slot, fullKey) {
    const modulated = getSlotParam(slot, `${fullKey}:modulated`);
    if (modulated === "1") return true;
    if (modulated === "0") return false;

    /* Fallback for targets that don't implement :modulated. */
    const baseVal = getSlotParam(slot, `${fullKey}:base`);
    /*
     * EMPTY is a failed read, not a base of "".
     *
     * A key nobody serves does not come back null: the shim answers with
     * error=4 and a zeroed value buffer, and js_shadow_get_param does not look
     * at `error`, so JS receives "". Comparing a real live value against that
     * empty string then said "modulated" for every param whose live read
     * happens to succeed — which is every slot-level setting, since `slot:*`
     * keys are real but `slot:*:base` is not. Volume, Mute, Solo, Transpose,
     * Recv and Fwd all wore the modulation tilde on the slot-settings grid.
     *
     * A base is a parameter value; "" is never one.
     */
    if (baseVal === null || baseVal === undefined || baseVal === "") return false;
    const liveVal = getSlotParam(slot, fullKey);
    return liveVal !== null && liveVal !== undefined && liveVal !== baseVal;
}

function buildHierarchyParamKeyForLevel(levelDef, key, childIndex) {
    const prefix = getComponentParamPrefix(hierEditorComponent);
    if (levelDef && levelDef.child_prefix && childIndex >= 0) {
        return `${prefix}:${levelDef.child_prefix}${childIndex}_${key}`;
    }
    return `${prefix}:${key}`;
}

function resetHierarchyEditState() {
    hierEditorEditKey = "";
    hierEditorEditValue = null;
}

function beginHierarchyParamEdit(key) {
    const meta = getParamMetadata(key);
    if (meta && (meta.type === "string" || meta.type === "canvas")) {
        return false;
    }

    const fullKey = buildHierarchyParamKey(key);
    const baseVal = getSlotParam(hierEditorSlot, `${fullKey}:base`);
    const liveVal = getSlotParam(hierEditorSlot, fullKey);
    if (baseVal === null && liveVal === null) return false;

    hierEditorEditKey = fullKey;
    hierEditorEditValue = String((baseVal !== null) ? baseVal : liveVal);
    return true;
}

function shouldRefreshDynamicRateMeta(key) {
    return typeof key === "string" && /_rate_mode$/.test(key);
}

/* Adjust selected param value via jog */
function adjustHierSelectedParam(delta) {
    if (hierEditorSelectedIdx >= hierEditorParams.length) return;

    const param = hierEditorParams[hierEditorSelectedIdx];
    if (param && typeof param === "object" && param.isChild) return;
    const key = typeof param === "string" ? param : param.key || param;

    /* Skip special actions */
    if (key === SWAP_MODULE_ACTION) return;
    const fullKey = buildHierarchyParamKey(key);

    const usingStableEditVal = hierEditorEditMode &&
                               hierEditorEditKey === fullKey &&
                               hierEditorEditValue !== null;
    const currentVal = usingStableEditVal ? String(hierEditorEditValue) : getSlotParam(hierEditorSlot, fullKey);
    if (currentVal === null) return;

    const meta = getParamMetadata(key);

    /* Debug: log what we found */
    debugLog(`adjustHierSelectedParam: key=${key}, currentVal=${currentVal}, meta=${JSON.stringify(meta)}, chainParams=${JSON.stringify(hierEditorChainParams)}`);

    if (meta && (meta.type === "string" || meta.type === "canvas")) {
        return;
    }

    /* Handle enum type - cycle through options */
    if (meta && meta.type === "enum" && meta.options && meta.options.length > 0) {
        /* Plugin may return option string ("Sine") or numeric index ("0") */
        let currentIndex = meta.options.indexOf(currentVal);
        const pluginUsesIndex = (currentIndex < 0);
        if (pluginUsesIndex) {
            const parsed = parseInt(currentVal, 10);
            currentIndex = (!isNaN(parsed) && parsed >= 0 && parsed < meta.options.length) ? parsed : 0;
        }
        let newIndex = currentIndex + delta;
        if (newIndex < 0) newIndex = meta.options.length - 1;
        if (newIndex >= meta.options.length) newIndex = 0;
        const newVal = meta.options[newIndex];
        /* Send in the format the plugin expects */
        setSlotParam(hierEditorSlot, fullKey, pluginUsesIndex ? String(newIndex) : newVal);
        if (usingStableEditVal) {
            hierEditorEditValue = pluginUsesIndex ? String(newIndex) : newVal;
        }
        if (shouldRefreshDynamicRateMeta(key)) {
            refreshHierarchyChainParams();
        }
        refreshHierarchyVisibility();
        return;
    }

    /* Legacy single-marker zoom (no view_group): Shift+jog still adjusts zoom
     * one step per click. Multi-marker (view_group) views use the dedicated
     * zoom knob instead — Shift+jog there is plain fine-step value editing. */
    if (meta && meta.ui_type === "wav_position" && meta.enable_zoom && !meta.view_group && isShiftHeld()) {
        const cur = getWavZoomLevel(hierEditorSlot, meta, fullKey);
        setWavZoomLevel(hierEditorSlot, meta, fullKey, cur + (delta > 0 ? 1 : -1));
        needsRedraw = true;
        return;
    }

    /* Handle numeric types — jog-click semantics: one declared step per click, no accel. */
    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    const isInt = meta && meta.type === "int";
    let step = (meta && meta.step > 0) ? meta.step : (isInt ? 1 : 0.01);
    if (meta && meta.ui_type === "wav_position" && isShiftHeld()) {
        const fineStep = Math.abs(step) * getWavPositionShiftMultiplier(meta);
        if (fineStep > 0) step = fineStep;
    }
    /* wav_position with enable_zoom: when zoomed in, scale step inversely
     * so jog-click moves a proportional fraction of the visible viewport. */
    if (meta && meta.ui_type === "wav_position" && meta.enable_zoom) {
        const z = getWavZoomLevel(hierEditorSlot, meta, fullKey);
        if (z > 0) step = step / Math.pow(2, z);
    }
    const min = meta && typeof meta.min === "number" ? meta.min : 0;
    const max = meta && typeof meta.max === "number" ? meta.max : 1;
    const newVal = Math.max(min, Math.min(max, num + delta * step));
    const formatted = formatParamForSet(newVal, meta);
    setSlotParam(hierEditorSlot, fullKey, formatted);
    if (usingStableEditVal) {
        hierEditorEditValue = formatted;
    }
    refreshHierarchyVisibility();
}

/*
 * Invalidate knob context cache - call when view/slot/component/level changes
 */
function invalidateKnobContextCache() {
    cachedKnobContexts = [];
    cachedKnobContextsView = "";
    cachedKnobContextsSlot = -1;
    cachedKnobContextsComp = -1;
    cachedKnobContextsLevel = "";
    cachedKnobContextsChildIndex = -1;
    /* NOTE: do NOT invalidate knob value cache here — this gets called on
     * every knob turn via refreshHierarchyVisibility. Value cache is invalidated
     * separately on actual slot/view/level changes. */
}

/*
 * What one physical knob does at one position of a chain — for EITHER chain.
 *
 * This was two ~90-line branches of buildKnobContextForKnob, one per editor.
 * They looked like the same code twice and were not: they disagreed on the
 * FALLBACK RULE, silently, and only one of them was right.
 *
 * ---------------------------------------------------------------------------
 * THE FALLBACK RULE, and why it is this one
 * ---------------------------------------------------------------------------
 * A module declares its knob row in `ui_hierarchy` and its parameter metadata
 * in `chain_params`. The two lists are not the same length. The question is
 * what a knob does when the declared row runs out before the eight knobs do.
 *
 *   Slot editor  (kept): fall back to chain_params ONLY when there is no
 *                        hierarchy at all. A declared row is the author's
 *                        answer, including for the knobs it leaves empty.
 *   Master FX (dropped): fall back whenever the hierarchy had no knob at THAT
 *                        INDEX, filling the rest from chain_params[knobIndex].
 *
 * Six audio-FX modules in tests/fixtures/module-contracts.json declare a
 * hierarchy with fewer than eight knobs AND carry extra chain_params, so six
 * modules behaved differently depending on which chain they were loaded into:
 * belt, freeverb, nam, ottx, psxverb, smack. Two of them show why the dropped
 * rule is not merely "more knobs":
 *
 *   psxverb  knobs [model, decay, mix, reverb_level], chain_params [model,
 *            decay, mix, input_gain, reverb_level]. Knob 5 became
 *            chain_params[4] = reverb_level — a DUPLICATE of knob 4 — while
 *            input_gain stayed unreachable. The index into chain_params has no
 *            relationship to the knobs already mapped.
 *   smack    knobs [loop_len, slice_res, fx_density, order_density],
 *            chain_params[4..7] = capture, arm, ab, reroll — trigger enums
 *            ("Capture Now", "Arm Record", "Re-Roll"). On Master FX, knobs 5-8
 *            FIRED those. The author left them off the knob row on purpose.
 *
 * So the declared row wins, and the extra params stay reachable where they
 * always were: through the menu, which lists all of chain_params. This is a
 * USER-VISIBLE change on Master FX for those six modules — knobs past the
 * declared row now read "not mapped" instead of driving an arbitrary
 * parameter. Do not "fix" it back without redoing that count.
 * ---------------------------------------------------------------------------
 *
 * `pluginName` / `hasModule` come from the caller because resolving them is
 * the one thing the two chains genuinely do differently. `target.label` is
 * what the title says first ("S2" / "MFX").
 */
/*
 * `title` and `cardName` are TWO ANSWERS TO TWO QUESTIONS, and they were one
 * value until the header band started carrying it.
 *
 *   title    what the screen reader says: "MFX: cloudseed Mix". A blind user
 *            has no diagram behind the card, so the chain and the module are
 *            the only context there is and dropping them makes the utterance
 *            useless.
 *   cardName what the card's header band shows: "Mix". A sighted user is
 *            looking AT the diagram the card floats over, with the selected
 *            box already highlighted — so the band would spend a 116px
 *            content width restating what is on screen, and "MFX: cloudseed
 *            mix" does not fit next to its value anyway. Reported from the
 *            device: "you know where you are".
 *
 * Do not collapse these back into one field. Whichever one you keep, one of
 * the two users loses.
 */
function buildChainKnobContext(target, comp, knobIndex, pluginName, hasModule) {
    const generic = (name, title, extra) => Object.assign({
        slot: target.slot,
        key: null,
        fullKey: null,
        meta: null,
        pluginName: name,
        displayName: `Knob ${knobIndex + 1}`,
        title,
        /* The box's own label ("FX 2") or the module name — the one thing the
         * long title adds nothing to once you can see which box is selected. */
        cardName: name,
    }, extra);

    if (!hasModule) {
        return generic(comp.label, `${target.label} ${comp.label}`, { noModule: true });
    }

    const mapped = (key, meta, displayName) => ({
        slot: target.slot,
        key,
        fullKey: target.key(comp.key, key),
        meta,
        pluginName,
        displayName,
        title: `${target.label}: ${pluginName} ${displayName}`,
        /* The parameter, alone. See the note above buildChainKnobContext. */
        cardName: displayName,
    });

    const hierarchy = chainTargetHierarchy(target, comp.key);
    if (hierarchy && hierarchy.levels) {
        /* knobLevelForHierarchy reports the level the mapping ACTUALLY uses —
         * root, or the first child when root declares no knobs. */
        const levelDef = knobLevelForHierarchy(hierarchy);
        if (levelDef && levelDef.knobs && knobIndex < levelDef.knobs.length) {
            const key = levelDef.knobs[knobIndex];
            const chainParams = chainTargetChainParams(target, comp.key);
            /*
             * TWO SOURCES, and chain_params is only the preferred one.
             *
             * A module may declare a parameter's type inline in the same level
             * that named the knob (`{key, name, type: "int", min, max}`), and
             * many do — it is the shape the ui_hierarchy docs show. The host
             * normally renders chain_params out of exactly those declarations,
             * so the two agree and the merge is invisible. It stops being
             * invisible the moment a plugin ANSWERS chain_params itself: the
             * host takes any answer of non-zero length (`result > 0`), so a
             * module that serves the two characters "[]" — impressive-chords
             * does, when the chain_params.json it reads at runtime is not
             * installed — suppresses the host's correct fallback and the knob
             * is left with no metadata at all.
             *
             * The knob model does not fail on that; with no min/max declared
             * it falls back to a float 0..1. So an int -24..24 is turned as a fraction, the
             * module's atoi reads it as 0, and an enum takes option 0 — while
             * the overlay, which runs on local arithmetic, moves normally.
             * Reported from the device as "i could see the values change, but
             * when i release, it reset to the default".
             *
             * The list editor never had this: getParamMetadata has always
             * merged the hierarchy's own params under chain_params, which is
             * why the same parameter was editable from the menu and not from
             * the knob. Same precedence here — chain_params wins key by key,
             * because a plugin serving it at runtime is serving something it
             * computed (a live range, a dynamic options list) and the static
             * declaration is the floor under it, never an override.
             */
            const declared = hierarchyLevelParamMeta(levelDef, key);
            const served = chainParams.find(p => p && p.key === key);
            const merged = (declared && served) ? { ...declared, ...served }
                                                : (served || declared);
            const meta = normalizeExpandedParamMeta(key, merged);
            return mapped(key, meta, meta && meta.name ? meta.name : key.replace(/_/g, " "));
        }
        debugLog(`buildKnobContext: no knob mapping for knobIndex=${knobIndex} on ${comp.key}`);
    } else {
        /* No declared row at all — see THE FALLBACK RULE above. This is the
         * only branch where chain_params order decides what a knob does, and
         * it is defensible here because there is nothing else to go on. */
        const chainParams = chainTargetChainParams(target, comp.key);
        if (chainParams && knobIndex < chainParams.length) {
            const param = chainParams[knobIndex];
            return mapped(param.key, normalizeExpandedParamMeta(param.key, param),
                          param.name || param.key.replace(/_/g, " "));
        }
    }

    return generic(pluginName, `${target.label} ${pluginName}`, { noMapping: true });
}

/*
 * Build knob context for a single knob - internal, called by rebuildKnobContextCache
 */
function buildKnobContextForKnob(knobIndex) {
    /* Hierarchy editor context */
    if (view === VIEWS.HIERARCHY_EDITOR && knobIndex < hierEditorKnobs.length) {
        const key = hierEditorKnobs[knobIndex];
        const fullKey = buildHierarchyParamKey(key);
        const meta = getParamMetadata(key);
        const prefix = getComponentParamPrefix(hierEditorComponent);
        const pluginName = getSlotParam(hierEditorSlot, `${prefix}:name`) || "";
        const displayName = meta && meta.name ? meta.name : key.replace(/_/g, " ");
        return {
            slot: hierEditorSlot,
            key,
            fullKey,
            meta,
            pluginName,
            displayName,
            title: `S${hierEditorSlot + 1}: ${pluginName} ${displayName}`,
            /* See the note above buildChainKnobContext: announcement keeps the
             * context, the card header does not. */
            cardName: displayName
        };
    }
    /* Multi-marker editor view: knob 8 is the dedicated zoom knob even if the
     * level didn't declare a 7th knob, and knobs 1..N map to group members
     * (overriding whatever the level put there). Return a synthetic ctx so
     * adjustKnobAndShow / processPendingHierKnob can intercept. */
    if (view === VIEWS.HIERARCHY_EDITOR) {
        const mmRole = getMultiMarkerKnobRole(knobIndex);
        if (mmRole && (mmRole.type === "zoom" || mmRole.type === "marker")) {
            return {
                slot: hierEditorSlot,
                key: null,
                fullKey: null,
                meta: null,
                pluginName: "",
                displayName: mmRole.type === "zoom" ? "Zoom" : (mmRole.member.meta.name || mmRole.member.key),
                title: mmRole.type === "zoom" ? "Zoom" : (mmRole.member.meta.name || mmRole.member.key),
                /* Already bare — this title carries no chain or module. */
                cardName: mmRole.type === "zoom" ? "Zoom" : (mmRole.member.meta.name || mmRole.member.key),
                noMapping: true
            };
        }
    }

    /* Slot chain editor with a component selected. Only the IDENTITY of the
     * module is resolved here — how a chain answers "what is loaded at this
     * position and what is it called" is genuinely per-chain (a slot serves
     * `fx1_module`, Master FX serves `master_fx:fx1:name`). Everything after
     * that is buildChainKnobContext, once. */
    if (view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0 &&
        selectedChainComponent < slotChainComponents(selectedSlot).length) {
        const comp = slotChainComponents(selectedSlot)[selectedChainComponent];
        /* Only a module position has knobs. Asking for "add_fx_module"
         * would be a real IPC round trip answering "". */
        if (comp && isChainModuleKey(comp.key)) {
            const prefix = getComponentParamPrefix(comp.key);
            /* MIDI FX serves no `:name`, so its header falls back to the id */
            const isMidiFx = comp.key === "midiFx";
            const moduleId = getSlotParam(selectedSlot, `${prefix}_module`) || "";
            const pluginName = (isMidiFx ? null : getSlotParam(selectedSlot, `${prefix}:name`))
                || moduleId || "";
            debugLog(`buildKnobContext: slot=${selectedSlot}, comp=${comp.key}, ` +
                     `prefix=${prefix}, pluginName=${pluginName}, moduleId=${moduleId}`);
            return buildChainKnobContext(slotChainTarget(selectedSlot), comp, knobIndex,
                                         pluginName, moduleId.length > 0);
        }
    }

    /* Master FX with an FX position selected. Same builder, same rules.
     * Gated on the component's KIND rather than on an index compared against a
     * fixed settings position: the list is as long as the chain now, so the
     * settings box is not at a constant index and the `+` is not a module. */
    if (view === VIEWS.MASTER_FX && selectedMasterFxComponent >= 0) {
        const comp = masterFxChainComponents()[selectedMasterFxComponent];
        if (comp && comp.kind === "module") {
            /* The shim answers ":name" with the module id, so this one read is
             * both the identity and the display name. */
            const pluginName = getMasterFxParam(selectedMasterFxComponent, "name");
            return buildChainKnobContext(MASTER_CHAIN_TARGET, comp, knobIndex,
                                         pluginName, !!(pluginName && pluginName.length));
        }
    }

    /* Default: no special context */
    return null;
}

/*
 * Rebuild knob context cache for all 8 knobs
 */
function rebuildKnobContextCache() {
    cachedKnobContexts = [];
    for (let i = 0; i < NUM_KNOBS; i++) {
        cachedKnobContexts.push(buildKnobContextForKnob(i));
    }
    cachedKnobContextsView = view;
    cachedKnobContextsSlot = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorSlot : selectedSlot;
    cachedKnobContextsComp = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorComponent : selectedChainComponent;
    cachedKnobContextsLevel = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorLevel : "";
    cachedKnobContextsChildIndex = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorChildIndex : -1;
}

/*
 * Unified knob context resolution - used by both touch (peek) and turn (adjust)
 * Returns context object or null if no mapping exists for this knob
 * Uses caching to avoid IPC calls on every CC message
 */
let cachedKnobContextsMasterFxComp = -1;  /* Track Master FX component for cache */

function getKnobContext(knobIndex) {
    /* Check if cache is valid */
    const currentSlot = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorSlot : selectedSlot;
    const currentComp = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorComponent : selectedChainComponent;
    const currentLevel = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorLevel : "";
    const currentChildIndex = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorChildIndex : -1;
    const currentMasterFxComp = (view === VIEWS.MASTER_FX) ? selectedMasterFxComponent : -1;

    const cacheValid = (
        cachedKnobContexts.length === NUM_KNOBS &&
        cachedKnobContextsView === view &&
        cachedKnobContextsSlot === currentSlot &&
        cachedKnobContextsComp === currentComp &&
        cachedKnobContextsLevel === currentLevel &&
        cachedKnobContextsChildIndex === currentChildIndex &&
        cachedKnobContextsMasterFxComp === currentMasterFxComp
    );

    if (!cacheValid) {
        rebuildKnobContextCache();
        cachedKnobContextsMasterFxComp = currentMasterFxComp;
    }

    return cachedKnobContexts[knobIndex] || null;
}

/*
 * Show overlay for a knob - shared by touch and turn
 * If value is provided, shows that value; otherwise reads current value
 */
function showKnobOverlay(knobIndex, value) {
    const ctx = getKnobContext(knobIndex);

    if (ctx) {
        if (ctx.noModule) {
            /* Show "No Module Selected" when no module is loaded in slot */
            showKnobFeedback(knobIndex, ctx.title, "No Module Selected", undefined, ctx.cardName);
        } else if (ctx.noMapping) {
            /* Show "not mapped" for unmapped knob */
            showKnobFeedback(knobIndex, `Knob ${knobIndex + 1}`, "not mapped");
        } else if (ctx.fullKey) {
            /* Mapped knob - show value */
            const title = isHierarchyParamModulated(ctx.slot, ctx.fullKey) ? `${ctx.title}~` : ctx.title;
            let displayVal;
            const isEnum = ctx.meta && (ctx.meta.type === "enum" || ctx.meta.type === "bool");
            if (value !== undefined) {
                /* For enums, show string directly; for numbers, format */
                if (isEnum) {
                    displayVal = String(formatMetaOptionValue(ctx.meta, value));
                } else if (ctx.meta && ctx.meta.type === "canvas") {
                    displayVal = formatCanvasDisplayValue(value, ctx.meta);
                } else if (ctx.meta && ctx.meta.type === "string") {
                    displayVal = String(value || "");
                } else {
                    displayVal = formatParamForOverlay(value, ctx.meta);
                }
            } else {
                const currentVal = getHierarchyDisplayRawValue(ctx.slot, ctx.fullKey);
                /* For enums, show string directly; for numbers, parse and format */
                if (isEnum) {
                    if (isTriggerEnumMeta(ctx.meta)) {
                        displayVal = getTriggerEnumOverlayValue(knobIndex);
                    } else {
                        displayVal = (currentVal !== null && currentVal !== undefined && currentVal !== "")
                            ? formatMetaOptionValue(ctx.meta, currentVal)
                            : "-";
                    }
                } else if (ctx.meta && ctx.meta.type === "canvas") {
                    displayVal = formatCanvasDisplayValue(currentVal || "", ctx.meta);
                } else if (ctx.meta && ctx.meta.type === "string") {
                    displayVal = String(currentVal || "");
                } else {
                    const num = parseFloat(currentVal);
                    displayVal = !isNaN(num) ? formatParamForOverlay(num, ctx.meta) : (currentVal || "-");
                }
            }
            /* `value` is the raw the caller turned to, if any — on a pure touch
             * it is undefined and the card's own touch-down read supplies it. */
            showKnobFeedback(knobIndex, title, displayVal, value, ctx.cardName);
        }
        needsRedraw = true;
        return true;
    }
    return false;
}

/*
 * Adjust knob value and show overlay - used by turn handler
 * THROTTLED: Just accumulates delta, actual work done once per tick
 * Returns true if handled, false to fall through to default
 */
function adjustKnobAndShow(knobIndex, delta) {
    debugLog(`adjustKnobAndShow: knobIndex=${knobIndex}, delta=${delta}, view=${view}, selectedChainComponent=${selectedChainComponent}`);
    const ctx = getKnobContext(knobIndex);
    debugLog(`adjustKnobAndShow: ctx=${ctx ? 'present' : 'null'}, noModule=${ctx?.noModule}, noMapping=${ctx?.noMapping}, fullKey=${ctx?.fullKey}`);

    if (ctx) {
        if (ctx.noModule) {
            /* No module loaded - show "No Module Selected" */
            debugLog(`adjustKnobAndShow: noModule, showing overlay`);
            showKnobFeedback(knobIndex, ctx.title, "No Module Selected", undefined, ctx.cardName);
            needsRedraw = true;
            return true;
        }
        if (ctx.noMapping || !ctx.fullKey) {
            /* Multi-marker view overrides the level's knob row — knob 8 is
             * the zoom override and knobs 1..N are group markers, both of
             * which need to accumulate delta even when the level didn't
             * declare them. */
            const mmRole = getMultiMarkerKnobRole(knobIndex);
            const overrideAccepts = mmRole && (mmRole.type === "zoom" || mmRole.type === "marker");
            if (!overrideAccepts) {
                debugLog(`adjustKnobAndShow: noMapping or no fullKey, showing not mapped`);
                showKnobFeedback(knobIndex, `Knob ${knobIndex + 1}`, "not mapped");
                needsRedraw = true;
                return true;
            }
        }

        /* Accumulate delta for throttled processing */
        debugLog(`adjustKnobAndShow: accumulating delta, fullKey=${ctx.fullKey}`);
        if (pendingHierKnobIndex !== knobIndex) {
            /* Different knob - reset accumulator */
            pendingHierKnobIndex = knobIndex;
            pendingHierKnobDelta = delta;
        } else {
            /* Same knob - accumulate delta */
            pendingHierKnobDelta += delta;
        }
        needsRedraw = true;
        return true;
    }
    return false;
}

/*
 * Get cached knob value, or read from plugin if not yet cached.
 * Auto-invalidates if the knob's fullKey has changed (navigation, slot switch, etc).
 * For floats/ints, caches parsed number. For enums, caches string.
 */
function getKnobCachedValue(knobIndex, ctx) {
    if (!ctx || !ctx.fullKey) return null;

    /* Auto-invalidate if key changed (slot switch, navigation, etc) */
    if (knobValueCacheKey[knobIndex] !== ctx.fullKey) {
        knobValueCache[knobIndex] = null;
        knobValueCacheKey[knobIndex] = ctx.fullKey;
    }

    /* Return cached value if available */
    if (knobValueCache[knobIndex] !== null) {
        return knobValueCache[knobIndex];
    }

    /* First access: do a blocking read (one-time cost) */
    const baseVal = getSlotParam(ctx.slot, `${ctx.fullKey}:base`);
    const raw = (baseVal !== null) ? baseVal : getSlotParam(ctx.slot, ctx.fullKey);
    if (raw === null) return null;

    /* Cache as string for enums, number for float/int */
    const isEnum = ctx.meta && (ctx.meta.type === "enum" || ctx.meta.type === "bool");
    if (isEnum) {
        knobValueCache[knobIndex] = raw;
    } else {
        const num = parseFloat(raw);
        knobValueCache[knobIndex] = isNaN(num) ? null : num;
    }
    return knobValueCache[knobIndex];
}

/* Invalidate the knob value cache */
function invalidateKnobValueCache() {
    knobValueCache.fill(null);
    knobValueCacheKey.fill("");
    clearHierKnobStates();
}

/*
 * Process pending hierarchy knob adjustment - called once per tick.
 * Uses local value cache to avoid blocking IPC reads during active turning.
 * Only setSlotParam (write) is done per tick — zero reads during active turning.
 */
function processPendingHierKnob() {
    if (pendingHierKnobIndex < 0 || pendingHierKnobDelta === 0) {
        /* No pending adjustment, but still show overlay if knob active.
         * Use cached value — no IPC reads during active knob holding. */
        if (pendingHierKnobIndex >= 0) {
            const ctx = getKnobContext(pendingHierKnobIndex);
            if (ctx && ctx.fullKey) {
                const cached = getKnobCachedValue(pendingHierKnobIndex, ctx);
                if (cached !== null) {
                    /* The knob index here is pendingHierKnobIndex, NOT a local
                     * `knobIndex` — this branch runs at tick time for whatever
                     * knob is still pending, and feeding the wrong one to the
                     * card shows the wrong parameter. */
                    if (ctx.meta && (ctx.meta.type === "enum" || ctx.meta.type === "bool")) {
                        if (isTriggerEnumMeta(ctx.meta)) {
                            showKnobFeedback(pendingHierKnobIndex, ctx.title,
                                             getTriggerEnumOverlayValue(pendingHierKnobIndex),
                                             undefined, ctx.cardName);
                        } else {
                            showKnobFeedback(pendingHierKnobIndex, ctx.title,
                                             formatMetaOptionValue(ctx.meta, cached), cached,
                                             ctx.cardName);
                        }
                    } else if (ctx.meta && ctx.meta.type === "canvas") {
                        showKnobFeedback(pendingHierKnobIndex, ctx.title,
                                         formatCanvasDisplayValue(String(cached), ctx.meta), cached,
                                         ctx.cardName);
                    } else if (ctx.meta && ctx.meta.type === "string") {
                        showKnobFeedback(pendingHierKnobIndex, ctx.title,
                                         String(cached || ""), cached, ctx.cardName);
                    } else {
                        showKnobFeedback(pendingHierKnobIndex, ctx.title,
                                         formatParamForOverlay(cached, ctx.meta), cached,
                                         ctx.cardName);
                    }
                    needsRedraw = true;
                }
            }
        }
        return;
    }

    const knobIndex = pendingHierKnobIndex;
    const delta = pendingHierKnobDelta;
    pendingHierKnobDelta = 0;  /* Clear accumulated delta */

    /* Multi-marker view overrides the level's knob row entirely. Compute the
     * knob's role first so non-group knob mappings (loop_mode, xfade, etc.)
     * don't leak into the editor. */
    const mmRole = getMultiMarkerKnobRole(knobIndex);
    if (mmRole) {
        if (mmRole.type === "zoom") {
            const anchor = mmRole.anchor;
            const groupKey = anchor.meta.view_group
                ? `group:${hierEditorSlot}::${anchor.meta.view_group}`
                : `single:${anchor.fullKey}`;
            const cur = getWavZoomLevel(hierEditorSlot, anchor.meta, anchor.fullKey);
            /* Float zoom with step 0.5 → half-octave per click; knob_engine
             * accel divides further so slow turns feel smooth, fast turns snap. */
            const cfg = { type: "float", min: 0, max: 8, step: 0.5 };
            const st = getWavZoomKnobState(groupKey, cur);
            const newZoom = knobStep(st, cfg, delta, Date.now(), isShiftHeld());
            setWavZoomLevel(hierEditorSlot, anchor.meta, anchor.fullKey, newZoom);
            needsRedraw = true;
            const factor = Math.pow(2, newZoom);
            const label = newZoom > 0.01 ? `${factor.toFixed(factor < 10 ? 1 : 0)}x` : "1x (off)";
            showOverlay("Zoom", label);
            return;
        }
        if (mmRole.type === "marker") {
            const m = mmRole.member;
            const slot = hierEditorSlot;
            /* Read current via the same path other handlers use. */
            const currentMarkerVal = getSlotParam(slot, m.fullKey);
            if (currentMarkerVal === null) return;
            const num = parseFloat(currentMarkerVal);
            if (isNaN(num)) return;
            /* Zooming in makes the marker knob finer, so the step is scaled
             * on a COPY of the metadata — mutating chain_params metadata in
             * place would leak the zoomed step to every other consumer. */
            const z = getWavZoomLevel(slot, m.meta, m.fullKey);
            const knobMeta = z > 0
                ? { ...m.meta, step: (m.meta.step > 0 ? m.meta.step : 1) / Math.pow(2, z) }
                : m.meta;
            const st = getPhysKnobState(m.fullKey, num);
            const newVal = knobStep(st, knobMeta, delta, Date.now(), isShiftHeld());
            const formatted = formatParamForSet(newVal, m.meta);
            setSlotParam(slot, m.fullKey, formatted);
            if (hierEditorEditMode && hierEditorEditKey === m.fullKey) {
                hierEditorEditValue = formatted;
            }
            knobValueCache[knobIndex] = newVal;
            showKnobFeedback(knobIndex, m.meta.name || m.key,
                             formatParamForOverlay(newVal, m.meta), newVal);
            needsRedraw = true;
            return;
        }
        /* unmapped — swallow the turn silently */
        return;
    }

    const ctx = getKnobContext(knobIndex);
    if (!ctx || ctx.noMapping || !ctx.fullKey) return;

    /* Single-marker wav_position editor view: silence enum knob turns so the
     * user can't toggle e.g. loop_mode by accident from the editor. Only
     * applies when the active wav_position has opted into the new editor
     * (enable_zoom or view_group); legacy modules like MrDrums still let
     * the user turn enum knobs while editing pad_start. */
    if (hierEditorEditMode && ctx.meta && ctx.meta.type === "enum") {
        const sel = getSelectedHierarchyEditableKey();
        const selMeta = sel ? getParamMetadata(sel) : null;
        if (selMeta && selMeta.ui_type === "wav_position" &&
            (selMeta.enable_zoom || selMeta.view_group)) {
            return;
        }
    }

    /*
     * A trigger is FIRED, never scrubbed: turning walks THROUGH the fire
     * value, so a nudge runs the action. A readout has nothing to set.
     * Neither may write from a knob turn -- the grid has refused this since
     * the access axis landed and this surface never did.
     */
    if (isTriggerParam(ctx.meta) || isReadoutParam(ctx.meta)) {
        /*
         * Show the VALUE, not a sentence about the parameter.
         *
         * This said "Read only" and "Click to fire", which is the surface
         * explaining itself in the slot where the reading goes. A readout is a
         * static value and displaying it is the entire point of declaring one
         * -- keydetect exists to be READ. Reported from the device: "we show
         * READ ONLY in the header, that doesn't make sense, it just is a
         * static value".
         *
         * A trigger has no value worth reading, but it draws a BUTTON in its
         * cell and the button is the affordance; the footer already says PUSH.
         */
        const cached = getKnobCachedValue(knobIndex, ctx);
        showKnobFeedback(knobIndex, ctx.title,
                         cached === null ? "" : formatParamForOverlay(cached, ctx.meta),
                         cached === null ? undefined : cached, ctx.cardName);
        needsRedraw = true;
        return;
    }

    /* Get current value from cache (one-time IPC read, then local) */
    const currentVal = getKnobCachedValue(knobIndex, ctx);
    if (currentVal === null) return;

    /* Handle enum type - cycle through options (clamp at ends, don't wrap) */
    if (ctx.meta && ctx.meta.type === "enum" && ctx.meta.options && ctx.meta.options.length > 0) {
        if (isTriggerEnumMeta(ctx.meta)) {
            const shouldFire = updateTriggerEnumAccum(knobIndex, delta);
            if (shouldFire) {
                setSlotParam(ctx.slot, ctx.fullKey, "trigger");
                showKnobFeedback(knobIndex, ctx.title, "Triggered", undefined, ctx.cardName);
            } else {
                showKnobFeedback(knobIndex, ctx.title, getTriggerEnumOverlayValue(knobIndex),
                                 undefined, ctx.cardName);
            }
            return;
        }

        /* Find current index — plugin may return the option string ("Sine")
         * or the numeric index ("0"). Handle both. */
        let currentIndex = ctx.meta.options.indexOf(currentVal);
        if (currentIndex < 0) {
            /* Not a string match — try as numeric index */
            const parsed = parseInt(currentVal, 10);
            if (!isNaN(parsed) && parsed >= 0 && parsed < ctx.meta.options.length) {
                currentIndex = parsed;
            } else {
                currentIndex = 0;
            }
        }
        /* Run through knob_engine so the divisor curve applies — many ticks
         * required per option change, with the same staleness reset semantics. */
        const st = getPhysKnobState(ctx.fullKey, currentIndex);
        const newIndex = knobStep(st, ctx.meta, delta, Date.now(), isShiftHeld());
        if (newIndex === currentIndex) {
            /* No option crossed yet — only update the overlay so the user sees
             * something happening, but DON'T setSlotParam (no value change). */
            showKnobFeedback(knobIndex, ctx.title,
                             formatMetaOptionValue(ctx.meta, ctx.meta.options[currentIndex]),
                             ctx.meta.options[currentIndex], ctx.cardName);
            return;
        }
        const newVal = ctx.meta.options[newIndex];
        /* Cache the value in the same format the plugin returned (string or index) */
        const pluginUsesIndex = (ctx.meta.options.indexOf(currentVal) < 0);
        knobValueCache[knobIndex] = pluginUsesIndex ? String(newIndex) : newVal;
        /* Send in the format the plugin expects */
        setSlotParam(ctx.slot, ctx.fullKey, pluginUsesIndex ? String(newIndex) : newVal);
        if (shouldRefreshDynamicRateMeta(ctx.key)) {
            refreshHierarchyChainParams();
        }
        refreshHierarchyVisibility();
        showKnobFeedback(knobIndex, ctx.title, formatMetaOptionValue(ctx.meta, newVal), newVal,
                         ctx.cardName);
        return;
    }

    if (ctx.meta && ctx.meta.type === "canvas") {
        showKnobFeedback(knobIndex, ctx.title,
                         formatCanvasDisplayValue(String(currentVal), ctx.meta), currentVal,
                         ctx.cardName);
        return;
    }

    if (ctx.meta && ctx.meta.type === "string") {
        showKnobFeedback(knobIndex, ctx.title, String(currentVal || ""), currentVal, ctx.cardName);
        return;
    }

    const num = (typeof currentVal === "number") ? currentVal : parseFloat(currentVal);
    if (isNaN(num)) return;

    /*
     * wav_position carries two step overrides of its own — a shift multiplier
     * and a zoom scale. They are applied to a COPY of the metadata: mutating
     * ctx.meta in place would leak a zoomed step into every other reader of
     * the same chain_params entry.
     */
    let knobMeta = ctx.meta;
    if (ctx.meta && ctx.meta.ui_type === "wav_position") {
        let step = ctx.meta.step > 0 ? ctx.meta.step : 0.01;
        if (isShiftHeld()) {
            const mult = getWavPositionShiftMultiplier(ctx.meta);
            if (Math.abs(step) * mult > 0) step = Math.abs(step) * mult;
        }
        if (ctx.meta.enable_zoom) {
            const z = getWavZoomLevel(ctx.slot, ctx.meta, ctx.fullKey);
            if (z > 0) step = step / Math.pow(2, z);
        }
        knobMeta = { ...ctx.meta, step };
    }
    /*
     * Shift is FINE ADJUST here too.
     *
     * The chain editor's knob overlay never passed it -- reported from the
     * device as "shift doesn't work on an overlay knob from the chain
     * editor". The gesture existed only on the param-pages grid, so the same
     * knob refined under shift on one screen and ignored it on the other.
     *
     * NOT for wav_position, which folds shift into its own step multiplier a
     * few lines above (getWavPositionShiftMultiplier). Passing it here as
     * well would apply the gesture twice.
     */
    const wavPos = !!(ctx.meta && ctx.meta.ui_type === "wav_position");
    const st = getPhysKnobState(ctx.fullKey, num);
    const newVal = knobStep(st, knobMeta, delta, Date.now(), !wavPos && isShiftHeld());

    /* Update local cache — no IPC read needed on next turn */
    knobValueCache[knobIndex] = newVal;

    /* Set the new value (fire-and-forget write, no blocking read) */
    const formattedKnobVal = formatParamForSet(newVal, ctx.meta);
    setSlotParam(ctx.slot, ctx.fullKey, formattedKnobVal);

    /* If a wav_position fullscreen editor is showing this param, keep its
     * stable edit value in sync — otherwise the renderer reads stale state
     * and the marker doesn't move when turning the knob. */
    if (hierEditorEditMode && hierEditorEditKey === ctx.fullKey) {
        hierEditorEditValue = formattedKnobVal;
    }

    /* Skip refreshHierarchyVisibility for float/int turns — it does IPC
     * (evaluateVisibilityCondition) and invalidates the context cache
     * (triggering 16+ IPC reads to rebuild). Only enum changes can affect
     * visibility; float/int knob turns never change which params are shown. */

    /* Show overlay directly — avoid showKnobOverlay which calls
     * isHierarchyParamModulated (1-3 blocking IPC reads). */
    const displayVal = formatParamForOverlay(newVal, ctx.meta);
    showKnobFeedback(knobIndex, ctx.title, displayVal, newVal, ctx.cardName);
    needsRedraw = true;
}

/* Format a value for display in hierarchy editor */
function formatHierDisplayValue(key, val) {
    const meta = getParamMetadata(key);

    /* For enums, always return the raw string value */
    if (meta && meta.type === "enum") {
        if (meta.picker_type && (val === "" || val === null || val === undefined)) {
            return meta.none_label || "(none)";
        }
        return formatMetaOptionValue(meta, val);
    }

    if (meta && meta.type === "filepath") {
        if (!val) return "";
        const slashIdx = val.lastIndexOf('/');
        return slashIdx >= 0 ? val.slice(slashIdx + 1) : val;
    }

    if (meta && meta.ui_type === "wav_position") {
        return formatWavPositionDisplayValue(val, meta);
    }

    if (meta && meta.type === "canvas" && meta.show_value === false) {
        return "";
    }
    if (meta && meta.type === "canvas") {
        return formatCanvasDisplayValue(val, meta);
    }

    if (meta && meta.type === "string") {
        return String(val || "");
    }

    const num = parseFloat(val);
    if (isNaN(num)) return val;

    /* Unit or display_format → unified formatter (handles "440.00 Hz", "5.0 ms", etc.) */
    if (meta && (meta.unit || meta.display_format)) {
        return ufFormatParamValue(num, meta);
    }

    /* Show as percentage for 0-1 float values */
    if (meta && meta.type === "float") {
        const min = typeof meta.min === "number" ? meta.min : 0;
        const max = typeof meta.max === "number" ? meta.max : 1;
        if (min === 0 && max === 1) {
            return Math.round(num * 100) + "%";
        }
    }
    /* For int or other types, show raw value */
    if (meta && meta.type === "int") {
        return Math.round(num).toString();
    }
    return num.toFixed(2);
}

function getSelectedHierarchyEditableKey() {
    if (!Array.isArray(hierEditorParams) || hierEditorParams.length === 0) return "";
    const selected = hierEditorParams[hierEditorSelectedIdx];
    if (!selected) return "";
    if (typeof selected === "string") return selected;
    if (typeof selected === "object" && selected.key) return selected.key;
    return "";
}

function getCachedWavDurationSec(filePath) {
    if (!filePath) return 0;
    if (Object.prototype.hasOwnProperty.call(wavDurationCache, filePath)) {
        return wavDurationCache[filePath];
    }
    const seconds = getWavDurationSec(filePath);
    wavDurationCache[filePath] = seconds;
    return seconds;
}

function normalizeWavPathString(path) {
    if (!path) return "";
    let value = String(path).trim();
    if (value.startsWith("file://")) value = value.slice("file://".length);
    if ((value.startsWith("\"") && value.endsWith("\"")) || (value.startsWith("'") && value.endsWith("'"))) {
        value = value.slice(1, -1).trim();
    }
    return value;
}

function wavPositionPathExists(path) {
    if (!path || typeof path !== "string") return false;
    try {
        const st = os.stat(path);
        return !!(st && st[1] === 0);
    } catch (e) {
        return false;
    }
}

function joinWavPath(base, leaf) {
    if (!base) return leaf || "";
    if (!leaf) return base;
    const b = String(base).replace(/\/+$/, "");
    const l = String(leaf).replace(/^\/+/, "");
    return `${b}/${l}`;
}

function getWavPositionSourcePath(meta) {
    const filepathParam = String(meta && meta.filepath_param || "").trim();
    if (!filepathParam) return "";

    const linkedKey = filepathParam.includes(":") ? filepathParam : buildHierarchyParamKey(filepathParam);
    const rawPath = normalizeWavPathString(getSlotParam(hierEditorSlot, linkedKey) || "");
    if (!rawPath) return "";
    if (rawPath.startsWith("/") && wavPositionPathExists(rawPath)) return rawPath;
    if (wavPositionPathExists(rawPath)) return rawPath;

    const lookupKey = filepathParam.includes(":")
        ? String(filepathParam.split(":").pop() || "").trim()
        : filepathParam;
    const sourceMeta = lookupKey ? (getParamMetadata(lookupKey) || {}) : {};
    const candidates = [];
    if (sourceMeta.start_path) candidates.push(joinWavPath(sourceMeta.start_path, rawPath));
    if (sourceMeta.root) candidates.push(joinWavPath(sourceMeta.root, rawPath));

    for (const candidate of candidates) {
        if (wavPositionPathExists(candidate)) return candidate;
    }
    return rawPath;
}

function getWavPositionSourcePathForLevel(meta, levelDef, childIndex) {
    const filepathParam = String(meta && meta.filepath_param || "").trim();
    if (!filepathParam) return "";

    const linkedKey = filepathParam.includes(":")
        ? filepathParam
        : buildHierarchyParamKeyForLevel(levelDef, filepathParam, childIndex);
    const rawPath = normalizeWavPathString(getSlotParam(hierEditorSlot, linkedKey) || "");
    if (!rawPath) return "";
    if (rawPath.startsWith("/") && wavPositionPathExists(rawPath)) return rawPath;
    if (wavPositionPathExists(rawPath)) return rawPath;

    const lookupKey = filepathParam.includes(":")
        ? String(filepathParam.split(":").pop() || "").trim()
        : filepathParam;
    const sourceMeta = lookupKey ? (getParamMetadata(lookupKey) || {}) : {};
    const candidates = [];
    if (sourceMeta.start_path) candidates.push(joinWavPath(sourceMeta.start_path, rawPath));
    if (sourceMeta.root) candidates.push(joinWavPath(sourceMeta.root, rawPath));

    for (const candidate of candidates) {
        if (wavPositionPathExists(candidate)) return candidate;
    }
    return rawPath;
}

function normalizeWavPositionRatio(rawValue, meta, durationSec) {
    const num = Number(rawValue);
    if (!Number.isFinite(num)) return 0;

    const unit = String(meta && meta.display_unit || "percent").toLowerCase();
    if (unit === "sec" || unit === "s") {
        if (!durationSec || durationSec <= 0) return 0;
        return Math.max(0, Math.min(1, num / durationSec));
    }
    if (unit === "ms") {
        if (!durationSec || durationSec <= 0) return 0;
        return Math.max(0, Math.min(1, (num / 1000) / durationSec));
    }

    const min = parseMetaNumber(meta && meta.min, 0);
    const max = parseMetaNumber(meta && meta.max, 1);
    const span = max - min;
    if (span <= 0) return 0;
    return Math.max(0, Math.min(1, (num - min) / span));
}

function getWavPositionDisplayText(rawValue, meta, durationSec) {
    const num = Number(rawValue);
    if (!Number.isFinite(num)) return String(rawValue || "");

    const unit = String(meta && meta.display_unit || "percent").toLowerCase();
    if (unit === "ms") return `${Math.round(num)} ms`;
    if (unit === "sec" || unit === "s") return `${num.toFixed(3)} s`;

    const ratio = normalizeWavPositionRatio(num, meta, durationSec);
    return `${(ratio * 100).toFixed(2)}%`;
}

function getWavPositionShiftMultiplier(meta) {
    const raw = parseMetaNumber(meta && meta.shift_increment_multiplier, 0.1);
    return (Number.isFinite(raw) && raw > 0) ? raw : 0.1;
}

function isEmptyParamValue(rawValue) {
    return rawValue === null || rawValue === undefined || String(rawValue).trim() === "";
}

function getWavPositionMode(meta) {
    return String(meta && meta.wav_mode || "position").toLowerCase();
}

function getWavPositionEndDefaultValue(meta, durationSec) {
    const unit = String(meta && meta.display_unit || "percent").toLowerCase();
    if ((unit === "sec" || unit === "s") && durationSec > 0) {
        return Number(durationSec).toFixed(3);
    }
    if (unit === "ms" && durationSec > 0) {
        return String(Math.round(durationSec * 1000));
    }

    if (unit === "sec" || unit === "s") {
        const fallback = parseMetaNumber(meta && meta.max, 1);
        return Number(fallback).toFixed(3);
    }
    if (unit === "ms") {
        const fallback = parseMetaNumber(meta && meta.max, 1);
        return String(Math.round(fallback));
    }

    const min = parseMetaNumber(meta && meta.min, 0);
    const max = parseMetaNumber(meta && meta.max, 1);
    return String(Math.max(min, max));
}

function wavPositionGetBaseName(path) {
    if (!path) return "";
    const idx = path.lastIndexOf("/");
    return idx >= 0 ? path.slice(idx + 1) : path;
}

function wavContentToBytes(content) {
    if (!content) return null;
    try {
        if (content instanceof ArrayBuffer) {
            return new Uint8Array(content);
        }
        if (typeof ArrayBuffer !== "undefined" &&
            typeof ArrayBuffer.isView === "function" &&
            ArrayBuffer.isView(content)) {
            return new Uint8Array(content.buffer, content.byteOffset || 0, content.byteLength || 0);
        }
    } catch (e) {
        /* Fall through to string handling. */
    }

    if (typeof content === "string") {
        const bytes = new Uint8Array(content.length);
        for (let i = 0; i < content.length; i++) {
            bytes[i] = content.charCodeAt(i) & 0xff;
        }
        return bytes;
    }
    return null;
}

function wavByteAt(bytes, idx) {
    if (!bytes || idx < 0 || idx >= bytes.length) return 0;
    return bytes[idx] & 0xff;
}

function wavReadChunkId(bytes, idx) {
    return String.fromCharCode(
        wavByteAt(bytes, idx),
        wavByteAt(bytes, idx + 1),
        wavByteAt(bytes, idx + 2),
        wavByteAt(bytes, idx + 3)
    );
}

function wavReadU16LE(bytes, idx) {
    return wavByteAt(bytes, idx) | (wavByteAt(bytes, idx + 1) << 8);
}

function wavReadS16LE(bytes, idx) {
    const v = wavReadU16LE(bytes, idx);
    return v > 0x7fff ? v - 0x10000 : v;
}

function wavReadU32LE(bytes, idx) {
    return (wavByteAt(bytes, idx) |
        (wavByteAt(bytes, idx + 1) << 8) |
        (wavByteAt(bytes, idx + 2) << 16) |
        (wavByteAt(bytes, idx + 3) << 24)) >>> 0;
}

function wavReadF32LE(bytes, idx) {
    if (!bytes || idx < 0 || idx + 4 > bytes.length) return 0;
    try {
        const view = new DataView(bytes.buffer, bytes.byteOffset || 0, bytes.byteLength || bytes.length);
        return view.getFloat32(idx, true);
    } catch (e) {
        return 0;
    }
}

function wavFindRiffOffset(bytes) {
    if (!bytes || bytes.length < 12) return -1;
    if (wavReadChunkId(bytes, 0) === "RIFF" && wavReadChunkId(bytes, 8) === "WAVE") return 0;

    const limit = Math.min(bytes.length - 12, 4096);
    for (let i = 0; i <= limit; i++) {
        if (wavReadChunkId(bytes, i) === "RIFF" && wavReadChunkId(bytes, i + 8) === "WAVE") {
            return i;
        }
    }
    return -1;
}

function parseWavPositionPeaks(content, width) {
    const bytes = wavContentToBytes(content);
    if (!bytes || bytes.length < 44) return { error: "file too small", points: [] };

    const riffOffset = wavFindRiffOffset(bytes);
    if (riffOffset < 0) return { error: "not a wav file", points: [] };

    let fmtOffset = -1;
    let dataOffset = -1;
    let dataSize = 0;
    let cursor = riffOffset + 12;

    while (cursor + 8 <= bytes.length) {
        const chunkId = wavReadChunkId(bytes, cursor);
        const chunkSize = wavReadU32LE(bytes, cursor + 4);
        const chunkData = cursor + 8;
        const chunkEnd = chunkData + chunkSize;
        const available = Math.max(0, bytes.length - chunkData);

        if (chunkId === "fmt " && available >= 16) {
            fmtOffset = chunkData;
        } else if (chunkId === "data") {
            dataOffset = chunkData;
            dataSize = Math.min(chunkSize, available);
            break;
        }

        if (chunkEnd <= chunkData || chunkEnd > bytes.length) break;
        cursor = chunkEnd + (chunkSize % 2);
    }

    if (fmtOffset < 0 || dataOffset < 0 || dataSize <= 0) {
        return { error: "missing wav chunks", points: [] };
    }

    const audioFmt = wavReadU16LE(bytes, fmtOffset);
    const channels = Math.max(1, wavReadU16LE(bytes, fmtOffset + 2));
    const bits = wavReadU16LE(bytes, fmtOffset + 14);
    const blockAlign = Math.max(1, wavReadU16LE(bytes, fmtOffset + 12));
    if (audioFmt !== 1 && audioFmt !== 3) return { error: "unsupported wav codec", points: [] };
    if (!((audioFmt === 1 && (bits === 8 || bits === 16)) || (audioFmt === 3 && bits === 32))) {
        return { error: "unsupported wav format", points: [] };
    }

    const sampleBytes = bits / 8;
    const effectiveBlockAlign = blockAlign > 0 ? blockAlign : Math.max(1, channels * sampleBytes);
    const frameCount = Math.max(1, Math.floor(dataSize / effectiveBlockAlign));
    const points = new Array(width).fill(0);
    const dataEnd = dataOffset + dataSize;

    for (let x = 0; x < width; x++) {
        const start = Math.floor((x * frameCount) / width);
        const end = Math.max(start + 1, Math.floor(((x + 1) * frameCount) / width));
        const span = end - start;
        const stride = Math.max(1, Math.floor(span / 32));
        let maxAbs = 0;

        for (let frame = start; frame < end; frame += stride) {
            const base = dataOffset + frame * effectiveBlockAlign;
            if (base + sampleBytes > dataEnd) break;
            let sample = 0;
            if (audioFmt === 1 && bits === 16) {
                sample = wavReadS16LE(bytes, base) / 32768;
            } else if (audioFmt === 1 && bits === 8) {
                sample = (wavByteAt(bytes, base) - 128) / 128;
            } else if (audioFmt === 3 && bits === 32) {
                sample = wavReadF32LE(bytes, base);
            }
            const abs = Math.abs(sample);
            if (abs > maxAbs) maxAbs = abs;
        }

        points[x] = Math.max(0, Math.min(1, maxAbs));
    }

    return { error: "", points };
}

function getWavPositionWaveformPreview(path, width) {
    if (!path) {
        wavPositionWaveformCache = { signature: "", path: "", points: [], error: "select a wav file" };
        return wavPositionWaveformCache;
    }

    let signature = `${path}:${width}`;
    try {
        const st = os.stat(path);
        if (st && st[1] === 0 && st[0]) {
            const size = st[0].size || 0;
            const mtime = st[0].mtime || 0;
            signature = `${path}:${size}:${mtime}:${width}`;
        }
    } catch (e) {
        wavPositionWaveformCache = { signature: "", path, points: [], error: "file not found" };
        return wavPositionWaveformCache;
    }

    if (wavPositionWaveformCache.signature === signature) {
        return wavPositionWaveformCache;
    }

    let content = null;
    const readBinaryWithStdOpen = function(filePath) {
        let file = null;
        try {
            const st = os.stat(filePath);
            if (!st || st[1] !== 0 || !st[0]) return null;
            const size = Number(st[0].size || 0);
            if (!(size > 0)) return null;

            file = std.open(filePath, "rb");
            if (!file) return null;

            const buf = new ArrayBuffer(size);
            let total = 0;
            while (total < size) {
                const n = file.read(buf, total, size - total);
                if (!(n > 0)) break;
                total += n;
            }
            file.close();
            file = null;

            if (total <= 0) return null;
            const full = new Uint8Array(buf);
            if (total === size) return full;
            const out = new Uint8Array(total);
            out.set(full.subarray(0, total));
            return out;
        } catch (e) {
            if (file) {
                try { file.close(); } catch (closeErr) {}
            }
            return null;
        }
    };

    try {
        content = readBinaryWithStdOpen(path);
        if (!content) {
            content = std.loadFile(path, "binary");
            if (!content) {
                content = std.loadFile(path);
            }
        }
    } catch (e) {
        content = null;
    }

    if (!content) {
        wavPositionWaveformCache = { signature, path, points: [], error: "unable to read file" };
        return wavPositionWaveformCache;
    }

    const parsed = parseWavPositionPeaks(content, width);
    if (parsed.error) {
        const sig = `${path}|${parsed.error}`;
        if (wavPositionWaveformErrorSignature !== sig) {
            wavPositionWaveformErrorSignature = sig;
            debugLog(`wav_position parse error: ${parsed.error} path=${path}`);
        }
    } else {
        wavPositionWaveformErrorSignature = "";
    }

    wavPositionWaveformCache = {
        signature,
        path,
        points: parsed.points || [],
        error: parsed.error || ""
    };
    return wavPositionWaveformCache;
}

function sampleWavPointAt(points, idxF) {
    if (!Array.isArray(points) || points.length === 0) return 0;
    const len = points.length;
    const clamped = Math.max(0, Math.min(len - 1, idxF));
    const i0 = Math.floor(clamped);
    const i1 = Math.min(len - 1, i0 + 1);
    const frac = clamped - i0;
    const a = points[i0] || 0;
    const b = points[i1] || a;
    return (a * (1 - frac)) + (b * frac);
}

function sampleWavPointRange(points, startNorm, endNorm) {
    if (!Array.isArray(points) || points.length === 0) return 0;
    const len = points.length;
    const start = Math.max(0, Math.min(1, startNorm));
    const end = Math.max(start, Math.min(1, endNorm));
    const startF = start * (len - 1);
    const endF = end * (len - 1);
    const span = Math.max(0, endF - startF);

    if (span < 0.5) {
        return sampleWavPointAt(points, (startF + endF) * 0.5);
    }

    const steps = Math.min(32, Math.max(8, Math.ceil(span)));
    let maxAmp = 0;
    for (let i = 0; i <= steps; i++) {
        const t = i / steps;
        const amp = sampleWavPointAt(points, startF + span * t);
        if (amp > maxAmp) maxAmp = amp;
    }
    return Math.max(0, Math.min(1, maxAmp));
}

function getWavPositionPreviewData(fullKey, meta) {
    let value = (hierEditorEditMode && hierEditorEditKey === fullKey && hierEditorEditValue !== null)
        ? String(hierEditorEditValue)
        : (getSlotParam(hierEditorSlot, fullKey) || "");
    const mode = getWavPositionMode(meta);
    const wavPath = getWavPositionSourcePath(meta);
    let durationSec = 0;
    if (wavPath && wavPositionPathExists(wavPath)) {
        durationSec = getCachedWavDurationSec(wavPath);
    }

    if (!wavPath) {
        return {
            ok: false,
            ratio: normalizeWavPositionRatio(value, meta, 0),
            reason: "No file",
            value
        };
    }

    if (!wavPositionPathExists(wavPath)) {
        return {
            ok: false,
            ratio: normalizeWavPositionRatio(value, meta, 0),
            reason: "File missing",
            path: wavPath,
            value
        };
    }

    return {
        ok: true,
        ratio: normalizeWavPositionRatio(value, meta, durationSec),
        durationSec,
        path: wavPath,
        mode,
        value
    };
}

function applyLinkedWavEndDefaultsForFilepath(filepathKey) {
    const targetKey = String(filepathKey || "").trim();
    if (!targetKey || hierEditorSlot < 0) return false;
    const targetFullKey = buildHierarchyParamKey(targetKey);
    const levels = hierEditorHierarchy && hierEditorHierarchy.levels && typeof hierEditorHierarchy.levels === "object"
        ? Object.values(hierEditorHierarchy.levels)
        : [];
    if (levels.length === 0) return false;

    const chainMetaByKey = new Map();
    if (Array.isArray(hierEditorChainParams)) {
        for (const p of hierEditorChainParams) {
            if (p && p.key) chainMetaByKey.set(p.key, p);
        }
    }

    const selectedChildIndex = hierEditorChildIndex >= 0 ? hierEditorChildIndex : -1;
    const seen = new Set();
    let changed = false;

    for (const levelDef of levels) {
        if (!levelDef || !Array.isArray(levelDef.params)) continue;
        const hasChildren = !!(levelDef.child_prefix && typeof levelDef.child_prefix === "string");
        const childIndices = hasChildren && selectedChildIndex >= 0 ? [selectedChildIndex] : [-1];

        for (const entry of levelDef.params) {
            const wavKey = (typeof entry === "string")
                ? entry
                : (entry && typeof entry === "object" ? entry.key : "");
            if (!wavKey || wavKey === targetKey) continue;

            const chainMeta = chainMetaByKey.get(wavKey) || null;
            const levelMeta = (entry && typeof entry === "object") ? entry : null;
            const mergedMeta = chainMeta && levelMeta
                ? { ...levelMeta, ...chainMeta }
                : (chainMeta || levelMeta);
            const wavMeta = normalizeExpandedParamMeta(wavKey, mergedMeta);
            if (!wavMeta || wavMeta.ui_type !== "wav_position") continue;
            if (getWavPositionMode(wavMeta) !== "end") continue;

            const linked = String(wavMeta.filepath_param || "").trim();
            if (!linked) continue;

            for (const childIndex of childIndices) {
                const fullWavKey = buildHierarchyParamKeyForLevel(levelDef, wavKey, childIndex);
                if (seen.has(fullWavKey)) continue;
                seen.add(fullWavKey);

                const linkedFull = linked.includes(":")
                    ? linked
                    : buildHierarchyParamKeyForLevel(levelDef, linked, childIndex);
                const linkedSimple = linked.includes(":")
                    ? String(linked.split(":").pop() || "").trim()
                    : linked;
                if (linkedSimple !== targetKey && linkedFull !== targetFullKey) continue;

                const current = getSlotParam(hierEditorSlot, fullWavKey);
                if (!isEmptyParamValue(current)) continue;

                const wavPath = getWavPositionSourcePathForLevel(wavMeta, levelDef, childIndex);
                const durationSec = (wavPath && wavPositionPathExists(wavPath))
                    ? getCachedWavDurationSec(wavPath)
                    : 0;
                const endDefault = getWavPositionEndDefaultValue(wavMeta, durationSec);
                setSlotParam(hierEditorSlot, fullWavKey, String(endDefault));
                if (hierEditorEditMode && hierEditorEditKey === fullWavKey) {
                    hierEditorEditValue = String(endDefault);
                }
                changed = true;
            }
        }
    }

    return changed;
}

function drawWavPositionEditor(selectedKey, selectedMeta) {
    clear_screen();
    hideOverlay();

    const fullKey = buildHierarchyParamKey(selectedKey);
    const preview = getWavPositionPreviewData(fullKey, selectedMeta);
    const ratio = Math.max(0, Math.min(1, Number(preview.ratio) || 0));
    const shiftHeld = isShiftHeld();
    const enableZoom = !!(selectedMeta && selectedMeta.enable_zoom);
    const zoomLevel = enableZoom ? getWavZoomLevel(hierEditorSlot, selectedMeta, fullKey) : 0;
    /* Two regimes:
     *   enable_zoom + state-driven sticky zoom (zoomLevel > 0 → window = 1/2^z)
     *   legacy: shift-held preview zoom (10% window) when no sticky state
     */
    let zoomWindow;
    if (zoomLevel > 0) {
        zoomWindow = 1 / Math.pow(2, zoomLevel);
    } else {
        zoomWindow = shiftHeld ? 0.1 : 1.0;
    }
    /* Center viewport on the active marker (= currently edited param). */
    const zoomStart = Math.max(0, Math.min(1 - zoomWindow, ratio - (zoomWindow / 2)));
    const zoomEnd = zoomStart + zoomWindow;
    const zoomRange = Math.max(0.000001, zoomEnd - zoomStart);

    const plotX = 3;
    const plotY = 12;
    const plotW = 122;
    const plotH = 40;
    const innerW = plotW - 2;
    const midY = plotY + Math.floor(plotH / 2);
    const cursorNorm = Math.max(0, Math.min(1, (ratio - zoomStart) / zoomRange));
    const cursorX = plotX + 1 + Math.round(cursorNorm * (innerW - 1));

    const label = selectedMeta && (selectedMeta.label || selectedMeta.name)
        ? String(selectedMeta.label || selectedMeta.name)
        : selectedKey;
    let suffix = "";
    if (zoomLevel > 0.01) {
        const factor = Math.pow(2, zoomLevel);
        suffix = ` ${factor.toFixed(factor < 10 ? 1 : 0)}x`;
    } else if (shiftHeld) {
        suffix = " [fine]";
    }
    const valueText = `${getWavPositionDisplayText(preview.value, selectedMeta, preview.durationSec)}${suffix}`;
    const sourceText = wavPositionGetBaseName(preview.path || "") || "(no file)";

    /* Multi-marker overlay support: when this wav_position is part of a
     * view_group, collect all sibling wav_position members in the same
     * group so we can draw their markers on the same waveform. */
    const groupMembers = (selectedMeta && selectedMeta.view_group)
        ? getWavViewGroupMembers(selectedMeta.view_group)
        : [];
    const isMultiMarker = groupMembers.length > 1;

    print(Math.max(0, Math.floor((SCREEN_WIDTH - label.length * 5) / 2)), 2, truncateText(label, 24), 1);
    draw_rect(plotX, plotY, plotW, plotH, 1);

    if (!preview.ok) {
        const msg = preview.reason || "No WAV";
        print(Math.max(0, Math.floor((SCREEN_WIDTH - msg.length * 5) / 2)), 30, truncateText(msg, 24), 1);
    } else {
        const previewWidth = Math.max(1024, innerW * (shiftHeld ? 24 : 8));
        const waveform = getWavPositionWaveformPreview(preview.path, previewWidth);
        if (waveform.error || waveform.points.length === 0) {
            const msg = waveform.error || "no waveform";
            print(Math.max(0, Math.floor((SCREEN_WIDTH - msg.length * 5) / 2)), 30, truncateText(msg, 24), 1);
        } else {
            const points = waveform.points;
            const mode = preview.mode === "start" || preview.mode === "end"
                ? preview.mode
                : "position";
            /* In multi-marker mode the outline-on-one-side shading is
             * misleading because there are several reference points;
             * fall back to plain filled waveform. */
            const drawOutline = !isMultiMarker;
            for (let i = 0; i < innerW; i++) {
                const colStartNorm = zoomStart + ((i / innerW) * zoomRange);
                const colEndNorm = zoomStart + (((i + 1) / innerW) * zoomRange);
                const amp = sampleWavPointRange(points, colStartNorm, colEndNorm);
                const half = Math.floor(amp * (plotH - 4) / 2);
                if (half <= 0) continue;
                const x = plotX + 1 + i;
                const top = Math.max(plotY + 1, midY - half);
                const bottom = Math.min(plotY + plotH - 2, midY + half);
                let outline = false;
                if (drawOutline) {
                    if (mode === "start" && x <= cursorX) outline = true;
                    if (mode === "end" && x >= cursorX) outline = true;
                }

                if (!outline) {
                    for (let y = top; y <= bottom; y++) {
                        set_pixel(x, y, 1);
                    }
                } else {
                    /* Start/end modes render an envelope-style outline only. */
                    set_pixel(x, top, 1);
                    set_pixel(x, bottom, 1);
                }
            }
        }
    }

    if (isMultiMarker) {
        /* Draw all sibling markers; active = solid line, others = dashed. */
        for (const m of groupMembers) {
            const mPreview = getWavPositionPreviewData(m.fullKey, m.meta);
            const mRatio = Math.max(0, Math.min(1, Number(mPreview.ratio) || 0));
            const isActive = (m.fullKey === fullKey);
            let mX;
            let offscreen = 0;  /* -1 = left, +1 = right, 0 = in view */
            if (mRatio < zoomStart) {
                mX = plotX + 1;
                offscreen = -1;
            } else if (mRatio > zoomEnd) {
                mX = plotX + innerW;
                offscreen = 1;
            } else {
                const norm = (mRatio - zoomStart) / zoomRange;
                mX = plotX + 1 + Math.round(norm * (innerW - 1));
            }
            /* Marker line: active solid every pixel, others every other pixel */
            for (let y = plotY + 1; y < plotY + plotH - 1; y++) {
                if (isActive || ((y & 1) === 0)) {
                    set_pixel(mX, y, 1);
                }
            }
            /* Tiny label above plot. */
            const lbl = (m.meta && m.meta.marker_label) ? m.meta.marker_label
                       : (m.meta && m.meta.name ? String(m.meta.name).slice(0, 2) : String(m.key).slice(0, 2));
            const lblX = Math.max(0, Math.min(SCREEN_WIDTH - lbl.length * 5, mX - Math.floor(lbl.length * 5 / 2)));
            print(lblX, plotY - 8, lbl, 1);
            /* Offscreen arrow at clamped edge. */
            if (offscreen !== 0) {
                const arrow = offscreen < 0 ? "<" : ">";
                print(mX - (offscreen < 0 ? 0 : 4), midY - 3, arrow, 1);
            }
        }
    } else {
        /* Single-marker legacy: just the active cursor. */
        for (let y = plotY + 1; y < plotY + plotH - 1; y++) {
            set_pixel(cursorX, y, 1);
        }
    }

    drawFooter([
        truncateText(valueText, 20),
        truncateText(sourceText, 12)
    ]);
}

function resetCanvasState() {
    canvasParamKey = "";
    canvasParamMeta = null;
    canvasRuntime = null;
    canvasTickCounter = 0;
}

function moduleFileExists(path) {
    if (!path || typeof path !== "string") return false;
    try {
        const st = os.stat(path);
        return !!(st && st[1] === 0);
    } catch (e) {
        return false;
    }
}

function getHierarchyActiveModuleId() {
    if (hierEditorSlot < 0 || !hierEditorComponent) return "";
    if (hierEditorIsMasterFx) {
        return getSlotParam(0, `${hierEditorComponent}:module`) || "";
    }

    const prefix = getComponentParamPrefix(hierEditorComponent);
    if (!prefix) return "";
    return getSlotParam(hierEditorSlot, `${prefix}_module`) || "";
}

function getModuleBasePath(moduleId) {
    if (!moduleId) return "";
    const searchDirs = [
        `${MODULES_ROOT}/${moduleId}`,
        `${MODULES_ROOT}/sound_generators/${moduleId}`,
        `${MODULES_ROOT}/audio_fx/${moduleId}`,
        `${MODULES_ROOT}/midi_fx/${moduleId}`,
        `${MODULES_ROOT}/utilities/${moduleId}`,
        `${MODULES_ROOT}/tools/${moduleId}`,
        `${MODULES_ROOT}/other/${moduleId}`
    ];
    for (const dir of searchDirs) {
        if (moduleFileExists(`${dir}/module.json`)) return dir;
    }
    return "";
}

function parseOverlayScriptSpec(value, fallbackScript) {
    const fallback = (typeof fallbackScript === "string" && fallbackScript.trim())
        ? fallbackScript.trim()
        : "";
    const raw = (typeof value === "string" && value.trim())
        ? value.trim()
        : fallback;
    if (!raw) return { scriptRef: "", overlayRef: "" };

    const hashPos = raw.indexOf("#");
    if (hashPos < 0) return { scriptRef: raw, overlayRef: "" };

    const scriptRef = raw.slice(0, hashPos).trim() || fallback;
    const overlayRef = raw.slice(hashPos + 1).trim();
    return { scriptRef, overlayRef };
}

function getObjectPathValue(root, pathRef) {
    if (!root || !pathRef || typeof pathRef !== "string") return undefined;
    const parts = pathRef.split(".").map((part) => part.trim()).filter(Boolean);
    if (parts.length === 0) return undefined;

    let cur = root;
    for (const part of parts) {
        if (!cur || (typeof cur !== "object" && typeof cur !== "function")) return undefined;
        cur = cur[part];
    }
    return cur;
}

function resolveOverlayObject(candidate) {
    if (!candidate) return { overlay: null, error: "" };
    if (typeof candidate === "function") {
        try {
            const built = candidate();
            if (built && typeof built === "object") return { overlay: built, error: "" };
            return { overlay: null, error: "overlay factory returned invalid value" };
        } catch (e) {
            return { overlay: null, error: String(e) };
        }
    }
    if (candidate && typeof candidate === "object") return { overlay: candidate, error: "" };
    return { overlay: null, error: "" };
}

function resolveOverlayFromGlobals(overlayRef, fallbackCandidates, missingMessage) {
    const seen = new Set();
    const candidates = [];
    let firstError = "";

    const pushCandidate = function(value) {
        if (!value || seen.has(value)) return;
        seen.add(value);
        candidates.push(value);
    };

    if (overlayRef && typeof overlayRef === "string" && overlayRef.trim()) {
        const key = overlayRef.trim();
        pushCandidate(getObjectPathValue(globalThis, key));
        pushCandidate(globalThis[key]);
        pushCandidate(getObjectPathValue(globalThis.canvas_overlays, key));
    }

    if (Array.isArray(fallbackCandidates)) {
        for (const candidate of fallbackCandidates) pushCandidate(candidate);
    }

    for (const candidate of candidates) {
        const resolved = resolveOverlayObject(candidate);
        if (resolved.overlay) return { overlay: resolved.overlay, error: "" };
        if (!firstError && resolved.error) firstError = resolved.error;
    }

    if (firstError) return { overlay: null, error: firstError };
    if (overlayRef && typeof overlayRef === "string" && overlayRef.trim()) {
        return { overlay: null, error: `overlay not found: ${overlayRef.trim()}` };
    }
    return { overlay: null, error: missingMessage };
}

function resolveCanvasScriptPath(meta) {
    const moduleId = getHierarchyActiveModuleId();
    const moduleDir = getModuleBasePath(moduleId);
    if (!moduleDir) return { scriptPath: "", overlayRef: "" };

    let scriptSpec = "canvas.js";
    let overlayRef = "";
    if (meta && meta.canvas_script !== undefined) {
        if (typeof meta.canvas_script === "string") {
            scriptSpec = meta.canvas_script.trim() || "canvas.js";
        } else if (meta.canvas_script && typeof meta.canvas_script === "object") {
            scriptSpec = String(meta.canvas_script.script || meta.canvas_script.file || meta.canvas_script.path || "canvas.js");
            if (typeof meta.canvas_script.overlay === "string" && meta.canvas_script.overlay.trim()) {
                overlayRef = meta.canvas_script.overlay.trim();
            } else if (typeof meta.canvas_script.target === "string" && meta.canvas_script.target.trim()) {
                overlayRef = meta.canvas_script.target.trim();
            } else if (typeof meta.canvas_script.entry === "string" && meta.canvas_script.entry.trim()) {
                overlayRef = meta.canvas_script.entry.trim();
            } else if (typeof meta.canvas_script.element === "string" && meta.canvas_script.element.trim()) {
                overlayRef = meta.canvas_script.element.trim();
            }
        }
    }

    if (meta && typeof meta.canvas_overlay === "string" && meta.canvas_overlay.trim()) {
        overlayRef = meta.canvas_overlay.trim();
    } else if (meta && typeof meta.overlay === "string" && meta.overlay.trim()) {
        overlayRef = meta.overlay.trim();
    } else if (meta && typeof meta.canvas_target === "string" && meta.canvas_target.trim()) {
        overlayRef = meta.canvas_target.trim();
    }

    const parsedSpec = parseOverlayScriptSpec(scriptSpec, "canvas.js");
    const scriptRef = parsedSpec.scriptRef;
    if (!overlayRef && parsedSpec.overlayRef) overlayRef = parsedSpec.overlayRef;

    const scriptPath = scriptRef.startsWith("/") ? scriptRef : `${moduleDir}/${scriptRef}`;
    return {
        scriptPath: moduleFileExists(scriptPath) ? scriptPath : "",
        overlayRef
    };
}

function loadCanvasOverlayScript(scriptPath, overlayRef) {
    if (!scriptPath || typeof shadow_load_ui_module !== "function") {
        return { overlay: null, error: "canvas script unavailable" };
    }

    const savedInit = globalThis.init;
    const savedTick = globalThis.tick;
    const savedMidiInternal = globalThis.onMidiMessageInternal;
    const savedMidiExternal = globalThis.onMidiMessageExternal;
    const hadCanvasOverlay = Object.prototype.hasOwnProperty.call(globalThis, "canvas_overlay");
    const hadCanvasOverlays = Object.prototype.hasOwnProperty.call(globalThis, "canvas_overlays");
    const savedCanvasOverlay = globalThis.canvas_overlay;
    const savedCanvasOverlays = globalThis.canvas_overlays;

    let ok = false;
    let loadError = "";
    try {
        ok = shadow_load_ui_module(scriptPath);
    } catch (e) {
        ok = false;
        loadError = String(e);
    }

    const resolved = resolveOverlayFromGlobals(
        overlayRef,
        [globalThis.canvas_overlay],
        "canvas script missing globalThis.canvas_overlay"
    );

    globalThis.init = savedInit;
    globalThis.tick = savedTick;
    globalThis.onMidiMessageInternal = savedMidiInternal;
    globalThis.onMidiMessageExternal = savedMidiExternal;

    if (hadCanvasOverlay) globalThis.canvas_overlay = savedCanvasOverlay;
    else delete globalThis.canvas_overlay;

    if (hadCanvasOverlays) globalThis.canvas_overlays = savedCanvasOverlays;
    else delete globalThis.canvas_overlays;

    if (!ok) return { overlay: null, error: loadError || "failed to load canvas script" };
    if (!resolved.overlay) return { overlay: null, error: resolved.error || "canvas script missing overlay object" };
    return { overlay: resolved.overlay, error: "" };
}

function createCanvasRuntimeContext() {
    const fullCanvasKey = canvasParamKey ? buildHierarchyParamKey(canvasParamKey) : "";
    const prefix = getComponentParamPrefix(hierEditorComponent);
    const toFullParam = function(key) {
        if (!key || typeof key !== "string") return "";
        if (key.includes(":")) return key;
        return prefix ? `${prefix}:${key}` : key;
    };

    return {
        width: SCREEN_WIDTH,
        height: SCREEN_HEIGHT,
        state: canvasRuntime ? canvasRuntime.state : {},
        clear() { clear_screen(); },
        setPixel(x, y, value) { set_pixel(Math.round(x), Math.round(y), value ? 1 : 0); },
        drawRect(x, y, w, h, value) { draw_rect(Math.round(x), Math.round(y), Math.round(w), Math.round(h), value ? 1 : 0); },
        fillRect(x, y, w, h, value) { fill_rect(Math.round(x), Math.round(y), Math.round(w), Math.round(h), value ? 1 : 0); },
        drawLine(x1, y1, x2, y2, value) {
            if (display && typeof display.drawLine === "function") {
                display.drawLine(Math.round(x1), Math.round(y1), Math.round(x2), Math.round(y2), value ? 1 : 0);
            }
        },
        print(x, y, text, color = 1) { print(Math.round(x), Math.round(y), String(text), color ? 1 : 0); },
        now() { return Date.now(); },
        random() { return Math.random(); },
        getValue() {
            if (!fullCanvasKey) return "";
            return getSlotParam(hierEditorSlot, fullCanvasKey) || "";
        },
        setValue(value) {
            if (!fullCanvasKey) return false;
            return setSlotParam(hierEditorSlot, fullCanvasKey, String(value));
        },
        getParam(key) {
            const full = toFullParam(key);
            if (!full) return null;
            return getSlotParam(hierEditorSlot, full);
        },
        setParam(key, value) {
            const full = toFullParam(key);
            if (!full) return false;
            return setSlotParam(hierEditorSlot, full, String(value));
        },
        sourcePath() {
            return canvasRuntime ? (canvasRuntime.scriptPath || "") : "";
        }
    };
}

function invokeCanvasOverlayHook(hookName, payload) {
    if (!canvasRuntime || !canvasRuntime.overlay) return false;
    const fn = canvasRuntime.overlay[hookName];
    if (typeof fn !== "function") return false;
    try {
        fn(canvasRuntime.ctx, payload || {});
    } catch (e) {
        canvasRuntime.error = `${hookName} error: ${e}`;
        debugLog(`canvas ${hookName} hook error: ${e}`);
    }
    return true;
}

function dispatchCanvasMidi(data, source) {
    /* Also fire when the canvas is the active co-run overlay (outer view is
     * OVERTAKE_MODULE then). Runs before the co-run input block, so jog-turn /
     * encoders / knob-touch reach the canvas onMidi instead of the tool. */
    const canvasCorun = coRunUiActive() && coRunView === VIEWS.CANVAS;
    if (view !== VIEWS.CANVAS && !canvasCorun) return false;
    const midi = Array.isArray(data) ? data.slice() : Array.from(data || []);
    if (!midi || midi.length === 0) return true;

    /* In co-run the canvas is an overlay over a STILL-RUNNING tool. Consume only
     * the events the module's co-run spec routes to the peer (this shadow_ui
     * side); tool-kept and unclassified events must fall through so the buttons
     * available before the canvas opened (pads / steps / transport / ...) keep
     * working. Single source of truth = the host's corun_event_owner, so the
     * keep/cede spec and the legacy carve-out are honored without duplicating
     * the logic here. (Jog-click / Back are the close gesture, stolen before
     * this. A full-screen canvas — not co-run — still owns the whole surface.) */
    if (canvasCorun && typeof shadow_corun_event_owner === "function") {
        if (shadow_corun_event_owner(midi[0] | 0, midi[1] | 0) !== CORUN_OWNER_PEER) return false;
    }

    invokeCanvasOverlayHook("onMidi", { source, data: midi });
    return true;
}

function openCanvasPreview(paramKey, meta) {
    resetCanvasState();
    canvasParamKey = paramKey || "";
    canvasParamMeta = meta || null;
    canvasRuntime = {
        moduleId: getHierarchyActiveModuleId(),
        scriptPath: "",
        overlayRef: "",
        overlay: null,
        state: {},
        ctx: null,
        error: ""
    };

    const scriptSpec = resolveCanvasScriptPath(meta);
    canvasRuntime.scriptPath = scriptSpec.scriptPath;
    canvasRuntime.overlayRef = scriptSpec.overlayRef || "";
    if (canvasRuntime.scriptPath) {
        const loaded = loadCanvasOverlayScript(canvasRuntime.scriptPath, canvasRuntime.overlayRef);
        canvasRuntime.overlay = loaded.overlay;
        canvasRuntime.error = loaded.error || "";
    } else {
        canvasRuntime.error = "No canvas script found";
    }

    canvasRuntime.ctx = createCanvasRuntimeContext();
    invokeCanvasOverlayHook("onOpen", {
        param_key: canvasParamKey,
        module_id: canvasRuntime.moduleId,
        script_path: canvasRuntime.scriptPath,
        overlay_ref: canvasRuntime.overlayRef
    });

    setView(VIEWS.CANVAS);
    hideOverlay();
    const label = meta && (meta.label || meta.name) ? (meta.label || meta.name) : "Canvas";
    announce(`${label} canvas`);
    needsRedraw = true;
}

function closeCanvasPreview(cancelled) {
    invokeCanvasOverlayHook("onClose", { cancelled: !!cancelled });
    invokeCanvasOverlayHook("onExit", { cancelled: !!cancelled });
    resetCanvasState();
    setView(VIEWS.HIERARCHY_EDITOR);
    needsRedraw = true;
}

function tickCanvasPreview() {
    if (view !== VIEWS.CANVAS) return;
    invokeCanvasOverlayHook("tick", {});
}

function drawCanvasPreview() {
    clear_screen();
    const drew = invokeCanvasOverlayHook("draw", {});
    const title = canvasParamMeta && (canvasParamMeta.label || canvasParamMeta.name)
        ? String(canvasParamMeta.label || canvasParamMeta.name)
        : "Canvas";

    if (!drew) {
        const message = canvasRuntime && canvasRuntime.error ? canvasRuntime.error : "No module canvas overlay";
        print(Math.max(0, Math.floor((SCREEN_WIDTH - title.length * 5) / 2)), 10, truncateText(title, 24), 1);
        print(3, 29, truncateText(message, 24), 1);
        print(3, 50, "Click/Back: return", 1);
    }

    const showCanvasValue = !canvasParamMeta || canvasParamMeta.show_value !== false;
    let valueText = showCanvasValue ? "-" : "";
    if (showCanvasValue && canvasParamKey) {
        const fullKey = buildHierarchyParamKey(canvasParamKey);
        const raw = getSlotParam(hierEditorSlot, fullKey);
        if (raw !== null && raw !== undefined && raw !== "") {
            valueText = formatHierDisplayValue(canvasParamKey, raw);
        }
    }
    if (!canvasParamMeta || canvasParamMeta.show_footer !== false) {
        drawFooter([
            truncateText(String(valueText || "-"), 20),
            truncateText(title, 12)
        ]);
    }
}

/* Draw filepath browser for filepath chain params */
function drawFilepathBrowser() {
    clear_screen();

    const state = filepathBrowserState;
    const title = state && state.title ? state.title : "File Browser";
    drawHeader(truncateText(title, 24));

    if (!state) {
        print(4, LIST_TOP_Y, "Browser unavailable", 1);
        drawFooter(["Click: return", "Jog: scroll"]);
        return;
    }

    if (state.error) {
        print(4, LIST_TOP_Y, truncateText(state.error, 20), 1);
    }

    if (!state.items || state.items.length === 0) {
        print(4, LIST_TOP_Y + 10, "No files", 1);
    } else {
        drawMenuList({
            items: state.items,
            selectedIndex: state.selectedIndex,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => item.label,
            getValue: () => ""
        });
    }

    const selected = state.items && state.items.length > 0
        ? state.items[state.selectedIndex]
        : null;
    const actionText = selected && selected.kind === "file" ? "Click: select" : "Click: open";
    drawFooter([actionText, "Jog: scroll"]);
}

/* Track plugin async-load state per draw so a preset switch's deferred
 * convert/load can refresh ui_hierarchy + chain_params once it completes.
 * The schwung-sfz xsynth fork (and similar) defer the actual SFZ load by
 * a few audio blocks after setSlotParam("preset", N); a naive refetch
 * inside changeHierPreset observes the previous preset's knob list. */
let hierEditorPrevLoading = false;

/*
 * When the deferred contract re-read below comes due, or 0 for none pending.
 *
 * The refetch in changeHierPreset happens on the line after the write, and for
 * a module that publishes its new contract synchronously that is right and
 * stays. schwung-airwindows does not: writing the selection only SCHEDULES the
 * plugin load, 300 ms later on a worker thread (clap_fx.cpp:806-822), and
 * `chain_params` describes the plugin that is still LOADED until it finishes.
 * The immediate refetch therefore wins the race and caches the previous
 * effect — the grid and the list both sat exactly one selection behind.
 *
 * So the immediate refetch is kept (nothing that answers straight away should
 * get slower) and a second one is booked for after the debounce. Re-armed on
 * every detent, so a spin down a 519-effect list costs one extra refetch when
 * the hand stops rather than one per step.
 */
let hierEditorContractDueMs = 0;

function armHierEditorContractSettle() {
    hierEditorContractDueMs = Date.now() + CONTRACT_SETTLE_MS;
}

/* Re-read the contract a selection made stale, once it has settled. */
function serviceHierEditorContractSettle() {
    if (!hierEditorContractDueMs || Date.now() < hierEditorContractDueMs) return;
    hierEditorContractDueMs = 0;
    let newHier = null;
    if (hierEditorIsMasterFx) {
        hierEditorChainParams = getMasterFxChainParams(hierEditorMasterFxSlot);
        newHier = getMasterFxHierarchy(hierEditorMasterFxSlot);
    } else {
        hierEditorChainParams = getComponentChainParams(hierEditorSlot, hierEditorComponent);
        newHier = getComponentHierarchy(hierEditorSlot, hierEditorComponent);
    }
    if (newHier) {
        hierEditorHierarchy = newHier;
        loadHierarchyLevel();
    }
    invalidateKnobContextCache();
    invalidateKnobValueCache();
    needsRedraw = true;
}

/* Draw the hierarchy-based parameter editor */
function drawHierarchyEditor() {
    clear_screen();

    /* Re-fetch chain_params if empty — module may have still been loading
     * when we entered the hierarchy editor (e.g., Virus ROM loading) */
    if (!hierEditorChainParams || hierEditorChainParams.length === 0) {
        refreshHierarchyChainParams();
    }

    /* Poll is_loading and re-fetch hierarchy + chain_params on the
     * loading→ready transition. */
    {
        const prefix2 = getComponentParamPrefix(hierEditorComponent);
        const loadingStr = getSlotParam(hierEditorSlot, `${prefix2}:is_loading`);
        const loadingNow = loadingStr === "1";
        if (hierEditorPrevLoading && !loadingNow) {
            let newHier = null;
            if (hierEditorIsMasterFx) {
                hierEditorChainParams = getMasterFxChainParams(hierEditorMasterFxSlot);
                newHier = getMasterFxHierarchy(hierEditorMasterFxSlot);
            } else {
                hierEditorChainParams = getComponentChainParams(hierEditorSlot, hierEditorComponent);
                newHier = getComponentHierarchy(hierEditorSlot, hierEditorComponent);
            }
            if (newHier) {
                hierEditorHierarchy = newHier;
                loadHierarchyLevel();
            }
            invalidateKnobContextCache();
            /* Async preset load just landed — plugin may have overwritten
             * knob-mapped values during the deferred load. Cached values
             * predate the load, so drop them and re-read on next touch. */
            invalidateKnobValueCache();
        }
        hierEditorPrevLoading = loadingNow;
    }

    /* The deferred half of a preset change. Costs nothing on any frame where
     * nothing is pending, which is almost all of them. */
    serviceHierEditorContractSettle();

    /* Get plugin info */
    const prefix = getComponentParamPrefix(hierEditorComponent);
    const cfg = chainConfigs[hierEditorSlot] || createEmptyChainConfig();
    const moduleData = getChainComponentModule(cfg, hierEditorComponent);
    const abbrev = moduleData ? getModuleAbbrev(moduleData.module) : hierEditorComponent.toUpperCase();

    /* Get bank or preset name for header depending on view */
    const hierMid = moduleData ? moduleData.module : hierEditorComponent;
    let headerName;
    if (hierEditorIsPresetLevel && !hierEditorPresetEditMode) {
        /* Preset browser: show bank/soundfont name */
        headerName = getSlotParamCached(hierEditorSlot, `${prefix}:bank_name`, hierMid) ||
                     getSlotParamCached(hierEditorSlot, `${prefix}:name`, hierMid) || "";
    } else {
        /* Edit mode: show preset name */
        headerName = getSlotParamCached(hierEditorSlot, `${prefix}:preset_name`, hierMid) ||
                     getSlotParamCached(hierEditorSlot, `${prefix}:name`, hierMid) || "";
    }

    /* Check for mode indicator - show * for performance mode */
    let modeIndicator = "";
    if (hierEditorHierarchy && hierEditorHierarchy.modes && hierEditorHierarchy.mode_param) {
        const modeVal = getSlotParam(hierEditorSlot, `${prefix}:${hierEditorHierarchy.mode_param}`);
        const modeIndex = modeVal !== null ? parseInt(modeVal) : 0;
        /* modes[1] is typically "performance" - show * indicator */
        if (modeIndex === 1) {
            modeIndicator = "*";
        }
    }

    /* Build header: S#: Module: Bank (preset browser) or Preset (edit view) */
    const dirtyMark = slotDirtyCache[hierEditorSlot] ? "*" : "";
    let headerText;
    if (hierEditorIsPresetLevel && !hierEditorPresetEditMode) {
        /* In preset browser, always show bank name */
        headerText = `S${hierEditorSlot + 1}${dirtyMark}: ${abbrev}: ${headerName}${modeIndicator}`;
    } else if (hierEditorPath.length > 0) {
        /* If navigated into sub-levels, append path */
        headerText = `S${hierEditorSlot + 1}${dirtyMark}: ${abbrev} > ${hierEditorPath[hierEditorPath.length - 1]}`;
    } else {
        headerText = `S${hierEditorSlot + 1}${dirtyMark}: ${abbrev}: ${headerName}${modeIndicator}`;
    }

    drawHeader(truncateText(headerText, 24));

    /* Check if this is a preset browser level (and not in edit mode) */
    if (hierEditorIsPresetLevel && !hierEditorPresetEditMode) {
        /* Draw preset browser UI */
        const centerY = 32;

        /* Re-fetch preset count if zero (module may still be loading) */
        if (hierEditorPresetCount === 0 && hierEditorLevel && hierEditorHierarchy && hierEditorHierarchy.levels) {
            const levelDef = hierEditorHierarchy.levels[hierEditorLevel];
            if (levelDef && levelDef.count_param) {
                const retryPrefix = getComponentParamPrefix(hierEditorComponent);
                const countStr = getSlotParam(hierEditorSlot, `${retryPrefix}:${levelDef.count_param}`);
                const newCount = countStr ? parseInt(countStr) : 0;
                if (newCount > 0) {
                    hierEditorPresetCount = newCount;
                    const presetStr = getSlotParam(hierEditorSlot, `${retryPrefix}:${levelDef.list_param}`);
                    hierEditorPresetIndex = presetStr ? parseInt(presetStr) : 0;
                    const nameParam = levelDef.name_param || "preset_name";
                    hierEditorPresetName = getSlotParam(hierEditorSlot, `${retryPrefix}:${nameParam}`) || "";
                }
            }
        }

        if (hierEditorPresetCount > 0) {
            /* Show preset number */
            const presetNum = `${hierEditorPresetIndex + 1} / ${hierEditorPresetCount}`;
            const numX = Math.floor((SCREEN_WIDTH - presetNum.length * 5) / 2);
            print(numX, centerY - 8, presetNum, 1);

            /* Show preset name — re-fetch each draw so plugins can publish
             * transient state (e.g. "Loading... <name> <spinner>") that
             * updates without requiring another preset-change. Falls back to
             * the cached value while the IPC read settles. */
            const freshPresetName = getSlotParam(hierEditorSlot, `${prefix}:preset_name`);
            const presetNameForDraw = (freshPresetName && freshPresetName.length > 0)
                ? freshPresetName : hierEditorPresetName;
            const name = truncateText(presetNameForDraw || "(unnamed)", 22);
            const nameX = Math.floor((SCREEN_WIDTH - name.length * 5) / 2);
            print(nameX, centerY + 4, name, 1);

            /* Check for load error and display if present */
            const loadError = getSlotParam(hierEditorSlot, `${prefix}:load_error`);
            if (loadError && loadError.length > 0) {
                /* Word-wrap error into lines of ~20 chars */
                const maxChars = 20;
                const words = loadError.split(' ');
                const lines = [];
                let line = '';
                for (const word of words) {
                    if (line.length + word.length + 1 > maxChars && line.length > 0) {
                        lines.push(line);
                        line = word;
                    } else {
                        line = line.length > 0 ? line + ' ' + word : word;
                    }
                }
                if (line.length > 0) lines.push(line);
                const errY = centerY + 16;
                for (let i = 0; i < Math.min(lines.length, 2); i++) {
                    const errX = Math.floor((SCREEN_WIDTH - lines[i].length * 5) / 2);
                    print(errX, errY + i * 10, lines[i], 1);
                }
            }

            /* Draw navigation arrows */
            print(4, centerY - 2, "<", 1);
            print(SCREEN_WIDTH - 10, centerY - 2, ">", 1);
        } else {
            print(4, centerY, "No presets available", 1);
        }

        /* Footer hints - always push to edit (for swap/params) */
        drawFooter(["Click: edit", "Jog: browse"]);
    } else {
        const selectedKey = getSelectedHierarchyEditableKey();
        const selectedMeta = selectedKey ? getParamMetadata(selectedKey) : null;

        if (hierEditorEditMode && selectedMeta && selectedMeta.ui_type === "wav_position") {
            drawWavPositionEditor(selectedKey, selectedMeta);
            return;
        }

        /* Draw param list */
        if (hierEditorParams.length === 0) {
            print(4, 24, "No parameters", 1);
        } else {
            /* Calculate visible range - only fetch values for items on screen */
            const maxVisible = Math.max(1, Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / LIST_LINE_HEIGHT));
            const halfVisible = Math.floor(maxVisible / 2);
            const visibleStart = Math.max(0, hierEditorSelectedIdx - halfVisible);
            const visibleEnd = Math.min(hierEditorParams.length, visibleStart + maxVisible + 1);

            /* Build items with labels and values */
            const items = hierEditorParams.map((param, idx) => {
                if (param && typeof param === "object" && param.isChild) {
                    return {
                        label: param.label,
                        value: "",
                        key: `child_${param.childIndex}`,
                        isChild: true,
                        childIndex: param.childIndex
                    };
                }
                /* Handle navigation params (params with level property) */
                if (param && typeof param === "object" && param.level) {
                    return {
                        label: `[${param.label || param.level}...]`,
                        value: "",
                        key: `nav_${param.level}`,
                        isNavigation: true,
                        targetLevel: param.level
                    };
                }

                /* Handle dynamic items (from items_param) */
                if (param && typeof param === "object" && param.isDynamicItem) {
                    return {
                        label: param.label,
                        value: "",
                        key: `item_${param.index}`,
                        isDynamicItem: true,
                        itemIndex: param.index
                    };
                }

                const key = typeof param === "string" ? param : param.key || param;

                /* Handle special swap module action */
                if (key === SWAP_MODULE_ACTION) {
                    return { label: "[Swap module...]", value: "", key, isAction: true };
                }

                const meta = getParamMetadata(key);
                const label = meta && meta.name ? meta.name : key.replace(/_/g, " ");
                let displayLabel = label;

                /* Only fetch param value if this item is visible on screen */
                let displayVal = "";
                if (idx >= visibleStart && idx < visibleEnd) {
                    const fullKey = buildHierarchyParamKey(key);
                    const usingStableEditVal = hierEditorEditMode &&
                                               hierEditorEditKey === fullKey && hierEditorEditValue !== null;
                    const val = usingStableEditVal ? String(hierEditorEditValue) : getHierarchyDisplayRawValue(hierEditorSlot, fullKey);
                    displayVal = val !== null ? formatHierDisplayValue(key, val) : "";
                    if (isHierarchyParamModulated(hierEditorSlot, fullKey)) {
                        displayLabel = `${label}~`;
                    }
                }
                return { label: displayLabel, value: displayVal, key };
            });

            drawMenuList({
                items,
                selectedIndex: hierEditorSelectedIdx,
                listArea: { topY: LIST_TOP_Y, bottomY: LIST_INDICATOR_BOTTOM_Y },
                getLabel: (item) => item.label,
                getValue: (item) => item.value,
                valueAlignRight: true,
                valueX: 72,  // Lower floor for non-selected rows to maximize label width before truncation
                getValueX: (val, floor) => Math.max(floor, SCREEN_WIDTH - text_width(val) - VALUE_RIGHT_CLEARANCE),
                editMode: hierEditorEditMode,
                scrollSelectedValue: true,
                prioritizeSelectedValue: true,
                selectedMinLabelChars: 6
            });
        }

        /* Footer hints */
        let hint = hierEditorEditMode ? ["Click: done", "Jog: adjust"] : ["Click: edit", "Jog: scroll"];
        if (!hierEditorEditMode && selectedMeta && selectedMeta.type === "string") {
            hint = ["Click: keyboard", "Jog: scroll"];
        } else if (!hierEditorEditMode && selectedMeta && selectedMeta.type === "canvas") {
            hint = ["Click: open", "Jog: scroll"];
        }
        drawFooter(hint);
    }
}

/* Change preset in component edit mode */
function changeComponentPreset(delta) {
    if (editComponentPresetCount <= 0) return;

    /* Calculate new preset with wrapping */
    let newPreset = editComponentPreset + delta;
    if (newPreset < 0) newPreset = editComponentPresetCount - 1;
    if (newPreset >= editComponentPresetCount) newPreset = 0;

    /* Apply the preset change */
    const prefix = getComponentParamPrefix(editingComponentKey);
    setSlotParam(selectedSlot, `${prefix}:preset`, String(newPreset));

    /* Update local state */
    editComponentPreset = newPreset;

    /* Fetch new preset name */
    editComponentPresetName = getSlotParam(selectedSlot, `${prefix}:preset_name`) || "";

    /* Announce preset change - name first for easier scrolling */
    const presetName = editComponentPresetName || `Preset ${editComponentPreset + 1}`;
    announce(`${presetName}, Preset ${editComponentPreset + 1} of ${editComponentPresetCount}`);
}

/* getSlotSettingValue(), adjustSlotSetting() -> shadow_ui_slots.mjs */

/*
 * MASTER FX SETTINGS LIST -- the value column and the jog, for the
 * screen-reader fallback list under VIEWS.MASTER_FX.
 *
 * These two used to be a 120-line if-chain apiece, and the name is a lie about
 * nearly all of it: every branch but one served GLOBAL SETTINGS, which is a
 * synthesised module contract now (shadow_ui_global_grid.mjs) with its own
 * absolute writes and its own persistence. What is left is what the name always
 * claimed -- the master bus's own Volume -- plus the actions, which carry no
 * value and are not adjustable.
 */
function getMasterFxSettingValue(setting) {
    if (setting.key === "master_volume") {
        const val = shadow_get_param(0, "master_fx:volume");
        if (!val) return "100%";
        const num = parseFloat(val);
        return isNaN(num) ? val : `${Math.round(num * 100)}%`;
    }
    /* master_fx_midi_channel is a MASTER FX setting, not a Global Settings one,
     * so it stays here after the Global Settings keys moved into the contract
     * (shadow_ui_global_grid.mjs). It arrived on main in #266 while this branch
     * was deleting its neighbours -- carried across the merge deliberately.
     *
     * Branch on the RAW value: a failed read is null, an unserved key is "",
     * and both parse to the same thing. Reporting "All" for a read that never
     * completed would show the user a setting they do not have. */
    if (setting.key === "master_fx_midi_channel") {
        const raw = shadow_get_param(0, "master_fx:midi_channel");
        if (raw === null || raw === "") return "--";
        return MFX_MIDI_CHANNEL_OPTIONS[mfxMidiChannelToIndex(raw)];
    }
    return "-";
}

function adjustMasterFxSetting(setting, delta) {
    if (setting.type === "action") return;

    if (setting.key === "master_volume") {
        let val = parseFloat(shadow_get_param(0, "master_fx:volume") || "1.0");
        val += delta * setting.step;
        val = Math.max(setting.min, Math.min(setting.max, val));
        shadow_set_param(0, "master_fx:volume", val.toFixed(2));
        return;
    }

    /* master_fx_midi_channel is a MASTER FX setting, not a Global Settings one,
     * so it stays after the Global Settings keys moved into the contract
     * (shadow_ui_global_grid.mjs). It arrived on main in #266 while this branch
     * was deleting its neighbours -- carried across the merge deliberately. */
    if (setting.key === "master_fx_midi_channel") {
        const raw = shadow_get_param(0, "master_fx:midi_channel");
        /* A failed read must not produce a write. Stepping from a value we
         * never actually saw would move the setting somewhere the user did
         * not ask for, and the shim would then persist it. */
        if (raw === null || raw === "") return;
        const n = MFX_MIDI_CHANNEL_OPTIONS.length;
        /* Honour delta rather than always stepping +1: the click path passes 1,
         * but a jog turn passes -1 and a hardcoded increment would make the
         * encoder only ever go forwards. Modulo twice so a negative wraps. */
        const idx = ((mfxMidiChannelToIndex(raw) + delta) % n + n) % n;
        const newVal = mfxMidiChannelFromIndex(idx);
        shadow_set_param(0, "master_fx:midi_channel", String(newVal));
        cachedMasterFxMidiChannel = newVal;
        saveMasterFxChainConfig();
        return;
    }
}

/* Update the focused slot in shared memory for knob CC routing */
function updateFocusedSlot(slot) {
    if (typeof shadow_set_focused_slot === "function") {
        shadow_set_focused_slot(slot);
    }

    /* Check for synth errors when selecting a chain slot (not Master FX) */
    if (slot >= 0 && slot < SHADOW_UI_SLOTS) {
        if (!warningShownForSlots.has(slot)) {
            const synthModule = getSlotParam(slot, "synth_module");
            if (synthModule && synthModule.length > 0) {
                /* Slot has a synth - check for errors */
                if (checkAndShowSynthError(slot)) {
                    warningShownForSlots.add(slot);
                    return;
                }
            }
        }

    }

    /* Check for Master FX errors when selecting the Master FX slot (slot 4) */
    if (slot === SHADOW_UI_SLOTS) {  /* Slot 4 = Master FX */
        for (let fx = 0; fx < MASTER_FX_SLOTS; fx++) {
            if (warningShownForMasterFx.has(fx)) continue;
            const fxModule = getMasterFxParam(fx, "module");
            if (fxModule && fxModule.length > 0) {
                /* FX slot has a module loaded - check for errors */
                if (checkAndShowMasterFxError(fx)) {
                    warningShownForMasterFx.add(fx);
                    break;  /* Show one warning at a time */
                }
            }
        }
    }
}

/*
 * Shift+jog in EITHER chain editor: move the selected module, and take the
 * selection with it.
 *
 * Returns whether the gesture was CONSUMED, which is not the same as whether
 * anything moved — a module already at the end of its section consumes the
 * jog and says so, rather than quietly turning back into a selection change
 * halfway through a reorder.
 *
 * Written once against a chain target, so Master FX gets the gesture in the
 * same commit rather than in some later one that never comes. Nothing in here
 * asks which chain it is holding: the selection, the list and the move all come
 * off the target.
 */
function chainReorderJog(target, delta) {
    const comp = target.components()[target.selection()];
    if (!comp || comp.kind !== "module") return false;
    if (!comp.module) { announce(`${comp.label} empty`); return true; }
    if (!moveChainComponent(target, comp.key, delta)) {
        announce(`${comp.label} ${delta < 0 ? "at the start" : "at the end"}`);
        return true;
    }
    /* Re-anchor by where the module WENT, not by the index it left behind: the
     * list is as long as the chain and every shape edit shifts it, so an index
     * carried across one points at whatever took the vacated place. */
    const after = target.components();
    const want = chainEditorKeyAt(comp.section, comp.index + delta);
    const at = after.findIndex((c) => c.key === want);
    if (at >= 0) target.setSelection(at);
    const moved = after[target.selection()];
    announce(`${moved ? moved.label : comp.label} moved ${delta < 0 ? "left" : "right"}`);
    needsRedraw = true;
    return true;
}

/*
 * `shift` defaults to the live modifier rather than being passed by every
 * caller: there are two call sites (plain and co-run) and both want the same
 * answer, so a parameter they both had to remember to fill in would be a
 * parameter one of them eventually forgot. It is a parameter at all so the
 * gesture can be driven without a device.
 */
function handleJog(delta, shift = isShiftHeld()) {
    hideOverlay();
    switch (view) {
        case VIEWS.SLOTS:
            handleSlotsJog(delta);
            break;
        case VIEWS.MASTER_FX:
            if (masterShowingNamePreview) {
                /* Navigate Edit/OK */
                masterNamePreviewIndex = masterNamePreviewIndex === 0 ? 1 : 0;
                announceSavePreview(masterPendingSaveName, masterNamePreviewIndex, false);
            } else if (masterConfirmingOverwrite || masterConfirmingDelete) {
                /* Navigate No/Yes */
                masterConfirmIndex = masterConfirmIndex === 0 ? 1 : 0;
                announce(masterConfirmIndex === 0 ? "No" : "Yes");
            } else if (helpDetailScrollState) {
                handleScrollableTextJog(helpDetailScrollState, delta);
            } else if (helpNavStack.length > 0) {
                const frame = helpNavStack[helpNavStack.length - 1];
                frame.selectedIndex = Math.max(0, Math.min(frame.items.length - 1, frame.selectedIndex + delta));
                announce(frame.items[frame.selectedIndex].title);
            } else if (inMasterPresetPicker) {
                /* Navigate preset picker */
                const totalItems = 1 + masterPresets.length;  /* [New] + presets */
                selectedMasterPresetIndex = Math.max(0, Math.min(totalItems - 1, selectedMasterPresetIndex + delta));
                const presetName = selectedMasterPresetIndex === 0 ? "[New]" : masterPresets[selectedMasterPresetIndex - 1];
                announceMenuItem("Preset", presetName);
            } else if (inMasterFxSettingsMenu) {
                /* Navigate settings menu */
                const items = getMasterFxSettingsItems();
                if (editingMasterFxSetting) {
                    /* Adjust value */
                    const item = items[selectedMasterFxSetting];
                    if (item.type === "float" || item.type === "int" || item.type === "bool" || item.type === "enum") {
                        adjustMasterFxSetting(item, delta);
                        const newVal = getMasterFxSettingValue(item);
                        announceParameter(item.label, newVal);
                    }
                } else {
                    selectedMasterFxSetting = Math.max(0, Math.min(items.length - 1, selectedMasterFxSetting + delta));
                    const item = items[selectedMasterFxSetting];
                    const value = getMasterFxSettingValue(item);
                    announceMenuItem(item.label, value);
                }
            } else if (selectingMasterFxModule) {
                /* Navigate the picker's own rows — the modules plus this
                 * position's Move Left / Move Right. */
                selectedMasterFxModuleIndex = Math.max(0, Math.min(masterFxPickerItems.length - 1, selectedMasterFxModuleIndex + delta));
                const module = masterFxPickerItems[selectedMasterFxModuleIndex];
                if (module) announceMenuItem("Module", module.name);
            } else if (shift && chainReorderJog(MASTER_CHAIN_TARGET, delta)) {
                /* Shift turns the jog from "which module" into "where does this
                 * module go" — the SAME gesture the slot chain has, through the
                 * same function. It is consumed either way once a module is
                 * selected, so the selection cannot creep sideways underneath a
                 * reorder the user thinks they are performing. */
                needsRedraw = true;
            } else {
                /* Navigate chain components (-1 = preset selection, like instrument slots)
                 * Preset picker is only accessible via click, not scroll.
                 * Bounded by the list's own length, which is the LOADED chain
                 * plus its `+` and Settings — never by the cap. */
                const comps = masterFxChainComponents();
                selectedMasterFxComponent = Math.max(-1, Math.min(comps.length - 1, selectedMasterFxComponent + delta));
                if (selectedMasterFxComponent === -1) {
                    announce("Preset Selection");
                } else {
                    const comp = comps[selectedMasterFxComponent];
                    if (comp.kind === "add") {
                        /* "+, Empty" says nothing. The label already is the
                         * whole instruction. */
                        announce(comp.label);
                    } else {
                        const moduleName = masterFxConfig?.[comp.key]?.module || "Empty";
                        announceMenuItem(comp.label, moduleName);
                    }
                }
            }
            break;
        case VIEWS.SLOT_SETTINGS:
            handleSlotSettingsJog(delta);
            break;
        case VIEWS.PATCHES:
            handlePatchesJog(delta);
            break;
        case VIEWS.PATCH_DETAIL:
            handlePatchDetailJog(delta);
            break;
        case VIEWS.COMPONENT_PARAMS:
            handleComponentParamsJog(delta);
            break;
        case VIEWS.PRESETS:
            handlePresetsJog(delta);
            break;
        case VIEWS.PRESET_DETAIL:
            handlePresetDetailJog(delta);
            break;
        case VIEWS.CHAIN_EDIT:
            /* Navigate horizontally through chain components (-1 = chain/patch selection) */
            {
                /* Shift turns the jog from "which module" into "where does
                 * this module go". It is consumed either way once a module is
                 * selected, so the selection cannot creep sideways underneath a
                 * reorder the user thinks they are performing. */
                if (shift && chainReorderJog(slotChainTarget(selectedSlot), delta)) break;
                const comps = slotChainComponents(selectedSlot);
                selectedChainComponent = Math.max(-1, Math.min(comps.length - 1, selectedChainComponent + delta));
                lastChainComponent[selectedSlot] = selectedChainComponent;
                /* Announce component */
                if (selectedChainComponent === -1) {
                    announce("Patch Selection");
                } else {
                    const comp = comps[selectedChainComponent];
                    if (comp.kind === "add") {
                        /* "+, Empty" says nothing. The label already is the
                         * whole instruction. */
                        announce(comp.label);
                    } else {
                        const moduleName =
                            getChainComponentModule(chainConfigs[selectedSlot], comp.key)?.module || "Empty";
                        announceMenuItem(comp.label, moduleName);
                    }
                }
            }
            break;
        case VIEWS.COMPONENT_SELECT:
            /* Navigate available modules list */
            selectedModuleIndex = Math.max(0, Math.min(availableModules.length - 1, selectedModuleIndex + delta));
            if (availableModules.length > 0) {
                const mod = availableModules[selectedModuleIndex];
                announceMenuItem("Module", mod.name || mod.id || "Unknown");
            }
            break;
        case VIEWS.CHAIN_SETTINGS:
            if (showingNamePreview) {
                namePreviewIndex = namePreviewIndex === 0 ? 1 : 0;
                announceSavePreview(pendingSaveName, namePreviewIndex, false);
            } else if (confirmingOverwrite || confirmingDelete) {
                confirmIndex = confirmIndex === 0 ? 1 : 0;
                announce(confirmIndex === 0 ? "No" : "Yes");
            } else if (editingChainSettingValue) {
                const items = getChainSettingsItems(selectedSlot);
                const setting = items[selectedChainSetting];
                adjustChainSetting(selectedSlot, setting, delta);
                /* Announce new value */
                const newVal = getChainSettingValue(selectedSlot, setting);
                announceParameter(setting.label, newVal);
            } else {
                const items = getChainSettingsItems(selectedSlot);
                selectedChainSetting = Math.max(0, Math.min(items.length - 1, selectedChainSetting + delta));
                /* Announce selected setting */
                const setting = items[selectedChainSetting];
                const val = getChainSettingValue(selectedSlot, setting);
                announceMenuItem(setting.label, val);
            }
            break;
        case VIEWS.COMPONENT_EDIT:
            /* Jog changes preset */
            changeComponentPreset(delta);
            break;
        case VIEWS.HIERARCHY_EDITOR:
            if (hierEditorIsPresetLevel && !hierEditorPresetEditMode) {
                /* Browse presets */
                changeHierPreset(delta);
                /* Announcement happens in changeHierPreset */
            } else if (hierEditorEditMode) {
                /* Adjust selected param value */
                adjustHierSelectedParam(delta);
                /* Fetch fresh value and announce it */
                if (hierEditorParams.length > 0 && hierEditorSelectedIdx >= 0 && hierEditorSelectedIdx < hierEditorParams.length) {
                    const param = hierEditorParams[hierEditorSelectedIdx];
                    const key = typeof param === "string" ? param : param.key || param;
                    const fullKey = buildHierarchyParamKey(key);
                    const freshVal = (hierEditorEditKey === fullKey && hierEditorEditValue !== null)
                        ? String(hierEditorEditValue)
                        : getHierarchyDisplayRawValue(hierEditorSlot, fullKey);
                    const displayVal = freshVal !== null ? formatHierDisplayValue(key, freshVal) : "";
                    const modSuffix = isHierarchyParamModulated(hierEditorSlot, fullKey) ? "~" : "";
                    announceParameter((param.label || key) + modSuffix, displayVal);
                }
            } else {
                /* Scroll param list (includes preset edit mode) */
                hierEditorSelectedIdx = Math.max(0, Math.min(hierEditorParams.length - 1, hierEditorSelectedIdx + delta));
                /* Announce selected parameter */
                if (hierEditorParams.length > 0 && hierEditorSelectedIdx >= 0 && hierEditorSelectedIdx < hierEditorParams.length) {
                    const param = hierEditorParams[hierEditorSelectedIdx];
                    const key = typeof param === "string" ? param : (param && param.key ? param.key : "");
                    const label = (param && typeof param === "object" && param.label) ? param.label : key;
                    if (typeof key !== "string" || key.startsWith("nav_") || key.startsWith("item_") || key === SWAP_MODULE_ACTION) {
                        announceMenuItem(label || param.key, param.value || "");
                    } else {
                        let value = "";
                        if (key) {
                            const fullKey = buildHierarchyParamKey(key);
                            const val = getHierarchyDisplayRawValue(hierEditorSlot, fullKey);
                            value = val !== null ? formatHierDisplayValue(key, val) : "";
                            const modSuffix = isHierarchyParamModulated(hierEditorSlot, fullKey) ? "~" : "";
                            announceMenuItem((label || key) + modSuffix, value);
                        } else {
                            announceMenuItem(label || "Param", "");
                        }
                    }
                }
            }
            break;
        case VIEWS.CANVAS:
            /* Canvas animation is autonomous; jog is forwarded via onMidi hook. */
            break;
        case VIEWS.FILEPATH_BROWSER:
            if (filepathBrowserState) {
                moveFilepathBrowserSelection(filepathBrowserState, delta);
                const selected = filepathBrowserState.items[filepathBrowserState.selectedIndex];
                if (filepathBrowserState.livePreviewEnabled && selected && selected.kind === "file" && selected.path) {
                    filepathBrowserState.previewPendingPath = selected.path;
                    filepathBrowserState.previewPendingTime = Date.now();
                } else if (filepathBrowserState.livePreviewEnabled) {
                    filepathBrowserState.previewPendingPath = "";
                    filepathBrowserState.previewPendingTime = 0;
                }
                if (selected) announceMenuItem(selected.label || "File", "");
            }
            break;
        case VIEWS.KNOB_EDITOR:
            /* Navigate knob list (8 knobs) */
            knobEditorIndex = Math.max(0, Math.min(NUM_KNOBS - 1, knobEditorIndex + delta));
            /* Announce knob and current assignment */
            const knobNum = knobEditorIndex + 1;
            const assignment = knobEditorAssignments[knobEditorIndex];
            const assignLabel = assignment ? `${assignment.target}: ${assignment.label}` : "Unassigned";
            announceMenuItem(`Knob ${knobNum}`, assignLabel);
            break;
        case VIEWS.KNOB_PARAM_PICKER:
            if (knobParamPickerFolder === null) {
                /* Navigate targets list */
                const targets = getKnobTargets(knobEditorSlot);
                knobParamPickerIndex = Math.max(0, Math.min(targets.length - 1, knobParamPickerIndex + delta));
                if (targets.length > 0) {
                    const t = targets[knobParamPickerIndex];
                    announceMenuItem("Target", t.label || t.id || "None");
                }
            } else {
                /* Navigate params list */
                knobParamPickerIndex = Math.max(0, Math.min(knobParamPickerParams.length - 1, knobParamPickerIndex + delta));
                if (knobParamPickerParams.length > 0) {
                    const kp = knobParamPickerParams[knobParamPickerIndex];
                    announceMenuItem("Param", kp.label || kp.key || "Unknown");
                }
            }
            break;
        case VIEWS.DYNAMIC_PARAM_PICKER:
            if (dynamicPickerMode === "param") {
                dynamicPickerIndex = Math.max(0, Math.min(dynamicPickerParams.length - 1, dynamicPickerIndex + delta));
                if (dynamicPickerParams.length > 0) {
                    const paramItem = dynamicPickerParams[dynamicPickerIndex];
                    announceMenuItem("Param", paramItem.label || paramItem.key || "");
                }
            } else {
                dynamicPickerIndex = Math.max(0, Math.min(dynamicPickerTargets.length - 1, dynamicPickerIndex + delta));
                if (dynamicPickerTargets.length > 0) {
                    const targetItem = dynamicPickerTargets[dynamicPickerIndex];
                    announceMenuItem("Target", targetItem.label || targetItem.id || "");
                }
            }
            break;
        case VIEWS.OVERTAKE_MENU:
            selectedOvertakeModule += delta;
            if (selectedOvertakeModule < 0) selectedOvertakeModule = 0;
            if (selectedOvertakeModule >= overtakeModules.length) {
                selectedOvertakeModule = Math.max(0, overtakeModules.length - 1);
            }
            if (overtakeModules.length > 0) {
                const om = overtakeModules[selectedOvertakeModule];
                announceMenuItem("Module", om.name || om.id || "Unknown");
            }
            break;
        case VIEWS.TOOLS: {
            /* Skip divider rows when moving the cursor. */
            const step = delta > 0 ? 1 : -1;
            let remaining = Math.abs(delta);
            let idx = toolsMenuIndex;
            while (remaining > 0) {
                let next = idx + step;
                while (next >= 0 && next < toolModules.length && toolModules[next]?.type === 'divider') {
                    next += step;
                }
                if (next < 0 || next >= toolModules.length) break;
                idx = next;
                remaining--;
            }
            toolsMenuIndex = idx;
            if (toolModules.length > 0 && toolModules[toolsMenuIndex]?.type !== 'divider') {
                const item = toolModules[toolsMenuIndex];
                announce(item.suspended ? (item.name + ", suspended") : item.name);
            }
            break;
        }
        case VIEWS.TOOL_FILE_BROWSER:
            toolBrowserNavigate(delta);
            break;
        case VIEWS.TOOL_SET_PICKER:
            toolSetPickerNavigate(delta);
            break;
        case VIEWS.TOOL_ENGINE_SELECT:
            toolEngineNavigate(delta);
            break;
        case VIEWS.TOOL_STEM_REVIEW: {
            const maxIdx = toolStemFiles.length; // 0=Save All, 1..N=stems
            toolStemReviewIndex = Math.max(0, Math.min(maxIdx, toolStemReviewIndex + delta));
            if (toolStemReviewIndex === 0) {
                wavPlayerStop();
                const kc = toolStemKept.filter(k => k).length;
                if (kc === 0) announce("Cancel");
                else if (kc === toolStemFiles.length) announce("Save All");
                else announce("Save " + kc + " stems");
            } else {
                /* Preview the highlighted stem */
                wavPlayerPlay(toolOutputDir + "/" + toolStemFiles[toolStemReviewIndex - 1]);
                const name = toolStemFiles[toolStemReviewIndex - 1].replace(/\.wav$/i, "");
                const kept = toolStemKept[toolStemReviewIndex - 1];
                announce(name + (kept ? ", selected" : ", deselected"));
            }
            break;
        }
        case VIEWS.LFO_EDIT: {
            const items = getLfoItems();
            if (editingLfoValue) {
                const item = items[selectedLfoItem];
                adjustLfoParam(item, delta);
                const newVal = getLfoDisplayValue(item);
                announceParameter(item.label, newVal);
            } else {
                selectedLfoItem = Math.max(0, Math.min(items.length - 1, selectedLfoItem + delta));
                const item = items[selectedLfoItem];
                const val = getLfoDisplayValue(item);
                announceMenuItem(item.label, val);
            }
            break;
        }
        case VIEWS.LFO_TARGET_COMPONENT:
            selectedLfoTargetComp = Math.max(0, Math.min(lfoTargetComponents.length - 1, selectedLfoTargetComp + delta));
            if (lfoTargetComponents.length > 0) {
                announceMenuItem(lfoTargetComponents[selectedLfoTargetComp].label);
            }
            break;
        case VIEWS.LFO_TARGET_PARAM:
            selectedLfoTargetParam = Math.max(0, Math.min(lfoTargetParams.length - 1, selectedLfoTargetParam + delta));
            if (lfoTargetParams.length > 0) {
                announceMenuItem(lfoTargetParams[selectedLfoTargetParam].label);
            }
            break;
        case VIEWS.ENUM_PICKER:
            enumPickerJog(delta);
            break;
        case VIEWS.OVERTAKE_MODULE:
            /* Overtake module handles its own jog input */
            break;
        /* The settings themselves are a contract on the page chrome now
         * (enterGlobalSettingsGrid); this view is only the help viewer's host. */
        case VIEWS.GLOBAL_SETTINGS:
            if (helpDetailScrollState) {
                handleScrollableTextJog(helpDetailScrollState, delta);
            } else if (helpNavStack.length > 0) {
                const frame = helpNavStack[helpNavStack.length - 1];
                frame.selectedIndex = Math.max(0, Math.min(frame.items.length - 1, frame.selectedIndex + delta));
                announce(frame.items[frame.selectedIndex].title);
            }
            break;
    }
    needsRedraw = true;
}

function handleSelect() {
    debugLog("handleSelect called, view=" + view);
    hideOverlay();
    switch (view) {
        case VIEWS.SLOTS:
            handleSlotsSelect();
            break;
        case VIEWS.MASTER_FX:
            if (masterShowingNamePreview) {
                /* Name preview: Edit or OK */
                if (masterNamePreviewIndex === 0) {
                    /* Edit - open keyboard */
                    masterShowingNamePreview = false;
                    const savedName = masterPendingSaveName;
                    openTextEntry({
                        title: "Save As",
                        initialText: savedName,
                        onAnnounce: announce,
                        onConfirm: (newName) => {
                            masterPendingSaveName = newName;
                            masterShowingNamePreview = true;
                            masterNamePreviewIndex = 1;
                            announceSavePreview(masterPendingSaveName, masterNamePreviewIndex);
                            needsRedraw = true;
                        },
                        onCancel: () => {
                            masterShowingNamePreview = true;
                            announceSavePreview(masterPendingSaveName, masterNamePreviewIndex);
                            needsRedraw = true;
                        }
                    });
                } else {
                    /* OK - proceed with save (check for conflicts) */
                    masterShowingNamePreview = false;
                    const existingIdx = findMasterPresetByName(masterPendingSaveName);
                    if (existingIdx >= 0 && existingIdx !== masterOverwriteTargetIndex) {
                        /* Name conflict */
                        masterOverwriteTargetIndex = existingIdx;
                        masterConfirmingOverwrite = true;
                        masterConfirmIndex = 0;
                    } else {
                        doSaveMasterPreset(masterPendingSaveName);
                    }
                }
                needsRedraw = true;
                break;
            }
            if (masterConfirmingOverwrite) {
                /* Overwrite confirmation: No or Yes */
                if (masterConfirmIndex === 0) {
                    /* No - return to name preview */
                    masterConfirmingOverwrite = false;
                    if (masterOverwriteFromKeyboard) {
                        masterShowingNamePreview = true;
                        masterNamePreviewIndex = 0;
                        announceSavePreview(masterPendingSaveName, masterNamePreviewIndex);
                    }
                    /* If not from keyboard, just return to settings menu */
                } else {
                    /* Yes - overwrite */
                    doSaveMasterPreset(masterPendingSaveName);
                }
                needsRedraw = true;
                break;
            }
            if (masterConfirmingDelete) {
                /* Delete confirmation: No or Yes */
                if (masterConfirmIndex === 0) {
                    /* No - return to settings */
                    masterConfirmingDelete = false;
                } else {
                    /* Yes - delete */
                    doDeleteMasterPreset();
                }
                needsRedraw = true;
                break;
            }
            if (inMasterPresetPicker) {
                /* Preset picker click */
                if (selectedMasterPresetIndex === 0) {
                    /* [New] - clear master FX and exit picker */
                    clearMasterFx();
                    currentMasterPresetName = "";
                    exitMasterPresetPicker();
                } else {
                    /* Load selected preset */
                    const preset = masterPresets[selectedMasterPresetIndex - 1];
                    loadMasterPreset(preset.index, preset.name);
                    exitMasterPresetPicker();
                }
            } else if (helpDetailScrollState) {
                if (isActionSelected(helpDetailScrollState)) {
                    helpDetailScrollState = null;
                    needsRedraw = true;
                    const frame = helpNavStack[helpNavStack.length - 1];
                    announce(frame.title + ", " + frame.items[frame.selectedIndex].title);
                }
            } else if (helpNavStack.length > 0) {
                const frame = helpNavStack[helpNavStack.length - 1];
                const item = frame.items[frame.selectedIndex];
                if (item.children && item.children.length > 0) {
                    /* Branch node - push children onto stack */
                    helpNavStack.push({ items: item.children, selectedIndex: 0, title: item.title });
                    needsRedraw = true;
                    announce(item.title + ", " + item.children[0].title);
                } else if (item.lines && item.lines.length > 0) {
                    /* Leaf node - show scrollable text */
                    helpDetailScrollState = createScrollableText({
                        lines: item.lines,
                        actionLabel: "Back",
                        visibleLines: 4,
                        onActionSelected: (label) => announce(label)
                    });
                    needsRedraw = true;
                    announce(item.title + ". " + item.lines.join(". "));
                }
            } else if (inMasterFxSettingsMenu) {
                /* Settings menu click */
                const items = getMasterFxSettingsItems();
                const item = items[selectedMasterFxSetting];
                if (item.type === "action") {
                    handleMasterFxSettingsAction(item.key);
                } else if (item.type === "bool" || item.type === "enum") {
                    /* Toggle/cycle value immediately */
                    adjustMasterFxSetting(item, 1);
                    const newVal = getMasterFxSettingValue(item);
                    announceParameter(item.label, newVal);
                } else if (item.type === "float" || item.type === "int") {
                    /* Toggle value editing */
                    editingMasterFxSetting = !editingMasterFxSetting;
                }
                needsRedraw = true;
            } else if (selectingMasterFxModule) {
                /* Apply module selection */
                applyMasterFxModuleSelection();
            } else if (selectedMasterFxComponent === -1) {
                /* Preset selected - enter preset picker */
                enterMasterPresetPicker();
            } else {
                const selectedComp = masterFxChainComponents()[selectedMasterFxComponent];
                if (!selectedComp) {
                    /* The list is as long as the chain now, so a selection can
                     * outlive the position it named. Nothing to open. */
                    break;
                }
                if (selectedComp.kind === "add") {
                    /* The `+`: same gesture, same helper, same rules as the slot
                     * chain's audio-FX `+`. */
                    const at = beginChainInsertFromAddBox(MASTER_CHAIN_TARGET, selectedComp);
                    if (at >= 0) {
                        selectedMasterFxComponent = at;
                        enterMasterFxModuleSelect(at);
                    }
                    break;
                }
                if (selectedComp.key === "settings") {
                    /* Enter settings submenu */
                    /* Knob grid instead of the list, when the user has opted
                     * in — the SAME gate, and the same four pages, a slot's
                     * Settings position gets. The screen reader still gets the
                     * list (paramPagesEnabled returns false for it): a grid has
                     * eight cells and nothing selected to read out. */
                    if (paramPagesEnabled() && !suppressMasterGridOnce) {
                        enterMasterFxSettingsGrid();
                        break;
                    }
                    suppressMasterGridOnce = false;
                    inMasterFxSettingsMenu = true;
                    selectedMasterFxSetting = 0;
                    editingMasterFxSetting = false;
                    needsRedraw = true;
                    /* Announce menu title + initial selection */
                    const items = getMasterFxSettingsItems();
                    if (items.length > 0) {
                        const item = items[0];
                        const value = getMasterFxSettingValue(item);
                        announce(`Master FX Settings, ${item.label}: ${value}`);
                    }
                } else {
                    /* FX slot - check if module is loaded with hierarchy */
                    const moduleData = masterFxConfig[selectedComp.key];

                    /* Mute+JogClick: toggle bypass on a populated MFX slot */
                    if (hostMuteHeld && moduleData && moduleData.module) {
                        toggleChainComponentBypass(MASTER_CHAIN_TARGET,
                                                   selectedComp.key, selectedComp.label);
                        break;
                    }

                    if (moduleData && moduleData.module) {
                        /* Module is loaded - try hierarchy editor first */
                        const hierarchy = getMasterFxHierarchy(selectedMasterFxComponent);
                        if (hierarchy) {
                            enterMasterFxHierarchyEditor(selectedMasterFxComponent);
                        } else {
                            /* No hierarchy - enter module selection to swap */
                            enterMasterFxModuleSelect(selectedMasterFxComponent);
                        }
                    } else {
                        /* No module loaded - enter module selection */
                        enterMasterFxModuleSelect(selectedMasterFxComponent);
                    }
                }
            }
            break;
        case VIEWS.SLOT_SETTINGS:
            handleSlotSettingsSelect();
            break;
        case VIEWS.PATCHES:
            handlePatchesSelect();
            break;
        case VIEWS.PATCH_DETAIL:
            handlePatchDetailSelect();
            break;
        case VIEWS.COMPONENT_PARAMS:
            handleComponentParamsSelect();
            break;
        case VIEWS.PRESETS:
            handlePresetsSelect();
            break;
        case VIEWS.PRESET_DETAIL:
            handlePresetDetailSelect();
            break;
        case VIEWS.CHAIN_EDIT:
            if (selectedChainComponent === -1) {
                /* Chain selected - open patch browser */
                enterPatchBrowser(selectedSlot);
            } else if (selectedChainComponent === slotChainComponents(selectedSlot).length - 1) {
                /* Settings selected - go to chain settings */
                enterChainSettings(selectedSlot);
            } else {
                /* Component selected - check if populated or empty */
                const comp = slotChainComponents(selectedSlot)[selectedChainComponent];

                /* A `+` box: open the picker on a NEW position WHERE THE BOX IS
                 * DRAWN. See beginChainInsertFromAddBox, which Master FX's `+`
                 * goes through too. */
                if (comp && comp.kind === "add") {
                    const at = beginChainInsertFromAddBox(slotChainTarget(selectedSlot), comp);
                    if (at >= 0) enterComponentSelect(selectedSlot, at);
                    break;
                }

                const moduleData = getChainComponentModule(chainConfigs[selectedSlot], comp.key);

                /* Mute+JogClick: toggle bypass on a populated module */
                if (hostMuteHeld && moduleData && moduleData.module) {
                    toggleChainComponentBypass(slotChainTarget(selectedSlot),
                                               comp.key, comp.label);
                    break;
                }

                debugLog(`CHAIN_EDIT select: slot=${selectedSlot}, comp=${comp?.key}, moduleData=${JSON.stringify(moduleData)}`);

                if (moduleData && moduleData.module) {
                    /* Populated - enter component details (hierarchy editor) */
                    debugLog(`Entering component edit for ${moduleData.module}`);
                    enterComponentEdit(selectedSlot, comp.key);
                } else {
                    /* Empty - enter module selection */
                    debugLog(`Entering component select (empty slot)`);
                    enterComponentSelect(selectedSlot, selectedChainComponent);
                }
            }
            break;
        case VIEWS.COMPONENT_SELECT:
            /* Apply selected module to the component */
            if (availableModules.length > 0) {
                const selMod = availableModules[selectedModuleIndex];
                announce(`Loading ${selMod.name || selMod.id || "module"}`);
            }
            applyComponentSelection();
            break;
        case VIEWS.STORE_PICKER_RESULT:
            handleStorePickerResultSelect();
            break;
        case VIEWS.CHAIN_SETTINGS:
            {
                if (showingNamePreview) {
                    if (namePreviewIndex === 0) {
                        /* Edit - open keyboard to edit name */
                        showingNamePreview = false;
                        const savedName = pendingSaveName;
                        openTextEntry({
                            title: "Save As",
                            initialText: savedName,
                            onAnnounce: announce,
                            onConfirm: (newName) => {
                                pendingSaveName = newName;
                                /* Return to name preview with edited name */
                                showingNamePreview = true;
                                namePreviewIndex = 1;  /* Default to OK */
                                announceSavePreview(pendingSaveName, namePreviewIndex);
                                needsRedraw = true;
                            },
                            onCancel: () => {
                                /* Return to name preview unchanged */
                                showingNamePreview = true;
                                announceSavePreview(pendingSaveName, namePreviewIndex);
                                needsRedraw = true;
                            }
                        });
                    } else {
                        /* OK - proceed with save (check for conflicts) */
                        showingNamePreview = false;
                        overwriteTargetIndex = -1;
                        const existingIdx = findPatchByName(pendingSaveName);
                        if (existingIdx >= 0) {
                            /* Name exists - ask to overwrite */
                            overwriteTargetIndex = existingIdx;
                            confirmingOverwrite = true;
                            confirmIndex = 0;
                            announce(`Overwrite ${pendingSaveName}?`);
                        } else {
                            /* No conflict - save directly */
                            doSavePreset(selectedSlot, pendingSaveName);
                        }
                    }
                    needsRedraw = true;
                    break;
                }
                if (confirmingOverwrite) {
                    if (confirmIndex === 0) {
                        /* No - behavior depends on how we got here */
                        confirmingOverwrite = false;
                        if (overwriteFromKeyboard) {
                            /* Came from Save/Save As flow - return to name preview */
                            showingNamePreview = true;
                            namePreviewIndex = 0;  /* Default to Edit so they can change the name */
                            announceSavePreview(pendingSaveName, namePreviewIndex);
                        } else {
                            /* Direct Save on existing - just return to settings */
                            pendingSaveName = "";
                            overwriteTargetIndex = -1;
                        }
                    } else {
                        /* Yes - save */
                        doSavePreset(selectedSlot, pendingSaveName);
                    }
                    needsRedraw = true;
                    break;
                }
                if (confirmingDelete) {
                    if (confirmIndex === 0) {
                        /* No - cancel */
                        confirmingDelete = false;
                    } else {
                        /* Yes - delete */
                        doDeletePreset(selectedSlot);
                    }
                    needsRedraw = true;
                    break;
                }
                const items = getChainSettingsItems(selectedSlot);
                const setting = items[selectedChainSetting];
                if (setting.type === "action") {
                    runChainSettingAction(selectedSlot, setting.key);
                } else {
                    editingChainSettingValue = !editingChainSettingValue;
                }
            }
            break;
        case VIEWS.HIERARCHY_EDITOR:
            /* Check for mode selection (hierEditorLevel is null when modes exist) */
            if (!hierEditorLevel && hierEditorHierarchy.modes) {
                /* Select mode and navigate into it */
                const selectedMode = hierEditorParams[hierEditorSelectedIdx];
                /* Check for swap module action first */
                if (selectedMode === SWAP_MODULE_ACTION) {
                    const compIndex = slotChainComponentIndex(hierEditorSlot, hierEditorComponent);
                    const slotToSwap = hierEditorSlot;
                    if (compIndex >= 0) {
                        exitHierarchyEditor();
                        enterComponentSelect(slotToSwap, compIndex);
                    }
                } else if (selectedMode && hierEditorHierarchy.levels[selectedMode]) {
                    /* If hierarchy specifies mode_param, set it to the mode index */
                    if (hierEditorHierarchy.mode_param) {
                        const modeIndex = hierEditorHierarchy.modes.indexOf(selectedMode);
                        if (modeIndex >= 0) {
                            const modePrefix = getComponentParamPrefix(hierEditorComponent);
                            setSlotParam(hierEditorSlot, `${modePrefix}:${hierEditorHierarchy.mode_param}`, String(modeIndex));
                        }
                    }
                    hierEditorPath.push("Mode");
                    hierEditorLevel = selectedMode;
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                }
            } else if (hierEditorIsPresetLevel && !hierEditorPresetEditMode) {
                /* On preset browser - drill into children or enter edit mode */
                const levelDef = hierEditorHierarchy.levels[hierEditorLevel];
                if (levelDef && levelDef.children) {
                    /* Push current level onto path and enter children level */
                    hierEditorPath.push(hierEditorPresetName || `Preset ${hierEditorPresetIndex + 1}`);
                    hierEditorChildIndex = -1;
                    hierEditorChildCount = levelDef.child_count || 0;
                    hierEditorChildLabel = levelDef.child_label || "Child";
                    hierEditorLevel = levelDef.children;
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                    invalidateKnobContextCache();
                } else if (cameFromParamPages) {
                    /* The grid handed this preset browser off because it does
                     * not draw one itself (see enterHierarchyEditorFromParamPages).
                     * A committed choice belongs back on the grid, not the
                     * list's own preset-edit-mode screen (params + swap
                     * action) — otherwise selecting a preset strands you in
                     * the list UI with no way back to Knobs. */
                    const slotIndex = hierEditorSlot;
                    const componentKey = hierEditorComponent;
                    cameFromParamPages = false;
                    exitHierarchyEditor();
                    enterParamPages(slotIndex, componentKey, getComponentParamPrefix(componentKey),
                        null, null, paramPagesChromeFor(componentKey));
                } else {
                    /* No children - enter preset edit mode to show params/swap */
                    hierEditorPresetEditMode = true;
                    hierEditorSelectedIdx = 0;
                    /* Re-fetch chain_params now that a preset/plugin is selected */
                    if (hierEditorIsMasterFx) {
                        hierEditorChainParams = getMasterFxChainParams(hierEditorMasterFxSlot);
                    } else {
                        hierEditorChainParams = getComponentChainParams(hierEditorSlot, hierEditorComponent);
                    }
                    /* Invalidate knob context cache to use new chain_params */
                    invalidateKnobContextCache();
                }
            } else if (hierEditorPresetEditMode || !hierEditorIsPresetLevel) {
                /* On params level - check for special actions */
                const selectedParam = hierEditorParams[hierEditorSelectedIdx];
                if (selectedParam && typeof selectedParam === "object" && selectedParam.isChild) {
                    hierEditorChildIndex = selectedParam.childIndex;
                    hierEditorPath.push(selectedParam.label);
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                    invalidateKnobContextCache();
                    break;
                }
                /* Handle navigation params (params with level property) */
                if (selectedParam && typeof selectedParam === "object" && selectedParam.level) {
                    hierEditorPath.push(selectedParam.label || selectedParam.level);
                    hierEditorLevel = selectedParam.level;
                    hierEditorSelectedIdx = 0;
                    hierEditorPresetEditMode = false;
                    /* Set up child count/label if target level has child_prefix */
                    const targetLevel = hierEditorHierarchy.levels[selectedParam.level];
                    if (targetLevel && targetLevel.child_prefix && targetLevel.child_count) {
                        hierEditorChildIndex = -1;
                        hierEditorChildCount = targetLevel.child_count;
                        hierEditorChildLabel = targetLevel.child_label || "Child";
                    }
                    loadHierarchyLevel();
                    invalidateKnobContextCache();
                    break;
                }
                /* Handle dynamic items (from items_param) */
                if (selectedParam && typeof selectedParam === "object" && selectedParam.isDynamicItem) {
                    /* Set the select_param to this item's index */
                    if (hierEditorSelectParam) {
                        const prefix = getComponentParamPrefix(hierEditorComponent);
                        setSlotParam(hierEditorSlot, `${prefix}:${hierEditorSelectParam}`, String(selectedParam.index));
                    }

                    /* Check if level specifies where to navigate after selection */
                    if (hierEditorNavigateTo) {
                        /* Navigate to specified level, clearing path */
                        hierEditorPath = [];
                        hierEditorLevel = hierEditorNavigateTo;
                    } else {
                        /* Go back to previous level */
                        if (hierEditorPath.length > 0) {
                            hierEditorPath.pop();
                        }
                        /* Find parent level - look for level that navigates here */
                        const levels = hierEditorHierarchy.levels;
                        let parentLevel = "root";
                        for (const [name, def] of Object.entries(levels)) {
                            if (def.params) {
                                for (const p of def.params) {
                                    if (p && typeof p === "object" && p.level === hierEditorLevel) {
                                        parentLevel = name;
                                        break;
                                    }
                                }
                            }
                        }
                        hierEditorLevel = parentLevel;
                    }
                    hierEditorSelectedIdx = 0;
                    /* Selecting a dynamic item (e.g. an instrument from
                     * the library browser) loads a new SFZ/preset, which
                     * for plugins like schwung-sfz changes the emitted
                     * ui_hierarchy + chain_params. Re-fetch both before
                     * rebuilding the level so the next param list shows
                     * the freshly-loaded preset's knobs instead of the
                     * previous one's stale slots. */
                    let newHierarchy = null;
                    if (hierEditorIsMasterFx) {
                        hierEditorChainParams = getMasterFxChainParams(hierEditorMasterFxSlot);
                        newHierarchy = getMasterFxHierarchy(hierEditorMasterFxSlot);
                    } else {
                        hierEditorChainParams = getComponentChainParams(hierEditorSlot, hierEditorComponent);
                        newHierarchy = getComponentHierarchy(hierEditorSlot, hierEditorComponent);
                    }
                    if (newHierarchy) hierEditorHierarchy = newHierarchy;
                    loadHierarchyLevel();
                    invalidateKnobContextCache();
                    /* Dynamic-item commit (e.g. select a mode/soundfont) may
                     * have rewritten knob-mapped param values inside the
                     * plugin. Drop stale cached values so next touch re-reads. */
                    invalidateKnobValueCache();
                    break;
                }
                if (selectedParam === SWAP_MODULE_ACTION) {
                    /* Swap module - handle Master FX vs regular chain slots */
                    if (hierEditorIsMasterFx) {
                        /* Master FX: use Master FX module select */
                        const fxSlot = hierEditorMasterFxSlot;
                        exitHierarchyEditor();
                        /* Restore Master FX component selection and enter module select */
                        selectedMasterFxComponent = fxSlot;
                        enterMasterFxModuleSelect(fxSlot);
                    } else {
                        /* Regular chain slot: find component index and enter module select */
                        const compIndex = slotChainComponentIndex(hierEditorSlot, hierEditorComponent);
                        const slotToSwap = hierEditorSlot;  /* Save before exit clears it */
                        if (compIndex >= 0) {
                            exitHierarchyEditor();
                            enterComponentSelect(slotToSwap, compIndex);
                        }
                    }
                } else {
                    /* Normal param - open filepath browser or toggle edit mode */
                    const selectedKey = (selectedParam && typeof selectedParam === "object")
                        ? (selectedParam.key || selectedParam)
                        : selectedParam;
                    if (!selectedKey || typeof selectedKey !== "string") {
                        break;
                    }
                    const meta = getParamMetadata(selectedKey);
                    if (!hierEditorEditMode && meta && meta.picker_type) {
                        openDynamicParamPicker(selectedKey, meta);
                    } else {
                        openHierarchyParamEditor(selectedKey, meta, false);
                    }
                }
            }
            break;
        case VIEWS.CANVAS:
            closeCanvasPreview(false);
            announce("Hierarchy Editor");
            break;
        case VIEWS.FILEPATH_BROWSER:
            if (!filepathBrowserState) {
                closeHierarchyFilepathBrowser();
                break;
            }
            {
                const result = activateFilepathBrowserItem(filepathBrowserState);
                if (result.action === "open") {
                    refreshFilepathBrowser(filepathBrowserState, FILEPATH_BROWSER_FS);
                    if (filepathBrowserState.livePreviewEnabled) {
                        const selected = filepathBrowserState.items[filepathBrowserState.selectedIndex];
                        filepathBrowserState.previewPendingPath = "";
                        filepathBrowserState.previewPendingTime = 0;
                        applyLivePreview(filepathBrowserState, selected);
                    }
                } else if (result.action === "select") {
                    const key = filepathBrowserParamKey || result.key;
                    const fullKey = buildHierarchyParamKey(key);
                    if (filepathBrowserState.livePreviewEnabled) {
                        filepathBrowserState.previewCommitted = true;
                        filepathBrowserState.previewCurrentValue = result.value || "";
                        filepathBrowserState.previewPendingPath = "";
                        filepathBrowserState.previewPendingTime = 0;
                    }
                    filepathBrowserState.previewSelectedPath = result.value || "";
                    setSlotParam(hierEditorSlot, fullKey, result.value || "");
                    applyLinkedWavEndDefaultsForFilepath(key);
                    announceParameter(filepathBrowserState.title || key, result.filename || result.value || "");
                    closeHierarchyFilepathBrowser();
                }
            }
            break;
        case VIEWS.KNOB_EDITOR:
            /* Edit this knob's assignment */
            enterKnobParamPicker();
            break;
        case VIEWS.KNOB_PARAM_PICKER:
            if (knobParamPickerFolder === null) {
                /* Main view - selecting a target */
                const targets = getKnobTargets(knobEditorSlot);
                const selected = targets[knobParamPickerIndex];
                if (selected) {
                    if (selected.id === "") {
                        /* (None) selected - clear assignment */
                        applyKnobAssignment("", "");
                    } else {
                        /* Enter param selection for this target */
                        knobParamPickerFolder = selected.id;
                        knobParamPickerIndex = 0;
                        knobParamPickerPath = [];

                        /* Try to load hierarchy for this target */
                        const hierarchyJson = getSlotParam(knobEditorSlot, `${selected.id}:ui_hierarchy`);
                        if (hierarchyJson) {
                            try {
                                knobParamPickerHierarchy = JSON.parse(hierarchyJson);
                                /* Find first level with actual params (skip preset browser) */
                                knobParamPickerLevel = findFirstParamLevel(knobParamPickerHierarchy);
                                knobParamPickerParams = getKnobPickerLevelItems(knobParamPickerHierarchy, knobParamPickerLevel);
                            } catch (e) {
                                /* Parse error - fall back to flat mode */
                                knobParamPickerHierarchy = null;
                                knobParamPickerLevel = null;
                                knobParamPickerParams = getKnobParamsForTarget(knobEditorSlot, selected.id);
                            }
                        } else {
                            /* No hierarchy - use flat mode */
                            knobParamPickerHierarchy = null;
                            knobParamPickerLevel = null;
                            knobParamPickerParams = getKnobParamsForTarget(knobEditorSlot, selected.id);
                        }
                    }
                }
            } else if (knobParamPickerHierarchy && knobParamPickerLevel) {
                /* Hierarchy mode - check if selecting nav item or param */
                const selected = knobParamPickerParams[knobParamPickerIndex];
                if (selected) {
                    if (selected.type === "nav") {
                        /* Navigate into sub-level */
                        knobParamPickerPath.push(knobParamPickerLevel);
                        knobParamPickerLevel = selected.level;
                        knobParamPickerIndex = 0;
                        knobParamPickerParams = getKnobPickerLevelItems(knobParamPickerHierarchy, knobParamPickerLevel);
                    } else if (selected.type === "param") {
                        /* Select this param */
                        applyKnobAssignment(knobParamPickerFolder, selected.key);
                    }
                }
            } else {
                /* Flat mode - selecting a param */
                const selected = knobParamPickerParams[knobParamPickerIndex];
                if (selected) {
                    applyKnobAssignment(knobParamPickerFolder, selected.key);
                }
            }
            break;
        case VIEWS.DYNAMIC_PARAM_PICKER:
            handleDynamicParamPickerSelect();
            break;
        case VIEWS.OVERTAKE_MENU:
            /* Select and load the overtake module */
            if (overtakeModules.length > 0 && selectedOvertakeModule < overtakeModules.length) {
                const selected = overtakeModules[selectedOvertakeModule];
                if (selected.id === "__get_more__") {
                    /* Open store picker for overtake modules */
                    /* Stay in overtake menu mode (1) so store picker receives input */
                    enterStorePicker('overtake');
                } else {
                    announce(`Loading ${selected.name || selected.id}`);
                    loadOvertakeModule(selected);
                }
            }
            break;
        case VIEWS.TOOLS:
            debugLog("TOOLS SELECT: idx=" + toolsMenuIndex + " count=" + toolModules.length);
            if (toolsMenuIndex >= 0 && toolsMenuIndex < toolModules.length) {
                const tool = toolModules[toolsMenuIndex];
                if (tool.type === 'divider') break;
                if (tool.id) {
                    let meta = null;
                    try {
                        if (typeof host_get_module_metadata === 'function') {
                            meta = host_get_module_metadata(tool.id);
                        }
                    } catch (err) {
                        if (typeof host_log === 'function') {
                            host_log(`tools: feedback gate metadata error for ${tool.id}: ${err}`);
                        }
                    }
                    maybeConfirmForModule(meta, (ok) => {
                        if (!ok) {
                            if (typeof host_log === 'function') {
                                host_log(`tools: declined feedback gate for ${tool.id}`);
                            }
                            needsRedraw = true;
                            return;
                        }
                        launchToolConfirmed(tool);
                    });
                    break;
                }
                /* No tool.id (shouldn't happen, but defensive): launch directly. */
                launchToolConfirmed(tool);
            }
            break;
        case VIEWS.TOOL_FILE_BROWSER:
            toolBrowserSelect();
            break;
        case VIEWS.TOOL_SET_PICKER:
            toolSetPickerSelect();
            break;
        case VIEWS.TOOL_ENGINE_SELECT:
            toolEngineConfirm();
            break;
        case VIEWS.TOOL_CONFIRM:
            wavPlayerStop();
            unloadWavPlayerDsp();
            if (toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.interactive) {
                startInteractiveTool(toolActiveTool, toolSelectedFile);
            } else {
                startToolProcess();
            }
            break;
        case VIEWS.TOOL_RESULT:
            enterToolsMenu();
            break;
        case VIEWS.TOOL_STEM_REVIEW: {
            if (toolStemReviewIndex === 0) {
                /* Action button — save selected or cancel */
                wavPlayerStop();
                unloadWavPlayerDsp();
                const kc = toolStemKept.filter(k => k).length;
                if (kc === 0) {
                    /* Cancel — discard all */
                    for (const f of toolStemFiles) {
                        try { os.remove(toolOutputDir + "/" + f); } catch (e) {}
                    }
                    try { os.remove(toolOutputDir + "/.done"); } catch (e) {}
                    try { os.remove(toolOutputDir); } catch (e) {}
                    enterToolsMenu();
                    announce("Cancelled");
                } else {
                    /* Save selected, delete unselected */
                    for (let i = 0; i < toolStemFiles.length; i++) {
                        if (!toolStemKept[i]) {
                            try { os.remove(toolOutputDir + "/" + toolStemFiles[i]); } catch (e) {}
                        }
                    }
                    try { os.remove(toolOutputDir + "/.done"); } catch (e) {}
                    const baseName = toolOutputDir.substring(toolOutputDir.lastIndexOf("/") + 1);
                    toolResultMessage = kc + " stem" + (kc > 1 ? "s" : "") + " saved to\nStems/" + baseName + "/";
                    setView(VIEWS.TOOL_RESULT);
                    needsRedraw = true;
                    announce(kc + " stem" + (kc > 1 ? "s" : "") + " saved");
                }
            } else {
                /* Toggle individual stem */
                const idx = toolStemReviewIndex - 1;
                toolStemKept[idx] = !toolStemKept[idx];
                const stemName = toolStemFiles[idx].replace(/\.wav$/i, "");
                needsRedraw = true;
                announce(stemName + (toolStemKept[idx] ? " selected" : " deselected"));
            }
            break;
        }
        case VIEWS.LFO_EDIT: {
            const items = getLfoItems();
            const item = items[selectedLfoItem];
            if (item.key === "target") {
                /* Open target picker */
                enterLfoTargetPicker();
            } else if (item.type === "action") {
                /* Other actions - ignore */
            } else if (editingLfoValue) {
                /* Exit edit mode */
                editingLfoValue = false;
                needsRedraw = true;
                announce(item.label + ", " + getLfoDisplayValue(item));
            } else {
                /* Enter edit mode */
                editingLfoValue = true;
                needsRedraw = true;
                announceParameter(item.label, getLfoDisplayValue(item));
            }
            break;
        }
        case VIEWS.LFO_TARGET_COMPONENT: {
            if (lfoTargetComponents.length > 0 && lfoCtx) {
                const comp = lfoTargetComponents[selectedLfoTargetComp];
                if (comp.key === "__clear__") {
                    lfoCtx.setParamBlocking("target", "");
                    lfoCtx.setParamBlocking("target_param", "");
                    if (!returnToSlotGridFromLfoTarget()) setView(VIEWS.LFO_EDIT);
                    announce("Target cleared");
                    needsRedraw = true;
                } else {
                    enterLfoTargetParamPicker(comp.key);
                }
            }
            break;
        }
        case VIEWS.LFO_TARGET_PARAM: {
            if (lfoTargetParams.length > 0 && lfoCtx) {
                const comp = lfoTargetComponents[selectedLfoTargetComp];
                const param = lfoTargetParams[selectedLfoTargetParam];
                lfoCtx.setParamBlocking("target", comp.key);
                lfoCtx.setParamBlocking("target_param", param.key);
                if (!returnToSlotGridFromLfoTarget()) setView(VIEWS.LFO_EDIT);
                announce("Target set: " + comp.label + " " + param.label);
                needsRedraw = true;
            }
            break;
        }
        case VIEWS.ENUM_PICKER:
            closeEnumPicker(true);
            break;
        case VIEWS.OVERTAKE_MODULE:
            /* Overtake module handles its own select input */
            break;
        /* Help only — see the jog arm. */
        case VIEWS.GLOBAL_SETTINGS:
            if (helpDetailScrollState) {
                if (isActionSelected(helpDetailScrollState)) {
                    helpDetailScrollState = null;
                    needsRedraw = true;
                    const frame = helpNavStack[helpNavStack.length - 1];
                    announce(frame.title + ", " + frame.items[frame.selectedIndex].title);
                }
            } else if (helpNavStack.length > 0) {
                const frame = helpNavStack[helpNavStack.length - 1];
                const item = frame.items[frame.selectedIndex];
                if (item.children && item.children.length > 0) {
                    helpNavStack.push({ items: item.children, selectedIndex: 0, title: item.title });
                    needsRedraw = true;
                    announce(item.title + ", " + item.children[0].title);
                } else if (item.lines && item.lines.length > 0) {
                    helpDetailScrollState = createScrollableText({
                        lines: item.lines,
                        actionLabel: "Back",
                        visibleLines: 4,
                        onActionSelected: (label) => announce(label)
                    });
                    needsRedraw = true;
                    announce(item.title + ". " + item.lines.join(". "));
                }
            }
            break;
    }
    needsRedraw = true;
}

function handleBack() {
    /* Pre-emption: in component-edit, let a module with a custom chain_ui
     * handle Back for its own internal navigation. Truthy = consumed;
     * falsy/absent falls through to the host unload logic below. */
    if (view === VIEWS.COMPONENT_EDIT && loadedModuleUi &&
        typeof loadedModuleUi.handleBack === "function" &&
        loadedModuleUi.handleBack()) {
        needsRedraw = true;
        return;
    }
    hideOverlay();
    switch (view) {
        case VIEWS.SLOTS:
            handleSlotsBack();
            break;
        case VIEWS.SLOT_SETTINGS:
            handleSlotSettingsBack();
            break;
        case VIEWS.PATCHES:
            handlePatchesBack();
            break;
        case VIEWS.PATCH_DETAIL:
            handlePatchDetailBack();
            break;
        case VIEWS.COMPONENT_PARAMS:
            handleComponentParamsBack();
            break;
        case VIEWS.PRESETS:
            handlePresetsBack();
            break;
        case VIEWS.PRESET_DETAIL:
            handlePresetDetailBack();
            break;
        case VIEWS.MASTER_FX:
            if (masterShowingNamePreview) {
                /* Cancel name preview */
                masterShowingNamePreview = false;
                needsRedraw = true;
                announce("Master FX Settings");
            } else if (masterConfirmingOverwrite) {
                /* Cancel overwrite - return to settings */
                masterConfirmingOverwrite = false;
                needsRedraw = true;
                announce("Master FX Settings");
            } else if (masterConfirmingDelete) {
                /* Cancel delete */
                masterConfirmingDelete = false;
                needsRedraw = true;
                announce("Master FX Settings");
            } else if (helpDetailScrollState) {
                helpDetailScrollState = null;
                needsRedraw = true;
                const frame = helpNavStack[helpNavStack.length - 1];
                announce(frame.title + ", " + frame.items[frame.selectedIndex].title);
            } else if (helpNavStack.length > 0) {
                helpNavStack.pop();
                needsRedraw = true;
                if (helpNavStack.length > 0) {
                    const frame = helpNavStack[helpNavStack.length - 1];
                    announce(frame.title + ", " + frame.items[frame.selectedIndex].title);
                } else if (helpReturnView === VIEWS.GLOBAL_SETTINGS) {
                    helpReturnView = null;
                    enterGlobalSettings();
                } else {
                    announce("Master FX Settings");
                }
            } else if (inMasterPresetPicker) {
                /* Exit preset picker, return to FX list */
                exitMasterPresetPicker();
                announce("Master FX");
            } else if (inMasterFxSettingsMenu) {
                /* Exit settings menu */
                inMasterFxSettingsMenu = false;
                editingMasterFxSetting = false;
                needsRedraw = true;
                announce("Master FX");
            } else if (selectingMasterFxModule) {
                /* Cancel module selection, return to chain view. Backing out of
                 * a `+` picker WRITES NOTHING AT ALL — the position it opened
                 * only ever existed in the model, and dropping it is a reload.*/
                cancelPendingChainInsert();
                selectingMasterFxModule = false;
                needsRedraw = true;
                announce("Master FX");
            } else {
                /* Exit shadow mode and return to Move */
                if (typeof shadow_request_exit === "function") {
                    shadow_request_exit();
                }
            }
            break;
        case VIEWS.CHAIN_EDIT:
            /* Exit shadow mode and return to Move */
            if (typeof shadow_request_exit === "function") {
                shadow_request_exit();
            }
            break;
        case VIEWS.COMPONENT_SELECT:
            /* Return to chain edit. A picker opened from a `+` box leaves with
             * the position it materialised — and with the RECORD of it, which
             * matters more: left set, the next ordinary swap at that same key
             * would be mistaken for the insert and rewritten as a renumber
             * of an edit that never happened. */
            cancelPendingChainInsert();
            setView(VIEWS.CHAIN_EDIT);
            announce("Chain Editor");
            needsRedraw = true;
            break;
        case VIEWS.STORE_PICKER_RESULT:
            handleStorePickerBack();
            break;
        case VIEWS.CHAIN_SETTINGS:
            if (showingNamePreview) {
                showingNamePreview = false;
                pendingSaveName = "";
                needsRedraw = true;
                announce("Chain Settings");
            } else if (confirmingOverwrite) {
                confirmingOverwrite = false;
                pendingSaveName = "";
                overwriteTargetIndex = -1;
                needsRedraw = true;
                announce("Chain Settings");
            } else if (confirmingDelete) {
                confirmingDelete = false;
                needsRedraw = true;
                announce("Chain Settings");
            } else if (editingChainSettingValue) {
                editingChainSettingValue = false;
                needsRedraw = true;
                announce("Chain Settings");
            } else {
                setView(VIEWS.CHAIN_EDIT);
                announce("Chain Editor");
                needsRedraw = true;
            }
            break;
        case VIEWS.COMPONENT_EDIT:
            /* Unload module UI and return to chain edit */
            unloadModuleUi();
            setView(VIEWS.CHAIN_EDIT);
            announce("Chain Editor");
            needsRedraw = true;
            break;
        case VIEWS.FILEPATH_BROWSER:
            closeHierarchyFilepathBrowser();
            {
                const ld = getHierarchyLevelDef();
                announce(ld && ld.label ? ld.label : "Parameters");
            }
            needsRedraw = true;
            break;
        case VIEWS.CANVAS:
            closeCanvasPreview(true);
            announce("Hierarchy Editor");
            needsRedraw = true;
            break;
        case VIEWS.HIERARCHY_EDITOR: {
            /* Helper: announce current hierarchy level label after navigation */
            const announceHierLevel = () => {
                const ld = getHierarchyLevelDef();
                announce(ld && ld.label ? ld.label : "Parameters");
            };
            if (hierEditorEditMode) {
                /* Exit param edit mode first */
                hierEditorEditMode = false;
                resetHierarchyEditState();
                needsRedraw = true;
                if (paramEditorOpenedFromGrid) {
                    /* The grid opened this one param; closing it goes back to
                     * the grid, not to a list the user never asked for. */
                    returnToParamPagesFromEditor();
                } else {
                    announceHierLevel();
                }
            } else if (paramEditorOpenedFromGrid) {
                /* Not in edit mode, but we still got here from the grid — a
                 * filepath/canvas/string editor is its own VIEW, so it has
                 * already returned here by the time Back is pressed again. */
                returnToParamPagesFromEditor();
            } else if (hierEditorPresetEditMode) {
                /* Exit preset edit mode - return to preset browser */
                hierEditorPresetEditMode = false;
                needsRedraw = true;
                announceHierLevel();
            } else if (hierEditorChildIndex >= 0) {
                const levelDef = getHierarchyLevelDef();
                if (levelDef && levelDef.child_prefix) {
                    /* Return to child selector list */
                    hierEditorChildIndex = -1;
                    if (hierEditorPath.length > 0) {
                        hierEditorPath.pop();
                    }
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                    invalidateKnobContextCache();
                    needsRedraw = true;
                    announceHierLevel();
                }
            } else if (hierEditorPath.length > 0) {
                /* Go back to parent level */
                hierEditorPath.pop();

                /* Check if current level is a mode (top-level) - go back to mode selection */
                if (hierEditorHierarchy.modes && hierEditorHierarchy.modes.includes(hierEditorLevel)) {
                    hierEditorLevel = null;  // Return to mode selection
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                    needsRedraw = true;
                    announce("Mode Selection");
                } else {
                    const levelDef = getHierarchyLevelDef();
                    if (levelDef && levelDef.child_prefix) {
                        hierEditorChildIndex = -1;
                        hierEditorChildCount = 0;
                        hierEditorChildLabel = "";
                    }
                    /* Find the parent level that has children pointing to current level,
                     * or has a navigation param with level pointing to current level */
                    const levels = hierEditorHierarchy.levels;
                    let parentLevel = "root";
                    let foundParent = false;
                    for (const [name, def] of Object.entries(levels)) {
                        /* Check children property */
                        if (def.children === hierEditorLevel) {
                            parentLevel = name;
                            foundParent = true;
                            break;
                        }
                        /* Check params array for navigation params with level property */
                        if (def.params && Array.isArray(def.params)) {
                            for (const p of def.params) {
                                if (p && typeof p === "object" && p.level === hierEditorLevel) {
                                    parentLevel = name;
                                    foundParent = true;
                                    break;
                                }
                            }
                            if (foundParent) break;
                        }
                    }
                    hierEditorLevel = parentLevel;
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                    needsRedraw = true;
                    announceHierLevel();
                }
            } else {
                /* At root level - exit hierarchy editor */
                const wasMasterFx = hierEditorIsMasterFx;
                exitHierarchyEditor();
                announce(wasMasterFx ? "Master FX" : "Chain Editor");
            }
            break;
        }
        case VIEWS.KNOB_EDITOR:
            /* Return to chain settings */
            setView(VIEWS.CHAIN_SETTINGS);
            announce("Chain Settings");
            needsRedraw = true;
            break;
        case VIEWS.KNOB_PARAM_PICKER:
            if (knobParamPickerFolder !== null) {
                if (knobParamPickerHierarchy && knobParamPickerPath.length > 0) {
                    /* In hierarchy mode with path - go back up one level */
                    knobParamPickerLevel = knobParamPickerPath.pop();
                    knobParamPickerIndex = 0;
                    knobParamPickerParams = getKnobPickerLevelItems(knobParamPickerHierarchy, knobParamPickerLevel);
                    needsRedraw = true;
                    announce("Knob Target");
                } else {
                    /* At top of hierarchy or flat mode - return to target selection */
                    knobParamPickerFolder = null;
                    knobParamPickerIndex = 0;
                    knobParamPickerParams = [];
                    knobParamPickerHierarchy = null;
                    knobParamPickerLevel = null;
                    knobParamPickerPath = [];
                    needsRedraw = true;
                    announce("Knob Target");
                }
            } else {
                /* Return to knob editor */
                setView(VIEWS.KNOB_EDITOR);
                announce("Knob Editor");
                needsRedraw = true;
            }
            break;
        case VIEWS.DYNAMIC_PARAM_PICKER:
            if (dynamicPickerMode === "param" && dynamicPickerMeta && dynamicPickerMeta.picker_type === "parameter_picker") {
                dynamicPickerMode = "target";
                const targetIdx = dynamicPickerTargets.findIndex(t => t.id === dynamicPickerSelectedTarget);
                dynamicPickerIndex = targetIdx >= 0 ? targetIdx : 0;
                needsRedraw = true;
                announce("Select target component");
            } else {
                closeDynamicParamPicker("Hierarchy Editor");
            }
            break;
        case VIEWS.OVERTAKE_MENU:
            /* Exit overtake menu and return to Move */
            debugLog("OVERTAKE_MENU back: exiting to Move");
            if (typeof shadow_set_overtake_mode === "function") {
                shadow_set_overtake_mode(0);
            }
            setView(VIEWS.SLOTS);
            if (typeof shadow_request_exit === "function") {
                shadow_request_exit();
            }
            needsRedraw = true;
            break;
        case VIEWS.TOOLS:
            /* Exit Tools menu → exit shadow mode */
            if (typeof shadow_request_exit === "function") {
                shadow_request_exit();
            }
            break;
        case VIEWS.TOOL_FILE_BROWSER:
            toolBrowserBack();
            break;
        case VIEWS.TOOL_SET_PICKER:
            enterToolsMenu();
            break;
        case VIEWS.TOOL_ENGINE_SELECT:
            /* Return to file browser */
            setView(VIEWS.TOOL_FILE_BROWSER);
            needsRedraw = true;
            announce("Back to files");
            break;
        case VIEWS.TOOL_CONFIRM:
            wavPlayerStop();
            unloadWavPlayerDsp();
            /* Return to set picker, engine selection, or file browser */
            if (toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.set_picker) {
                setView(VIEWS.TOOL_SET_PICKER);
                needsRedraw = true;
                announce("Back to sets");
            } else if (toolActiveTool && toolActiveTool.tool_config &&
                toolActiveTool.tool_config.engines && toolActiveTool.tool_config.engines.length > 1) {
                enterToolEngineSelect();
                announce("Back to engine selection");
            } else {
                setView(VIEWS.TOOL_FILE_BROWSER);
                needsRedraw = true;
                announce("Cancelled, back to files");
            }
            break;
        case VIEWS.TOOL_PROCESSING:
            cancelToolProcess();
            break;
        case VIEWS.TOOL_RESULT:
            enterToolsMenu();
            break;
        case VIEWS.TOOL_STEM_REVIEW: {
            wavPlayerStop();
            if (toolStemReviewIndex === 0) {
                /* Already at action button — back exits, discards all */
                unloadWavPlayerDsp();
                for (const f of toolStemFiles) {
                    try { os.remove(toolOutputDir + "/" + f); } catch (e) {}
                }
                try { os.remove(toolOutputDir + "/.done"); } catch (e) {}
                try { os.remove(toolOutputDir); } catch (e) {}
                enterToolsMenu();
                announce("Cancelled");
            } else {
                /* Jump to action button first */
                toolStemReviewIndex = 0;
                needsRedraw = true;
                const keptCount = toolStemKept.filter(k => k).length;
                if (keptCount === 0) announce("Cancel");
                else if (keptCount === toolStemFiles.length) announce("Save All");
                else announce("Save " + keptCount + " stems");
            }
            break;
        }
        case VIEWS.LFO_EDIT:
            if (editingLfoValue) {
                editingLfoValue = false;
                needsRedraw = true;
                const lfoItems = getLfoItems();
                const curItem = lfoItems[selectedLfoItem];
                announceMenuItem(curItem.label, getLfoDisplayValue(curItem));
            } else {
                setView(lfoCtx ? lfoCtx.returnView : VIEWS.CHAIN_SETTINGS);
                announce(lfoCtx ? lfoCtx.returnAnnounce : "Chain Settings");
                needsRedraw = true;
            }
            break;
        case VIEWS.LFO_TARGET_COMPONENT:
            /* Opened from a grid cell? Go back to the grid, not to the list
             * editor screen the user never opened. */
            if (returnToSlotGridFromLfoTarget()) break;
            setView(VIEWS.LFO_EDIT);
            announce(lfoCtx ? lfoCtx.title : "LFO");
            needsRedraw = true;
            break;
        case VIEWS.LFO_TARGET_PARAM:
            setView(VIEWS.LFO_TARGET_COMPONENT);
            if (lfoTargetComponents.length > 0) {
                announce("Target, " + lfoTargetComponents[selectedLfoTargetComp].label);
            }
            needsRedraw = true;
            break;
        case VIEWS.ENUM_PICKER:
            /* Nothing was written on the way in or while scrolling, so cancel
             * is simply not writing now. */
            closeEnumPicker(false);
            break;
        case VIEWS.OVERTAKE_MODULE:
            /* Overtake module handles its own back input.
             * Use Shift+Vol+Jog Click to exit overtake mode. */
            break;
        /* Help only — see the jog arm. Popping the last frame leaves the stack
         * empty and maybeReturnToGlobalGrid takes it from there; Back at the
         * top of the SETTINGS themselves is the page chrome's exit intent
         * (chrome.onExit), not this. */
        case VIEWS.GLOBAL_SETTINGS:
            if (helpDetailScrollState) {
                helpDetailScrollState = null;
                needsRedraw = true;
                const frame = helpNavStack[helpNavStack.length - 1];
                announce(frame.title + ", " + frame.items[frame.selectedIndex].title);
            } else if (helpNavStack.length > 0) {
                helpNavStack.pop();
                needsRedraw = true;
                if (helpNavStack.length > 0) {
                    const frame = helpNavStack[helpNavStack.length - 1];
                    announce(frame.title + ", " + frame.items[frame.selectedIndex].title);
                }
            } else if (typeof shadow_request_exit === "function") {
                /* Nothing left on this view to back out of. */
                shadow_request_exit();
            }
            break;
    }
}

/* Handle knob turn for global slot knob mappings - accumulate delta and refresh overlay
 * Called when no component is selected (entire slot highlighted) */
function handleKnobTurn(knobIndex, delta) {
    if (pendingKnobIndex !== knobIndex) {
        /* Different knob - reset accumulator */
        pendingKnobIndex = knobIndex;
        pendingKnobDelta = delta;
    } else {
        /* Same knob - accumulate delta */
        pendingKnobDelta += delta;
    }
    pendingKnobRefresh = true;
    needsRedraw = true;
}

/* Refresh knob overlay value - called once per tick to avoid display lag
 * Also applies accumulated delta for global slot knob adjustments */
function refreshPendingKnobOverlay() {
    if (!pendingKnobRefresh || pendingKnobIndex < 0) return;

    /* Use track-selected slot (what knobs actually control) */
    let targetSlot = 0;
    if (typeof shadow_get_selected_slot === "function") {
        targetSlot = shadow_get_selected_slot();
    }

    /* Refresh knob mappings if slot changed */
    if (lastKnobSlot !== targetSlot) {
        fetchKnobMappings(targetSlot);
        invalidateKnobContextCache();  /* Clear stale contexts when target slot changes */
    }

    /* Apply accumulated delta to global slot knob mapping */
    if (pendingKnobDelta !== 0) {
        const adjustKey = `knob_${pendingKnobIndex + 1}_adjust`;
        const deltaStr = pendingKnobDelta > 0 ? `+${pendingKnobDelta}` : `${pendingKnobDelta}`;
        setSlotParam(targetSlot, adjustKey, deltaStr);
        pendingKnobDelta = 0;
    }

    /* Get current value from DSP (only once per frame) */
    const newValue = getSlotParam(targetSlot, `knob_${pendingKnobIndex + 1}_value`);
    if (knobMappings[pendingKnobIndex]) {
        knobMappings[pendingKnobIndex].value = newValue || "-";
    }

    /* Show the feedback (card in the chain editor, centred box elsewhere).
     * The knob index is pendingKnobIndex, which this function CLEARS below —
     * read it here, while it is still the knob this refresh is about. */
    const mapping = knobMappings[pendingKnobIndex];
    if (mapping && mapping.name) {
        const displayName = `S${targetSlot + 1}: ${mapping.name}`;
        /* Announced with the slot, drawn without it — see showKnobFeedback. */
        showKnobFeedback(pendingKnobIndex, displayName, mapping.value, undefined, mapping.name);
    } else {
        /* No mapping for this knob */
        showKnobFeedback(pendingKnobIndex, `Knob ${pendingKnobIndex + 1}`, "not mapped");
    }

    pendingKnobRefresh = false;
    pendingKnobIndex = -1;
}

/* drawSlots() -> shadow_ui_slots.mjs */

/* getMasterFxDisplayName() -> shadow_ui_master_fx.mjs */

/* drawSlotSettings() -> shadow_ui_slots.mjs */

/* drawPatches(), drawPatchDetail(), drawComponentParams() -> shadow_ui_patches.mjs */

/* Draw horizontal chain editor with boxed icons */
function drawChainEdit() {
    clear_screen();
    const dirtyMark = slotDirtyCache[selectedSlot] ? "*" : "";
    const patchName = isExistingPreset(selectedSlot) ? slots[selectedSlot].name : null;
    const headerText = patchName
        ? truncateText(`${dirtyMark}${patchName}`, 20)
        : `${dirtyMark}Slot ${selectedSlot + 1}`;
    /*
     * The knob-grid chrome. This is the screen you pass through to reach every
     * other one, so it was the last place still announcing itself in a
     * different font with no footer — you left the editor and the furniture
     * changed. The body is a diagram rather than a list, so only the bands
     * move: header at the top, hints at the bottom, and the boxes refitted
     * between them.
     */
    /*
     * The same primitive set shadow_ui_param_pages.mjs hands the knob grid: the
     * knob card draws real widgets, and the arc knob takes a C path when
     * draw_arc is there and a slow JS fallback when it is not. Each is probed
     * because the harness and the older host builds do not have all of them.
     */
    const movy = {
        fillRect: fill_rect, print, textWidth: text_width, setPixel: set_pixel,
        line: typeof draw_line === "function" ? draw_line : undefined,
        fillCircle: typeof fill_circle === "function" ? fill_circle : undefined,
        drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
        drawArc: typeof draw_arc === "function" ? draw_arc : undefined,
    };

    /* The chain config, reloaded from the DSP only when something has made it
     * stale — see chainConfigFresh. This was an unconditional reload per frame,
     * `3 + <chain length>` IPC round trips at ~2.8ms, which is several frame
     * budgets on a short chain and grows with a long one. An external load
     * (patch restore, the shim loading a slot) is caught by the periodic
     * refreshSlotModuleSignature, which is what that reload was really standing
     * in for. */
    const cfg = ensureChainConfigFresh(selectedSlot);
    const chainSelected = selectedChainComponent === -1;

    /*
     * The header's right-hand side names the SYNTH, not the screen.
     *
     * Once the chain is longer than the five boxes that fit, the diagram
     * scrolls and the synth can be off-screen — and it is the one position
     * every chain has and the only landmark in a row of two-letter
     * abbreviations. "CHAIN" was already obvious from everything else on the
     * screen; which synth you are building on is not. Cached, so this is not
     * an IPC read per frame.
     */
    const synthMod = cfg.synth && cfg.synth.module;
    const headerRight = synthMod
        ? (getSlotParamCached(selectedSlot, "synth:name", synthMod) || synthMod)
        : "CHAIN";

    const BOX_Y = DIAGRAM_Y;

    /* Draw slot indicators - 4 marks in left margin, spanning from below header to footer */
    const INDICATOR_X = 0;
    const INDICATOR_W = 4;
    const INDICATOR_GAP = 1;
    const INDICATOR_START_Y = BOX_Y;  // same margin below title rule as boxes
    const INDICATOR_END_Y = MOVY_RULE_Y;   // same margin above the footer rule
    const INDICATOR_H = Math.floor((INDICATOR_END_Y - INDICATOR_START_Y - 3 * INDICATOR_GAP) / 4);
    for (let s = 0; s < 4; s++) {
        const iy = INDICATOR_START_Y + s * (INDICATOR_H + INDICATOR_GAP);
        if (s === selectedSlot) {
            fill_rect(INDICATOR_X, iy, INDICATOR_W, INDICATOR_H, 1);
        } else {
            draw_rect(INDICATOR_X, iy, INDICATOR_W, INDICATOR_H, 1);
        }
    }

    /*
     * Which components an LFO is pointed at — shared with Master FX, see
     * chainLfoTargetMap. Four IPC reads, fixed.
     */
    const target = slotChainTarget(selectedSlot);
    const lfoTargets = chainLfoTargetMap(target);  /* key -> {lfo1: bool, lfo2: bool} */

    /*
     * The diagram itself -> shared/chain_diagram.mjs, which is pure and can
     * therefore be rendered into a framebuffer and inspected (see
     * tests/host/test_chain_diagram.sh). Everything it needs that costs an IPC
     * read is fetched HERE, once per box, so the module stays testable and the
     * read count stays visible in one place.
     */
    const comps = slotChainComponents(selectedSlot);
    drawChainDiagram(movy, comps, selectedChainComponent, {
        allSelected: chainSelected,
        abbrev: (comp) => {
            if (comp.kind === "add") return "+";
            if (comp.kind === "settings") return "*";
            const moduleData = getChainComponentModule(cfg, comp.key);
            return moduleData ? getModuleAbbrev(moduleData.module) : "--";
        },
        marks: (comp) => {
            const lfo = lfoTargets[comp.key];
            /* One read per DRAWN module box — five at most, because that is
             * how many fit — rather than one per position in the chain. */
            const bypassed = chainComponentBypassed(target, comp.key);
            if (!lfo && !bypassed) return null;
            return { bypassed, lfo1: lfo && lfo.lfo1, lfo2: lfo && lfo.lfo2 };
        },
    });

    /* What the two bands under the boxes say: the selected component's label,
     * and the module/preset it holds. drawChainEditorBands draws them — the
     * geometry and the centring are shared with Master FX. */
    const selectedComp = chainSelected ? null : comps[selectedChainComponent];
    const label = chainSelected ? "Chain" : (selectedComp ? selectedComp.label : "");

    let infoLine = "";
    if (chainSelected) {
        /* Show patch name when chain is selected */
        infoLine = slots[selectedSlot]?.name || "(no patch)";
    } else if (selectedComp && selectedComp.kind === "add") {
        infoLine = selectedComp.section === "midiFx" ? "New MIDI effect" : "New effect";
    } else if (selectedComp && selectedComp.key !== "settings") {
        const moduleData = getChainComponentModule(cfg, selectedComp.key);
        if (moduleData) {
            /* Get display name from DSP if available */
            const prefix = getComponentParamPrefix(selectedComp.key);
            /* Cached: these were three IPC round-trips per frame. */
            const mid = moduleData.module;
            let displayName = getSlotParamCached(selectedSlot, `${prefix}:name`, mid) || mid;
            /* Friendly name for RNBO pack entries */
            if (displayName === moduleData.module) {
                if (displayName.startsWith("rnbo-synth-")) displayName = displayName.substring(11) + " (RNBO)";
                else if (displayName.startsWith("rnbo-fx-")) displayName = displayName.substring(8) + " (RNBO)";
            }
            const preset = getSlotParamCached(selectedSlot, `${prefix}:preset_name`, mid) ||
                           getSlotParamCached(selectedSlot, `${prefix}:preset`, mid) || "";
            infoLine = preset ? `${displayName} (${truncateText(preset, 8)})` : displayName;
        } else {
            infoLine = "(empty)";
        }
    } else if (selectedComp && selectedComp.key === "settings") {
        infoLine = "Configure slot";
    }

    /*
     * The bands: header, label, info, footer -> shared/chain_editor_chrome.mjs,
     * the same call Master FX makes, so the two editors cannot drift apart on
     * their furniture again.
     *
     * Back leaves shadow mode entirely from here — one of the two screens where
     * it does, Master FX being the other — so the footer says EXIT rather than
     * OUT.
     *
     * The hints follow the modifier because Shift silently repurposes the jog:
     * a reorder gesture with a footer still reading SEL is a gesture nobody
     * finds. Shift drops CLK, which is unchanged, rather than adding a fourth
     * pair — three pairs only fit while every word is <= 4 characters, and
     * drawFooter drops what does not fit rather than squeezing it.
     *
     * isShiftHeld() is a read of the control SHM, not an IPC round trip, so
     * this is a per-frame call that costs nothing measurable.
     */
    drawChainEditorBands(movy, {
        headerLeft: headerText,
        headerRight,
        label,
        info: infoLine,
        hints: isShiftHeld() ? shiftHintsFor(selectedComp)
                             : CHAIN_HINTS_AT_REST,
    });

    /*
     * The card last, over everything — it is a modal. Every value it draws was
     * read on touch-down, so this costs no IPC. See knobCardOpen.
     */
    const card = knobCardDrawState();
    if (card) drawKnobCard(movy, card);
}

/* Draw component module selection list */
/*
 * The module picker, in the knob-grid chrome.
 *
 * You reach this from a component and you go straight back to it, so it is
 * part of that flow and should look like it: same header band and face, same
 * list rect, same footer. It used to wear the older list chrome — a different
 * header font, no footer at all — which made choosing a module feel like
 * leaving the editor rather than a step inside it.
 *
 * The DRAWING is drawChainPicker, shared with Master FX. Master FX kept the
 * older chrome after this screen moved, so the two pickers chose from the same
 * chainMoveEntries-built list and then looked like different products — see the
 * comment on drawChainPicker for why that had to stop being possible.
 *
 * No bank bar: there is no page set here, and drawing one would claim a
 * position among pages that do not exist.
 */
function drawComponentSelect() {
    clear_screen();
    const comp = slotChainComponents(selectedSlot)[selectedChainComponent];
    const ctx = { fillRect: fill_rect, print, textWidth: text_width };

    const current = comp ? getChainComponentModule(chainConfigs[selectedSlot], comp.key) : null;

    drawChainPicker(ctx, {
        /* Which chain, then which position in it — the slot editor's own header
         * grammar, one level deeper. */
        headerLeft: `S${selectedSlot + 1} > ${comp ? comp.label : "Module"}`,
        entries: availableModules,
        index: selectedModuleIndex,
        currentId: current ? current.module : null,
        emptyMessage: "No modules available",
    });
}

/* ===== Store Picker Drawing Functions ===== */



/* drawStorePickerResult() -> shadow_ui_store.mjs */

/* Draw analytics opt-out prompt (shown on first run) */
function drawAnalyticsPrompt() {
    clear_screen();
    drawHeader('Usage Statistics');

    print(2, 14, 'Send anonymous usage', 1);
    print(2, 24, 'data to help improve', 1);
    print(2, 34, 'Schwung?', 1);

    /* Yes / No selection */
    const yesPrefix = analyticsPromptSelection === 0 ? '> ' : '  ';
    const noPrefix = analyticsPromptSelection === 1 ? '> ' : '  ';
    print(30, 48, yesPrefix + 'Yes', 1);
    print(75, 48, noPrefix + 'No', 1);

    drawFooter('Click to confirm');
}

/* Draw component edit view (presets, params) */
function drawComponentEdit() {
    clear_screen();

    /* Get component info */
    const moduleData = getChainComponentModule(chainConfigs[selectedSlot], editingComponentKey);
    const moduleName = moduleData ? moduleData.module.toUpperCase() : "Unknown";

    /* Get display name from DSP if available */
    const prefix = getComponentParamPrefix(editingComponentKey);
    const cmpMid = moduleData ? moduleData.module : moduleName;
    const displayName = getSlotParamCached(selectedSlot, `${prefix}:name`, cmpMid) || moduleName;

    /* Build header: S#: Module: Bank (preset selection shows bank/soundfont) */
    const abbrev = moduleData ? getModuleAbbrev(moduleData.module) : moduleName;
    const bankName = getSlotParamCached(selectedSlot, `${prefix}:bank_name`, cmpMid) || displayName;
    const headerText = truncateText(`S${selectedSlot + 1}: ${abbrev}: ${bankName}`, 24);
    drawHeader(headerText);

    const centerY = 32;

    /* Check for load error */
    const synthError = getSlotParam(selectedSlot, "synth_error");
    if (editingComponentKey === "synth" && synthError && synthError.length > 0) {
        const titleText = "ERROR LOADING";
        const titleX = Math.floor((SCREEN_WIDTH - titleText.length * 5) / 2);
        print(titleX, centerY - 10, titleText, 1);

        const errorText = truncateText(synthError, 24);
        const errorX = Math.floor((SCREEN_WIDTH - errorText.length * 5) / 2);
        print(errorX, centerY + 2, errorText, 1);

        drawFooter(["Back: done"]);
        return;
    }

    /* Re-fetch preset count if zero (module may still be loading) */
    if (editComponentPresetCount === 0) {
        const countStr = getSlotParam(selectedSlot, `${prefix}:preset_count`);
        const newCount = countStr ? parseInt(countStr) : 0;
        if (newCount > 0) {
            editComponentPresetCount = newCount;
            const presetStr = getSlotParam(selectedSlot, `${prefix}:preset`);
            editComponentPreset = presetStr ? parseInt(presetStr) : 0;
            editComponentPresetName = getSlotParam(selectedSlot, `${prefix}:preset_name`) || "";
        }
    }

    if (editComponentPresetCount > 0) {
        /* Show preset number */
        const presetNum = `${editComponentPreset + 1}/${editComponentPresetCount}`;
        const numX = Math.floor((SCREEN_WIDTH - presetNum.length * 5) / 2);
        print(numX, centerY - 8, presetNum, 1);

        /* Show preset name */
        const name = truncateText(editComponentPresetName || "(unnamed)", 22);
        const nameX = Math.floor((SCREEN_WIDTH - name.length * 5) / 2);
        print(nameX, centerY + 4, name, 1);

        /* Draw navigation arrows */
        print(4, centerY - 2, "<", 1);
        print(SCREEN_WIDTH - 10, centerY - 2, ">", 1);
    } else {
        /* No presets - show message */
        const msg = "No presets";
        const msgX = Math.floor((SCREEN_WIDTH - msg.length * 5) / 2);
        print(msgX, centerY, msg, 1);
    }

    /* Show hint at bottom */
    drawFooter(["Back: done", "Jog: preset"]);
}

/* Draw chain settings view */
/* drawChainSettings() -> shadow_ui_settings.mjs */

/* Draw knob assignment editor - list of 8 knobs with their assignments */
function drawKnobEditor() {
    clear_screen();
    drawHeader(`S${knobEditorSlot + 1} Knobs`);

    /* List all 8 knobs */
    const items = [];
    for (let i = 0; i < NUM_KNOBS; i++) {
        items.push({
            type: "knob",
            label: `Knob ${i + 1}`,
            assignment: knobEditorAssignments[i]
        });
    }

    /* No editMode: the knob editor has no in-place editing state -- a click
     * opens the param picker instead. */
    drawMenuList({
        items,
        selectedIndex: knobEditorIndex,
        getLabel: (item) => item.label,
        getValue: (item) => item.type === "knob"
            ? truncateText(getKnobAssignmentLabel(item.assignment), 12)
            : "",
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true
    });

    drawFooter(["Back: cancel", "Click: edit"]);
}

/* Draw param picker - select target then param for knob assignment */
function drawKnobParamPicker() {
    clear_screen();
    const knobNum = knobEditorIndex + 1;

    if (knobParamPickerFolder === null) {
        /* Main view - show available targets */
        drawHeader(`Knob ${knobNum} Target`);

        const targets = getKnobTargets(knobEditorSlot);
        drawMenuList({
            items: targets,
            selectedIndex: knobParamPickerIndex,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => item.name,
            getValue: () => ""
        });

        drawFooter(["Back: cancel", "Click: select"]);
    } else if (knobParamPickerHierarchy && knobParamPickerLevel) {
        /* Hierarchy mode - show current level */
        const levelDef = knobParamPickerHierarchy.levels[knobParamPickerLevel];
        const levelLabel = levelDef && levelDef.label ? levelDef.label : knobParamPickerLevel;
        drawHeader(`K${knobNum}: ${levelLabel}`);

        drawMenuList({
            items: knobParamPickerParams,
            selectedIndex: knobParamPickerIndex,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => item.type === "nav" ? `${item.label} >` : item.label,
            getValue: () => ""
        });

        const hasNav = knobParamPickerParams.some(p => p.type === "nav");
        drawFooter(hasNav ? ["Back: up", "Click: select"] : ["Back: up", "Click: assign"]);
    } else {
        /* Flat mode - show params for selected target */
        drawHeader(`Knob ${knobNum} Param`);

        /* Get params for this target */
        if (knobParamPickerParams.length === 0) {
            knobParamPickerParams = getKnobParamsForTarget(knobEditorSlot, knobParamPickerFolder);
        }

        drawMenuList({
            items: knobParamPickerParams,
            selectedIndex: knobParamPickerIndex,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => item.label,
            getValue: () => ""
        });

        drawFooter(["Back: targets", "Click: assign"]);
    }
}

function drawDynamicParamPicker() {
    clear_screen();

    if (dynamicPickerMode === "param") {
        drawHeader("Select Param");
        drawMenuList({
            items: dynamicPickerParams,
            selectedIndex: dynamicPickerIndex,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => item.label || item.key || "",
            getValue: () => ""
        });
        drawFooter(["Back: targets", "Click: select"]);
    } else {
        drawHeader("Select Target");
        drawMenuList({
            items: dynamicPickerTargets,
            selectedIndex: dynamicPickerIndex,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => item.label || item.id || "",
            getValue: () => ""
        });
        drawFooter(["Back: cancel", "Click: select"]);
    }
}

/* ========== Master Preset Draw Functions ========== */

/* drawMasterNamePreview, drawMasterConfirmOverwrite, drawMasterConfirmDelete,
 * drawMasterFxSettingsMenu -> shadow_ui_master_fx.mjs */

/* ========== Help Viewer Draw Functions ========== */

function drawHelpList() {
    const frame = helpNavStack[helpNavStack.length - 1];
    drawHeader(truncateText(frame.title, 18));

    drawMenuList({
        items: frame.items,
        selectedIndex: frame.selectedIndex,
        getLabel: (item) => item.title,
        getValue: () => "",
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true
    });

    const backTarget = helpNavStack.length > 1
        ? helpNavStack[helpNavStack.length - 2].title
        : "Settings";
    drawFooter("Back: " + backTarget);
}

function drawHelpDetail() {
    const frame = helpNavStack[helpNavStack.length - 1];
    const item = frame.items[frame.selectedIndex];
    drawHeader(truncateText(item.title, 18));

    if (helpDetailScrollState) {
        drawScrollableText({
            state: helpDetailScrollState,
            topY: LIST_TOP_Y,
            bottomY: FOOTER_RULE_Y,
            actionY: -1
        });
    }

    const backTarget = helpNavStack.length > 1
        ? helpNavStack[helpNavStack.length - 2].title
        : "Settings";
    drawFooter("Back: " + backTarget);
}

/* ========== End Master Preset Draw Functions ========== */

/* drawGlobalSettings() -> shadow_ui_settings.mjs */

/* drawMasterFx(), drawMasterFxModuleSelect() -> shadow_ui_master_fx.mjs */

/* ============================================================================
 * Populate shared context for extracted view modules.
 * Modules access ctx properties *inside function bodies*, not at import time.
 * ============================================================================ */
(function populateCtx() {
    /* Shared state - use defineProperty so modules read/write the live variables */
    Object.defineProperty(_ctx, 'selectedSlot', {
        get() { return selectedSlot; }, set(v) { selectedSlot = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'slots', {
        get() { return slots; }, set(v) { slots = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'needsRedraw', {
        get() { return needsRedraw; }, set(v) { needsRedraw = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'slotDirtyCache', {
        get() { return slotDirtyCache; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'patches', {
        get() { return patches; }, set(v) { patches = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'selectedPatch', {
        get() { return selectedPatch; }, set(v) { selectedPatch = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'view', {
        get() { return view; }, set(v) { view = v; }, enumerable: true
    });

    /* Constants */
    _ctx.VIEWS = VIEWS;
    _ctx.DEFAULT_SLOTS = DEFAULT_SLOTS;
    _ctx.KNOB_BASE_STEP_FLOAT = KNOB_BASE_STEP_FLOAT;

    /* Master FX state (read/write) */
    Object.defineProperty(_ctx, 'masterFxConfig', {
        get() { return masterFxConfig; }, set(v) { masterFxConfig = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'MASTER_FX_OPTIONS', {
        get() { return MASTER_FX_OPTIONS; }, set(v) { MASTER_FX_OPTIONS = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'selectedMasterFxComponent', {
        get() { return selectedMasterFxComponent; }, set(v) { selectedMasterFxComponent = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'selectingMasterFxModule', {
        get() { return selectingMasterFxModule; }, set(v) { selectingMasterFxModule = v; }, enumerable: true
    });

    /* Master FX state (read-only for module) */
    Object.defineProperty(_ctx, 'masterShowingNamePreview', {
        get() { return masterShowingNamePreview; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'masterConfirmingOverwrite', {
        get() { return masterConfirmingOverwrite; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'masterConfirmingDelete', {
        get() { return masterConfirmingDelete; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'helpDetailScrollState', {
        get() { return helpDetailScrollState; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'helpNavStack', {
        get() { return helpNavStack; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'inMasterPresetPicker', {
        get() { return inMasterPresetPicker; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'inMasterFxSettingsMenu', {
        get() { return inMasterFxSettingsMenu; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'currentMasterPresetName', {
        get() { return currentMasterPresetName; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'selectedMasterFxSetting', {
        get() { return selectedMasterFxSetting; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'masterFxPickerItems', {
        get() { return masterFxPickerItems; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'selectedMasterFxModuleIndex', {
        get() { return selectedMasterFxModuleIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'masterPresets', {
        get() { return masterPresets; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'selectedMasterPresetIndex', {
        get() { return selectedMasterPresetIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'masterPendingSaveName', {
        get() { return masterPendingSaveName; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'masterNamePreviewIndex', {
        get() { return masterNamePreviewIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'masterConfirmIndex', {
        get() { return masterConfirmIndex; }, enumerable: true
    });

    Object.defineProperty(_ctx, 'MASTER_FX_CHAIN_COMPONENTS', {
        /* A GETTER: the list is derived from the chain and changes length on
         * every shape edit, so a snapshot taken once at init would be the
         * eight fixed boxes this step exists to remove. */
        get() { return masterFxChainComponents(); }, enumerable: true
    });
    /* The chain target and the two draw helpers that take one. The Master FX
     * view module draws its diagram markers from the SAME code the slot chain
     * editor does, so an LFO marker or a bypass "B" cannot appear on one
     * screen and not the other. */
    _ctx.MASTER_CHAIN_TARGET = MASTER_CHAIN_TARGET;
    _ctx.chainLfoTargetMap = (...args) => chainLfoTargetMap(...args);
    _ctx.chainComponentBypassed = (...args) => chainComponentBypassed(...args);
    _ctx.MASTER_FX_SLOTS = MASTER_FX_SLOTS;
    /* The knob card's draw state — null unless a knob is being touched or has
     * just been turned. Costs no IPC: everything in it was read on touch-down.
     * Master FX draws the SAME card the slot chain editor does (4b). */
    _ctx.knobCardDrawState = () => knobCardDrawState();

    /* Utility functions */
    _ctx.setView = setView;
    _ctx.getSlotParam = getSlotParam;
    _ctx.setSlotParam = setSlotParam;
    _ctx.updateFocusedSlot = updateFocusedSlot;
    _ctx.getMasterFxDisplayName = () => getMasterFxDisplayName();
    _ctx.saveSlotsToConfig = (...args) => saveSlotsToConfig(...args);
    _ctx.fetchKnobMappings = (...args) => fetchKnobMappings(...args);
    _ctx.invalidateKnobContextCache = (...args) => invalidateKnobContextCache(...args);
    _ctx.loadChainConfigFromSlot = (...args) => loadChainConfigFromSlot(...args);
    _ctx.getSlotStateWithRetry = (...args) => getSlotStateWithRetry(...args);
    _ctx.showWarning = (...args) => showWarning(...args);

    /* Master FX functions */
    _ctx.scanForAudioFxModules = (...args) => scanForAudioFxModules(...args);
    _ctx.loadMasterFxChainConfig = (...args) => loadMasterFxChainConfig(...args);
    _ctx.ensureMasterFxConfigFresh = () => ensureMasterFxConfigFresh();
    _ctx.isShiftHeld = () => isShiftHeld();
    _ctx.getMasterFxSlotModule = (...args) => getMasterFxSlotModule(...args);
    _ctx.getMasterFxParam = (...args) => getMasterFxParam(...args);
    _ctx.getModuleAbbrev = (...args) => getModuleAbbrev(...args);
    _ctx.getModuleDisplayName = (...args) => getModuleDisplayName(...args);
    _ctx.isTextEntryActive = () => isTextEntryActive();
    _ctx.drawTextEntry = () => drawTextEntry();
    _ctx.drawHelpDetail = () => drawHelpDetail();
    _ctx.drawHelpList = () => drawHelpList();
    _ctx.getMasterFxSettingsItems = () => getMasterFxSettingsItems();
    _ctx.getMasterFxSettingValue = (...args) => getMasterFxSettingValue(...args);

    /* Tools state */
    Object.defineProperty(_ctx, 'toolModules', {
        get() { return toolModules; }, set(v) { toolModules = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolsMenuIndex', {
        get() { return toolsMenuIndex; }, set(v) { toolsMenuIndex = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolBrowserState', {
        get() { return toolBrowserState; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolAvailableEngines', {
        get() { return toolAvailableEngines; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolEngineIndex', {
        get() { return toolEngineIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolActiveTool', {
        get() { return toolActiveTool; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolSelectedFile', {
        get() { return toolSelectedFile; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolSelectedEngine', {
        get() { return toolSelectedEngine; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolProcessStartTime', {
        get() { return toolProcessStartTime; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolFileDurationSec', {
        get() { return toolFileDurationSec; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolStemsFound', {
        get() { return toolStemsFound; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolExpectedStems', {
        get() { return toolExpectedStems; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolProcessingDots', {
        get() { return toolProcessingDots; }, set(v) { toolProcessingDots = v; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolResultSuccess', {
        get() { return toolResultSuccess; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolResultMessage', {
        get() { return toolResultMessage; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolStemFiles', {
        get() { return toolStemFiles; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolStemReviewIndex', {
        get() { return toolStemReviewIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolStemKept', {
        get() { return toolStemKept; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolSetList', {
        get() { return toolSetList; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolSetPickerIndex', {
        get() { return toolSetPickerIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'toolSelectedSetName', {
        get() { return toolSelectedSetName; }, enumerable: true
    });
    _ctx.menuLayoutDefaults = menuLayoutDefaults;
    _ctx.debugLog = debugLog;

    /* Store state */
    Object.defineProperty(_ctx, 'storePickerCategory', {
        get() { return storePickerCategory; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'storePickerModules', {
        get() { return storePickerModules; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'storePickerCurrentModule', {
        get() { return storePickerCurrentModule; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'storeInstalledModules', {
        get() { return storeInstalledModules; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'storeHostVersion', {
        get() { return storeHostVersion; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'storePickerResultTitle', {
        get() { return storePickerResultTitle; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'storePickerMessage', {
        get() { return storePickerMessage; }, enumerable: true
    });
    _ctx.getModuleStatus = (...args) => getModuleStatus(...args);
    _ctx.CATEGORIES = CATEGORIES;
    /* Let a view opt out of REDRAW_INTERVAL for the frames it cares about.
     *
     * The global gate draws every other tick unless `needsRedraw` is set, and
     * a knob turn on the param grid does not set it — measured, the grid drew
     * 0.34 times per tick, which is the ~20fps behind the "laggy knobs"
     * report. Rather than change REDRAW_INTERVAL (every other view depends on
     * it), a view that paces itself can just ask. The grid does; see
     * MOVY_REDRAW_MIN_MS in shadow_ui_param_pages.mjs, which is its own
     * ceiling and is what should be raised if drawing ever gets expensive
     * again — a page render measures 1.68ms. */
    _ctx.requestRedraw = () => { needsRedraw = true; };
    _ctx.drawStatusOverlay = (...args) => drawStatusOverlay(...args);
    _ctx.createScrollableText = (...args) => createScrollableText(...args);
    _ctx.drawScrollableText = (...args) => drawScrollableText(...args);
    _ctx.wrapText = (...args) => wrapText(...args);

    /* Knob-grid view (shadow_ui_param_pages.mjs) */
    _ctx.evaluateVisibilityCondition = (...args) => evaluateVisibilityCondition(...args);
    _ctx.openParamEditor = (slot, fullKey, meta) => openParamEditorFromGrid(slot, fullKey, meta);
    /* Slot-settings actions (Save / Delete / LFO / Knob Mapping). Exposed so
     * every branch can be EXECUTED by the tests: this code was previously
     * reachable only by pressing a specific row on a specific screen, and a
     * ReferenceError inside one branch is swallowed by the tick try/catch into
     * "UI error, recovering" — invisible unless something runs it. */
    /*
     * Slot actions from the knob grid, with a hand-off for the ones that open a
     * modal.
     *
     * Save / Save As / Delete do not act immediately: they set
     * showingNamePreview or confirmingOverwrite/confirmingDelete and wait for a
     * confirmation. Both the DRAWING of those (drawChainSettings, via
     * shadow_ui_settings.mjs) and the jog/click that drive them live under
     * `case VIEWS.CHAIN_SETTINGS` -- the LIST view. Slot settings now open as
     * the grid, so from there the flag flipped and NOTHING rendered it and
     * nothing could answer it. On device: pressing Save did nothing at all, and
     * the jog gave no "Edit"/"OK", because that handler was never reached.
     *
     * So hand off to the list, the same way an opaque param does
     * (enterHierarchyEditorFromParamPages above). Teaching the grid to draw and
     * drive three confirm flows would be a second implementation of screens
     * that already work.
     *
     * Detected by ASKING WHETHER A MODAL IS NOW OPEN rather than by listing
     * which keys are modal ones: a fifth action that opens a confirm would
     * otherwise be silently broken in exactly the same way, and that is the
     * failure mode worth designing out.
     */
    _ctx.runSlotAction = (slot, key) => runSlotActionFromGrid(slot, key);
    /* The slot-settings entry point, exposed so the grid routing can be driven
     * from a test rather than by pressing Shift+Vol+Track on hardware. */
    _ctx.enterChainSettings = (slot) => enterChainSettings(slot);
    /* Which specialised editor is up, if any. Exposed so the editor-routing
     * pathways can be tested headlessly: "clicking a wav_position shows the
     * WAVEFORM, clicking a filepath shows the BROWSER, and Back from either
     * returns to the grid" is four states, and none of them was observable
     * from outside this file — which is why all three routing bugs shipped. */
    /* The enum option picker. Handed the list and the index rather than a key,
     * because the two callers (knob grid, list editor) write through different
     * owners — see openEnumPicker. */
    _ctx.openEnumPicker = (o) => openEnumPicker(o);
    _ctx.activeParamEditor = () => {
        if (view === VIEWS.ENUM_PICKER) return "enum";
        if (view === VIEWS.FILEPATH_BROWSER) return "filepath";
        if (view === VIEWS.CANVAS) return "canvas";
        if (view !== VIEWS.HIERARCHY_EDITOR) return null;
        if (!hierEditorEditMode) return null;
        return isInWavPositionEditor() ? "wav_position" : "value";
    };
    _ctx.isParamModulated = (slot, fullKey) => isHierarchyParamModulated(slot, fullKey);
    _ctx.isMuteHeld = () => hostMuteHeld;

    /* Overtake session state (for tools menu "Resume" indicator) */
    Object.defineProperty(_ctx, 'overtakeModuleLoaded', {
        get() { return overtakeModuleLoaded; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'overtakeModulePath', {
        get() { return overtakeModulePath; }, enumerable: true
    });

    /* Chain settings state */
    Object.defineProperty(_ctx, 'showingNamePreview', {
        get() { return showingNamePreview; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'pendingSaveName', {
        get() { return pendingSaveName; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'namePreviewIndex', {
        get() { return namePreviewIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'confirmingOverwrite', {
        get() { return confirmingOverwrite; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'confirmingDelete', {
        get() { return confirmingDelete; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'confirmIndex', {
        get() { return confirmIndex; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'selectedChainSetting', {
        get() { return selectedChainSetting; }, enumerable: true
    });
    Object.defineProperty(_ctx, 'editingChainSettingValue', {
        get() { return editingChainSettingValue; }, enumerable: true
    });
    _ctx.getChainSettingsItems = (...args) => getChainSettingsItems(...args);
    _ctx.getChainSettingValue = (...args) => getChainSettingValue(...args);

    /* Global settings state */

    /* View transitions - bound lazily since some may be defined after this block */
    _ctx.enterChainEdit = (...args) => enterChainEdit(...args);
    _ctx.enterPatchBrowser = (...args) => _enterPatchBrowser(...args);
    _ctx.enterMasterFxSettings = (...args) => enterMasterFxSettings(...args);
    _ctx.enterSlotSettings = (...args) => _enterSlotSettings(...args);
})();

/* Delegate draw/enter functions to extracted modules */
function drawSlots() { _drawSlots(); }
function drawSlotSettings() { _drawSlotSettings(); }
function enterSlotSettings(slotIndex) { _enterSlotSettings(slotIndex); }
function drawPatches() { _drawPatches(); }
function drawPatchDetail() { _drawPatchDetail(); }
function drawComponentParams() { _drawComponentParams(); }
function drawPresets() { _drawPresets(); }
function drawPresetDetail() { _drawPresetDetail(); }
function enterPresetBrowser(...args) { _enterPresetBrowser(...args); }
function enterPatchBrowser(slotIndex) { _enterPatchBrowser(slotIndex); }
function enterPatchDetail(slotIndex, patchIndex) { _enterPatchDetail(slotIndex, patchIndex); }
function enterComponentParams(slot, component) { _enterComponentParams(slot, component); }
function applyPatchSelection() { _applyPatchSelection(); }
function drawMasterFx() { _drawMasterFx(); }
function getMasterFxDisplayName() { return _getMasterFxDisplayName(); }
function enterMasterFxSettings() { _enterMasterFxSettings(); }
function scanForToolModules() { return _scanForToolModules(); }
function enterToolsMenu() {
    _enterToolsMenu();
    try {
        const overtakes = scanForOvertakeModules();
        if (!Array.isArray(overtakes) || overtakes.length === 0) return;
        const tools = Array.isArray(toolModules) ? toolModules : [];
        const merged = [];
        for (const t of tools) {
            const isSuspended = !!(t.id && suspendedOvertakes[t.id]);
            merged.push(Object.assign({}, t, { kind: 'tool', suspended: isSuspended }));
        }
        if (tools.length > 0) merged.push({ type: 'divider', label: 'Overtake Modules' });
        for (const o of overtakes) {
            const isSuspended = !!(o.id && suspendedOvertakes[o.id]);
            merged.push(Object.assign({}, o, { kind: 'overtake', suspended: isSuspended }));
        }
        toolModules = merged;
        if (toolsMenuIndex == null || toolsMenuIndex < 0 || toolsMenuIndex >= merged.length
            || (merged[toolsMenuIndex] && merged[toolsMenuIndex].type === 'divider')) {
            toolsMenuIndex = 0;
        }
    } catch (e) {
        debugLog("enterToolsMenu overtake merge failed: " + e);
    }
}

/* Tools shortcut → resume the most-recently-suspended tool when triggered
 * via double-tap of Shift+Vol+Step13 OR Shift+long-press-Step13 (shim sets the
 * resume_last_tool hint for the latter). Returns true if the resume fired,
 * false if the caller should fall through to the normal Tools-menu open. */
function tryResumeSuspendedTool() {
    const now = Date.now();
    const isDoubleTap = (now - lastToolsShortcutMs) < TOOLS_DOUBLE_TAP_MS;
    lastToolsShortcutMs = now;
    let shimHint = false;
    try {
        if (typeof shadow_consume_resume_last_tool === "function") {
            shimHint = shadow_consume_resume_last_tool() !== 0;
        }
    } catch (e) { /* ignore */ }
    if (!isDoubleTap && !shimHint) return false;
    const gesture = shimHint ? "long-press" : "double-tap";

    /* Resume wins when the last tool is still parked: it keeps the live JS and
     * DSP state, which a fresh load would throw away. */
    if (lastSuspendedToolId && suspendedOvertakes[lastSuspendedToolId]) {
        debugLog("Tools shortcut " + gesture + " → resuming " + lastSuspendedToolId);
        return resumeOvertakeModule(lastSuspendedToolId);
    }

    /* Nothing parked — relaunch the last tool that was loaded this session.
     * Interactive tools must go back through startInteractiveTool so their
     * tool-active flags and file path are re-established.
     *
     * Only when nothing is currently loaded: with a module still active the
     * relaunch would load on top of it without tearing it down (orphaning its
     * DSP in slot 0). Returning false there falls through to the caller's
     * exit-then-enterToolsMenu path, which is the pre-existing behaviour. */
    if (lastLaunchedTool && lastLaunchedTool.module && !overtakeModuleLoaded) {
        const t = lastLaunchedTool;
        debugLog("Tools shortcut " + gesture + " → relaunching " + t.kind + " " +
                 (t.module.id || "(unknown)") + (t.filePath ? " file=" + t.filePath : ""));
        if (t.kind === "interactive") {
            startInteractiveTool(t.module, t.filePath);
            return true;
        }
        /* Re-scan rather than trusting the descriptor captured at the original
         * load. It is a snapshot of module.json from whenever the module was
         * first opened this session, so a module updated on disk since — new
         * capabilities, a Module Store update — would keep relaunching with its
         * old metadata until a full menu visit refreshed it. Observed with a
         * newly added suspend_keeps_js: the file on disk had it, every relaunch
         * ignored it. Fall back to the cached descriptor if the scan cannot
         * find the id (module uninstalled mid-session). */
        let mod = t.module;
        try {
            const fresh = (scanForOvertakeModules() || []).find(o => o.id === mod.id);
            if (fresh) {
                mod = fresh;
                lastLaunchedTool.module = fresh;
            }
        } catch (e) {
            debugLog("tryResumeSuspendedTool: re-scan failed, using cached descriptor: " + e);
        }
        return loadOvertakeModule(mod, t.skipOvertake);
    }

    debugLog("tryResumeSuspendedTool: nothing suspended or previously launched");
    return false;
}
function drawToolsMenu() { _drawToolsMenu(); }
function drawToolFileBrowser() { _drawToolFileBrowser(); }
function drawToolEngineSelect() { _drawToolEngineSelect(); }
function drawToolConfirm() { _drawToolConfirm(); }
function drawToolProcessing() { _drawToolProcessing(); }
function drawToolResult() { _drawToolResult(); }
function drawToolStemReview() { _drawToolStemReview(); }
function drawToolSetPicker() { _drawToolSetPicker(); }
function drawStorePickerResult() { _drawStorePickerResult(); }
function drawChainSettings() { _drawChainSettings(); }
function drawGlobalSettings() { _drawGlobalSettings(); }

/* ============================================================================
 * LFO Editor
 * ============================================================================ */

/* --- LFO Context Factories --- */

function makeSlotLfoCtx(slot, lfoIdx) {
    const prefix = "lfo" + (lfoIdx + 1) + ":";
    return {
        lfoIdx: lfoIdx,
        /* Identifies the ROUTING SPACE for the label cache — `title` does not:
         * slot 2's "LFO 1" and slot 3's "LFO 1" are different components. */
        scopeId: "slot" + slot + ":lfo" + lfoIdx,
        getParam: function(key) { return getSlotParam(slot, prefix + key); },
        setParam: function(key, val) { setSlotParam(slot, prefix + key, val); },
        setParamBlocking: function(key, val) { return shadowSetParamBlocking(slot, prefix + key, val); },
        getTargetComponents: function() {
            const comps = [];
            const synthModule = getSlotParam(slot, "synth_module");
            if (synthModule) {
                let name = getSlotParam(slot, "synth:name") || synthModule;
                if (name === synthModule && name.startsWith("rnbo-synth-")) {
                    name = name.substring("rnbo-synth-".length) + " (RNBO)";
                } else if (name === synthModule && name.startsWith("rnbo-fx-")) {
                    name = name.substring("rnbo-fx-".length) + " (RNBO)";
                }
                comps.push({ key: "synth", label: "Synth: " + name });
            }
            /*
             * Both sections are as long as the chain SAYS it is.
             *
             * They used to stop at two, which was the old fixed shape; the DSP
             * has held eight of each for a while, so an eight-FX chain offered
             * modulation of the first two positions and silently pretended the
             * rest were not there.
             *
             * The bound is the PUBLISHED COUNT, never the cap. This function
             * runs inside a draw on a describeLfoTargetFor cache miss, and a
             * reorder forces one (resetLfoTargetLabels), so every position
             * walked costs two IPC round trips at ~2.8ms — walking to the cap
             * would turn the frame after every reorder into ~16 reads, ~45ms,
             * for positions that mostly hold nothing. One read for the count
             * buys the right length, and a two-FX chain costs what it always
             * did.
             *
             * The count is a HIGH-WATER MARK (chain_host.c keeps
             * `fx_count = slot + 1` and only trims a trailing NULL), so an
             * interior position inside it can still be empty — hence the
             * per-position guard stays.
             */
            const count = (key, cap) => {
                const n = parseInt(getSlotParam(slot, key), 10);
                return (isNaN(n) || n < 0) ? 0 : Math.min(n, cap);
            };
            const fxCount = count("fx_count", CHAIN_CAP.fx);
            for (let i = 1; i <= fxCount; i++) {
                const fxModule = getSlotParam(slot, "fx" + i + "_module");
                if (fxModule) {
                    const name = getSlotParam(slot, "fx" + i + ":name") || fxModule;
                    comps.push({ key: "fx" + i, label: "FX " + i + ": " + name });
                }
            }
            const midiFxCount = count("midi_fx_count", CHAIN_CAP.midiFx);
            for (let i = 1; i <= midiFxCount; i++) {
                const mfxModule = getSlotParam(slot, "midi_fx" + i + "_module");
                if (mfxModule) {
                    const name = getSlotParam(slot, "midi_fx" + i + ":name") || mfxModule;
                    comps.push({ key: "midi_fx" + i, label: "MIDI FX " + i + ": " + name });
                }
            }
            /* Add the other LFO as a target (skip self) */
            const otherIdx = lfoIdx === 0 ? 1 : 0;
            comps.push({ key: "lfo" + (otherIdx + 1), label: "LFO " + (otherIdx + 1) });
            comps.push({ key: "__clear__", label: "[Clear Target]" });
            return comps;
        },
        getTargetParams: function(compKey) {
            return lfoTargetParamsFor(slotChainTarget(slot), compKey, "LFO");
        },
        title: "LFO " + (lfoIdx + 1),
        returnView: VIEWS.CHAIN_SETTINGS,
        returnAnnounce: "Chain Settings",
        supportsRetrigger: true,
    };
}

/* Hardcoded LFO param list for LFO-to-LFO modulation */
const LFO_TARGET_PARAMS = [
    { key: "depth", label: "Depth" },
    { key: "rate_hz", label: "Rate Hz" },
    { key: "phase_offset", label: "Phase Offset" },
];

/*
 * What a component offers an LFO to modulate — for EITHER chain.
 *
 * The slot and Master FX LFO editors held two copies of this that differed
 * only in how the chain_params key was spelled, which the chain target now
 * answers. Only continuous types are offered: a string or an action has no
 * range for a depth to mean anything against.
 */
function lfoTargetParamsFor(target, compKey, logLabel) {
    /* LFO-to-LFO: return hardcoded LFO params */
    if (compKey === "lfo1" || compKey === "lfo2") return LFO_TARGET_PARAMS.slice();
    const result = [];
    try {
        const json = chainTargetGetParam(target, compKey, "chain_params");
        if (json) {
            const params = JSON.parse(json);
            for (let i = 0; i < params.length; i++) {
                const p = params[i];
                if (p.type === "float" || p.type === "int" || p.type === "enum") {
                    result.push({ key: p.key, label: p.name || p.label || p.key });
                }
            }
        }
    } catch (e) {
        debugLog(logLabel + ": Failed to parse chain_params: " + e);
    }
    return result;
}

function makeMfxLfoCtx(lfoIdx) {
    const prefix = "master_fx:lfo" + (lfoIdx + 1) + ":";
    return {
        lfoIdx: lfoIdx,
        scopeId: "mfx:lfo" + lfoIdx,
        /* Through getSlotParam / setSlotParam, exactly as makeSlotLfoCtx does:
         * Master FX is addressed at IPC slot 0 by convention (it is NOT
         * instrument slot 0), and the raw host calls it used instead threw
         * when the binding was absent where the slot side returned null. */
        getParam: function(key) { return getSlotParam(0, prefix + key); },
        setParam: function(key, val) { setSlotParam(0, prefix + key, val); },
        setParamBlocking: function(key, val) { return shadowSetParamBlocking(0, prefix + key, val); },
        getTargetComponents: function() {
            const comps = [];
            /* The cap, not a published count: Master FX is a FIXED array of
             * MASTER_FX_SLOTS (shadow_chain_mgmt.h), not a variable-length
             * list, and it publishes no count to ask. So do NOT "fix" this to
             * read a count the way the slot version does — there isn't one.
             * It must, however, TRACK THE CAP: this used to be a bare 4, and a
             * bare 4 walks only half an 8-slot Master FX, silently dropping
             * FX 5-8 from the LFO target picker. Bound by the constant so the
             * cap moves it. */
            for (let i = 0; i < MASTER_FX_SLOTS; i++) {
                const name = getMasterFxParam(i, "name");
                if (name) {
                    comps.push({ key: "fx" + (i + 1), label: "FX " + (i + 1) + ": " + name });
                }
            }
            /* Add the other LFO as a target (skip self) */
            const otherIdx = lfoIdx === 0 ? 1 : 0;
            comps.push({ key: "lfo" + (otherIdx + 1), label: "MFX LFO " + (otherIdx + 1) });
            comps.push({ key: "__clear__", label: "[Clear Target]" });
            return comps;
        },
        getTargetParams: function(compKey) {
            return lfoTargetParamsFor(MASTER_CHAIN_TARGET, compKey, "MFX LFO");
        },
        title: "MFX LFO " + (lfoIdx + 1),
        returnView: VIEWS.MASTER_FX,
        returnAnnounce: "Master FX Settings",
    };
}

/* --- Generic LFO Editor Functions (driven by lfoCtx) --- */

/*
 * What the current LFO's target is CALLED — see shared/lfo_target_label.mjs.
 *
 * CACHED on the stored pair, because resolving it is not cheap: building the
 * component list reads synth_module, each fx module, the midi fx count and a
 * name per component, and the param list parses that component's whole
 * chain_params — a dozen-odd IPC round trips at ~2.8ms each. Called from a
 * DRAW, that is the entire frame budget several times over.
 *
 * The pair only changes when the user commits a new routing, so a cache keyed
 * on it resolves once and then costs a string compare. `lfoCtx.title` is in
 * the key too: a slot LFO and a Master FX LFO can hold the identical pair and
 * mean different components.
 */
/* Per SCOPE, not one entry: the knob grid asks for a slot's LFO while the
 * editor's own lfoCtx may be a Master FX LFO, and a single slot would thrash
 * between them — every miss is the dozen round trips above, inside a draw. */
const _lfoTargetLabelCache = Object.create(null);
function describeLfoTargetFor(ctx) {
    if (!ctx) return null;
    const scope = ctx.scopeId || ctx.title || "lfo";
    const target = ctx.getParam("target") || "";
    const param = ctx.getParam("target_param") || "";
    const cacheKey = target + "|" + param;
    const hit = _lfoTargetLabelCache[scope];
    if (hit && hit.key === cacheKey) return hit.value;
    const value = describeLfoTarget({
        target: target,
        targetParam: param,
        components: ctx.getTargetComponents ? ctx.getTargetComponents() : [],
        params: (ctx.getTargetParams && target) ? ctx.getTargetParams(target) : [],
    });
    _lfoTargetLabelCache[scope] = { key: cacheKey, value: value };
    return value;
}

/** Drop every cached label. Called when a component module changes: the cache
 *  keys on the routing, and a swap changes the NAME without changing it. */
function resetLfoTargetLabels() {
    for (const k in _lfoTargetLabelCache) delete _lfoTargetLabelCache[k];
}

/** The LFO the editor is currently pointed at. */
function describeCurrentLfoTarget() {
    return lfoCtx ? describeLfoTargetFor(lfoCtx) : null;
}

function getLfoItems() {
    if (!lfoCtx) return [];
    const sync = lfoCtx.getParam("sync") === "1";

    const items = [
        { key: "target", label: "Target", type: "action" },
        { key: "enabled", label: "Enabled", type: "enum", options: ["Off", "On"] },
        { key: "shape", label: "Shape", type: "enum", options: LFO_SHAPES },
        { key: "polarity", label: "Mode", type: "enum", options: ["Unipolar", "Bipolar"] },
        { key: "sync", label: "Sync", type: "enum", options: ["Free", "Sync"] },
    ];

    if (sync) {
        items.push({ key: "rate_div", label: "Rate", type: "enum", options: LFO_DIVISIONS });
    } else {
        items.push({ key: "rate_hz", label: "Rate", type: "float", min: 0.1, max: 20.0, step: 0.1, unit: "Hz" });
    }

    items.push({ key: "depth", label: "Depth", type: "float", min: -1, max: 1, step: 0.01, unit: "%" });
    items.push({ key: "phase_offset", label: "Phase", type: "float", min: 0, max: 1, step: 0.0417, unit: "deg" });
    if (lfoCtx && lfoCtx.supportsRetrigger) {
        items.push({ key: "retrigger", label: "Retrigger", type: "enum", options: ["Off", "On"] });
    }

    return items;
}

function getLfoDisplayValue(item) {
    if (!lfoCtx) return "";
    const raw = lfoCtx.getParam(item.key);
    if (raw === null || raw === undefined) return "";

    if (item.key === "enabled") return raw === "1" ? "On" : "Off";
    if (item.key === "shape") {
        const idx = parseInt(raw);
        return (idx >= 0 && idx < LFO_SHAPES.length) ? LFO_SHAPES[idx] : raw;
    }
    if (item.key === "polarity") return raw === "1" ? "Bipolar" : "Unipolar";
    if (item.key === "sync") return raw === "1" ? "Sync" : "Free";
    if (item.key === "rate_div") {
        const idx = parseInt(raw);
        return (idx >= 0 && idx < LFO_DIVISIONS.length) ? LFO_DIVISIONS[idx] : raw;
    }
    if (item.key === "rate_hz") return parseFloat(raw).toFixed(1) + " Hz";
    if (item.key === "depth") return Math.round(parseFloat(raw) * 100) + "%";
    if (item.key === "phase_offset") return Math.round(parseFloat(raw) * 360) + "\u00b0";
    if (item.key === "retrigger") return raw === "1" ? "On" : "Off";
    if (item.key === "target") {
        /* The module by name, as the picker offered it — not the stored keys.
         * This row read "fx1:room_size" for as long as it has existed. */
        const d = describeCurrentLfoTarget();
        return d ? d.long : "None";
    }
    return raw;
}

function adjustLfoParam(item, delta) {
    if (!lfoCtx) return;

    if (item.type === "enum") {
        const raw = parseInt(lfoCtx.getParam(item.key) || "0");
        let newVal = raw + delta;
        if (newVal < 0) newVal = 0;
        if (newVal >= item.options.length) newVal = item.options.length - 1;
        lfoCtx.setParam(item.key, String(newVal));
    } else if (item.type === "float") {
        const raw = parseFloat(lfoCtx.getParam(item.key) || "0");
        let newVal = raw + item.step * delta;
        if (newVal < item.min) newVal = item.min;
        if (newVal > item.max) newVal = item.max;
        lfoCtx.setParam(item.key, newVal.toFixed(4));
    }
}

function drawLfoEdit() {
    if (!lfoCtx) return;
    clear_screen();
    const enabled = lfoCtx.getParam("enabled") === "1";
    const targetDesc = describeCurrentLfoTarget();

    let title = lfoCtx.title;
    /* The PARAM alone here, not the module: 22 characters is the whole title
     * and "LFO 1" plus a module name plus a param name does not come close.
     * Which module it is, is one row down. */
    if (enabled && targetDesc && !targetDesc.empty) {
        title += ": " + targetDesc.short;
    } else if (!enabled) {
        title += ": Off";
    }
    drawHeader(truncateText(title, 22));

    drawMenuList({
        items: getLfoItems(),
        selectedIndex: selectedLfoItem,
        getLabel: (item) => item.label,
        getValue: (item) => getLfoDisplayValue(item) || "",
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true,
        editMode: editingLfoValue
    });
}

/* LFO target picker: step 1 - select component */
function enterLfoTargetPicker() {
    if (!lfoCtx) return;
    lfoTargetComponents = lfoCtx.getTargetComponents();
    selectedLfoTargetComp = 0;
    setView(VIEWS.LFO_TARGET_COMPONENT);
    if (lfoTargetComponents.length > 0) {
        announce("Target, " + lfoTargetComponents[0].label);
    }
}

/* LFO target picker: step 2 - select parameter for chosen component */
function enterLfoTargetParamPicker(componentKey) {
    if (!lfoCtx) return;
    lfoTargetParams = lfoCtx.getTargetParams(componentKey);
    selectedLfoTargetParam = 0;
    setView(VIEWS.LFO_TARGET_PARAM);
    if (lfoTargetParams.length > 0) {
        announce("Param, " + lfoTargetParams[0].label);
    } else {
        announce("No parameters available");
    }
}

function drawLfoTargetComponent() {
    clear_screen();
    drawHeader((lfoCtx ? lfoCtx.title : "LFO") + " Target");

    drawMenuList({
        items: lfoTargetComponents,
        selectedIndex: selectedLfoTargetComp,
        getLabel: function(item) { return item.label; },
    });
}

function drawLfoTargetParam() {
    clear_screen();
    const compIdx = selectedLfoTargetComp;
    const compLabel = (compIdx >= 0 && compIdx < lfoTargetComponents.length)
        ? lfoTargetComponents[compIdx].label : "Param";
    drawHeader((lfoCtx ? lfoCtx.title : "LFO") + " > " + compLabel);

    if (lfoTargetParams.length === 0) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No parameters", 1);
        return;
    }

    drawMenuList({
        items: lfoTargetParams,
        selectedIndex: selectedLfoTargetParam,
        getLabel: function(item) { return item.label; },
    });
}

/* ======================================================================== *
 * ENUM OPTION PICKER
 *
 * A scrolling list of one enum param's options. Reached two ways and it is the
 * SAME screen both times, on purpose:
 *
 *   the knob grid   clicking a held enum knob (footer: CLK OPEN)
 *   the list editor jog-clicking an enum row
 *
 * Why it exists: an enum is turnable, so both editors let you step it one
 * detent at a time — which is fine for Off/On and useless for a Recv Ch with
 * seventeen options or a macro-oscillator model with forty-seven. Stepping does
 * not go away; this is the other half.
 *
 * It owns NO param knowledge. The caller hands over the option list, where in
 * it we currently are, and a `commit` closure — because the two callers write
 * through different owners: the grid writes through its controller (so the slot
 * contract's Fwd offset and MPE compound write still apply) and the list editor
 * writes through setSlotParam. One screen, two owners, no second copy of the
 * wire-format decision.
 *
 * Nothing is written while you scroll, so Back is a genuine cancel and there is
 * nothing to revert. That also keeps the draw path free of IPC: every value it
 * shows was resolved once, at open.
 * ======================================================================== */
let enumPickerTitle = "";
let enumPickerOptions = [];
let enumPickerIndex = 0;
let enumPickerOpenIndex = 0;
let enumPickerCommit = null;
let enumPickerReturnToGrid = false;
/* Knob-scroll accumulator for the picker. The JOG stays 1:1 — a jog detent
 * is a deliberate click; a knob detent is a fraction of a twist. */
let enumPickerKnob = listKnobInit();

function openEnumPicker(o) {
    const options = Array.isArray(o && o.options) ? o.options : [];
    if (options.length === 0) return false;
    enumPickerTitle = String((o && o.title) || "Options");
    enumPickerOptions = options.map((v) => String(v));
    const i = Math.round(Number(o && o.index));
    enumPickerIndex = (isFinite(i) && i >= 0 && i < options.length) ? i : 0;
    enumPickerOpenIndex = enumPickerIndex;
    enumPickerCommit = (o && typeof o.commit === "function") ? o.commit : null;
    enumPickerReturnToGrid = !!(o && o.returnToGrid);
    enumPickerKnob = listKnobInit();
    setView(VIEWS.ENUM_PICKER);
    needsRedraw = true;
    announce(enumPickerTitle + ", " + enumPickerOptions[enumPickerIndex]
             + ", " + (enumPickerIndex + 1) + " of " + enumPickerOptions.length);
    return true;
}

/* Leave the picker. `commit` is false for Back, which must leave the value
 * exactly where it was — nothing was written on the way in or while scrolling,
 * so that is simply a matter of not writing now. */
function closeEnumPicker(commit) {
    const title = enumPickerTitle;
    const chosen = enumPickerOptions[enumPickerIndex];
    const write = commit ? enumPickerCommit : null;
    const idx = enumPickerIndex;
    const toGrid = enumPickerReturnToGrid;

    enumPickerCommit = null;
    enumPickerOptions = [];
    enumPickerTitle = "";
    enumPickerIndex = 0;
    enumPickerOpenIndex = 0;
    enumPickerReturnToGrid = false;

    if (write) write(idx);

    if (toGrid && paramPagesActive()) {
        setView(VIEWS.PARAM_PAGES);
    } else {
        setView(VIEWS.HIERARCHY_EDITOR);
    }
    needsRedraw = true;
    if (commit) announceParameter(title, chosen);
    else announce("Cancelled, " + title);
}

function enumPickerJog(delta) {
    if (enumPickerOptions.length === 0) return;
    enumPickerIndex = Math.max(0, Math.min(enumPickerOptions.length - 1,
                                           enumPickerIndex + delta));
    needsRedraw = true;
    announce(enumPickerOptions[enumPickerIndex] + ", "
             + (enumPickerIndex + 1) + " of " + enumPickerOptions.length);
}

/*
 * THE LIST RECT, and why it is nine and not ten.
 *
 * The movy bands cost vertical space the old chrome did not: a footer rule at
 * 55 with an 8-row hint band under it takes the bottom of the screen, where
 * drawMenuList's default indicator row (62) used to sit. Left at its defaults
 * the list would have run its last row and its down-arrow straight through the
 * footer, and the device clips silently — nothing would have said so.
 *
 * The obvious top is MENU_LIST_Y (10), the rect the knob grid's own menu pages
 * use. It costs a row: 10..54 is 44px, and at a 9px line that is FOUR options
 * where the old chrome showed FIVE. One row up is 45px and buys the fifth back,
 * and it is free here because this header is NOT inverted — drawHeader only
 * fills the band when told to, so under a plain header the glyphs stop at row 5
 * and the selected row's highlight starting at row 8 still has clear air above
 * it. (A menu page cannot do the same: its bank bar owns row 7.)
 *
 * Losing the last option of a list to a band drawn over it is a failure this
 * codebase has hit before, which is why test_enum_picker_chrome.sh asserts the
 * row COUNT and clipped() === 0 rather than just eyeballing the render.
 */
const ENUM_PICKER_LIST_TOP_Y = 9;
const ENUM_PICKER_LIST_BOTTOM_Y = MOVY_RULE_Y - 1;

/*
 * The picker wears the KNOB GRID's chrome — movy header band, hint footer —
 * unconditionally, from BOTH entry points.
 *
 * It is reached from the grid (holding an enum knob and clicking) and from the
 * hierarchy list editor (jog-clicking an enum row), and the list editor still
 * wears the older device chrome. So there was a choice: follow the caller, or
 * be one screen. Following the caller means a `cameFromGrid` branch in the draw
 * — which is precisely the `kind === "master"` that drawChainPicker's comment
 * forbids, for the reason it gives: a shared function with a caller test in it
 * drifts exactly as well as two functions did, and this screen's own header
 * comment already promises it is the SAME screen both times. It is also the
 * shape of the bug a user reported from the device about the module pickers
 * ("the module select here is different than the module select in slots").
 *
 * So: one look. The list editor is the frame that will move to match, not this.
 */
function drawEnumPicker() {
    clear_screen();
    const ctx = { fillRect: fill_rect, print, textWidth: text_width };
    /* SELECT on the right, the same word drawChainPicker puts there: both are
     * "a list, pick one", and the grammar of the band is what tells you so
     * before you have read the title. */
    drawMovyHeader(ctx, enumPickerTitle, "SELECT", false);
    if (enumPickerOptions.length === 0) {
        print(LIST_LABEL_X, ENUM_PICKER_LIST_TOP_Y + 8, "No options", 1);
        /* Still a footer. openEnumPicker refuses an empty list so this should
         * be unreachable, but a screen with nothing on it is the one place a
         * way out most needs naming. */
        drawMovyFooter(ctx, [["BACK", "EXIT"]]);
        return;
    }
    /* The same list every other picker on this device uses. A second list
     * widget is how Master FX and the chain editor drifted apart. The tick
     * marks which option is CURRENTLY set, so scrolling away from it stays
     * legible as "you have moved off the live value". */
    drawMenuList({
        items: enumPickerOptions,
        selectedIndex: enumPickerIndex,
        listArea: { topY: ENUM_PICKER_LIST_TOP_Y, bottomY: ENUM_PICKER_LIST_BOTTOM_Y },
        getLabel: function(item) { return String(item); },
        /* Which option is CURRENTLY set, so scrolling away from it still reads
         * as "you have moved off the live value" rather than as nothing. */
        getValue: function(item, i) { return i === enumPickerOpenIndex ? "*" : ""; },
        /* This screen announces its own, richer string ("Room, 2 of 17"), so
         * the list must not also announce "Room: *". */
        announce: false,
    });
    drawMovyFooter(ctx, enumPickerFooterHints());
}

globalThis.init = function() {
    debugLog("Shadow UI init");

    /* Opt-in one-shot draw benchmark. The param-pages draw design is built
     * around an assumed ~90-100us per QuickJS->C binding call, which nothing
     * in the tree re-measures; this settles it. Costs nothing when the flag
     * file is absent. See src/shared/draw_bench.mjs. */
    if (typeof host_file_exists === "function" &&
        host_file_exists("/data/UserData/schwung/draw_bench_on")) {
        try { runDrawBench(); } catch (e) { debugLog("draw_bench failed: " + e); }
    }

    /* Opt-in param read/write tally. Tracing showed param IPC is ~98% of tick
     * time and that ~6.7 of the ~7.7 reads per tick come from this file rather
     * than the param-pages controller — but a span carries a name, not a key
     * or a caller, so it cannot say which reads or from where. This can. Costs
     * one host_file_exists when the flag is absent. See src/shared/param_tally.mjs. */
    try { installParamTally(debugLog); } catch (e) { debugLog("param_tally failed: " + e); }
    refreshSlots();
    loadPatchList();
    initChainConfigs();
    updateFocusedSlot(selectedSlot);
    fetchKnobMappings(selectedSlot);

    /* Scan for audio FX modules so display names are available */
    MASTER_FX_OPTIONS = scanForAudioFxModules();
    /* Also prime moduleAbbrevCache for synth/midi_fx modules so slots restored
     * from a saved patch show their real abbrev instead of the substring(0,2)
     * fallback in getModuleAbbrev() before their picker has ever been opened. */
    scanModulesForType("synth");
    scanModulesForType("midiFx");

    /* Load auto-update preference */
    loadAutoUpdateConfig();
    loadBrowserPreviewConfig();
    loadPadTypingConfig();
    loadTextPreviewConfig();
    loadParamViewConfig();
    loadFilebrowserConfig();

    /* Legacy: migrate old single master_fx config to slot 1 */
    const savedMasterFx = loadMasterFxFromConfig();
    if (savedMasterFx.path && !masterFxConfig.fx1.module) {
        setMasterFxSlotModule(0, savedMasterFx.path);
        masterFxConfig.fx1.module = savedMasterFx.id;
    }
    /* Note: Jump-to-slot check moved to first tick() to avoid race condition */

    /* Auto-update check is manual only (Settings → Updates → Check Updates) */

    /* Detect whether the self-heal entrypoint is installed. If not, the
     * device is in the "needs bootstrap" state — first tick will surface
     * a one-shot banner pointing at the web manager / GUI installer. */
    shimBootstrapNeeded = detectShimBootstrapNeeded();
    if (shimBootstrapNeeded) {
        debugLog("init: self-heal bootstrap needed (entrypoint at /opt/move/Move lacks schwung-heal)");
    }

    /* Process any HTML left by a prior background download, then kick off
     * a new background download if the cache is stale. Both are non-blocking. */
    try { processDownloadedHtml(); } catch (e) { debugLog("Manual process: " + e); }
    try { refreshManualBackground(); } catch (e) { debugLog("Manual refresh: " + e); }

    /* Read active set UUID to point autosave at the correct per-set directory.
     * File format: line 1 = UUID, line 2 = set name */
    {
        const raw = host_read_file("/data/UserData/schwung/active_set.txt");
        if (raw) {
            const lines = raw.split("\n");
            const uuid = lines[0] ? lines[0].trim() : "";
            if (uuid) {
                const setDir = "/data/UserData/schwung/set_state/" + uuid;
                if (host_file_exists(setDir + "/slot_0.json") || host_file_exists(setDir + "/shadow_chain_config.json")) {
                    activeSlotStateDir = setDir;
                    invalidateAutosaveWriteCache();
                    debugLog("Init: using per-set state dir " + setDir);
                }
            }
        }
    }
    /* Re-apply Master FX sync after activeSlotStateDir resolves to the active set. */
    loadMasterFxChainFromConfig();

    /* Sync dirty cache and slot names from autosave files (shim loaded them on startup) */
    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
        const dirty = getSlotParam(i, "dirty");
        slotDirtyCache[i] = (dirty === "1");
        /* Sync slot names + per-component bypass from autosave if present.
         * The shim's load_file restores synth/FX/MIDI-FX modules + params via
         * the chain_host parser, but bypass flags are not in the C parser path;
         * apply them here via setSlotParam after autosave has settled. */
        const path = activeSlotStateDir + "/slot_" + i + ".json";
        if (host_file_exists(path)) {
            const raw = host_read_file(path);
            if (raw) {
                const m = raw.match(/"name"\s*:\s*"([^"]+)"/);
                if (m && m[1] && !slots[i].name) {
                    slots[i].name = m[1];
                }
                try {
                    const parsed = JSON.parse(raw);
                    /* Autosave wraps the patch as {name, version, modified, chain: ...}.
                     * Older legacy files might be unwrapped — accept either shape. */
                    const chain = (parsed && parsed.chain) ? parsed.chain : parsed;
                    if (chain && chain.synth && chain.synth.bypassed) {
                        setSlotParam(i, "synth:bypassed", "1");
                    }
                    /* Restore which user preset each component was on. Absent
                     * is the common case — every patch written before this
                     * existed, and every component nobody has loaded a preset
                     * into — and must CLEAR any stale record rather than leave
                     * one behind from a previous load into this slot. */
                    setUserPresetRecord(i, "synth", entryUserPreset(chain && chain.synth));
                    if (chain && Array.isArray(chain.midi_fx)) {
                        for (let mf = 0; mf < chain.midi_fx.length && mf < MAX_MIDI_FX; mf++) {
                            setUserPresetRecord(i, `midi_fx${mf + 1}`, entryUserPreset(chain.midi_fx[mf]));
                        }
                    }
                    if (chain && Array.isArray(chain.audio_fx)) {
                        for (let fx = 0; fx < chain.audio_fx.length && fx < MAX_FX; fx++) {
                            setUserPresetRecord(i, `fx${fx + 1}`, entryUserPreset(chain.audio_fx[fx]));
                        }
                    }
                    /*
                     * BOTH lists, and bounded by the CAP rather than by a
                     * number that used to be the cap.
                     *
                     * This read `chain.midi_fx[0]` and `fx < 4` — written when
                     * a chain was one MIDI FX and two audio FX. The chain has
                     * been a list of up to MAX_MIDI_FX and MAX_FX since, so
                     * bypass silently stopped being restored past the first
                     * MIDI FX and past the fourth audio FX: you bypassed a
                     * module, rebooted, and it came back live with the B glyph
                     * gone. Nothing failed and nothing logged.
                     */
                    if (chain && Array.isArray(chain.midi_fx)) {
                        for (let mf = 0; mf < chain.midi_fx.length && mf < MAX_MIDI_FX; mf++) {
                            if (chain.midi_fx[mf] && chain.midi_fx[mf].bypassed) {
                                setSlotParam(i, `midi_fx${mf + 1}:bypassed`, "1");
                            }
                        }
                    }
                    if (chain && Array.isArray(chain.audio_fx)) {
                        for (let fx = 0; fx < chain.audio_fx.length && fx < MAX_FX; fx++) {
                            if (chain.audio_fx[fx] && chain.audio_fx[fx].bypassed) {
                                setSlotParam(i, `fx${fx + 1}:bypassed`, "1");
                            }
                        }
                    }
                } catch (e) {
                    /* JSON parse failed — autosave is malformed or partial; skip
                     * bypass restore for this slot. Slot continues with bypass=0. */
                }
            }
        }
    }
    saveSlotsToConfig(slots);

    /* Analytics: emit app_launched + census + diff against previous snapshot.
     * app_launched must emit here (not in shadow_ui.c main()) because
     * analytics_enabled() returns false until the opt-in prompt resolves,
     * which is JS-side — firing from C would drop the first-boot event. */
    if (typeof host_track_event === "function" && typeof host_get_analytics_enabled === "function" && host_get_analytics_enabled()) {
        try {
            host_track_event('app_launched', '');

            const modules = host_list_modules();
            if (modules && modules.length > 0) {
                /* Send census, excluding dev/test harness modules that ship
                 * with the host but aren't meaningful user choices. */
                const INTERNAL_IDS = new Set(['ui-test', 'text-test', 'splash-test', 'seq-test']);
                const reportable = modules.filter(m => !INTERNAL_IDS.has(m.id));
                const ids = reportable.map(m => `"${m.id}"`).join(',');
                host_track_event('module_census',
                    `"module_count":${reportable.length},"modules":[${ids}]`);

                /* Diff against previous snapshot (skip on first run) */
                const snapshotPath = "/data/UserData/schwung/module-snapshot.txt";
                const oldContent = host_read_file(snapshotPath);
                if (oldContent) {
                    const oldSnap = {};
                    for (const line of oldContent.split("\n")) {
                        const eq = line.indexOf("=");
                        if (eq > 0) oldSnap[line.substring(0, eq)] = line.substring(eq + 1);
                    }

                    /* Only emit upgrades. module_added was removed as
                     * noisy — see analytics.c for details. */
                    for (const mod of modules) {
                        if (oldSnap[mod.id] && oldSnap[mod.id] !== mod.version) {
                            host_track_event('module_upgraded',
                                `"module_id":"${mod.id}","old_version":"${oldSnap[mod.id]}","new_version":"${mod.version || 'unknown'}"`);
                        }
                    }
                }

                /* Save snapshot for next boot */
                const snapshot = modules.map(m => `${m.id}=${m.version || 'unknown'}`).join("\n");
                host_write_file(snapshotPath, snapshot);
            }

            /* Track modules loaded in each slot at startup.
             * Filter out non-module-id responses (we've seen the shim
             * occasionally return a stale float like "0.005000" when the
             * slot instance isn't fully initialized yet — reject anything
             * that doesn't look like a module id). */
            const MODULE_ID_RE = /^[a-z][a-z0-9_-]*$/;
            for (let i = 0; i < 4; i++) {
                const synthModule = getSlotParam(i, "synth_module");
                if (synthModule && MODULE_ID_RE.test(synthModule)) {
                    host_track_event('module_loaded', '"module_id":"' + synthModule + '","source":"startup","slot":' + i);
                }
            }
        } catch (e) {
            debugLog("analytics census error: " + e);
        }
    }

    /* Announce initial view + selection */
    const slotName = slots[selectedSlot]?.name || "Unknown";
    announce(`Slots Menu, S${selectedSlot + 1} ${slotName}`);
};

/* Called by shadow_ui.c during controlled exits (restart/shutdown paths)
 * to guarantee one final persistence flush before process termination. */
globalThis.shadow_save_state_now = function() {
    autosaveAllSlots();
    saveMasterFxChainConfig();
    /* Also persist volumes/channels/mute/solo — otherwise the set's
     * shadow_chain_config.json drifts from slot_N.json across reboots,
     * e.g. toggling MPE (recv=All) before shutdown would revert on boot. */
    saveChainConfigToDir(activeSlotStateDir);
    debugLog("shadow_save_state_now: flushed set state before exit");
    return true;
};

function corunTeardown() {
    if (typeof shadow_corun_end === "function") shadow_corun_end();
    coRunChainEditSlot = -1;
    coRunView = -1;
    coRunKeepMask = 0;
}

/* Co-run helpers — see coRunChainEditSlot / coRunView declarations near top.
 *
 * runCoRunChainEdit(fn): temporarily set the outer `view` to coRunView so any
 * code that dispatches on view (handleJog/handleSelect/draws) lands in the
 * chain-editor branch. Captures view-change side-effects back into coRunView
 * so navigation into deeper views (patch browser, component edit, etc.)
 * sticks across frames. Always restores the outer view to its prior value so
 * the main tick's view-switch still routes to VIEWS.OVERTAKE_MODULE. */
function runCoRunChainEdit(fn) {
    const _saved = view;
    view = coRunView;
    try { fn(); } finally {
        coRunView = view;
        view = _saved;
    }
}

/* dispatchCoRunDraw(): mirror of the chain-edit subtree of the main draw
 * switch (~line 14874). Called from co-run with view already set to coRunView
 * by runCoRunChainEdit. The chain editor's navigation lands on many views
 * (PATCHES, COMPONENT_PARAMS, COMPONENT_SELECT, etc.), each with its own
 * draw function. drawSlots() only renders the top-level slot LIST — we must
 * dispatch every reachable view explicitly. */
function dispatchCoRunDraw() {
    switch (view) {
        /* Addressable-view overlay roots (CORUN_ENTRIES) — the co-run draw path
         * must render these too, not just the chain-editor subtree. */
        case VIEWS.SLOTS:                drawSlots(); break;
        case VIEWS.MASTER_FX:            drawMasterFx(); break;
        case VIEWS.GLOBAL_SETTINGS:      drawGlobalSettings(); break;
        case VIEWS.CHAIN_EDIT:           drawChainEdit(); break;
        case VIEWS.PATCHES:              drawPatches(); break;
        case VIEWS.PATCH_DETAIL:         drawPatchDetail(); break;
        case VIEWS.COMPONENT_PARAMS:     drawComponentParams(); break;
        case VIEWS.PRESETS:              drawPresets(); break;
        case VIEWS.PRESET_DETAIL:        drawPresetDetail(); break;
        case VIEWS.COMPONENT_SELECT:     drawComponentSelect(); break;
        case VIEWS.CHAIN_SETTINGS:       drawChainSettings(); break;
        case VIEWS.SLOT_SETTINGS:        drawSlotSettings(); break;
        case VIEWS.COMPONENT_EDIT:
            /* In co-run, never invoke loadedModuleUi.tick(): the chain module's
             * own UI module takes over shadow_ui's drawing/MIDI/IPC and starves
             * the active tool. drawComponentEdit() is the simple preset-browser
             * fallback that lets the user pick patches without the deep
             * module-specific editor — still useful, and keeps the tool alive. */
            drawComponentEdit();
            break;
        case VIEWS.HIERARCHY_EDITOR:     drawHierarchyEditor(); break;
        /* The grid draws grids; every other page kind it plans (preset
         * browser, items list, mode select, child selector) belongs to the
         * list editor, which drawParamPages declines by returning false. */
        case VIEWS.PARAM_PAGES:
            if (!drawParamPages()) { enterHierarchyEditorFromParamPages(); drawHierarchyEditor(); }
            break;
        case VIEWS.CANVAS:               drawCanvasPreview(); break;
        case VIEWS.KNOB_EDITOR:          drawKnobEditor(); break;
        case VIEWS.KNOB_PARAM_PICKER:    drawKnobParamPicker(); break;
        case VIEWS.DYNAMIC_PARAM_PICKER: drawDynamicParamPicker(); break;
        case VIEWS.LFO_EDIT:             drawLfoEdit(); break;
        case VIEWS.LFO_TARGET_COMPONENT: drawLfoTargetComponent(); break;
        case VIEWS.LFO_TARGET_PARAM:     drawLfoTargetParam(); break;
        case VIEWS.STORE_PICKER_RESULT:  drawStorePickerResult(); break;
        case VIEWS.FILEPATH_BROWSER:     drawFilepathBrowser(); break;
        default:
            /* Unknown view in co-run — render slot list as a recoverable
             * fallback so user can navigate back. */
            drawSlots();
    }
}

let lastDrawError = null;  /* one-shot log guard for the tick draw catch */
globalThis.tick = function() {
    /* Background tick for JS-suspended overtake modules.
     * Each parked module's tick() keeps firing so it can emit MIDI or advance
     * internal state. Display and LED bindings are swapped for no-ops so the
     * suspended module can't stomp on whatever view is currently on screen. */
    {
        const parkedIds = Object.keys(suspendedOvertakes);
        if (parkedIds.length > 0) {
            /* Signal to each parked module's tick() that it is running blind in
             * the background (draw calls below are no-ops). Modules read
             * globalThis.overtakeParked to skip display/LED work while parked. */
            globalThis.overtakeParked = true;
            const _noop = function() {};
            const _saved = {
                clear_screen: globalThis.clear_screen,
                print: globalThis.print,
                draw_rect: globalThis.draw_rect,
                fill_rect: globalThis.fill_rect,
                draw_line: globalThis.draw_line,
                draw_image: globalThis.draw_image,
                move_midi_internal_send: globalThis.move_midi_internal_send
            };
            for (const k in _saved) globalThis[k] = _noop;
            /* Bug D fix: param-shim globals may have been mutated by a chain-
             * component editor while we were parked. Snapshot whatever is on
             * globalThis now (so we can restore it after the tick run), then
             * swap in each parked entry's own captured shims for its tick().
             * Per-parked: each module's tick sees ITS OWN overtake-DSP shim,
             * so multiple parked modules stay isolated. */
            const _savedShimGet = globalThis.host_module_get_param;
            const _savedShimSet = globalThis.host_module_set_param;
            const _savedShimSetBlocking = globalThis.host_module_set_param_blocking;
            try {
                for (let i = 0; i < parkedIds.length; i++) {
                    const id = parkedIds[i];
                    const parked = suspendedOvertakes[id];
                    if (parked && parked.callbacks && parked.callbacks.tick) {
                        if (parked.shimGet) globalThis.host_module_get_param = parked.shimGet;
                        if (parked.shimSet) globalThis.host_module_set_param = parked.shimSet;
                        if (parked.shimSetBlocking) globalThis.host_module_set_param_blocking = parked.shimSetBlocking;
                        try {
                            parked.callbacks.tick();
                        } catch (e) {
                            /* Don't evict on a transient tick() exception — that
                             * silently drops the parked module and the user's
                             * next "open" lands on a fresh load with stale DSP
                             * state. Log and keep it parked; re-entry will
                             * resume normally. */
                            debugLog("suspended overtake (" + id + ") tick() exception: " + e);
                        }
                    }
                }
            } finally {
                globalThis.overtakeParked = false;
                for (const k in _saved) globalThis[k] = _saved[k];
                if (_savedShimGet === undefined) delete globalThis.host_module_get_param;
                else globalThis.host_module_get_param = _savedShimGet;
                if (_savedShimSet === undefined) delete globalThis.host_module_set_param;
                else globalThis.host_module_set_param = _savedShimSet;
                if (_savedShimSetBlocking === undefined) delete globalThis.host_module_set_param_blocking;
                else globalThis.host_module_set_param_blocking = _savedShimSetBlocking;
            }
        }
    }

    /* Live preset audition: debounced apply of the highlighted module preset
     * while scrolling the list (see shadow_ui_presets.mjs). Called every frame —
     * it self-gates on its own pending state (only set while in the browser), so
     * no view guard is needed (and the view guard was unreliable here). */
    tickPresetPreview();
    /* Per-second param read/write report; one boolean test when disarmed. */
    if (paramTallyArmed()) paramTallyTick();
    /* One staggered param read per frame while the grid is up. */
    if (view === VIEWS.PARAM_PAGES) tickParamPages();

    /* Splash screen on boot */
    if (splashActive) {
        splashTick++;
        if (splashTick >= SPLASH_TOTAL_TICKS) {
            splashActive = false;
            /* Check if we need to show analytics prompt (first run only) */
            if (!host_file_exists(ANALYTICS_PROMPTED_PATH)) {
                view = VIEWS.ANALYTICS_PROMPT;
                analyticsPromptSelection = 0;
                announce("Usage Statistics, Send anonymous data? Yes");
                /* Don't dismiss display — keep showing prompt */
            } else if (shimBootstrapNeeded && !shimBootstrapPromptShown) {
                /* One-shot repair prompt: the live entrypoint at /opt/move/Move
                 * doesn't include the schwung-heal call, so the self-heal
                 * mechanism isn't running. Updates via web manager will
                 * silently no-op at the privileged-write step until this is
                 * repaired (web manager / GUI installer / SSH). Show the
                 * pointer screen once per boot so the user is unmistakeably
                 * informed instead of staring at a silently-stale install. */
                shimBootstrapPromptShown = true;
                storePickerResultTitle = 'Schwung Repair';
                storePickerMessage = 'Repair needed. visit\n' +
                                     'http://move.local:7700\n' +
                                     'or rerun GUI installer';
                storePickerFromSettings = false;
                storeReturnView = null;
                view = VIEWS.STORE_PICKER_RESULT;
                announce(storePickerMessage);
            } else {
                /* Dismiss shadow display mode — return to Move's native UI */
                if (typeof shadow_request_exit === "function") {
                    shadow_request_exit();
                }
            }
        } else {
            drawSplashScreen();
            return;
        }
    }

    /* Analytics prompt (first run) */
    if (view === VIEWS.ANALYTICS_PROMPT) {
        drawAnalyticsPrompt();
        return;
    }

    /* Periodic config sync for JS-only variables from web UI.
     * Shared-memory settings (display_mirror, TTS, etc.) are handled
     * directly by the Go web server via mmap. This only syncs JS variables
     * that have no shared memory representation. No C calls here. */
    if (++_configSyncTickCounter >= CONFIG_SYNC_INTERVAL) {
        _configSyncTickCounter = 0;
        /* Spanned: this is TWO eMMC file reads plus a JSON parse landing on a
         * single tick, roughly every 1.5s — the right shape and cadence for
         * the periodic dips, and the same class of mistake as the diagnostic
         * that once caused the stalls it was reporting. Attribute it before
         * assuming it. */
        const _h = (typeof host_trace_begin === 'function') ? host_trace_begin("js.config_sync") : 0;
        try {
            syncJsOnlySettings();

            /* Check for web-initiated upgrade status and show OLED overlay */
            try {
                const status = host_read_file("/data/UserData/schwung/upgrade_status");
                if (status && status.trim()) {
                    _upgradeOverlayText = status.trim();
                } else if (_upgradeOverlayText) {
                    _upgradeOverlayText = null;
                }
            } catch (e) {
                _upgradeOverlayText = null;
            }
        } finally {
            if (_h && typeof host_trace_end === 'function') host_trace_end(_h);
        }
    }

    /* Continuous feedback guard: bypass Line In slots while speaker-feedback risk
     * is present (boot or headphones unplugged), un-bypass when safe, and raise
     * the modal when the shadow UI is on screen. Throttled to a few times/sec. */
    if (++_feedbackHoldTickCounter >= FEEDBACK_HOLD_CHECK_INTERVAL) {
        _feedbackHoldTickCounter = 0;
        /* Spanned: reads `synth_module` for all FOUR slots unconditionally,
         * which is four synchronous IPC round trips at ~2.8ms — ~11ms on ONE
         * tick, six times a second, on top of the grid's own ~7ms. That is
         * over the 16.67ms period, and six overruns a second is exactly the
         * unexplained 60 -> 54-56. It also accounts for the 0.4 reads/tick of
         * `synth_module` the last session logged and attributed elsewhere.
         * Predicted, not yet measured — this span is the test. */
        const _h = (typeof host_trace_begin === 'function') ? host_trace_begin("js.feedback_guard") : 0;
        try { reconcileFeedbackHolds(); } catch (e) { debugLog("reconcileFeedbackHolds error: " + e); }
        finally { if (_h && typeof host_trace_end === 'function') host_trace_end(_h); }
    }

    /* Draw upgrade overlay if active (takes priority over normal UI) */
    if (_upgradeOverlayText) {
        clear_screen();
        drawStatusOverlay("Upgrading", _upgradeOverlayText);
        host_flush_display();
        return;
    }

    /* Check for jump-to-slot flag on EVERY tick (flag can be set while UI is running) */
    if (typeof shadow_get_ui_flags === "function") {
        const flags = shadow_get_ui_flags();
        globalThis._debugFlags = flags;  /* Debug: store for display */

        {
            /* Settings/Screenreader flags take priority (clear conflicting SLOT flag) */
            if (flags & SHADOW_UI_FLAG_JUMP_TO_TOOLS) {
                debugLog("TOOLS flag detected, entering Tools menu");
                try {
                    if (!tryResumeSuspendedTool()) {
                        /* Park (or fully exit) the active overtake before
                         * the view flips to TOOLS — otherwise its DSP and
                         * JS callbacks orphan: not in suspendedOvertakes
                         * (no * marker), still loaded in slot 0. Re-selecting
                         * the same module from the menu then takes the
                         * fresh-load path and the shim destroys+recreates
                         * the DSP, wiping all sequencer state. */
                        if (overtakeModuleLoaded && overtakeModuleCallbacks) {
                            if (overtakeSuspendKeepsJs) {
                                debugLog("TOOLS flag: suspending active overtake before menu");
                                suspendOvertakeMode();
                            } else if (toolOvertakeActive) {
                                debugLog("TOOLS flag: exiting active tool before menu");
                                exitToolOvertake();
                            } else {
                                debugLog("TOOLS flag: exiting active overtake before menu");
                                exitOvertakeMode();
                                /* exitOvertakeMode() only *arms* the exit
                                 * (overtakeExitPending); it completes on the
                                 * next VIEWS.OVERTAKE_MODULE tick. We are
                                 * about to leave that view, so that tick never
                                 * comes and the flag stays set forever:
                                 * overtake_mode never drops to 0 (so the C-side
                                 * never replays Move's LED snapshot), and the
                                 * next load of ANY overtake module hits the
                                 * exit branch instead of init(). Drain it here.
                                 * skipNavigation=true: we want the teardown,
                                 * not its return-to-Move tail — the
                                 * enterToolsMenu() below owns navigation. */
                                completeOvertakeExit(true);
                            }
                        }
                        enterToolsMenu();
                    }
                } catch (e) {
                    debugLog("TOOLS flag: enterToolsMenu threw: " + e + " stack=" + (e && e.stack ? e.stack : "none"));
                }
                /* Always clear flag, even on exception, so we don't loop forever */
                if (typeof shadow_clear_ui_flags === "function") {
                    shadow_clear_ui_flags(SHADOW_UI_FLAG_JUMP_TO_TOOLS | SHADOW_UI_FLAG_JUMP_TO_SLOT);
                }
            } else if (flags & SHADOW_UI_FLAG_JUMP_TO_SETTINGS) {
                debugLog("SETTINGS flag detected, entering Global Settings");
                enterGlobalSettings();
                if (typeof shadow_clear_ui_flags === "function") {
                    shadow_clear_ui_flags(SHADOW_UI_FLAG_JUMP_TO_SETTINGS | SHADOW_UI_FLAG_JUMP_TO_SLOT);
                }
            } else if (flags & SHADOW_UI_FLAG_JUMP_TO_SCREENREADER) {
                debugLog("SCREENREADER flag detected, entering Global Settings -> Screen Reader");
                enterGlobalSettingsScreenReader();
                if (typeof shadow_clear_ui_flags === "function") {
                    shadow_clear_ui_flags(SHADOW_UI_FLAG_JUMP_TO_SCREENREADER | SHADOW_UI_FLAG_JUMP_TO_SLOT);
                }
            } else if (flags & SHADOW_UI_FLAG_JUMP_TO_SLOT) {
                /* Get the slot to jump to (from ui_slot, set by shim) */
                if (typeof shadow_get_ui_slot === "function") {
                    const jumpSlot = shadow_get_ui_slot();
                    if (jumpSlot >= 0 && jumpSlot < SHADOW_UI_SLOTS) {
                        selectedSlot = jumpSlot;
                        enterChainEdit(jumpSlot);
                    }
                }
                /* Clear the flag */
                if (typeof shadow_clear_ui_flags === "function") {
                    shadow_clear_ui_flags(SHADOW_UI_FLAG_JUMP_TO_SLOT);
                }
            }
            if (flags & SHADOW_UI_FLAG_JUMP_TO_MASTER_FX) {
                /* Always jump to Master FX view */
                enterMasterFxSettings();
                /* Clear the flag */
                if (typeof shadow_clear_ui_flags === "function") {
                    shadow_clear_ui_flags(SHADOW_UI_FLAG_JUMP_TO_MASTER_FX);
                }
            }
            if (flags & SHADOW_UI_FLAG_JUMP_TO_OVERTAKE) {
                const suspendedCount = Object.keys(suspendedOvertakes).length;
                debugLog("OVERTAKE flag detected, view=" + view + " suspended=" + suspendedCount);
                const isSuspend = (typeof shadow_get_suspend_overtake === "function") &&
                                  shadow_get_suspend_overtake() !== 0;
                try {
                    if (view === VIEWS.OVERTAKE_MODULE) {
                        if (isSuspend) {
                            debugLog("suspending overtake mode (shim-initiated)");
                            suspendOvertakeMode();
                        } else {
                            debugLog("exiting overtake mode");
                            exitOvertakeMode();
                        }
                    } else {
                        debugLog("entering tools menu (overtake merged)");
                        enterToolsMenu();
                    }
                } catch (e) {
                    debugLog("OVERTAKE flag handler threw: " + e + " stack=" + (e && e.stack ? e.stack : "none"));
                }
                /* Always clear flag so we don't loop */
                if (typeof shadow_clear_ui_flags === "function") {
                    shadow_clear_ui_flags(SHADOW_UI_FLAG_JUMP_TO_OVERTAKE);
                }
            }
        }
        if (flags & SHADOW_UI_FLAG_SAVE_STATE) {
            debugLog("SAVE_STATE flag detected — shutdown imminent, saving all state");
            autosaveAllSlots();
            saveMasterFxChainConfig();
            saveChainConfigToDir(activeSlotStateDir);
            if (typeof shadow_clear_ui_flags === "function") {
                shadow_clear_ui_flags(SHADOW_UI_FLAG_SAVE_STATE);
            }
        }
        if (flags & SHADOW_UI_FLAG_SET_CHANGED) {
            debugLog("SET_CHANGED flag detected — switching slot state directory");

            /* 1. Save current state to outgoing directory */
            autosaveAllSlots();
            saveMasterFxChainConfig();
            /* Save chain config (volumes, channels, mute/solo) to outgoing set dir */
            saveChainConfigToDir(activeSlotStateDir);
            /* Save current RNBO graph (if RNBO is running) */
            saveRnboGraphToDir(activeSlotStateDir);

            /* 2. Get UUID and set name from shim (in-memory, no file I/O on audio thread) */
            const activeSetRaw = getSlotParam(0, "active_set");
            const activeSetLines = activeSetRaw ? activeSetRaw.split("\n") : [];
            const uuid = activeSetLines[0] ? activeSetLines[0].trim() : "";
            const setName = activeSetLines[1] ? activeSetLines[1].trim() : "";
            /* Write active_set.txt for boot persistence (UI thread, not audio thread) */
            if (uuid) {
                host_write_file("/data/UserData/schwung/active_set.txt", uuid + "\n" + setName);
            }

            /* 3. Determine new directory */
            const newDir = uuid
                ? "/data/UserData/schwung/set_state/" + uuid
                : SLOT_STATE_DIR_DEFAULT;

            if (uuid && typeof host_ensure_dir === "function") {
                host_ensure_dir("/data/UserData/schwung/set_state");
                host_ensure_dir(newDir);
            }

            /* 4. First visit to this set: seed its state directory.
             *    Detect if this is a duplicated set by comparing Song.abl sizes,
             *    then copy state from the source. Otherwise start with empty slots. */
            if (uuid && !host_file_exists(newDir + "/slot_0.json")) {
                let copySourceDir = null;
                /* Check for pre-existing copy_source.txt (from older shim) */
                const copySourceUuid = host_read_file(newDir + "/copy_source.txt");
                if (copySourceUuid && copySourceUuid.trim()) {
                    const srcDir = "/data/UserData/schwung/set_state/" + copySourceUuid.trim();
                    if (host_file_exists(srcDir + "/slot_0.json")) {
                        copySourceDir = srcDir;
                    }
                }
                /* Detect copy by comparing Song.abl sizes (if name suggests a copy) */
                if (!copySourceDir && setName &&
                    (setName.toLowerCase().indexOf("copy") >= 0 ||
                     setName.toLowerCase().indexOf("duplicate") >= 0)) {
                    copySourceDir = detectCopySource(uuid);
                }
                if (copySourceDir) {
                    debugLog("SET_CHANGED: duplicated set, copying from " + copySourceDir);
                    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                        const src = host_read_file(copySourceDir + "/slot_" + i + ".json");
                        if (src) host_write_file(newDir + "/slot_" + i + ".json", src);
                    }
                    /* master_fx_N.json is bounded by MASTER_FX_SLOTS, NOT by
                     * the instrument-slot count. They are different concepts
                     * that merely happen to both be 4 today; copying both
                     * families in one SHADOW_UI_SLOTS loop means duplicating a
                     * set would silently drop Master FX 5-8. The C seeder
                     * (shadow_set_pages.c) is split the same way. */
                    for (let i = 0; i < MASTER_FX_SLOTS; i++) {
                        const mfx = host_read_file(copySourceDir + "/master_fx_" + i + ".json");
                        if (mfx) host_write_file(newDir + "/master_fx_" + i + ".json", mfx);
                    }
                    /* Also copy chain config */
                    const chainCfg = host_read_file(copySourceDir + "/shadow_chain_config.json");
                    if (chainCfg) host_write_file(newDir + "/shadow_chain_config.json", chainCfg);
                } else {
                    /* New set — start with empty slots */
                    debugLog("SET_CHANGED: new set, starting with empty slots");
                    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                        host_write_file(newDir + "/slot_" + i + ".json", "{}\n");
                    }
                    /* Separate bound — see the copy path above. */
                    for (let i = 0; i < MASTER_FX_SLOTS; i++) {
                        host_write_file(newDir + "/master_fx_" + i + ".json", "{}\n");
                    }
                    /* Seed a default chain config so receive channels reset to
                     * per-track defaults (Ch 1-4). Without this, the upcoming
                     * loadChainConfigFromDir silently bails and the shim's
                     * slot.channel keeps stale values from the prior set —
                     * symptom: no audio on new sets until user toggles Recv Ch. */
                    const defaultCfg = { slots: [] };
                    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                        defaultCfg.slots.push({
                            name: "",
                            channel: i + 1,
                            volume: 1.0,
                            forward_channel: -1,
                            muted: 0,
                            soloed: 0
                        });
                    }
                    host_write_file(newDir + "/shadow_chain_config.json",
                        JSON.stringify(defaultCfg, null, 2) + "\n");
                }
            }

            /* 5. Switch directory and load chain config (volumes/channels/mute/solo) */
            const oldDir = activeSlotStateDir;
            activeSlotStateDir = newDir;
            /* Different directory — what we last wrote says nothing about
             * what is in THIS set's files, so never skip a write on its
             * behalf. */
            invalidateAutosaveWriteCache();
            debugLog("SET_CHANGED: " + oldDir + " -> " + newDir);
            /* A remembered position belongs to the set it was chosen in. Carried
             * across, it points at whatever occupies that index in the NEW
             * chain -- usually the MIDI FX `+`, since a fresh set is empty and
             * that is index 0. Forgetting lets defaultChainComponent put the
             * selection on the synth, which is what a slot is about. */
            for (let i = 0; i < lastChainComponent.length; i++) lastChainComponent[i] = null;
            loadChainConfigFromDir(newDir);

            /* 6. Two-pass reload: clear ALL old slots first (freeing memory),
             *    then load new slots. This reduces peak memory when switching
             *    between sets with heavy synths. */

            /* Pass 1: Clear all slots to free memory before loading anything new */
            debugLog("SET_CHANGED: pass 1 — clearing all slots");
            for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                setSlotParamWithTimeout(i, "clear", "", 1500);
            }

            /* Pass 2: Load new state for non-empty slots */
            debugLog("SET_CHANGED: pass 2 — loading new slot states");
            for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                const path = activeSlotStateDir + "/slot_" + i + ".json";
                if (host_file_exists(path)) {
                    const raw = host_read_file(path);
                    /* Non-empty state: try load_file with extended timeout + retry. */
                    if (raw && raw.length > 10) {
                        let loadOk = setSlotParamWithTimeout(i, "load_file", path, 1500);
                        if (!loadOk) {
                            debugLog("SET_CHANGED: load_file timeout slot " + (i + 1) + " path " + path + " (retry)");
                            loadOk = setSlotParamWithTimeout(i, "load_file", path, 3000);
                        }
                        if (loadOk) {
                            debugLog("SET_CHANGED: slot " + (i + 1) + " loaded");
                        } else {
                            debugLog("SET_CHANGED: slot " + (i + 1) + " not restored (load timeout)");
                        }
                    } else {
                        debugLog("SET_CHANGED: slot " + (i + 1) + " empty state (already cleared)");
                    }
                } else {
                    debugLog("SET_CHANGED: slot " + (i + 1) + " no state file (already cleared)");
                }
            }
            /* Refresh UI state immediately so display reflects new slot contents */
            for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                lastSlotModuleSignatures[i] = "";  /* force refresh */
                refreshSlotModuleSignature(i);
                /* Drop any pending user-cleared flag from the outgoing set;
                 * the new set's emptiness/non-emptiness is determined by its
                 * own files, not by what the user did before the switch. */
                slotUserCleared[i] = false;
            }

            /* Suppress autosave briefly so async DSP settling doesn't
             * overwrite the freshly-written slot files */
            autosaveSuppressUntil = 150; /* ~5 seconds at 30fps */

            /* 7. Reload master FX modules from per-set state files */
            for (let mfxi = 0; mfxi < MASTER_FX_SLOTS; mfxi++) {
                const mfxPath = activeSlotStateDir + "/master_fx_" + mfxi + ".json";
                let mfxDspPath = "";
                let mfxModuleId = "";
                let mfxData = null;
                if (host_file_exists(mfxPath)) {
                    try {
                        const mfxRaw = host_read_file(mfxPath);
                        if (mfxRaw && mfxRaw.length > 10) {
                            mfxData = JSON.parse(mfxRaw);
                            mfxDspPath = mfxData.module_path || "";
                            mfxModuleId = mfxData.module_id || "";
                        }
                    } catch (e) {}
                }
                /* Load or unload the module */
                setMasterFxSlotModule(mfxi, mfxDspPath);
                const key = `fx${mfxi + 1}`;
                masterFxConfig[key].module = mfxModuleId;
                /* Different module — it may implement display_name even if the
                 * last one didn't, so poll it at full rate again. */
                delete fxDisplayNameCache[`master:${key}`];
                delete fxDisplayNameSkip[`master:${key}`];
                delete fxDisplayNameBackoff[`master:${key}`];

                /* Restore plugin state/params if available */
                if (mfxData) {
                    try {
                        if (mfxDspPath && mfxData.state) {
                            const stateStr = (typeof mfxData.state === "string")
                                ? mfxData.state
                                : JSON.stringify(mfxData.state);
                            shadow_set_param(0, `master_fx:fx${mfxi + 1}:state`, stateStr);
                        }
                        if (mfxDspPath && mfxData.params) {
                            for (const [pk, pv] of Object.entries(mfxData.params)) {
                                shadow_set_param(0, `master_fx:fx${mfxi + 1}:${pk}`, String(pv));
                            }
                        }
                        if (mfxi === 0 && mfxData.lfos && typeof shadow_set_param === "function") {
                            for (let li = 1; li <= 2; li++) {
                                const lfoConfig = mfxData.lfos["lfo" + li];
                                if (!lfoConfig) continue;
                                restoreMasterFxLfo(li, lfoConfig);
                            }
                        }
                    } catch (e) {}
                }
                debugLog("SET_CHANGED: MFX " + mfxi + " -> " + (mfxModuleId || "(none)"));
            }
            /* 8. Refresh slot names from new autosave files */
            for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                slots[i].name = "";
                const raw = host_read_file(activeSlotStateDir + "/slot_" + i + ".json");
                if (raw) {
                    const m = raw.match(/"name"\s*:\s*"([^"]+)"/);
                    if (m && m[1]) slots[i].name = m[1];
                }
            }
            saveSlotsToConfig(slots);
            needsRedraw = true;

            /* 9. Show overlay notification (~2 seconds) */
            if (setName) {
                showOverlay("Set Loaded", setName, 60);
            }

            /* 10. Load RNBO graph for this set (if RNBO is running) */
            loadRnboGraphFromDir(activeSlotStateDir);

            /* 10b. Request Link tempo override to match the new set's tempo.
             * link-subscriber picks this up and only applies when numPeers==1
             * (Move alone), so we don't clobber collaboration with Live. */
            if (uuid && setName) {
                try {
                    const songPath = "/data/UserData/UserLibrary/Sets/" + uuid + "/" + setName + "/Song.abl";
                    const songJson = host_read_file(songPath);
                    if (songJson) {
                        const m = songJson.match(/"tempo"\s*:\s*([0-9.]+)/);
                        if (m && m[1]) {
                            const bpm = parseFloat(m[1]);
                            if (bpm >= 20 && bpm <= 999) {
                                host_write_file("/data/UserData/schwung/desired-tempo", bpm.toFixed(4) + "\n");
                                debugLog("SET_CHANGED: requested Link tempo override " + bpm.toFixed(2) + " BPM");
                            }
                        }
                    }
                } catch (e) {
                    debugLog("SET_CHANGED: tempo-override write failed: " + e);
                }
            }

            /* 11. Clear flag */
            if (typeof shadow_clear_ui_flags === "function") {
                shadow_clear_ui_flags(SHADOW_UI_FLAG_SET_CHANGED);
            }
            debugLog("SET_CHANGED: reload complete");
        }
        /* SETTINGS and SCREENREADER flags are handled earlier with SLOT/MFX/OVERTAKE */
    }

    /*
     * FLEET CONTRACT CAPTURE, on a trigger file.
     *
     * tools/param-pages/dump_contracts_device.js can only run in this QuickJS
     * context -- ui_hierarchy and chain_params are served by the LOADED DSP,
     * not by files on disk, so every module has to be instantiated to be
     * captured. Its own header says to call it "from the shadow UI\'s script
     * hook", and there was no such hook: the tool has been unrunnable since it
     * was written, which is why the test fixture is still a third-party
     * capture from 2026-07-15 covering 76 of ~113 modules.
     *
     * Same idiom as align_dump_trigger and the other debug files: absent by
     * default, costs one host_file_exists per second, and deletes itself so a
     * forgotten trigger cannot re-run on every boot.
     *
     * DESTRUCTIVE and LOUD: it loads each module into slot 3 in turn and you
     * will hear them. The tool saves and restores that slot, but a crash
     * mid-run leaves the wrong module there. Use an empty slot.
     *
     *   ssh ableton@move.local "touch /data/UserData/schwung/dump_contracts_trigger"
     *   ...then collect /data/UserData/schwung/module-contracts.json
     */
    const _cdNow = Date.now();
    if (!contractDumpDone && (_cdNow - contractDumpCheckedMs) > 1000 &&
        typeof host_file_exists === "function") {
        contractDumpCheckedMs = _cdNow;
        if (host_file_exists("/data/UserData/schwung/dump_contracts_trigger")) {
        contractDumpDone = true;
        /* DELETE the trigger, don't empty it. contractDumpDone only latches for
         * the life of this process, so an emptied-but-present trigger re-fired
         * the whole loud capture on the next service restart -- which is
         * exactly what a deploy does. Observed: install.sh restarted the shadow
         * UI and the capture ran again unbidden, before the new tool had even
         * been staged.
         *
         * There is deliberately no "empty means disarmed" fallback: `touch`
         * creates an empty file, so that rule would disarm the documented way
         * of arming it. */
        try { os.remove("/data/UserData/schwung/dump_contracts_trigger"); } catch (e) {}
        try {
            const src = host_read_file("/data/UserData/schwung/tools/dump_contracts_device.js");
            if (!src) {
                debugLog("dump_contracts: tools/dump_contracts_device.js not on the device");
            } else {
                /* Evaluated rather than imported: the tool is written in ES5
                 * var/function style with no exports precisely so it can be
                 * loaded this way.
                 *
                 * new Function, NOT (0, eval). Indirect eval runs in GLOBAL
                 * scope, and `os` here is an ES module IMPORT -- a
                 * module-scoped binding that is not on globalThis. The tool
                 * enumerates the fleet with os.readdir, so under indirect eval
                 * every one of its three category scans threw ReferenceError
                 * into a `catch { continue; }` and it captured zero modules in
                 * 18ms while reporting success. Passing os/std in as
                 * parameters makes the dependency explicit and keeps them off
                 * globalThis. */
                new Function("os", "std", src)(os, std);
                if (typeof globalThis.dumpModuleContracts === "function") {
                    debugLog("dump_contracts: starting -- slot 3 will make noise");
                    globalThis.dumpModuleContracts();
                    /* The tool returns its OUTPUT PATH, not a count -- reading
                     * the count back out of the file is the only honest report,
                     * and a zero here means the run found no modules at all
                     * rather than that the fleet is empty. */
                    let n = -1;
                    try {
                        n = JSON.parse(host_read_file(
                            "/data/UserData/schwung/module-contracts.json")).module_count;
                    } catch (e) {}
                    if (n > 0) debugLog("dump_contracts: captured " + n + " modules");
                    else debugLog("dump_contracts: FAILED -- captured " + n +
                                  " modules; the fleet scan found nothing");
                } else {
                    debugLog("dump_contracts: the tool defined no dumpModuleContracts()");
                }
            }
        } catch (e) {
            debugLog("dump_contracts: failed: " + e);
        }
        }
    }

    /* Check for open-in-tool command from web UI */
    if (typeof shadow_get_open_tool_cmd === "function") {
        const toolCmd = shadow_get_open_tool_cmd();
        if (toolCmd === 1) {
            const cmdJson = host_read_file("/data/UserData/schwung/open_tool_cmd.json");
            if (cmdJson) {
                try {
                    const cmd = JSON.parse(cmdJson);
                    if (cmd.file_path && cmd.tool_id) {
                        debugLog("open_tool_cmd: opening " + cmd.file_path + " in " + cmd.tool_id);
                        /* host_open_file_in_tool is only defined inside setupModuleParamShims,
                         * so we replicate its logic here using the global functions directly. */
                        if (!toolModules || !toolModules.length) {
                            toolModules = scanForToolModules();
                        }
                        const tool = toolModules.find(t => t.id === cmd.tool_id);
                        if (tool) {
                            unloadModuleUi();
                            startInteractiveTool(tool, cmd.file_path);
                        } else {
                            /* Fall back to the OVERTAKE list before giving up.
                             * The two live in different scans — component_type
                             * "tool" here, "overtake" there — so an overtake
                             * module could never be opened this way, and the
                             * command answered "tool not found" for an id that
                             * plainly exists on disk. That blocks
                             * pytest-schwung from launching one, which is the
                             * difference between an unattended UI test run and
                             * one that needs a person to press a button first. */
                            const overtakes = scanForOvertakeModules() || [];
                            const ot = overtakes.find(o => o.id === cmd.tool_id);
                            if (ot) {
                                debugLog("open_tool_cmd: " + cmd.tool_id +
                                         " is an overtake module, loading it");
                                unloadModuleUi();
                                loadOvertakeModule(ot);
                            } else {
                                debugLog("open_tool_cmd: not found as tool or " +
                                         "overtake module: " + cmd.tool_id);
                            }
                        }
                    }
                } catch (e) {
                    debugLog("open_tool_cmd: JSON parse error: " + e);
                }
            }
        }
    }

    if (filepathBrowserState &&
        filepathBrowserState.livePreviewEnabled &&
        filepathBrowserState.previewPendingPath &&
        Date.now() - filepathBrowserState.previewPendingTime >= 150) {
        applyLivePreview(filepathBrowserState, { kind: "file", path: filepathBrowserState.previewPendingPath });
        filepathBrowserState.previewPendingPath = "";
        filepathBrowserState.previewPendingTime = 0;
    }

    /* Tool file browser audio preview debounce */
    previewTick();

    refreshCounter++;

    /* Skip all IPC polling and file I/O while an overtake module is running. */
    const isOvertakeActive = (view === VIEWS.OVERTAKE_MODULE || view === VIEWS.OVERTAKE_MENU);

    if (!isOvertakeActive && refreshCounter % 120 === 0) {
        refreshSlots();
    }

    /* Periodic autosave (suppressed briefly after set change) */
    if (autosaveSuppressUntil > 0) {
        autosaveSuppressUntil--;
        autosaveCounter = 0;
    } else {
        autosaveCounter++;
        if (!isOvertakeActive && autosaveCounter >= AUTOSAVE_INTERVAL) {
            autosaveCounter = 0;
            autosaveJob = 0;   /* arm; drained one step per tick below */
        }
    }

    /*
     * Autosave, ONE SLOT PER TICK.
     *
     * It used to do all four slots and the master FX chain in a single tick.
     * Measured on device that was ~70 sequential IPC reads landing on one
     * frame — ~200ms, i.e. about eleven dropped frames, every five seconds.
     * It is the visible hitch while nothing is being touched, and the read
     * histogram showed it plainly: 758 ticks doing 3 reads, and a handful
     * doing 68-72.
     *
     * Nothing about autosave is latency-sensitive — it is background
     * persistence on a five-second timer — so it has no business being
     * atomic with respect to the frame. Spread over five ticks (~83ms) it
     * finishes just as promptly in wall-clock terms while no single frame
     * carries more than a slot's worth of reads.
     *
     * Deliberately NOT restructured below slot granularity: buildSlotPatchJson
     * has a history of silently dropping saves, and a resumable version of it
     * would risk persisting a half-read slot. A slot is the unit that is
     * already all-or-nothing.
     */
    if (autosaveJob !== null) {
        if (isOvertakeActive) {
            autosaveJob = null;          /* overtake owns the surface; abandon */
        } else {
            if (autosaveJob < SHADOW_UI_SLOTS) autosaveOneSlot(autosaveJob);
            else saveMasterFxChainConfig();
            autosaveJob++;
            if (autosaveJob > SHADOW_UI_SLOTS) autosaveJob = null;
        }
    }
    /* Refresh dirty cache frequently for responsive UI */
    if (!isOvertakeActive && refreshCounter % 15 === 0) {
        for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
            const dirty = getSlotParam(i, "dirty");
            const isDirty = (dirty === "1");
            if (slotDirtyCache[i] !== isDirty) {
                slotDirtyCache[i] = isDirty;
                needsRedraw = true;
            }
        }
    }

    /* Poll FX display_name for change-based announcements (e.g. key detection).
     * Check every ~1 second (30 ticks at 30fps). Only poll slots that have FX loaded. */
    if (!isOvertakeActive && refreshCounter % 30 === 0) {
        /* Per-slot FX */
        for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
            const cfg = chainConfigs[i];
            if (!cfg) continue;
            for (let f = 0; f < cfg.fx.length; f++) {
                if (!cfg.fx[f] || !cfg.fx[f].module) continue;
                const comp = `fx${f + 1}`;
                const cacheKey = `${i}:${comp}`;
                const name = pollFxDisplayName(i, `${comp}:display_name`, cacheKey);
                if (name && name !== fxDisplayNameCache[cacheKey]) {
                    const prev = fxDisplayNameCache[cacheKey];
                    fxDisplayNameCache[cacheKey] = name;
                    if (prev) {
                        announce(name);
                        needsRedraw = true;
                    }
                }
            }
        }
        /* Master FX */
        for (const { key } of masterFxChainComponents()) {
            if (key === "settings") continue;
            if (!masterFxConfig[key] || !masterFxConfig[key].module) continue;
            const cacheKey = `master:${key}`;
            const name = pollFxDisplayName(0, `master_fx:${key}:display_name`, cacheKey);
            if (name && name !== fxDisplayNameCache[cacheKey]) {
                const prev = fxDisplayNameCache[cacheKey];
                fxDisplayNameCache[cacheKey] = name;
                if (prev) {
                    announce(name);
                    needsRedraw = true;
                }
            }
        }
    }

    let currentTargetSlot = 0;
    if (typeof shadow_get_selected_slot === "function") {
        currentTargetSlot = shadow_get_selected_slot();
    }
    if (!isOvertakeActive && refreshCounter % 30 === 0) {
        refreshSlotModuleSignature(selectedSlot);
        if (currentTargetSlot !== selectedSlot) {
            refreshSlotModuleSignature(currentTargetSlot);
        }
    }

    /* Update text entry state */
    if (isTextEntryActive()) {
        if (tickTextEntry()) {
            needsRedraw = true;
        }
    }

    /* Update shared overlay timeout */
    if (tickOverlay()) {
        needsRedraw = true;
    }

    /* Throttled knob overlay refresh - once per frame instead of per CC */
    refreshPendingKnobOverlay();

    /* Throttled hierarchy knob adjustment - once per frame. During co-run the
     * outer view is OVERTAKE_MODULE; wrap so getKnobContext (cache keyed on
     * view) resolves against the chain editor's view and the accumulated knob
     * delta isn't silently dropped under the wrong context. */
    if (coRunUiActive()) {
        runCoRunChainEdit(processPendingHierKnob);
    } else {
        processPendingHierKnob();
    }

    if (view === VIEWS.CANVAS || (coRunUiActive() && coRunView === VIEWS.CANVAS)) {
        /* In co-run the outer view is OVERTAKE_MODULE, so call through
         * runCoRunChainEdit (sets view=coRunView) — otherwise tickCanvasPreview's
         * own `view !== VIEWS.CANVAS` guard early-returns and the module's tick
         * hook (animation/state polling) never fires. Mirrors the draw path. */
        if (coRunUiActive()) {
            runCoRunChainEdit(tickCanvasPreview);
        } else {
            tickCanvasPreview();
        }
        canvasTickCounter = (canvasTickCounter || 0) + 1;
        if (canvasTickCounter % 3 === 0) needsRedraw = true;
    }

    /* Refresh knob mappings if track-selected slot changed */
    if (lastKnobSlot !== currentTargetSlot) {
        fetchKnobMappings(currentTargetSlot);
        invalidateKnobContextCache();  /* Clear stale contexts when target slot changes */
        /* If in Master FX view, switch to that slot's detail when track button pressed */
        if (view === VIEWS.MASTER_FX) {
            enterChainEdit(currentTargetSlot);
        }
    }

    /* Poll overlay state from shim (sampler/skipback) */
    if (typeof shadow_get_overlay_sequence === "function") {
        const seq = shadow_get_overlay_sequence();
        if (seq !== lastOverlaySeq) {
            lastOverlaySeq = seq;
            overlayState = shadow_get_overlay_state();
        }
    }

    redrawCounter++;
    /* Force redraw every frame when overlay is active (for VU meter + flash) */
    const overlayActive = overlayState && overlayState.type !== OVERLAY_NONE;
    if (!needsRedraw && !overlayActive && (redrawCounter % REDRAW_INTERVAL !== 0)) {
        return;
    }
    needsRedraw = false;

    /* Fullscreen sampler overlay takes over the entire display */
    if (overlayState && drawSamplerOverlay(overlayState)) {
        if (typeof shadow_set_display_overlay === "function") {
            shadow_set_display_overlay(2, 0, 0, 0, 0);
        }
        return;
    }

    /* Skipback toast - render to shadow display, request rect overlay on native */
    if (overlayState && overlayState.type === OVERLAY_SKIPBACK &&
        overlayState.skipbackActive && overlayState.skipbackOverlayTimeout > 0) {
        clear_screen();
        drawSkipbackToast();
        if (typeof shadow_set_display_overlay === "function") {
            shadow_set_display_overlay(1, 9, 22, 110, 20);
        }
        return;
    }

    /* Set page toast - render to shadow display, request rect overlay on native */
    if (overlayState && overlayState.type === OVERLAY_SET_PAGE &&
        overlayState.setPageActive && overlayState.setPageTimeout > 0) {
        clear_screen();
        drawSetPageToast(overlayState);
        if (typeof shadow_set_display_overlay === "function") {
            shadow_set_display_overlay(1,
                SET_PAGE_BOX_X, SET_PAGE_BOX_Y,
                SET_PAGE_BOX_W, SET_PAGE_BOX_H);
        }
        return;
    }

    /* Shift+knob overlay - render to shadow display, request rect overlay on native */
    if (overlayState && overlayState.type === OVERLAY_SHIFT_KNOB &&
        overlayState.shiftKnobActive && overlayState.shiftKnobTimeout > 0) {
        clear_screen();
        drawShiftKnobOverlay(overlayState);
        if (typeof shadow_set_display_overlay === "function") {
            shadow_set_display_overlay(1,
                SHIFT_KNOB_BOX_X, SHIFT_KNOB_BOX_Y,
                SHIFT_KNOB_BOX_W, SHIFT_KNOB_BOX_H);
        }
        return;
    }

    /* No overlay active - clear overlay display mode */
    if (typeof shadow_set_display_overlay === "function") {
        shadow_set_display_overlay(0, 0, 0, 0, 0);
    }

    /* Flush deferred wav player file_path after DSP load */
    wavPlayerTick();

    /* CO-RUN: reconcile chain-edit slot from SHM each frame. The tool calls
     * shadow_corun_begin(CORUN_TARGET_CHAIN_EDIT, slot, keep_mask) to enter;
     * the framework calls shadow_corun_end() on Back press, after which
     * shadow_corun_state() returns null and we tear down the editor. */
    if (typeof shadow_corun_state === "function") {
        const _st = shadow_corun_state();
        /* Overlay teardown: if the tool ended co-run (its own exit gesture / Menu /
         * set change) while an addressed-view overlay was open, the framework
         * already restored the display owner + keep_mask via shadow_corun_end();
         * clear the JS overlay state here so coRunUiActive() goes false and shadow_ui
         * hands the screen back to the tool instead of stranding the overlay. */
        if (corunOverlayId != null && !_st) {
            corunOverlayId = null;
            corunOverlayRootView = -1;
            coRunView = VIEWS.OVERTAKE_MODULE;
            needsRedraw = true;
        }
        const _slot = (_st && _st.target === CORUN_TARGET_CHAIN_EDIT) ? _st.id : -1;
        coRunKeepMask = (_st && typeof _st.keep_mask === "number") ? (_st.keep_mask | 0) : 0;
        if (_slot !== coRunChainEditSlot) {
            coRunChainEditSlot = _slot;
            if (_slot >= 0) {
                /* Entering co-run: prime the chain editor's state for slot N.
                 * Mirrors enterChainEdit() but does NOT touch the outer view
                 * (must stay VIEWS.OVERTAKE_MODULE so the tool keeps ticking). */
                selectedSlot = _slot;
                if (typeof updateFocusedSlot === "function") updateFocusedSlot(_slot);
                restoreChainComponent(_slot);
                coRunView = VIEWS.CHAIN_EDIT;
                needsRedraw = true;
            } else {
                /* Framework ended co-run (Back press) — drop back to the tool
                 * view; the chain editor stops drawing on next tick. */
                coRunView = VIEWS.OVERTAKE_MODULE;
                needsRedraw = true;
            }
        }
    }

    /* A slot-action modal that the grid handed to the list has finished — go
     * back to the grid the user actually opened. Flags only, no IPC. Runs
     * before the draw so the frame that notices is already the grid's. */
    if (view === VIEWS.CHAIN_SETTINGS) maybeReturnToSlotGrid();
    /* The Master FX half of the same reconcile. Its modals live under
     * VIEWS.MASTER_FX rather than a settings view of their own. */
    if (view === VIEWS.MASTER_FX) maybeReturnToMasterGrid();
    /* ...and the Global Settings half. VIEWS.GLOBAL_SETTINGS is nothing but the
     * help viewer's host now, so "the surface is idle again" means the help
     * stack has emptied. */
    if (view === VIEWS.GLOBAL_SETTINGS) maybeReturnToGlobalGrid();

    /* Guarded: a throw in any draw function would otherwise repeat every
     * frame — frozen screen with no recovery, since the C loop keeps
     * calling tick() regardless. (The OVERTAKE_MODULE case has its own
     * handling; this guard covers every other view and the overlays.) */
    try {

    switch (view) {
        case VIEWS.SLOTS:
            drawSlots();
            break;
        case VIEWS.SLOT_SETTINGS:
            drawSlotSettings();
            break;
        case VIEWS.PATCHES:
            drawPatches();
            break;
        case VIEWS.PATCH_DETAIL:
            drawPatchDetail();
            break;
        case VIEWS.COMPONENT_PARAMS:
            drawComponentParams();
            break;
        case VIEWS.PRESETS:
            drawPresets();
            break;
        case VIEWS.PRESET_DETAIL:
            drawPresetDetail();
            break;
        case VIEWS.MASTER_FX:
            drawMasterFx();
            break;
        case VIEWS.CHAIN_EDIT:
            drawChainEdit();
            break;
        case VIEWS.COMPONENT_SELECT:
            drawComponentSelect();
            break;
        case VIEWS.CHAIN_SETTINGS:
            drawChainSettings();
            break;
        case VIEWS.COMPONENT_EDIT:
            if (loadedModuleUi && loadedModuleUi.tick) {
                /* Let the loaded module UI handle its own tick/draw */
                loadedModuleUi.tick();
            } else {
                /* Fall back to simple preset browser */
                drawComponentEdit();
            }
            break;
        case VIEWS.HIERARCHY_EDITOR:
            drawHierarchyEditor();
            break;
        /* The grid draws grids; every other page kind it plans (preset browser,
         * items list, mode select, child selector) belongs to the list editor,
         * which drawParamPages declines by returning false. */
        case VIEWS.PARAM_PAGES:
            if (!drawParamPages()) { enterHierarchyEditorFromParamPages(); drawHierarchyEditor(); }
            break;
        case VIEWS.CANVAS:
            drawCanvasPreview();
            break;
        case VIEWS.FILEPATH_BROWSER:
            drawFilepathBrowser();
            break;
        case VIEWS.KNOB_EDITOR:
            drawKnobEditor();
            break;
        case VIEWS.KNOB_PARAM_PICKER:
            drawKnobParamPicker();
            break;
        case VIEWS.DYNAMIC_PARAM_PICKER:
            drawDynamicParamPicker();
            break;
        case VIEWS.STORE_PICKER_RESULT:
            drawStorePickerResult();
            break;
        case VIEWS.OVERTAKE_MENU:
            drawOvertakeMenu();
            break;
        case VIEWS.GLOBAL_SETTINGS:
            drawGlobalSettings();
            break;
        case VIEWS.TOOLS:
            drawToolsMenu();
            break;
        case VIEWS.TOOL_FILE_BROWSER:
            drawToolFileBrowser();
            break;
        case VIEWS.TOOL_SET_PICKER:
            drawToolSetPicker();
            break;
        case VIEWS.TOOL_ENGINE_SELECT:
            drawToolEngineSelect();
            break;
        case VIEWS.TOOL_CONFIRM:
            drawToolConfirm();
            break;
        case VIEWS.TOOL_PROCESSING:
            pollToolProcess();
            drawToolProcessing();
            break;
        case VIEWS.TOOL_RESULT:
            drawToolResult();
            break;
        case VIEWS.TOOL_STEM_REVIEW:
            drawToolStemReview();
            break;
        case VIEWS.LFO_EDIT:
            drawLfoEdit();
            break;
        case VIEWS.LFO_TARGET_COMPONENT:
            drawLfoTargetComponent();
            break;
        case VIEWS.LFO_TARGET_PARAM:
            drawLfoTargetParam();
            break;
        case VIEWS.ENUM_PICKER:
            drawEnumPicker();
            break;
        case VIEWS.OVERTAKE_MODULE:
            try {
                /* Handle exit — LED restore is handled by C-side cache.
                 * When overtake_mode drops to 0, the shim detects the transition
                 * and progressively replays Move's cached LED state. */
                if (overtakeExitPending) {
                    debugLog("OVERTAKE tick: exit phase");
                    clear_screen();
                    print(40, 28, "Exiting...", 1);
                    completeOvertakeExit();
                }
                /* Handle deferred init - progressively clear LEDs then call module init() */
                else if (overtakeInitPending) {
                    overtakeInitTicks++;
                    /* Log every tick during init phase for debugging */
                    debugLog("OVERTAKE init phase: tick=" + overtakeInitTicks + " ledIdx=" + ledClearIndex);
                    /* Show loading screen while clearing LEDs */
                    clear_screen();
                    print(40, 28, "Loading...", 1);

                    /* Clear LEDs in batches (buffer is small) */
                    const ledsCleared = clearLedBatch();
                    flushLedQueue();  /* Drain queued LED clears to SHM */
                    debugLog("OVERTAKE init phase: ledsCleared=" + ledsCleared);

                    /* The DSP load runs on the shim worker (it used to stall
                     * the audio thread for ~11.5ms — an audible dropout on
                     * every module open). init() pushes params the moment it
                     * runs, and those are dropped if the instance does not
                     * exist yet, so hold init until the shim reports ready.
                     * Bounded: after OVERTAKE_DSP_READY_MAX_TICKS we proceed
                     * anyway rather than hang on a module whose DSP failed. */
                    let dspReady = true;
                    if (overtakeInitTicks < OVERTAKE_DSP_READY_MAX_TICKS &&
                        typeof shadow_get_param === "function") {
                        try {
                            dspReady = (shadow_get_param(0, "overtake_dsp:__ready") !== "0");
                        } catch (e) { dspReady = true; }
                    }

                    /* After LEDs cleared, delay passed and the DSP is up, call init */
                    if (ledsCleared && dspReady && overtakeInitTicks >= OVERTAKE_INIT_DELAY_TICKS) {
                        overtakeInitPending = false;
                        ledClearIndex = 0;  /* Reset for next time */
                        debugLog("loadOvertakeModule: init delay complete, calling init()");
                        if (overtakeModuleCallbacks && overtakeModuleCallbacks.init) {
                            try {
                                overtakeModuleCallbacks.init();
                                debugLog("loadOvertakeModule: init() returned successfully");
                                repairSwallowedShiftRelease("launch");
                            } catch (e) {
                                debugLog("loadOvertakeModule: init() threw exception: " + e);
                                /* Exit overtake on init error */
                                exitOvertakeMode();
                            }
                        }
                        /* Clean up reconnect flag after init (no-op for fresh loads) */
                        delete globalThis.host_tool_reconnect;
                        flushLedQueue();  /* Drain any LEDs set during init() */
                    }
                } else {
                    /* Flush accumulated encoder deltas as synthetic CC messages
                     * before calling tick(). This batches rapid knob turns into
                     * a single message per knob per frame. The accumulated count
                     * is encoded as: CW = count (1-63), CCW = 128 - abs(count).
                     * Modules using decodeDelta() will get direction; modules
                     * reading the raw value get the magnitude for acceleration. */
                    if (overtakeModuleCallbacks && overtakeModuleCallbacks.onMidiMessageInternal) {
                        for (let k = 0; k < NUM_KNOBS; k++) {
                            if (overtakeKnobDelta[k] !== 0) {
                                const d = overtakeKnobDelta[k];
                                const ccVal = d > 0 ? Math.min(d, 63) : Math.max(128 + d, 65);
                                try {
                                    runToolCallback(function() {
                                        overtakeModuleCallbacks.onMidiMessageInternal([0xB0, KNOB_CC_START + k, ccVal]);
                                    });
                                } catch (e) {
                                    debugLog("OVERTAKE flush knob exception: " + e);
                                    exitOvertakeMode();
                                    break;
                                }
                                overtakeKnobDelta[k] = 0;
                            }
                        }
                        if (overtakeJogDelta !== 0) {
                            const d = overtakeJogDelta;
                            const ccVal = d > 0 ? Math.min(d, 63) : Math.max(128 + d, 65);
                            try {
                                runToolCallback(function() {
                                    overtakeModuleCallbacks.onMidiMessageInternal([0xB0, MoveMainKnob, ccVal]);
                                });
                            } catch (e) {
                                debugLog("OVERTAKE flush jog exception: " + e);
                                exitOvertakeMode();
                            }
                            overtakeJogDelta = 0;
                        }
                    } else {
                        /* No MIDI handler, just clear accumulators */
                        for (let k = 0; k < NUM_KNOBS; k++) overtakeKnobDelta[k] = 0;
                        overtakeJogDelta = 0;
                    }

                    /* Call the overtake module's tick() function with the
                     * tool's real host APIs (see runToolCallback). */
                    if (overtakeModuleCallbacks && overtakeModuleCallbacks.tick) {
                        try {
                            runToolCallback(function() { overtakeModuleCallbacks.tick(); });
                        } catch (e) {
                            debugLog("OVERTAKE tick() exception: " + e);
                            /* Exit overtake on tick error */
                            exitOvertakeMode();
                        }
                    }
                    flushLedQueue();  /* Drain queued LED updates to SHM after module tick */

                    /* CO-RUN: render chain editor over the tool's frame. By
                     * contract, a tool that enables co-run agrees not to draw
                     * to OLED while coRunChainEditSlot >= 0 — but even if it
                     * does, drawSlots() (and chain-edit subtree draws) start
                     * with clear_screen() so the editor's pixels win. */
                    if (coRunUiActive()) {
                        runCoRunChainEdit(dispatchCoRunDraw);
                    }
                }
            } catch (e) {
                debugLog("OVERTAKE_MODULE case EXCEPTION: " + e);
                /* Exit overtake on any exception */
                exitOvertakeMode();
            }
            break;
        default:
            drawSlots();
    }

    if (view !== VIEWS.CANVAS) {
        /* Draw text entry on top if active */
        if (isTextEntryActive()) {
            drawTextEntry();
        }

        /* Draw warning overlay if active */
        if (warningActive) {
            drawMessageOverlay(warningTitle, warningLines);
        }

        if (feedbackGateActive() &&
            (typeof shadow_get_display_mode !== "function" || shadow_get_display_mode() === 1)) {
            feedbackGateDraw();
        }

        /* Draw overlay on top of main view (uses shared overlay system) */
        drawOverlay();
    }

    } catch (e) {
        /* tick draw EXCEPTION — log once per distinct error, show a brief
         * error screen this frame, and fall back to the slots view so the
         * next frame draws something known-good. */
        if (lastDrawError !== String(e)) {
            lastDrawError = String(e);
            debugLog("tick draw EXCEPTION in view " + view + ": " + e +
                     (e && e.stack ? "\n" + e.stack : ""));
        }
        try {
            clear_screen();
            print(10, 22, "UI error, recovering", 1);
            print(10, 34, String(e).substring(0, 21), 1);
        } catch (e2) { /* display unavailable — nothing more to do */ }
        if (view !== VIEWS.SLOTS) {
            view = VIEWS.SLOTS;
            announce("UI error, returning to slots");
        }
        needsRedraw = true;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    /* Knob-grid view consumes the controls it owns. One early-out rather than a
     * case in each of the per-view switches: the grid's input mapping lives in
     * shared/param_pages/page_input.mjs and is tested there. */
    /* The message overlay outranks the page, because a write ON the page is what
     * raises it (Schwung Mix, Link Audio, File Browser — all three are Global
     * Settings writes now). Asked as "IS A MODAL OPEN?" rather than as a list of
     * the keys that raise one, which is the rule runMasterFxActionFromGrid
     * records: it is what keeps a fourth modal-raising setting from being
     * silently unanswerable. Left the overlay up with the grid eating every
     * button and there is no press that can clear it. */
    if (view === VIEWS.PARAM_PAGES && paramPagesActive()) {
        if (maybeDismissWarningFromInput(status, d1, d2)) { needsRedraw = true; return; }
        if (handleParamPagesMidi(data)) { needsRedraw = true; return; }
    }

    /* Skip splash on any button press */
    if (splashActive && d2 > 0) {
        splashActive = false;
        /* Check if we need to show analytics prompt (first run only) */
        if (!host_file_exists(ANALYTICS_PROMPTED_PATH)) {
            view = VIEWS.ANALYTICS_PROMPT;
            analyticsPromptSelection = 0;
            announce("Usage Statistics, Send anonymous data? Yes");
        } else {
            if (typeof shadow_request_exit === "function") {
                shadow_request_exit();
            }
        }
        return;
    }

    /* Analytics prompt input handling */
    if (view === VIEWS.ANALYTICS_PROMPT) {
        if ((status & 0xF0) === 0xB0 && d2 > 0) {
            if (d1 === 14) {
                /* Jog turn — toggle selection */
                analyticsPromptSelection = analyticsPromptSelection === 0 ? 1 : 0;
                announce(analyticsPromptSelection === 0 ? "Yes" : "No");
                needsRedraw = true;
            } else if (d1 === 3) {
                /* Jog click — confirm */
                const enabled = (analyticsPromptSelection === 0);
                if (typeof host_set_analytics_enabled === "function") {
                    host_set_analytics_enabled(enabled);
                }
                /* Mark as prompted so we never show again */
                host_write_file(ANALYTICS_PROMPTED_PATH, "1");
                /* Dismiss display */
                if (typeof shadow_request_exit === "function") {
                    shadow_request_exit();
                }
                view = VIEWS.SLOTS;
                needsRedraw = true;
                announce(enabled ? "Analytics enabled" : "Analytics disabled");
            }
        }
        return;
    }

    /* Always track shift state (CC 49), even when canvas or other views consume MIDI */
    if ((status & 0xF0) === 0xB0 && d1 === 49) {
        hostShiftHeld = (d2 > 0);
    }
    /* Always track mute state (CC 88) — modifier for Mute+JogClick module-bypass shortcut */
    if ((status & 0xF0) === 0xB0 && d1 === 88) {
        hostMuteHeld = (d2 > 0);
    }

    /* Debug: log all MIDI when in overtake mode to diagnose escape issues */
    if (OVERTAKE_MIDI_LOG &&
        (view === VIEWS.OVERTAKE_MODULE || view === VIEWS.OVERTAKE_MENU)) {
        debugLog(`MIDI_IN: view=${view} status=${status} d1=${d1} d2=${d2} loaded=${overtakeModuleLoaded} callbacks=${!!overtakeModuleCallbacks}`);
    }

    /* Feedback gate intercepts CC input while modal is showing — but only while
     * the shadow UI is on screen, so a pending boot/unplug feedback modal can
     * never steal Move's native jog/back when the shadow UI is hidden. */
    if (feedbackGateActive() && (status & 0xF0) === 0xB0 &&
        (typeof shadow_get_display_mode !== "function" || shadow_get_display_mode() === 1)) {
        if (feedbackGateInput(d1, d2)) {
            needsRedraw = true;
            return;
        }
    }

    /* In co-run the outer view is OVERTAKE_MODULE; the canvas is the active
     * co-run overlay when coRunView === CANVAS. Steal jog-click/Back to close it
     * (wrapped so coRunView returns to the hierarchy editor), mirroring the
     * non-co-run steal below. */
    var canvasInCorun = coRunUiActive() && coRunView === VIEWS.CANVAS;
    if ((view === VIEWS.CANVAS || canvasInCorun) && (status & 0xF0) === 0xB0) {
        if (d1 === MoveMainButton && d2 > 0) {
            if (canvasInCorun) runCoRunChainEdit(function() { closeCanvasPreview(false); });
            else closeCanvasPreview(false);
            announce("Hierarchy Editor");
            needsRedraw = true;
            return;
        }
        if (d1 === MoveBack && d2 > 0) {
            if (canvasInCorun) runCoRunChainEdit(function() { closeCanvasPreview(true); });
            else closeCanvasPreview(true);
            announce("Hierarchy Editor");
            needsRedraw = true;
            return;
        }
    }



    /* Handle text entry MIDI if active */
    if (isTextEntryActive()) {
        if (handleTextEntryMidi(data)) {
            needsRedraw = true;
            return;  /* Consumed by text entry */
        }
    }

    if (dispatchCanvasMidi(data, "internal")) {
        needsRedraw = true;
        return;
    }

    /* Dismiss warning overlay on button presses, but not knob turns. */
    if (maybeDismissWarningFromInput(status, d1, d2)) return;  /* Consumed */

    /* When a module UI is loaded, route MIDI to it (except Back button) */
    if (view === VIEWS.COMPONENT_EDIT && loadedModuleUi) {
        /* Always handle Back ourselves to allow exiting */
        if ((status & 0xF0) === 0xB0 && d1 === MoveBack && d2 > 0) {
            handleBack();
            return;
        }

        /* Route everything else to the loaded module UI */
        if (loadedModuleUi.onMidiMessageInternal) {
            loadedModuleUi.onMidiMessageInternal(data);
            needsRedraw = true;
        }
        return;
    }

    /* When in overtake module view, route MIDI to the overtake module */
    /* Don't forward MIDI until init() has been called (overtakeInitPending = false) */
    if (view === VIEWS.OVERTAKE_MODULE && overtakeModuleLoaded && overtakeModuleCallbacks && !overtakeInitPending) {
        /* Track shift locally - shim's shift tracking doesn't work in overtake mode */
        if ((status & 0xF0) === 0xB0 && d1 === 49) {  /* CC 49 = Shift */
            hostShiftHeld = (d2 > 0);
        }

        /* Track volume knob touch for Shift+Vol+Jog escape detection */
        if ((status & 0xF0) === MidiNoteOn) {
            if (d1 === VOLUME_TOUCH_NOTE) {
                hostVolumeKnobTouched = (d2 > 0);
            }
        }

        /* Debug: log key state */
        if (OVERTAKE_MIDI_LOG) {
            debugLog(`OVERTAKE MIDI: status=${status} d1=${d1} d2=${d2} hostShift=${hostShiftHeld} volTouch=${hostVolumeKnobTouched}`);
        }

        /* HOST-LEVEL ESCAPE: Shift+Vol+Jog Click always exits overtake mode
         * This runs BEFORE passing MIDI to the module, ensuring escape always works */
        if ((status & 0xF0) === 0xB0 && d1 === MoveMainButton && d2 > 0) {
            debugLog(`JOG CLICK: hostShift=${hostShiftHeld} volTouch=${hostVolumeKnobTouched}`);
            if (hostShiftHeld && hostVolumeKnobTouched) {
                debugLog("HOST: Shift+Vol+Jog detected, exiting overtake mode");
                if (toolOvertakeActive) {
                    exitToolOvertake();
                } else {
                    exitOvertakeMode();
                }
                return;
            }
        }

        /* Back button handling for suspend_keeps_js modules — Wave Editor convention.
         * Back alone: suspend (module parks in background, ticks continue).
         * Shift+Back: full exit (unload module).
         * Skip while co-run is active: the chain editor claims Back. */
        if ((status & 0xF0) === 0xB0 && d1 === MoveBack && d2 > 0 && overtakeSuspendKeepsJs && !coRunUiActive()) {
            if (hostShiftHeld) {
                debugLog("HOST: Shift+Back → full exit (suspend_keeps_js module)");
                if (toolOvertakeActive) exitToolOvertake();
                else exitOvertakeMode();
                return;
            }
            if (!overtakeSuspendSelfManaged) {
                debugLog("HOST: Back → suspend (module parks in background)");
                suspendOvertakeMode();
                return;
            }
            /* suspend_self_managed: the module owns plain Back for its own
             * navigation and calls host_suspend_overtake() when it decides to
             * park. Fall through to its onMidiMessageInternal (Shift+Back above
             * is still the host's universal full-exit). */
        }

        /* CO-RUN: intercept chain-editor navigation CCs (jog turn, jog click,
         * track buttons) BEFORE encoder accumulation or tool dispatch. Tool
         * keeps everything else (pads, step buttons, knobs, transport, Shift).
         * Back is conditionally intercepted: when the editor is in a sub-view
         * (PATCHES, COMPONENT_*, KNOB_*, etc.) Back navigates up within the
         * editor; at CHAIN_EDIT (the top level) Back is silent so the tool's
         * own exit gesture (e.g. Menu) takes over. */
        if (coRunUiActive() && (status & 0xF0) === 0xB0) {
            if (d1 === MoveMainKnob && coRunWants(CORUN_GRP_JOG)) {
                const delta = decodeDelta(d2);
                if (delta !== 0) runCoRunChainEdit(function() { handleJog(delta); });
                needsRedraw = true;
                return;
            }
            if (d1 === MoveMainButton && d2 > 0 && coRunWants(CORUN_GRP_JOG)) {
                /* Mirror the non-overtake Shift+Click handler (line ~15259):
                 * Shift+Click in CHAIN_EDIT → handleShiftSelect (enter
                 * component edit). Plain Click → handleSelect. */
                runCoRunChainEdit(function() {
                    if (hostShiftHeld && view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0) {
                        handleShiftSelect();
                    } else if (hostShiftHeld && view === VIEWS.MASTER_FX && masterFxSelectedIsModule()) {
                        enterMasterFxModuleSelect(selectedMasterFxComponent);
                    } else {
                        handleSelect();
                    }
                });
                needsRedraw = true;
                return;
            }
            /* Back during co-run: framework-reserved as exit gesture by
             * default. When the tool sets CORUN_KEEP_BACK in keep_mask (opts
             * out of the framework auto-exit so its peer UI can use Back for
             * sub-view nav), the chain editor handles Back itself —
             * deeper views call handleBack() to pop one level; at CHAIN_EDIT
             * (the top of the editor's view stack) we end co-run, giving
             * the chain-edit target the "Back exits at top, navigates within"
             * UX even though the tool opted out. Other targets (move_native)
             * still rely on the tool's own exit gesture; only chain-edit gets
             * the auto-exit-at-top because only shadow_ui can see its own
             * view depth. */
            if (d1 === MoveBack && d2 > 0 && coRunWants(CORUN_GRP_BACK)) {
                if (corunOverlayId != null) {
                    /* Addressed-view overlay: pop within the view; at the overlay's
                     * root (corunOverlayRootView) close it to return to the underlay
                     * — never end co-run (Menu / the tool's own gesture does that). */
                    if (coRunView === corunOverlayRootView) {
                        shadow_corun_close();
                    } else {
                        runCoRunChainEdit(function() { handleBack(); });
                    }
                } else if (coRunView !== VIEWS.CHAIN_EDIT) {
                    runCoRunChainEdit(function() { handleBack(); });
                } else {
                    if (typeof shadow_corun_end === "function") shadow_corun_end();
                    coRunChainEditSlot = -1;
                    coRunView = VIEWS.OVERTAKE_MODULE;
                }
                needsRedraw = true;
                return;
            }
            /* Shift (CC 49): give it ONLY to the chain editor. hostShiftHeld
             * was already updated earlier (line ~15091) before this branch, so
             * the editor's isShiftHeld()-based code paths see the right state.
             * Eating it here prevents the tool from reacting (e.g. its own
             * Shift+button shortcuts while you're navigating the editor). */
            if (d1 === 49 && coRunWants(CORUN_GRP_SHIFT)) {
                needsRedraw = true;
                return;
            }
            /* Track buttons: CC 43=Track 1 (slot 0), CC 40=Track 4 (slot 3).
             * Chain-edit only — an overlay has no chain slot; swallow so a stray
             * track press can't flip it into chain-edit co-run. */
            if (d1 >= 40 && d1 <= 43 && d2 > 0 && coRunWants(CORUN_GRP_TRACK_BUTTONS)) {
                if (corunOverlayId != null) { needsRedraw = true; return; }
                const _slot = 43 - d1;
                if (_slot >= 0 && _slot < SHADOW_UI_SLOTS && _slot !== coRunChainEditSlot) {
                    coRunChainEditSlot = _slot;
                    selectedSlot = _slot;
                    if (typeof updateFocusedSlot === "function") updateFocusedSlot(_slot);
                    restoreChainComponent(_slot);
                    coRunView = VIEWS.CHAIN_EDIT;
                    if (typeof shadow_corun_begin === "function") shadow_corun_begin(CORUN_TARGET_CHAIN_EDIT, _slot, 0);
                    needsRedraw = true;
                }
                return;
            }
            /* Knob CCs (71-78): drive the focused chain component's params,
             * mirroring the non-overtake editor path (adjustKnobAndShow, with the
             * handleKnobTurn slot-global fallback). Wrapped in runCoRunChainEdit
             * so getKnobContext resolves against the editor's view rather than
             * OVERTAKE_MODULE (the context cache is keyed on view). Returns
             * before the tool knob-accumulation below, so the tool no longer
             * receives knob turns while co-run is active. Knob TOUCH notes still
             * forward to the tool — intentional turn-only split. */
            if (d1 >= KNOB_CC_START && d1 <= KNOB_CC_END && coRunWants(CORUN_GRP_KNOBS)) {
                const _kIdx = d1 - KNOB_CC_START;
                const _kDelta = decodeDelta(d2);
                if (_kDelta !== 0) {
                    runCoRunChainEdit(function() {
                        if (!adjustKnobAndShow(_kIdx, _kDelta)) handleKnobTurn(_kIdx, _kDelta);
                    });
                    needsRedraw = true;
                }
                return;
            }
        }

        /* Accumulate encoder/jog CCs instead of forwarding immediately.
         * Deltas are flushed as synthetic messages before tick(). */
        if ((status & 0xF0) === 0xB0) {
            if (d1 >= KNOB_CC_START && d1 <= KNOB_CC_END) {
                const knobIdx = d1 - KNOB_CC_START;
                overtakeKnobDelta[knobIdx] += decodeDelta(d2);
                needsRedraw = true;
                return;
            }
            if (d1 === MoveMainKnob) {
                overtakeJogDelta += decodeDelta(d2);
                needsRedraw = true;
                return;
            }
        }

        /* CO-RUN: knob capacitive touch (NoteOn for MoveKnob1Touch..8Touch;
         * d2>0 = touch, d2==0 = release). Route to the chain editor for
         * value-peek on touch and overlay dismiss on release, mirroring the
         * non-overtake touch handlers, wrapped so getKnobContext resolves under
         * the editor's view. Does NOT return — falls through to the tool forward
         * below so the tool still receives both touch edges (it already gets these
         * notes pre-change; keeping that avoids a stranded knobTouched on exit).
         * Release drains BOTH the hierarchy (pendingHierKnob) and slot-global
         * (pendingKnob) paths, which is what actually clears the value popup. */
        if (coRunUiActive() &&
                ((status & 0xF0) === MidiNoteOn || (status & 0xF0) === MidiNoteOff) &&
                d1 >= MoveKnob1Touch && d1 <= MoveKnob8Touch && coRunWants(CORUN_GRP_TOUCH)) {
            const _tk = d1 - MoveKnob1Touch;
            /* Same touch bookkeeping as the non-overtake handler below, both
             * spellings of release included for the same reason: inside
             * runCoRunChainEdit the view IS the chain editor, so these edges
             * raise and drop the knob card and it must not be left held. */
            const _down = ((status & 0xF0) === MidiNoteOn && d2 > 0);
            knobTouched[_tk] = _down;
            if (_down) {
                runCoRunChainEdit(function() {
                    const mmRole = getMultiMarkerKnobRole(_tk);
                    if (mmRole) {
                        if (mmRole.type === "marker") {
                            selectActiveWavMarker(mmRole.member);
                            const val = getSlotParam(hierEditorSlot, mmRole.member.fullKey);
                            showOverlay(mmRole.member.meta.name || mmRole.member.key,
                                        formatParamForOverlay(parseFloat(val), mmRole.member.meta));
                        } else if (mmRole.type === "zoom") {
                            const anchor = mmRole.anchor;
                            const cur = getWavZoomLevel(hierEditorSlot, anchor.meta, anchor.fullKey);
                            const factor = Math.pow(2, cur);
                            const label = cur > 0.01 ? `${factor.toFixed(factor < 10 ? 1 : 0)}x` : "1x (off)";
                            showOverlay("Zoom", label);
                        }
                    } else if (!showKnobOverlay(_tk)) {
                        handleKnobTurn(_tk, 0);
                    }
                });
            } else {
                runCoRunChainEdit(function() {
                    if (pendingHierKnobIndex === _tk) {
                        processPendingHierKnob();
                        pendingHierKnobIndex = -1;
                        pendingHierKnobDelta = 0;
                    }
                    if (pendingKnobIndex === _tk && pendingKnobDelta !== 0) {
                        refreshPendingKnobOverlay();
                    }
                });
                /* After the drain, for the reason spelled out on the
                 * non-overtake release path: the drain shows feedback too. */
                if (knobCardKnob === _tk) knobCardClose();
            }
            needsRedraw = true;
        }

        /* Route non-encoder MIDI to the overtake module immediately, with
         * the tool's real host APIs swapped in. */
        if (overtakeModuleCallbacks.onMidiMessageInternal) {
            try {
                runToolCallback(function() { overtakeModuleCallbacks.onMidiMessageInternal(data); });
                needsRedraw = true;
            } catch (e) {
                debugLog("OVERTAKE onMidiMessageInternal exception: " + e);
                /* Exit overtake on MIDI handler error */
                exitOvertakeMode();
            }
        }
        return;
    }

    /* Handle CC messages */
    if ((status & 0xF0) === 0xB0) {
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                handleJog(delta);
            }
            return;
        }
        if (d1 === MoveMainButton && d2 > 0) {
            /*
             * A held TRIGGER is fired by the click, and the click stops there.
             *
             * Holding a knob and clicking normally dives -- into the focused
             * component from the chain editor, or into the parameter editor.
             * A trigger has nothing to dive into: it is a button, and the
             * gesture the card is already advertising is a push. So the click
             * fires it and is consumed; everything else still dives, which is
             * what makes this a special case rather than a mode.
             */
            if (knobCardKnob >= 0) {
                const tctx = getKnobContext(knobCardKnob);
                if (tctx && tctx.fullKey && isTriggerParam(tctx.meta)) {
                    const fire = triggerFireValue(tctx.meta,
                                                  getSlotParam(tctx.slot, tctx.fullKey));
                    if (fire !== null) {
                        setSlotParam(tctx.slot, tctx.fullKey, fire);
                        noteTriggerFired(tctx.fullKey);
                        /*
                         * Re-read AND re-seed the knob cache.
                         *
                         * A trigger is the one parameter whose value changes
                         * because of something other than the knob, so the
                         * cache -- which is only invalidated when the KEY
                         * changes -- goes stale the moment it fires. Pressing
                         * read fresh and showed "Fired 6" while turning showed
                         * the cached "Fired 1", reported from the device as
                         * exactly that disagreement. Both paths now read the
                         * same number because the press writes it back.
                         */
                        const after = getSlotParam(tctx.slot, tctx.fullKey);
                        knobValueCache[knobCardKnob] = after;
                        knobCardRowValues[knobCardKeys[knobCardKnob]] = after;
                        showKnobFeedback(knobCardKnob, tctx.title, after || "",
                                         undefined, tctx.cardName);
                        needsRedraw = true;
                    }
                }
                /*
                 * ...and the click STOPS HERE either way.
                 *
                 * The card is a panel over the diagram and the component
                 * behind it is only incidentally selected -- diving into it
                 * from a gesture aimed at the card is acting on something the
                 * user cannot see. Reported from the device: "when the
                 * overlay is active clicking shouldn't take you into the
                 * module, it's a hidden element that it's still selected".
                 *
                 * This replaces dismiss-and-descend, which was deliberate and
                 * is now wrong: releasing the knob drops the card, so there is
                 * a way out that does not also do something.
                 */
                return;
            }
            /* Shift+Click in chain edit enters component edit mode */
            if (isShiftHeld() && view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0) {
                handleShiftSelect();
            } else if (isShiftHeld() && view === VIEWS.MASTER_FX && masterFxSelectedIsModule()) {
                /* Shift+Click in Master FX view enters module selector for the slot */
                enterMasterFxModuleSelect(selectedMasterFxComponent);
            } else {
                handleSelect();
            }
            return;
        }
        if (d1 === MoveBack && d2 > 0) {
            handleBack();
            return;
        }

        /* Handle knob CCs (71-78) for parameter control */
        if (d1 >= KNOB_CC_START && d1 <= KNOB_CC_END) {
            const knobIndex = d1 - KNOB_CC_START;
            const delta = decodeDelta(d2);

            /*
             * While the option list is up, a knob SCROLLS IT.
             *
             * You reach this picker by holding a knob and clicking, so the
             * hand is already on the knob and the reflex is to keep turning
             * it: "I keep trying to keep turning it", reported from the
             * device. Jog-only made the gesture change hands halfway through.
             *
             * Any knob, not just the one that opened it — the picker is
             * modal and full-screen, so there is no other visible control a
             * turn could mean, and requiring the right knob would leave a
             * neighbour silently dead. It also has to work when the picker
             * was opened from the hierarchy list editor, where no knob
             * opened it at all.
             *
             * This is ALSO a fix, not only an affordance: without it the turn
             * fell through to adjustKnobAndShow and moved the value BEHIND
             * the list — invisibly, since the picker covers the grid, and
             * then Back "cancelled" a change that had already been written.
             *
             * One option per detent, matching the jog. The 4-detent enum gate
             * is for turning an enum blind on the grid; inside a list you are
             * looking straight at it.
             */
            if (view === VIEWS.ENUM_PICKER) {
                /* Through the list accumulator, NOT enumPickerJog directly:
                 * 1:1 is right for the jog and much too fast for a knob
                 * ("still like 1 detent per entry which feels way too fast").
                 * Slow turns cost several detents an entry; fast ones
                 * accelerate, but only as far as the list is long. */
                const step = listKnobStep(enumPickerKnob, delta, Date.now(),
                                          enumPickerOptions.length);
                if (step) enumPickerJog(step);
                return;
            }

            /* Use shared knob handler for hierarchy/chain editor contexts */
            if (adjustKnobAndShow(knobIndex, delta)) {
                return;
            }

            /* Default (slot selected, no component): adjust global slot knob mapping */
            handleKnobTurn(knobIndex, delta);
            return;
        }

        /* Handle track button CCs (40-43) for slot selection
         * Track 1 (top) = CC 43 → slot 0, Track 4 (bottom) = CC 40 → slot 3 */
        if (d1 >= TRACK_CC_START && d1 <= TRACK_CC_END && d2 > 0) {
            const slotIndex = TRACK_CC_END - d1;
            if (slotIndex >= 0 && slotIndex < SHADOW_UI_SLOTS) {
                selectedSlot = slotIndex;
                updateFocusedSlot(slotIndex);
                const slotName = slots[slotIndex]?.name || `Slot ${slotIndex + 1}`;
                announce(`Track ${slotIndex + 1}, ${slotName}`);
                needsRedraw = true;
            }
            return;
        }
    }

    /* Handle Note On for knob touch - peek at current value without turning
     * Move sends notes 0-7 for knob capacitive touch (Note On = touch start) */
    if ((status & 0xF0) === MidiNoteOn && d2 > 0) {
        if (d1 >= MoveKnob1Touch && d1 <= MoveKnob8Touch) {
            const knobIndex = d1 - MoveKnob1Touch;
            /* Recorded BEFORE any of the branches below, all of which can raise
             * the knob card: a card raised while the finger is down has no
             * decay deadline, and that distinction is read from here. */
            knobTouched[knobIndex] = true;

            /*
             * The picker owns the screen, so a knob touch raises nothing.
             *
             * Letting go and taking hold again re-raised the parameter
             * overlay OVER the option list — reported from the device. The
             * overlay answers "what does this knob do", which the list is
             * already answering, in more detail, with the full option set. It
             * also covers the very rows you are scrolling.
             *
             * The touch is still RECORDED above, so the knob-card decay logic
             * stays consistent for whatever is underneath when the picker
             * closes; only the drawing is suppressed.
             */
            if (view === VIEWS.ENUM_PICKER) return;

            /* Multi-marker view overrides the level's knob row:
             *   marker knobs (1..N) → switch active marker + show its value
             *   zoom knob (8)       → show current zoom level
             *   unmapped (rest)     → silent, no overlay */
            const mmRole = getMultiMarkerKnobRole(knobIndex);
            if (mmRole) {
                if (mmRole.type === "marker") {
                    selectActiveWavMarker(mmRole.member);
                    const val = getSlotParam(hierEditorSlot, mmRole.member.fullKey);
                    showOverlay(mmRole.member.meta.name || mmRole.member.key,
                                formatParamForOverlay(parseFloat(val), mmRole.member.meta));
                    return;
                }
                if (mmRole.type === "zoom") {
                    const anchor = mmRole.anchor;
                    const cur = getWavZoomLevel(hierEditorSlot, anchor.meta, anchor.fullKey);
                    const factor = Math.pow(2, cur);
                    const label = cur > 0.01 ? `${factor.toFixed(factor < 10 ? 1 : 0)}x` : "1x (off)";
                    showOverlay("Zoom", label);
                    return;
                }
                /* unmapped — swallow the touch so the user doesn't see
                 * stale level mappings (like loop_mode). */
                return;
            }

            /* Use shared knob overlay for hierarchy/chain editor contexts */
            if (showKnobOverlay(knobIndex)) {
                return;
            }

            /* Default (chain selected or settings): show overlay for slot's global knob mapping */
            handleKnobTurn(knobIndex, 0);
            return;
        }
    }

    /* Handle Note Off for knob release - clear pending knob state
     * This ensures accumulated deltas are processed before next touch.
     *
     * BOTH spellings of release. shared/param_pages/page_input.mjs has carried
     * the reason since the knob grid shipped: "Move sends note-on with velocity
     * 0 for release as well as note-off, so both spellings must clear the touch
     * or a knob can be left stuck as held." This branch used to match only the
     * velocity-0 note-on, which was harmless while it merely drained deltas —
     * knobTouched made it load-bearing, because a knob stuck as held stamps the
     * card with no decay deadline and a turn-raised card has no note-off
     * coming to clear it. */
    if (((status & 0xF0) === MidiNoteOn && d2 === 0) ||
        (status & 0xF0) === MidiNoteOff) {
        if (d1 >= MoveKnob1Touch && d1 <= MoveKnob8Touch) {
            const knobIndex = d1 - MoveKnob1Touch;
            knobTouched[knobIndex] = false;
            /* Process hierarchy knob delta */
            if (pendingHierKnobIndex === knobIndex) {
                processPendingHierKnob();
                pendingHierKnobIndex = -1;
                pendingHierKnobDelta = 0;
            }
            /* Process global slot knob delta */
            if (pendingKnobIndex === knobIndex && pendingKnobDelta !== 0) {
                refreshPendingKnobOverlay();
            }
            /* Let go and the diagram is back.
             *
             * LAST, after the pending flush, because that flush shows feedback
             * too: closing first only had the card reopen itself — with six
             * fresh IPC reads — on the way out of the gesture. Unconditional on
             * the knob matching, for the same reason: the flush has already
             * stamped a decay deadline on it (the finger is gone by then), so a
             * "only if it is held" test would leave the card up for another
             * 700ms after release. A card raised by a TURN never sees this
             * branch — no touch, so no note-off. */
            if (knobCardKnob === knobIndex) knobCardClose();
            return;
        }
    }
};

globalThis.onMidiMessageExternal = function(data) {
    if (dispatchCanvasMidi(data, "external")) {
        needsRedraw = true;
    }
};
