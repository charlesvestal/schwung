import * as os from 'os';
import * as std from 'std';

/* Import unified logger */
import { log as unifiedLog, installConsoleOverride } from '/data/UserData/move-anything/shared/logger.mjs';

/* Install console.log override to route to unified debug.log */
installConsoleOverride('shadow');

/* Debug logging function - now uses unified logger */
function debugLog(msg) {
    unifiedLog('shadow', msg);
}

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
    MidiNoteOn
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    SCREEN_WIDTH, SCREEN_HEIGHT,
    TITLE_Y, TITLE_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_TEXT_Y, FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/move-anything/shared/chain_ui_views.mjs';

import { decodeDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';

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
    menuLayoutDefaults
} from '/data/UserData/move-anything/shared/menu_layout.mjs';

import {
    wrapText,
    createScrollableText,
    handleScrollableTextJog,
    isActionSelected,
    drawScrollableText
} from '/data/UserData/move-anything/shared/scrollable_text.mjs';

import {
    fetchCatalog, getModulesForCategory, getModuleStatus,
    installModule as sharedInstallModule,
    removeModule as sharedRemoveModule,
    scanInstalledModules, getHostVersion, isNewerVersion,
    fetchReleaseJsonQuick, fetchReleaseNotes,
    CATEGORIES
} from '/data/UserData/move-anything/shared/store_utils.mjs';

import {
    openTextEntry,
    isTextEntryActive,
    handleTextEntryMidi,
    drawTextEntry,
    tickTextEntry
} from '/data/UserData/move-anything/shared/text_entry.mjs';

import {
    announce,
    announceMenuItem,
    announceParameter,
    announceView
} from '/data/UserData/move-anything/shared/screen_reader.mjs';

import {
    fetchAndParseManual,
    refreshManualBackground,
    processDownloadedHtml
} from '/data/UserData/move-anything/shared/parse_move_manual.mjs';

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
} from '/data/UserData/move-anything/shared/sampler_overlay.mjs';

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

/* Knob CC range for parameter control */
const KNOB_CC_START = MoveKnob1;  // CC 71
const KNOB_CC_END = MoveKnob8;    // CC 78
const NUM_KNOBS = 8;

const CONFIG_PATH = "/data/UserData/move-anything/shadow_chain_config.json";
const PATCH_DIR = "/data/UserData/move-anything/patches";
const SLOT_STATE_DIR_DEFAULT = "/data/UserData/move-anything/slot_state";
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
    COMPONENT_SELECT: "compselect", // Select module for a component
    COMPONENT_EDIT: "compedit",  // Edit component (presets, params) via Shift+Click
    MASTER_FX: "masterfx",    // Master FX selection
    HIERARCHY_EDITOR: "hierarch", // Hierarchy-based parameter editor
    KNOB_EDITOR: "knobedit",  // Edit knob assignments for a slot
    KNOB_PARAM_PICKER: "knobpick", // Pick parameter for a knob assignment
    STORE_PICKER_CATEGORIES: "storepickercats", // Store: browse categories
    STORE_PICKER_LIST: "storepickerlist",     // Store: browse modules for category
    STORE_PICKER_DETAIL: "storepickerdetail", // Store: module info and actions
    STORE_PICKER_LOADING: "storepickerloading", // Store: fetching catalog or installing
    STORE_PICKER_RESULT: "storepickerresult",  // Store: success/error message
    STORE_PICKER_POST_INSTALL: "storepickerpostinstall",  // Store: post-install message
    OVERTAKE_MENU: "overtakemenu",   // Overtake module selection menu
    OVERTAKE_MODULE: "overtakemodule", // Running an overtake module
    UPDATE_PROMPT: "updateprompt",    // Startup update prompt (updates available)
    UPDATE_DETAIL: "updatedetail",    // Detail view for a single update
    UPDATE_RESTART: "updaterestart",   // Restart prompt after core update
    GLOBAL_SETTINGS: "globalsettings"  // Global settings menu (display, audio, etc.)
};

/* Special action key for swap module option */
const SWAP_MODULE_ACTION = "__swap_module__";

/* Chain component types for horizontal editor */
const CHAIN_COMPONENTS = [
    { key: "midiFx", label: "MIDI FX", position: 0 },
    { key: "synth", label: "Synth", position: 1 },
    { key: "fx1", label: "FX 1", position: 2 },
    { key: "fx2", label: "FX 2", position: 3 },
    { key: "settings", label: "Settings", position: 4 }
];

/* Module abbreviations cache - populated from module.json "abbrev" field */
const moduleAbbrevCache = {
    /* Built-in fallbacks for special cases */
    "settings": "*",
    "empty": "--"
};

/* In-memory chain configuration (for future save/load) */
function createEmptyChainConfig() {
    return {
        midiFx: null,    // { module: "chord", params: {} } or null
        synth: null,     // { module: "dexed", params: {} } or null
        fx1: null,       // { module: "freeverb", params: {} } or null
        fx2: null        // { module: "cloudseed", params: {} } or null
    };
}

/* Master FX options - populated by scanning modules directory */
let MASTER_FX_OPTIONS = [{ id: "", name: "None" }];

let slots = [];
let patches = [];
let selectedSlot = 0;
let selectedPatch = 0;
let selectedDetailItem = 0;    // For patch detail view (0=synth, 1=fx1, 2=fx2, 3=load)
let selectedSetting = 0;       // For slot settings view
let editingSettingValue = false;
let view = VIEWS.SLOTS;
let needsRedraw = true;
let refreshCounter = 0;
let autosaveCounter = 0;
let autosaveSuppressUntil = 0;  /* suppress autosave after set change */
let slotDirtyCache = [false, false, false, false];

/* Overlay state (sampler/skipback from shim via SHM) */
let lastOverlaySeq = 0;
let overlayState = null;

/* FX display_name cache for change-based announcements (e.g. key detection) */
let fxDisplayNameCache = {};  /* key: "slot:component" -> last display_name string */

/* View names for screen reader announcements */
const VIEW_NAMES = {
    [VIEWS.SLOTS]: "Slots Menu",
    [VIEWS.SLOT_SETTINGS]: "Slot Settings",
    [VIEWS.CHAIN_EDIT]: "Chain Edit",
    [VIEWS.CHAIN_SETTINGS]: "Chain Settings",
    [VIEWS.PATCHES]: "Patch Browser",
    [VIEWS.PATCH_DETAIL]: "Patch Detail",
    [VIEWS.COMPONENT_PARAMS]: "Component Parameters",
    [VIEWS.COMPONENT_SELECT]: "Module Browser",
    [VIEWS.COMPONENT_EDIT]: "Preset Picker",
    [VIEWS.MASTER_FX]: "Master FX",
    [VIEWS.HIERARCHY_EDITOR]: "Hierarchy Editor",
    [VIEWS.KNOB_EDITOR]: "Knob Editor",
    [VIEWS.KNOB_PARAM_PICKER]: "Parameter Picker",
    [VIEWS.STORE_PICKER_CATEGORIES]: "Module Store",
    [VIEWS.STORE_PICKER_LIST]: "Module Store",
    [VIEWS.STORE_PICKER_DETAIL]: "Module Details",
    [VIEWS.STORE_PICKER_LOADING]: "Loading",
    [VIEWS.STORE_PICKER_RESULT]: "Operation Complete",
    [VIEWS.STORE_PICKER_POST_INSTALL]: "Installation Complete",
    [VIEWS.OVERTAKE_MENU]: "Overtake Menu",
    [VIEWS.OVERTAKE_MODULE]: "Overtake Module",
    [VIEWS.GLOBAL_SETTINGS]: "Settings"
};

/* Helper to change view and announce it */
function setView(newView, customLabel) {
    if (view === newView) return;  /* No change */
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
let previousView = VIEWS.SLOTS;   // View to return to after overtake
let overtakeModuleCallbacks = null; // {init, tick, onMidiMessageInternal} for loaded module

/* Auto-update state */
let autoUpdateCheckEnabled = true;   // Default: enabled (opt-out)
let pendingUpdates = [];              // Updates found on startup
let pendingUpdateIndex = 0;           // Selected update in prompt
let updateRestartFromVersion = '';    // For restart prompt display
let updateRestartToVersion = '';
let updateDetailScrollState = null;   // Scrollable text state for update detail view
let updateDetailModule = null;        // Module being viewed in update detail

/* Host-side tracking for Shift+Vol+Jog escape (redundant with shim, but ensures escape always works) */
let hostVolumeKnobTouched = false;
let hostShiftHeld = false;  /* Local shift tracking - shim tracking doesn't work in overtake mode */

/* Deferred module init - clear LEDs and wait before calling init() */
let overtakeInitPending = false;
let overtakeInitTicks = 0;
const OVERTAKE_INIT_DELAY_TICKS = 30; // ~500ms at 16ms tick

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

    /* All button/indicator CCs with LEDs */
    const ccLeds = [];
    /* Step icon LEDs (16-31) - CCs, separate from step note LEDs */
    for (let i = 16; i <= 31; i++) ccLeds.push(i);
    /* Tracks/Rows */
    ccLeds.push(40, 41, 42, 43);
    /* Shift */
    ccLeds.push(49);
    /* Menu, Back, Capture */
    ccLeds.push(50, 51, 52);
    /* Down, Up */
    ccLeds.push(54, 55);
    /* Undo */
    ccLeds.push(56);
    /* Loop */
    ccLeds.push(58);
    /* Copy */
    ccLeds.push(60);
    /* Left, Right */
    ccLeds.push(62, 63);
    /* Knob indicators */
    ccLeds.push(71, 72, 73, 74, 75, 76, 77, 78);
    /* Play, Rec */
    ccLeds.push(85, 86);
    /* Mute */
    ccLeds.push(88);
    /* Record, Delete */
    ccLeds.push(118, 119);

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

/* Knob acceleration settings */
const KNOB_ACCEL_MIN_MULT = 1;     // Multiplier for slow turns
const KNOB_ACCEL_MAX_MULT = 4;     // Multiplier for fast turns (floats)
const KNOB_ACCEL_MAX_MULT_INT = 2; // Multiplier for fast turns (ints)
const KNOB_ACCEL_ENUM_MULT = 1;    // Enums: always step by 1 (no acceleration)
const KNOB_ACCEL_SLOW_MS = 250;    // Slower than this = min multiplier
const KNOB_ACCEL_FAST_MS = 50;     // Faster than this = max multiplier
const KNOB_BASE_STEP_FLOAT = 0.002; // Base step for floats (acceleration multiplies this)
const KNOB_BASE_STEP_INT = 1;       // Base step for ints
const TRIGGER_ENUM_TURN_THRESHOLD = 1;  // Positive detents required before firing trigger action
const TRIGGER_ENUM_WINDOW_MS = 700;     // Pause longer than this to start a new trigger gesture

/* Time tracking for knob acceleration */
let knobLastTimeMs = [0, 0, 0, 0, 0, 0, 0, 0];  // Last event time per knob
let triggerEnumAccum = [0, 0, 0, 0, 0, 0, 0, 0];
let triggerEnumLastMs = [0, 0, 0, 0, 0, 0, 0, 0];
let triggerEnumLatched = [false, false, false, false, false, false, false, false];

/* Calculate knob acceleration multiplier based on time between events */
function calcKnobAccel(knobIndex, isInt) {
    if (knobIndex < 0 || knobIndex >= 8) return 1;

    const now = Date.now();
    const last = knobLastTimeMs[knobIndex];
    knobLastTimeMs[knobIndex] = now;

    if (last === 0) return KNOB_ACCEL_MIN_MULT;  // First event

    const elapsed = now - last;
    let accel;

    if (elapsed >= KNOB_ACCEL_SLOW_MS) {
        accel = KNOB_ACCEL_MIN_MULT;
    } else if (elapsed <= KNOB_ACCEL_FAST_MS) {
        accel = isInt ? KNOB_ACCEL_MAX_MULT_INT : KNOB_ACCEL_MAX_MULT;
    } else {
        // Linear interpolation between min and max
        const ratio = (KNOB_ACCEL_SLOW_MS - elapsed) / (KNOB_ACCEL_SLOW_MS - KNOB_ACCEL_FAST_MS);
        const maxMult = isInt ? KNOB_ACCEL_MAX_MULT_INT : KNOB_ACCEL_MAX_MULT;
        accel = Math.round(KNOB_ACCEL_MIN_MULT + ratio * (maxMult - KNOB_ACCEL_MIN_MULT));
    }

    return accel;
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
let lastSlotModuleSignatures = [];  // Track per-slot module changes for knob cache refresh

/* Master FX state */
let selectedMasterFx = 0;    // Index into MASTER_FX_OPTIONS
let currentMasterFxId = "";  // Currently loaded master FX module ID
let currentMasterFxPath = ""; // Full path to currently loaded DSP

/* Master FX chain components (4 FX slots + settings) */
const MASTER_FX_CHAIN_COMPONENTS = [
    { key: "fx1", label: "FX 1", position: 0, paramPrefix: "master_fx:fx1:" },
    { key: "fx2", label: "FX 2", position: 1, paramPrefix: "master_fx:fx2:" },
    { key: "fx3", label: "FX 3", position: 2, paramPrefix: "master_fx:fx3:" },
    { key: "fx4", label: "FX 4", position: 3, paramPrefix: "master_fx:fx4:" },
    { key: "settings", label: "Settings", position: 4, paramPrefix: "" }
];

/* Master FX chain editing state */
let masterFxConfig = {
    fx1: { module: "" },
    fx2: { module: "" },
    fx3: { module: "" },
    fx4: { module: "" }
};
let selectedMasterFxComponent = 0;    // -1=preset, 0-4: fx1, fx2, fx3, fx4, settings
let selectingMasterFxModule = false;  // True when selecting module for a component
let selectedMasterFxModuleIndex = 0;  // Index in MASTER_FX_OPTIONS during selection

/* Master FX settings (shown when Settings component is selected) */
const MASTER_FX_SETTINGS_ITEMS_BASE = [
    { key: "master_volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "save", label: "[Save MFX Preset]", type: "action" },
    { key: "save_as", label: "[Save As]", type: "action" },
    { key: "delete", label: "[Delete]", type: "action" }
];

/* Global Settings — hierarchical sections for Shift+Vol+Step2 menu */
const GLOBAL_SETTINGS_SECTIONS = [
    {
        id: "display", label: "Display",
        items: [
            { key: "display_mirror", label: "Mirror Display", type: "bool" },
            { key: "overlay_knobs", label: "Overlay Knobs", type: "enum",
              options: ["+Shift", "+Jog Touch", "Off", "Native"], values: [0, 1, 2, 3] }
        ]
    },
    {
        id: "audio", label: "Audio",
        items: [
            { key: "link_audio_routing", label: "Link Audio", type: "bool" },
            { key: "resample_bridge", label: "Resample Src", type: "enum",
              options: ["Off", "Replace"], values: [0, 2] }
        ]
    },
    {
        id: "accessibility", label: "Screen Reader",
        items: [
            { key: "screen_reader_enabled", label: "Screen Reader", type: "bool" },
            { key: "screen_reader_engine", label: "TTS Engine", type: "enum",
              options: ["eSpeak-NG", "Flite"], values: ["espeak", "flite"] },
            { key: "screen_reader_speed", label: "Voice Speed", type: "float", min: 0.5, max: 6.0, step: 0.1 },
            { key: "screen_reader_pitch", label: "Voice Pitch", type: "float", min: 80, max: 180, step: 5 },
            { key: "screen_reader_volume", label: "Voice Vol", type: "int", min: 0, max: 100, step: 5 },
            { key: "screen_reader_debounce", label: "Debounce", type: "int", min: 0, max: 1000, step: 50 }
        ]
    },
    {
        id: "set_pages", label: "Set Pages",
        items: [
            { key: "set_pages_enabled", label: "Set Pages", type: "bool" }
        ]
    },
    {
        id: "updates", label: "Updates",
        items: [
            { key: "check_updates", label: "[Check Updates]", type: "action" },
            { key: "module_store", label: "[Module Store]", type: "action" }
        ]
    },
    {
        id: "help", label: "[Help...]", isAction: true
    }
];

let globalSettingsSectionIndex = 0;
let globalSettingsInSection = false;
let globalSettingsItemIndex = 0;
let globalSettingsEditing = false;

const RESAMPLE_BRIDGE_LABEL_BY_MODE = { 0: "Off", 2: "Replace" };
const RESAMPLE_BRIDGE_VALUES = [0, 2];

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
    /* New/unsaved: hide Save As and Delete */
    return MASTER_FX_SETTINGS_ITEMS_BASE.filter(item =>
        item.key !== "save_as" && item.key !== "delete"
    );
}

let selectedMasterFxSetting = 0;
let editingMasterFxSetting = false;
let editMasterFxValue = "";
let inMasterFxSettingsMenu = false;  /* True when in settings submenu */

/* Help viewer state - stack-based for arbitrary depth */
let helpContent = null;
let helpNavStack = [];            /* [{ items, selectedIndex, title }] */
let helpDetailScrollState = null;


/* Return-view trackers for sub-flows */
let storeReturnView = null;   /* View to return to from store/update flows */
let helpReturnView = null;    /* View to return to from help viewer */

/* Slot settings definitions */
const SLOT_SETTINGS = [
    { key: "patch", label: "Patch", type: "action" },  // Opens patch browser
    { key: "chain", label: "Edit Chain", type: "action" },  // Opens chain editor
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "slot:muted", label: "Muted", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:soloed", label: "Soloed", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 0, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -2, max: 15, step: 1 },  // -2 = passthrough, -1 = auto, 0-15 = ch 1-16
];

/* Cached patch detail info */
let patchDetail = {
    synthName: "",
    synthPreset: "",
    fx1Name: "",
    fx1Wet: "",
    fx2Name: "",
    fx2Wet: ""
};

/* Component parameter editing state */
let editingComponent = "";     // "synth", "fx1", "fx2"
let componentParams = [];      // List of {key, label, value, type, min, max}
let selectedParam = 0;
let editingValue = false;      // True when adjusting value
let editValue = "";            // Current value being edited

/* Chain editing state */
let chainConfigs = [];         // In-memory chain configs per slot
let selectedChainComponent = 0; // -1=chain, 0-4 (midiFx, synth, fx1, fx2, settings)
let selectingModule = false;   // True when in module selection for a component
let availableModules = [];     // Modules available for selected component type
let selectedModuleIndex = 0;   // Index in availableModules

/* Store picker state */
let storeCatalog = null;               // Cached catalog from store_utils
let storeInstalledModules = {};        // {moduleId: version} map
let storeHostVersion = '1.0.0';        // Current host version
let storePickerCategory = null;        // Category ID being browsed (sound_generator, audio_fx, midi_fx)
let storePickerModules = [];           // Modules available for download in current category
let storePickerSelectedIndex = 0;      // Selection in list
let storePickerCurrentModule = null;   // Module being viewed in detail
let storePickerActionIndex = 0;        // Selected action in detail view (0=Install/Update, 1=Remove)
let storePickerMessage = '';           // Result/error message
let storePickerResultTitle = '';       // Result screen header (empty = 'Module Store')
let storePickerLoadingTitle = '';      // Loading screen title
let storePickerLoadingMessage = '';    // Loading screen message
let storeFetchPending = false;         // True while catalog fetch in progress
let storeDetailScrollState = null;     // Scroll state for module detail
let storePostInstallLines = [];        // Post-install message lines
let storePickerFromOvertake = false;   // True if entered from overtake menu
let storePickerFromMasterFx = false;  // True if entered from master FX module select
let storePickerFromSettings = false;  // True if entered from MFX settings (full store)
let storeCategoryIndex = 0;           // Selected index in category browser
let storeCategoryItems = [];          // Built category list with counts

/* Check if host update is available and create pseudo-module if so */
function getHostUpdateModule() {
    if (!storeCatalog || !storeCatalog.host) return null;
    const host = storeCatalog.host;
    if (!host.latest_version || !host.download_url) return null;
    if (!isNewerVersion(host.latest_version, storeHostVersion)) return null;
    return {
        id: "__core_update__",
        name: "Core Update",
        description: "Update Move Anything core",
        latest_version: host.latest_version,
        download_url: host.download_url,
        component_type: "core",
        _isHostUpdate: true
    };
}

/* Perform a staged core update with verification and backup.
 * Returns { success: bool, error: string? } */
function performCoreUpdate(mod) {
    const BASE = '/data/UserData/move-anything';
    const TMP = BASE + '/tmp';
    const STAGING = BASE + '/update-staging';
    const BACKUP = BASE + '/update-backup';
    const tarPath = TMP + '/move-anything.tar.gz';

    /* Required files that must exist after extraction */
    const REQUIRED_FILES = [
        'move-anything',
        'move-anything-shim.so',
        'host/version.txt',
        'shadow/shadow_ui.js'
    ];

    /* Helper to show status on display */
    function setStatus(msg) {
        clear_screen();
        drawStatusOverlay('Core Update', msg);
        host_flush_display();
    }

    /* --- Phase 1: Download --- */
    setStatus('Downloading...');

    host_ensure_dir(TMP);
    const downloadOk = host_http_download(mod.download_url, tarPath);
    if (!downloadOk) {
        host_system_cmd('rm -f "' + tarPath + '"');
        return { success: false, error: 'Download failed' };
    }

    /* --- Phase 2: Verify tarball --- */
    setStatus('Verifying...');

    const listOk = host_system_cmd('tar -tzf "' + tarPath + '" > /dev/null 2>&1');
    if (listOk !== 0) {
        host_system_cmd('rm -f "' + tarPath + '"');
        return { success: false, error: 'Bad archive' };
    }

    /* --- Phase 3: Extract to staging --- */
    setStatus('Extracting...');

    host_remove_dir(STAGING);
    host_ensure_dir(STAGING);

    const extractOk = host_extract_tar_strip(tarPath, STAGING, 1);
    if (!extractOk) {
        host_remove_dir(STAGING);
        host_system_cmd('rm -f "' + tarPath + '"');
        return { success: false, error: 'Extract failed' };
    }

    /* --- Phase 4: Verify staging --- */
    let allPresent = true;
    for (const f of REQUIRED_FILES) {
        if (!host_file_exists(STAGING + '/' + f)) {
            allPresent = false;
            break;
        }
    }
    if (!allPresent) {
        host_remove_dir(STAGING);
        host_system_cmd('rm -f "' + tarPath + '"');
        return { success: false, error: 'Incomplete update' };
    }

    /* --- Phase 5: Backup current files --- */
    setStatus('Backing up...');

    host_remove_dir(BACKUP);
    host_ensure_dir(BACKUP);
    host_system_cmd('cp "' + BASE + '/move-anything" "' + BACKUP + '/"');
    host_system_cmd('cp "' + BASE + '/move-anything-shim.so" "' + BACKUP + '/"');
    host_system_cmd('cp -r "' + BASE + '/shadow" "' + BACKUP + '/"');
    host_system_cmd('cp -r "' + BASE + '/host" "' + BACKUP + '/"');

    /* Write restore script */
    const restoreScript = '#!/bin/sh\n'
        + 'cd "' + BASE + '"\n'
        + 'cp "' + BACKUP + '/move-anything" .\n'
        + 'cp "' + BACKUP + '/move-anything-shim.so" .\n'
        + 'chmod u+s move-anything-shim.so\n'
        + 'cp -r "' + BACKUP + '/shadow" .\n'
        + 'cp -r "' + BACKUP + '/host" .\n'
        + 'echo "Restored. Restart Move to apply."\n';
    host_write_file(BASE + '/restore-update.sh', restoreScript);
    host_system_cmd('chmod +x "' + BASE + '/restore-update.sh"');

    /* --- Phase 6: Apply staged files --- */
    setStatus('Installing...');

    const applyOk = host_system_cmd('cp -r "' + STAGING + '/"* "' + BASE + '/"');
    if (applyOk !== 0) {
        /* Attempt restore from backup */
        host_system_cmd('sh "' + BASE + '/restore-update.sh"');
        host_remove_dir(STAGING);
        return { success: false, error: 'Install failed (restored)' };
    }

    /* Restore setuid bit on shim — required for LD_PRELOAD under AT_SECURE
     * (MoveOriginal has file capabilities that trigger secure-exec mode;
     *  without u+s the linker refuses to load the shim via symlink) */
    host_system_cmd('chmod u+s "' + BASE + '/move-anything-shim.so"');

    /* --- Phase 7: Cleanup --- */
    host_remove_dir(STAGING);
    host_system_cmd('rm -f "' + tarPath + '"');

    return { success: true };
}

/* Check for core and module updates (manual, called from Settings → Check Updates) */
function checkForUpdatesInBackground() {
    debugLog("checkForUpdatesInBackground: starting");
    const updates = [];

    clear_screen();
    drawStatusOverlay('Updates', 'Checking...');
    host_flush_display();

    /* Check core update */
    storeHostVersion = getHostVersion();
    debugLog("checkForUpdatesInBackground: hostVersion=" + storeHostVersion);
    const coreRelease = fetchReleaseJsonQuick('charlesvestal/move-anything');
    debugLog("checkForUpdatesInBackground: coreRelease=" + JSON.stringify(coreRelease));
    if (coreRelease && isNewerVersion(coreRelease.version, storeHostVersion)) {
        debugLog("checkForUpdatesInBackground: core update available " + storeHostVersion + " -> " + coreRelease.version);
        updates.push({
            id: '__core_update__',
            name: 'Core',
            from: storeHostVersion,
            to: coreRelease.version,
            _isHostUpdate: true,
            download_url: coreRelease.download_url,
            latest_version: coreRelease.version
        });
    }

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
        pendingUpdates = updates;
        pendingUpdateIndex = 0;
        view = VIEWS.UPDATE_PROMPT;
        needsRedraw = true;
        debugLog("checkForUpdatesInBackground: set view to UPDATE_PROMPT");
        announce(updates.length + " updates available, " + updates[0].name);
    } else {
        needsRedraw = true;
    }
}

/* Process all pending updates (Update All action) */
function processAllUpdates() {
    let coreUpdated = false;
    let coreFrom = '';
    let coreTo = '';
    let moduleCount = 0;
    const total = pendingUpdates.length;

    for (let i = 0; i < pendingUpdates.length; i++) {
        const upd = pendingUpdates[i];

        /* Show progress overlay */
        const progressLabel = (i + 1) + '/' + total + ': ' + (upd.name || upd.id || 'update');
        drawStatusOverlay('Updating', progressLabel);
        host_flush_display();

        if (upd._isHostUpdate) {
            const result = performCoreUpdate(upd);
            if (result.success) {
                coreUpdated = true;
                coreFrom = upd.from;
                coreTo = upd.to;
                /* Refresh host version so module min_host_version checks pass */
                storeHostVersion = coreTo;
            } else {
                /* Show error and stop */
                storePickerResultTitle = 'Updates';
                storePickerMessage = result.error || 'Core update failed';
                view = VIEWS.STORE_PICKER_RESULT;
                needsRedraw = true;
                return;
            }
        } else {
            const result = sharedInstallModule(upd, storeHostVersion);
            if (result.success) {
                moduleCount++;
            }
        }
    }

    pendingUpdates = [];

    if (coreUpdated) {
        updateRestartFromVersion = coreFrom;
        updateRestartToVersion = coreTo;
        view = VIEWS.UPDATE_RESTART;
        needsRedraw = true;
        announce("Core updated to " + coreTo + ". Click to restart now, Back to restart later");
    } else if (moduleCount > 0) {
        storeInstalledModules = scanInstalledModules();
        storePickerResultTitle = 'Updates';
        storePickerMessage = 'Updated ' + moduleCount + ' module' + (moduleCount > 1 ? 's' : '');
        view = VIEWS.STORE_PICKER_RESULT;
        needsRedraw = true;
        announce(storePickerMessage);
    }
}

/* Chain settings (shown when Settings component is selected) */
const CHAIN_SETTINGS_ITEMS = [
    { key: "knobs", label: "Knobs", type: "action" },  // Opens knob assignment editor
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "slot:muted", label: "Muted", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:soloed", label: "Soloed", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 0, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -2, max: 15, step: 1 },  // -2 = passthrough, -1 = auto, 0-15 = ch 1-16
    { key: "save", label: "[Save]", type: "action" },  // Save slot preset (overwrite for existing)
    { key: "save_as", label: "[Save As]", type: "action" },  // Save as new preset
    { key: "delete", label: "[Delete]", type: "action" }  // Delete slot preset
];
let selectedChainSetting = 0;
let editingChainSettingValue = false;

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
let editingComponentKey = "";    // "synth", "fx1", "fx2", "midiFx"
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
let hierEditorSelectedIdx = 0;
let hierEditorEditMode = false;   // true when editing a param value
let hierEditorChainParams = [];   // metadata from chain_params

/* Master FX flag - when true, exit returns to MASTER_FX view instead of CHAIN_EDIT */
let hierEditorIsMasterFx = false;
let hierEditorMasterFxSlot = -1;      // Which Master FX slot (0-3) we're editing

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

/* Loaded module UI state */
let loadedModuleUi = null;       // The chain_ui object from loaded module
let loadedModuleSlot = -1;       // Which slot the module UI is for
let loadedModuleComponent = "";  // "synth", "fx1", "fx2"
let moduleUiLoadError = false;   // True if load failed

/* Asset warning overlay state */
let assetWarningActive = false;  // True when showing asset warning overlay
let assetWarningTitle = "";      // e.g., "Dexed Warning"
let assetWarningLines = [];      // Wrapped error message lines
let assetWarningShownForSlots = new Set();  // Track which chain slots have shown warnings
let assetWarningShownForMasterFx = new Set();  // Track which Master FX slots have shown warnings

const MODULES_ROOT = "/data/UserData/move-anything/modules";

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

/* Convert component key to DSP param prefix (midiFx -> midi_fx1) */
function getComponentParamPrefix(componentKey) {
    return componentKey === "midiFx" ? "midi_fx1" : componentKey;
}

/* Set up shims for host_module_get_param and host_module_set_param
 * These route to the correct slot and component in shadow mode */
function setupModuleParamShims(slot, componentKey) {
    const prefix = getComponentParamPrefix(componentKey);

    globalThis.host_module_get_param = function(key) {
        return getSlotParam(slot, `${prefix}:${key}`);
    };

    globalThis.host_module_set_param = function(key, value) {
        return setSlotParam(slot, `${prefix}:${key}`, value);
    };
}

/* Clear the param shims */
function clearModuleParamShims() {
    delete globalThis.host_module_get_param;
    delete globalThis.host_module_set_param;
}

/* Load a module's UI for editing */
function loadModuleUi(slot, componentKey, moduleId) {
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
    const synthError = getSlotParam(slotIndex, "synth_error");
    if (synthError && synthError.length > 0) {
        const synthName = getSlotParam(slotIndex, "synth:name") || "Synth";
        assetWarningTitle = `${synthName} Warning`;
        assetWarningLines = wrapText(synthError, 18);
        assetWarningActive = true;
        /* Announce the error */
        announce(`${assetWarningTitle}: ${synthError}`);
        needsRedraw = true;
        return true;
    }
    return false;
}

/* Check for Master FX error in a slot and show warning if found */
function checkAndShowMasterFxError(fxSlot) {
    /* fxSlot is 0-3 for the 4 Master FX slots */
    const fxNum = fxSlot + 1;  /* fx1, fx2, fx3, fx4 */
    const fxError = getMasterFxParam(fxSlot, "error");
    if (fxError && fxError.length > 0) {
        const fxName = getMasterFxParam(fxSlot, "name") || `FX ${fxNum}`;
        assetWarningTitle = `${fxName} Warning`;
        assetWarningLines = wrapText(fxError, 18);
        assetWarningActive = true;
        /* Announce the error */
        announce(`${assetWarningTitle}: ${fxError}`);
        needsRedraw = true;
        return true;
    }
    return false;
}

/* Dismiss asset warning overlay */
function dismissAssetWarning() {
    assetWarningActive = false;
    assetWarningTitle = "";
    assetWarningLines = [];
    needsRedraw = true;
}

/* Initialize chain configs for all slots */
function initChainConfigs() {
    chainConfigs = [];
    lastSlotModuleSignatures = [];
    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
        chainConfigs.push(createEmptyChainConfig());
        lastSlotModuleSignatures.push("");
    }
}

/* Load chain config from current patch info */
function loadChainConfigFromSlot(slotIndex) {
    const cfg = chainConfigs[slotIndex] || createEmptyChainConfig();

    /* Read current patch configuration from DSP
     * Note: get_param uses underscores (synth_module), set_param uses colons (synth:module) */
    const synthModule = getSlotParam(slotIndex, "synth_module");
    const midiFxModule = getSlotParam(slotIndex, "midi_fx1_module");
    const fx1Module = getSlotParam(slotIndex, "fx1_module");
    const fx2Module = getSlotParam(slotIndex, "fx2_module");

    const oldFx1 = cfg.fx1 ? cfg.fx1.module : null;
    const oldFx2 = cfg.fx2 ? cfg.fx2.module : null;

    cfg.synth = synthModule && synthModule !== "" ? { module: synthModule.toLowerCase(), params: {} } : null;
    cfg.midiFx = midiFxModule && midiFxModule !== "" ? { module: midiFxModule.toLowerCase(), params: {} } : null;
    cfg.fx1 = fx1Module && fx1Module !== "" ? { module: fx1Module.toLowerCase(), params: {} } : null;
    cfg.fx2 = fx2Module && fx2Module !== "" ? { module: fx2Module.toLowerCase(), params: {} } : null;

    /* Clear display_name cache when FX modules change (prevents stale announcement on swap) */
    const newFx1 = cfg.fx1 ? cfg.fx1.module : null;
    const newFx2 = cfg.fx2 ? cfg.fx2.module : null;
    if (newFx1 !== oldFx1) delete fxDisplayNameCache[`${slotIndex}:fx1`];
    if (newFx2 !== oldFx2) delete fxDisplayNameCache[`${slotIndex}:fx2`];

    chainConfigs[slotIndex] = cfg;
    return cfg;
}

/* Build a signature of module IDs for a slot to detect changes */
function getSlotModuleSignature(slotIndex) {
    const synthModule = getSlotParam(slotIndex, "synth_module") || "";
    const midiFxModule = getSlotParam(slotIndex, "midi_fx1_module") || "";
    const fx1Module = getSlotParam(slotIndex, "fx1_module") || "";
    const fx2Module = getSlotParam(slotIndex, "fx2_module") || "";
    return `${synthModule}|${midiFxModule}|${fx1Module}|${fx2Module}`;
}

/* Refresh module signature for a slot and invalidate knob cache on changes */
function refreshSlotModuleSignature(slotIndex) {
    if (slotIndex < 0 || slotIndex >= SHADOW_UI_SLOTS) return false;
    const signature = getSlotModuleSignature(slotIndex);
    if (signature !== lastSlotModuleSignatures[slotIndex]) {
        lastSlotModuleSignatures[slotIndex] = signature;
        loadChainConfigFromSlot(slotIndex);
        invalidateKnobContextCache();
        needsRedraw = true;
        return true;
    }
    return false;
}

/* Cache a module's abbreviation from its module.json */
function cacheModuleAbbrev(json) {
    if (json && json.id && json.abbrev) {
        moduleAbbrevCache[json.id.toLowerCase()] = json.abbrev;
    }
}

/* Get abbreviation for a module */
function getModuleAbbrev(moduleId) {
    if (!moduleId) return "--";
    const lower = moduleId.toLowerCase();
    return moduleAbbrevCache[lower] || moduleId.substring(0, 2).toUpperCase();
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

function setSlotParam(slot, key, value) {
    if (typeof shadow_set_param !== "function") return false;
    try {
        return shadow_set_param(slot, key, String(value));
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

function clearSlotForEmptySetState(slot) {
    const keys = ["synth:module", "midi_fx1:module", "fx1:module", "fx2:module"];
    let allOk = true;
    for (const key of keys) {
        const ok = setSlotParamWithRetry(slot, key, "", 1500, 3000, "SET_CHANGED: clear");
        if (!ok) allOk = false;
    }
    return allOk;
}

/* Scan modules directory for audio_fx modules */
function scanForAudioFxModules() {
    const MODULES_DIR = "/data/UserData/move-anything/modules";
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
    const MODULES_DIR = "/data/UserData/move-anything/modules";
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
                debugLog("FOUND overtake: " + json.name);
                result.push({
                    id: json.id || name,
                    name: json.name || name,
                    path: dirPath,
                    uiPath: `${dirPath}/${json.ui || 'ui.js'}`,
                    dsp: json.dsp || null,
                    basePath: dirPath
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

    debugLog("returning " + result.length + " modules");
    return result;
}

/* Enter the overtake module selection menu */
function enterOvertakeMenu() {
    /* Reset all overtake state to ensure clean menu entry */
    overtakeModuleLoaded = false;
    overtakeModulePath = "";
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
    /* Deactivate LED queue before cleanup - restores original move_midi_internal_send */
    deactivateLedQueue();

    /* Unload overtake DSP if loaded */
    if (typeof shadow_set_param === "function") {
        shadow_set_param(0, "overtake_dsp:unload", "1");
    }
    delete globalThis.host_module_set_param;
    delete globalThis.host_module_get_param;

    overtakeModuleLoaded = false;
    overtakeModulePath = "";
    overtakeModuleCallbacks = null;

    /* Start progressive LED clearing before exiting */
    overtakeExitPending = true;
    ledClearIndex = 0;
    needsRedraw = true;
}

/* Complete the exit after LEDs are cleared */
function completeOvertakeExit() {
    overtakeExitPending = false;

    /* Disable overtake mode to allow MIDI to reach Move again */
    if (typeof shadow_set_overtake_mode === "function") {
        shadow_set_overtake_mode(0);
    }

    /* Return to slots view */
    setView(VIEWS.SLOTS);
    needsRedraw = true;
    /* Request exit from shadow UI to return to Move */
    if (typeof shadow_request_exit === "function") {
        shadow_request_exit();
    }
}

/* Load and run an overtake module */
function loadOvertakeModule(moduleInfo) {
    debugLog("loadOvertakeModule called with: " + JSON.stringify(moduleInfo));
    if (!moduleInfo || !moduleInfo.uiPath) {
        debugLog("loadOvertakeModule: no moduleInfo or uiPath");
        return false;
    }

    try {
        /* Step 1: Enable overtake mode 2 (module) - all MIDI forwarded including external */
        if (typeof shadow_set_overtake_mode === "function") {
            shadow_set_overtake_mode(2);  /* 2 = module mode (all events) */
            debugLog("loadOvertakeModule: overtake_mode=2 (module)");
        }

        /* Reset escape state variables for clean state */
        hostShiftHeld = false;
        hostVolumeKnobTouched = false;
        debugLog("loadOvertakeModule: escape state reset");

        /* Activate LED queue before loading module - intercepts move_midi_internal_send
         * to prevent SHM buffer flooding from modules that send many LEDs per tick */
        activateLedQueue();

        /* Save current globals before loading - module may overwrite them */
        const savedInit = globalThis.init;
        const savedTick = globalThis.tick;
        const savedMidi = globalThis.onMidiMessageInternal;

        overtakeModulePath = moduleInfo.uiPath;
        setView(VIEWS.OVERTAKE_MODULE);
        needsRedraw = true;

        /* Step 2: Load DSP plugin if the module has one (before JS load so params work) */
        if (moduleInfo.dsp && typeof shadow_set_param === "function") {
            const dspPath = moduleInfo.basePath + "/" + moduleInfo.dsp;
            debugLog("loadOvertakeModule: loading DSP from " + dspPath);
            shadow_set_param(0, "overtake_dsp:load", dspPath);
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
        globalThis.host_module_get_param = function(key) {
            if (typeof shadow_get_param === "function") {
                return shadow_get_param(0, "overtake_dsp:" + key);
            }
            return null;
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
                if (typeof shadow_set_param === "function") {
                    shadow_set_param(0, "overtake_dsp:unload", "1");
                }
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
            onMidiMessageInternal: (globalThis.onMidiMessageInternal !== savedMidi) ? globalThis.onMidiMessageInternal : null
        };

        /* Restore shadow UI's globals */
        globalThis.init = savedInit;
        globalThis.tick = savedTick;
        globalThis.onMidiMessageInternal = savedMidi;

        debugLog("loadOvertakeModule: callbacks captured - init:" + !!overtakeModuleCallbacks.init +
                 " tick:" + !!overtakeModuleCallbacks.tick +
                 " midi:" + !!overtakeModuleCallbacks.onMidiMessageInternal);

        overtakeModuleLoaded = true;

        /* Step 6: Defer init() call - LEDs will be cleared progressively during loading screen */
        overtakeInitPending = true;
        overtakeInitTicks = 0;
        ledClearIndex = 0;  /* Start LED clearing from beginning */
        debugLog("loadOvertakeModule: init deferred, LEDs will clear progressively");

        return true;
    } catch (e) {
        debugLog("loadOvertakeModule error: " + e);
        deactivateLedQueue();
        overtakeModuleLoaded = false;
        overtakeModuleCallbacks = null;
        /* Clean up DSP and param shims on error */
        if (typeof shadow_set_param === "function") {
            shadow_set_param(0, "overtake_dsp:unload", "1");
        }
        delete globalThis.host_module_set_param;
        delete globalThis.host_module_get_param;
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

    drawFooter({left: "Back: Exit", right: "Jog: Select"});
}

/* Handle input in overtake menu */
function handleOvertakeMenuInput(cc, value) {
    if (cc === MoveMainKnob) {
        /* Jog wheel */
        const delta = decodeDelta(value);
        selectedOvertakeModule += delta;
        if (selectedOvertakeModule < 0) selectedOvertakeModule = 0;
        if (selectedOvertakeModule >= overtakeModules.length) {
            selectedOvertakeModule = Math.max(0, overtakeModules.length - 1);
        }
        needsRedraw = true;
        return true;
    }

    if (cc === MoveMainButton && value > 0) {
        /* Jog click - select module */
        if (overtakeModules.length > 0 && selectedOvertakeModule < overtakeModules.length) {
            const selected = overtakeModules[selectedOvertakeModule];
            if (selected.id === "__back_to_move__") {
                exitOvertakeMode();
            } else if (selected.id === "__get_more__") {
                /* Open store picker - stay in menu mode so we receive input */
                enterStorePicker('overtake');
            } else {
                loadOvertakeModule(selected);
            }
        }
        return true;
    }

    if (cc === MoveBack && value > 0) {
        /* Back button - exit to Move */
        exitOvertakeMode();
        return true;
    }

    return false;
}

/* Master FX param helpers - use slot 0 with master_fx: prefix */
function getMasterFxModule() {
    if (typeof shadow_get_param !== "function") return "";
    try {
        return shadow_get_param(0, "master_fx:module") || "";
    } catch (e) {
        return "";
    }
}

function setMasterFxModule(moduleId) {
    if (typeof shadow_set_param !== "function") return false;
    try {
        return shadow_set_param(0, "master_fx:module", moduleId || "");
    } catch (e) {
        return false;
    }
}

function loadPatchByIndex(slot, index) {
    /* Clear warning tracking for this slot so new warnings can show */
    assetWarningShownForSlots.delete(slot);
    return setSlotParam(slot, "load_patch", index);
}

function getPatchCount(slot) {
    const val = getSlotParam(slot, "patch_count");
    return val ? parseInt(val) || 0 : 0;
}

function getPatchName(slot, index) {
    return getSlotParam(slot, `patch_name_${index}`);
}

function getSynthPreset(slot) {
    return getSlotParam(slot, "synth:preset");
}

function setSynthPreset(slot, preset) {
    return setSlotParam(slot, "synth:preset", preset);
}

function getFxParam(slot, fxNum, param) {
    return getSlotParam(slot, `fx${fxNum}:${param}`);
}

function setFxParam(slot, fxNum, param, value) {
    return setSlotParam(slot, `fx${fxNum}:${param}`, value);
}

/* Fetch chain_params metadata from a component */
function getComponentChainParams(slot, componentKey) {
    /* Chain params are typically in module.json, but we query via get_param */
    const key = componentKey === "synth" ? "synth:chain_params" :
                componentKey === "fx1" ? "fx1:chain_params" :
                componentKey === "fx2" ? "fx2:chain_params" :
                componentKey === "midiFx" ? "midi_fx1:chain_params" : null;
    if (!key) return [];

    const json = getSlotParam(slot, key);
    if (!json) return [];

    try {
        return JSON.parse(json);
    } catch (e) {
        return [];
    }
}

/* Fetch ui_hierarchy from a component */
function getComponentHierarchy(slot, componentKey) {
    const key = componentKey === "synth" ? "synth:ui_hierarchy" :
                componentKey === "fx1" ? "fx1:ui_hierarchy" :
                componentKey === "fx2" ? "fx2:ui_hierarchy" :
                componentKey === "midiFx" ? "midi_fx1:ui_hierarchy" : null;
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

/* Fetch chain_params metadata from a Master FX slot */
function getMasterFxChainParams(fxSlot) {
    if (fxSlot < 0 || fxSlot >= 4) return [];
    const fxKey = `fx${fxSlot + 1}`;
    const key = `master_fx:${fxKey}:chain_params`;
    const json = shadow_get_param(0, key);
    if (!json) return [];
    try {
        return JSON.parse(json);
    } catch (e) {
        return [];
    }
}

/* Fetch ui_hierarchy from a Master FX slot */
function getMasterFxHierarchy(fxSlot) {
    if (fxSlot < 0 || fxSlot >= 4) return null;
    const fxKey = `fx${fxSlot + 1}`;
    const key = `master_fx:${fxKey}:ui_hierarchy`;
    const json = shadow_get_param(0, key);
    if (!json) return null;
    try {
        return JSON.parse(json);
    } catch (e) {
        return null;
    }
}

/* Fetch patch detail info from chain DSP */
function fetchPatchDetail(slot) {
    patchDetail.synthName = getSlotParam(slot, "synth:name") || "Unknown";
    patchDetail.synthPreset = getSlotParam(slot, "synth:preset_name") || getSlotParam(slot, "synth:preset") || "-";
    patchDetail.fx1Name = getSlotParam(slot, "fx1:name") || "None";
    patchDetail.fx1Wet = getSlotParam(slot, "fx1:wet") || "-";
    patchDetail.fx2Name = getSlotParam(slot, "fx2:name") || "None";
    patchDetail.fx2Wet = getSlotParam(slot, "fx2:wet") || "-";
}

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

/* Get items for patch detail view */
function getDetailItems() {
    return [
        { label: "Synth", value: patchDetail.synthName, subvalue: patchDetail.synthPreset, editable: true, component: "synth" },
        { label: "FX1", value: patchDetail.fx1Name, subvalue: patchDetail.fx1Wet, editable: true, component: "fx1" },
        { label: "FX2", value: patchDetail.fx2Name, subvalue: patchDetail.fx2Wet, editable: true, component: "fx2" },
        { label: "Load Patch", value: "", subvalue: "", editable: false, component: "" }
    ];
}

/* Known synth parameters that can be edited */
const SYNTH_PARAMS = [
    { key: "preset", label: "Preset", type: "int", min: 0, max: 127 },
    { key: "volume", label: "Volume", type: "float", min: 0, max: 1 },
];

/* Known FX parameters that can be edited */
const FX_PARAMS = [
    { key: "wet", label: "Wet", type: "float", min: 0, max: 1 },
    { key: "dry", label: "Dry", type: "float", min: 0, max: 1 },
    { key: "room_size", label: "Size", type: "float", min: 0, max: 1 },
    { key: "damping", label: "Damp", type: "float", min: 0, max: 1 },
];

/* Fetch current parameter values for a component */
function fetchComponentParams(slot, component) {
    const prefix = component + ":";
    const params = component === "synth" ? SYNTH_PARAMS : FX_PARAMS;
    const result = [];

    for (const param of params) {
        const fullKey = prefix + param.key;
        const value = getSlotParam(slot, fullKey);
        if (value !== null) {
            result.push({
                key: fullKey,
                label: param.label,
                value: value,
                type: param.type,
                min: param.min,
                max: param.max
            });
        }
    }

    return result;
}

/* Enter component parameter editing view */
function enterComponentParams(slot, component) {
    editingComponent = component;
    componentParams = fetchComponentParams(slot, component);
    selectedParam = 0;
    editingValue = false;
    setView(VIEWS.COMPONENT_PARAMS);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    if (componentParams.length > 0) {
        const param = componentParams[0];
        const label = param.label || param.key;
        const value = formatParamValue(param);
        announce(`Component Parameters, ${label}: ${value}`);
    } else {
        announce("Component Parameters, No parameters");
    }
}

/* Format a parameter value for display */
function formatParamValue(param) {
    if (param.type === "float") {
        const num = parseFloat(param.value);
        if (isNaN(num)) return param.value;
        return num.toFixed(2);
    }
    return param.value;
}

/* Adjust parameter value by delta */
function adjustParamValue(param, delta) {
    let val;
    if (param.type === "float") {
        val = parseFloat(param.value) || 0;
        const step = (param.step > 0) ? param.step : KNOB_BASE_STEP_FLOAT;
        val += delta * step;
    } else {
        val = parseInt(param.value) || 0;
        val += delta;
    }

    /* Clamp to range */
    val = Math.max(param.min, Math.min(param.max, val));

    if (param.type === "float") {
        return val.toFixed(4);
    }
    return String(Math.round(val));
}

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
    const payload = {
        patches: nextSlots.map((slot, idx) => ({
            name: slot.name,
            channel: slot.channel,
            forward_channel: parseInt(getSlotParam(idx, "slot:forward_channel") || "-1")
        })),
        master_fx: currentMasterFxId || ""
    };
    try {
        host_write_file(CONFIG_PATH, JSON.stringify(payload, null, 2) + "\n");
    } catch (e) {
        /* ignore */
    }
}

function saveConfigMasterFx() {
    /* Save just the master FX setting to config */
    const data = safeLoadJson(CONFIG_PATH);
    const payload = {
        patches: data && Array.isArray(data.patches) ? data.patches : slots.map((slot, idx) => ({
            name: slot.name,
            channel: slot.channel,
            forward_channel: parseInt(getSlotParam(idx, "slot:forward_channel") || "-1")
        })),
        master_fx: currentMasterFxId || "",
        master_fx_path: currentMasterFxPath || ""
    };
    try {
        host_write_file(CONFIG_PATH, JSON.stringify(payload, null, 2) + "\n");
    } catch (e) {
        /* ignore */
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
    if (Array.isArray(hostSlots) && hostSlots.length) {
        slots = hostSlots.map((slot, idx) => ({
            channel: (typeof slot.channel === "number") ? slot.channel : (DEFAULT_SLOTS[idx] ? DEFAULT_SLOTS[idx].channel : 1 + idx),
            /* Prefer config name (set by save), fall back to shim name, then default */
            name: (configSlots[idx] && configSlots[idx].name) || slot.name || (DEFAULT_SLOTS[idx] ? DEFAULT_SLOTS[idx].name : "Unknown Patch")
        }));
    } else {
        slots = configSlots;
    }
    if (selectedSlot >= slots.length) {
        selectedSlot = Math.max(0, slots.length - 1);
    }
    needsRedraw = true;
}

function parsePatchName(path) {
    try {
        const raw = std.loadFile(path);
        if (!raw) return null;
        const match = raw.match(/"name"\s*:\s*"([^"]+)"/);
        if (match && match[1]) {
            return match[1];
        }
    } catch (e) {
        return null;
    }
    return null;
}

function loadPatchList() {
    const entries = [];
    let dir = [];
    try {
        dir = os.readdir(PATCH_DIR) || [];
    } catch (e) {
        dir = [];
    }
    const names = dir[0];
    if (!Array.isArray(names)) {
        patches = entries;
        return;
    }
    for (const name of names) {
        if (name === "." || name === "..") continue;
        if (!name.endsWith(".json")) continue;
        const path = `${PATCH_DIR}/${name}`;
        const patchName = parsePatchName(path);
        if (patchName) {
            entries.push({ name: patchName, file: name });
        }
    }
    entries.sort((a, b) => {
        const al = a.name.toLowerCase();
        const bl = b.name.toLowerCase();
        if (al < bl) return -1;
        if (al > bl) return 1;
        return 0;
    });
    /* Add "New Slot Preset" as first option to clear a slot */
    patches = [{ name: "[New Slot Preset]", file: null }, ...entries];
}

function findPatchIndexByName(name) {
    if (!name) return 0;
    const match = patches.findIndex((patch) => patch.name === name);
    return match >= 0 ? match : 0;
}

function enterPatchBrowser(slotIndex) {
    loadPatchList();
    selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    if (patches.length === 0) {
        /* No patches found - still enter view to show message */
        selectedPatch = 0;
    } else {
        selectedPatch = findPatchIndexByName(slots[slotIndex]?.name);
    }
    setView(VIEWS.PATCHES);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    if (patches.length === 0) {
        announce("Patch Browser, No patches found");
    } else {
        const patchName = patches[selectedPatch]?.name || "Unknown";
        announce(`Patch Browser, ${patchName}`);
    }
}

function enterPatchDetail(slotIndex, patchIndex) {
    selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    selectedPatch = patchIndex;
    selectedDetailItem = 0;
    fetchPatchDetail(slotIndex);
    setView(VIEWS.PATCH_DETAIL);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const patchName = patches[patchIndex]?.name || "Unknown";
    const items = getDetailItems();
    if (items.length > 0) {
        const item = items[0];
        const value = item.value || "Empty";
        announce(`${patchName}, ${item.label}: ${value}`);
    }
}

/* Special patch index value meaning clear the slot - must match shim */
const PATCH_INDEX_NONE = 65535;

function applyPatchSelection() {
    const patch = patches[selectedPatch];
    const slot = slots[selectedSlot];
    if (!patch || !slot) return;
    const isNewSlot = patch.name === "[New Slot Preset]";
    slot.name = isNewSlot ? "Untitled" : patch.name;
    saveSlotsToConfig(slots);
    if (typeof shadow_request_patch === "function") {
        try {
            /* "[New Slot Preset]" is at index 0 in patches array, use special value 65535
             * Real patches start at index 1, so subtract 1 for shim's index */
            const patchIndex = isNewSlot ? PATCH_INDEX_NONE : selectedPatch - 1;
            shadow_request_patch(selectedSlot, patchIndex);
        } catch (e) {
            /* ignore */
        }
    }
    /* Refresh detail info and knob mappings after loading/clearing patch */
    fetchPatchDetail(selectedSlot);
    fetchKnobMappings(selectedSlot);
    invalidateKnobContextCache();  /* Clear stale knob contexts after patch change */
    setView(VIEWS.SLOTS);
    needsRedraw = true;
}

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
    /* New preset: hide Save As and Delete (only Save makes sense) */
    return CHAIN_SETTINGS_ITEMS.filter(function(item) {
        return item.key !== "save_as" && item.key !== "delete";
    });
}

/* Find patch index by name (for conflict detection) */
function findPatchByName(name) {
    loadPatchList();
    for (let i = 1; i < patches.length; i++) {
        if (patches[i].name === name) {
            return i - 1;
        }
    }
    return -1;
}

/* Generate default name from chain components */
function generateSlotPresetName(slotIndex) {
    const cfg = chainConfigs[slotIndex];
    if (!cfg) return "Untitled";

    const parts = [];
    if (cfg.synth && cfg.synth.module) {
        const abbrev = moduleAbbrevCache[cfg.synth.module] || cfg.synth.module.toUpperCase().slice(0, 3);
        parts.push(abbrev);
    }
    if (cfg.fx1 && cfg.fx1.module) {
        const abbrev = moduleAbbrevCache[cfg.fx1.module] || cfg.fx1.module.toUpperCase().slice(0, 2);
        parts.push(abbrev);
    }
    if (cfg.fx2 && cfg.fx2.module) {
        const abbrev = moduleAbbrevCache[cfg.fx2.module] || cfg.fx2.module.toUpperCase().slice(0, 2);
        parts.push(abbrev);
    }

    return parts.length > 0 ? parts.join(" + ") : "Untitled";
}

/* Generate a unique preset name by appending _02, _03, etc. if needed */
function generateUniquePresetName(baseName) {
    if (findPatchByName(baseName) < 0) {
        return baseName;  /* Base name is unique */
    }
    /* Try suffixes _02 through _99 */
    for (let i = 2; i <= 99; i++) {
        const suffix = i < 10 ? `_0${i}` : `_${i}`;
        const name = `${baseName}${suffix}`;
        if (findPatchByName(name) < 0) {
            return name;
        }
    }
    /* Fallback: use timestamp */
    return `${baseName}_${Date.now() % 10000}`;
}

/* Build patch JSON for saving
 * Note: save_patch expects raw chain content (synth, audio_fx at root)
 * with "custom_name" for the name. It wraps it with name/version/chain.
 */
function buildSlotPatchJson(slotIndex, name) {
    const cfg = chainConfigs[slotIndex];
    if (!cfg) return null;

    const patch = {
        custom_name: name,
        input: "both",
        synth: null,
        audio_fx: []
    };

    if (cfg.synth && cfg.synth.module) {
        /* Try to get full state from synth plugin */
        let synthConfig = cfg.synth.params || {};
        const stateJson = getSlotParam(slotIndex, "synth:state");
        if (stateJson) {
            try {
                const state = JSON.parse(stateJson);
                synthConfig = { state: state };
            } catch (e) {
                /* Keep original params if state parse fails */
            }
        }
        patch.synth = {
            module: cfg.synth.module,
            config: synthConfig
        };
    }

    if (cfg.midiFx && cfg.midiFx.module) {
        /* Try to get full state from midi_fx plugin */
        let midiFxConfig = cfg.midiFx.params || {};
        const midiFxStateJson = getSlotParam(slotIndex, "midi_fx1:state");
        if (midiFxStateJson) {
            try {
                const state = JSON.parse(midiFxStateJson);
                midiFxConfig = { state: state };
            } catch (e) {
                /* Keep original params if state parse fails */
            }
        }
        patch.midi_fx = [{
            type: cfg.midiFx.module,
            params: midiFxConfig
        }];
    }

    if (cfg.fx1 && cfg.fx1.module) {
        /* Try to get full state from fx1 plugin */
        let fx1Config = cfg.fx1.params || {};
        const fx1StateJson = getSlotParam(slotIndex, "fx1:state");
        if (fx1StateJson) {
            try {
                const state = JSON.parse(fx1StateJson);
                fx1Config = { state: state };
            } catch (e) {
                /* Keep original params if state parse fails */
            }
        }
        patch.audio_fx.push({
            type: cfg.fx1.module,
            params: fx1Config
        });
    }
    if (cfg.fx2 && cfg.fx2.module) {
        /* Try to get full state from fx2 plugin */
        let fx2Config = cfg.fx2.params || {};
        const fx2StateJson = getSlotParam(slotIndex, "fx2:state");
        if (fx2StateJson) {
            try {
                const state = JSON.parse(fx2StateJson);
                fx2Config = { state: state };
            } catch (e) {
                /* Keep original params if state parse fails */
            }
        }
        patch.audio_fx.push({
            type: cfg.fx2.module,
            params: fx2Config
        });
    }

    /* Include slot channel settings */
    const recvCh = getSlotParam(slotIndex, "slot:receive_channel");
    const fwdCh = getSlotParam(slotIndex, "slot:forward_channel");
    if (recvCh !== null) patch.receive_channel = parseInt(recvCh);
    if (fwdCh !== null) patch.forward_channel = parseInt(fwdCh);

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

    return JSON.stringify(patch);
}

/* Autosave all slot states to slot_state/slot_N.json */
function autosaveAllSlots() {
    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
        /* Sync chainConfigs from DSP before checking - prevents clobbering
         * valid autosave files for slots we haven't navigated to yet */
        refreshSlotModuleSignature(i);
        const cfg = chainConfigs[i];
        const hasSynth = cfg && cfg.synth && cfg.synth.module;
        const hasFx1 = cfg && cfg.fx1 && cfg.fx1.module;
        const hasFx2 = cfg && cfg.fx2 && cfg.fx2.module;
        const hasMidiFx = cfg && cfg.midiFx && cfg.midiFx.module;
        if (!hasSynth && !hasFx1 && !hasFx2 && !hasMidiFx) {
            /* Empty slot - write empty marker to clear autosave */
            host_write_file(
                activeSlotStateDir + "/slot_" + i + ".json",
                "{}\n"
            );
            slotDirtyCache[i] = false;
            continue;
        }

        const dirty = getSlotParam(i, "dirty");
        slotDirtyCache[i] = (dirty === "1");

        const patchJson = buildSlotPatchJson(i, slots[i].name || "Untitled");
        if (!patchJson) continue;

        /* Wrap with name, version, modified flag */
        const wrapper = {
            name: slots[i].name || "Untitled",
            version: 1,
            modified: slotDirtyCache[i],
            chain: JSON.parse(patchJson)
        };

        host_write_file(
            activeSlotStateDir + "/slot_" + i + ".json",
            JSON.stringify(wrapper, null, 2) + "\n"
        );
    }
}

/* Actually save the preset */
function doSavePreset(slotIndex, name) {
    const json = buildSlotPatchJson(slotIndex, name);
    if (!json) {
        /* TODO: show error message */
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

    /* Refresh detail info and knob mappings */
    fetchPatchDetail(slotIndex);
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

/* Enter slot settings view */
function enterSlotSettings(slotIndex) {
    selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    selectedSetting = 0;
    editingSettingValue = false;
    setView(VIEWS.SLOT_SETTINGS);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const setting = SLOT_SETTINGS[0];
    const val = getSlotSettingValue(slotIndex, setting);
    announceMenuItem(`Slot Settings, ${setting.label}`, val);
}

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

function drawMasterPresetPicker() {
    drawHeader("Master Presets");

    /* Build items: [New] + presets */
    const items = [{ name: "[New]", index: -1 }];
    for (let i = 0; i < masterPresets.length; i++) {
        items.push(masterPresets[i]);
    }

    drawMenuList({
        items: items,
        selectedIndex: selectedMasterPresetIndex,
        getLabel: (item) => {
            const isCurrent = item.index >= 0 && masterPresets[item.index]?.name === currentMasterPresetName;
            return isCurrent ? `* ${item.name}` : item.name;
        },
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y }
    });

    drawFooter("Back: cancel");
}

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
    for (let i = 0; i < 4; i++) {
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
    /* Clear all 4 FX slots */
    for (let i = 0; i < 4; i++) {
        setMasterFxSlotModule(i, "");
        masterFxConfig[`fx${i + 1}`].module = "";
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
        for (let i = 0; i < 4; i++) {
            const key = `fx${i + 1}`;
            const fxConfig = fx[key];
            if (fxConfig && fxConfig.type) {
                /* Find module path from type */
                const opt = MASTER_FX_OPTIONS.find(o => o.id === fxConfig.type);
                if (opt) {
                    setMasterFxSlotModule(i, opt.dspPath || "");
                    masterFxConfig[key].module = opt.id;

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
                }
            } else {
                setMasterFxSlotModule(i, "");
                masterFxConfig[key].module = "";
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
    const preset = {
        custom_name: name,
        fx1: null,
        fx2: null,
        fx3: null,
        fx4: null
    };

    for (let i = 0; i < 4; i++) {
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

    return JSON.stringify(preset);
}

/* Actually save the master preset */
function doSaveMasterPreset(name) {
    const json = buildMasterPresetJson(name);

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
    if (key === "help") {
        if (!helpContent) {
            try {
                const raw = host_read_file("/data/UserData/move-anything/shared/help_content.json");
                if (raw) {
                    helpContent = JSON.parse(raw);
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
                        "Move Everything is",
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
        /* Check for updates now */
        checkForUpdatesInBackground();
        if (pendingUpdates.length === 0) {
            /* No updates found — show a brief message via store result view */
            storePickerResultTitle = 'Updates';
            storePickerMessage = 'No updates available';
            view = VIEWS.STORE_PICKER_RESULT;
            needsRedraw = true;
        }
        /* If updates found, checkForUpdatesInBackground already set view to UPDATE_PROMPT */
    } else if (key === "module_store") {
        /* Open full module store with category browser */
        enterStoreFromSettings();
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

/* ========== Global Settings Functions ========== */

function enterGlobalSettings() {
    globalSettingsSectionIndex = 0;
    globalSettingsInSection = false;
    globalSettingsItemIndex = 0;
    globalSettingsEditing = false;
    setView(VIEWS.GLOBAL_SETTINGS);
    needsRedraw = true;
    const section = GLOBAL_SETTINGS_SECTIONS[0];
    announce("Settings, " + section.label);
}

function enterGlobalSettingsScreenReader() {
    /* Enter Global Settings and jump directly to Screen Reader section */
    const srIdx = GLOBAL_SETTINGS_SECTIONS.findIndex(s => s.id === "accessibility");
    globalSettingsSectionIndex = srIdx >= 0 ? srIdx : 0;
    globalSettingsInSection = true;
    globalSettingsItemIndex = 0;
    globalSettingsEditing = false;
    setView(VIEWS.GLOBAL_SETTINGS);
    needsRedraw = true;
    const section = GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex];
    const item = section.items[0];
    const value = getMasterFxSettingValue(item);
    announce("Screen Reader Settings, " + item.label + ": " + value);
}

function handleGlobalSettingsAction(key) {
    if (key === "help") {
        helpReturnView = VIEWS.GLOBAL_SETTINGS;
        handleMasterFxSettingsAction("help");
        return;
    }
    if (key === "check_updates") {
        storeReturnView = VIEWS.GLOBAL_SETTINGS;
        announce("Checking for updates");
        checkForUpdatesInBackground();
        if (pendingUpdates.length === 0) {
            storePickerResultTitle = 'Updates';
            storePickerMessage = 'No updates available';
            view = VIEWS.STORE_PICKER_RESULT;
            needsRedraw = true;
            announce("No updates available");
        }
        /* If updates found, checkForUpdatesInBackground already announced */
        return;
    }
    if (key === "module_store") {
        storeReturnView = VIEWS.GLOBAL_SETTINGS;
        enterStoreFromSettings();
        return;
    }
}

/* ========== End Global Settings Functions ========== */

function enterMasterFxSettings() {
    /* Scan for available audio_fx modules */
    MASTER_FX_OPTIONS = scanForAudioFxModules();
    /* Load current master FX chain configuration from DSP */
    loadMasterFxChainConfig();
    selectedMasterFxComponent = 0;  // Start at FX 1
    selectingMasterFxModule = false;
    setView(VIEWS.MASTER_FX);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const comp = MASTER_FX_CHAIN_COMPONENTS[0];
    const moduleName = getMasterFxSlotModule(0) || "Empty";
    announce(`Master FX, ${comp.label} ${moduleName}`);
}

/* Load master FX chain configuration from DSP */
function loadMasterFxChainConfig() {
    masterFxConfig = {
        fx1: { module: "" },
        fx2: { module: "" },
        fx3: { module: "" },
        fx4: { module: "" }
    };

    /* Query each slot's module from DSP */
    for (let i = 1; i <= 4; i++) {
        const key = `fx${i}`;
        const moduleId = getMasterFxSlotModule(i - 1);
        masterFxConfig[key].module = moduleId || "";
    }
}

/* Get a parameter from a master FX slot (0-3) */
function getMasterFxParam(slotIndex, key) {
    if (typeof shadow_get_param !== "function") return "";
    try {
        return shadow_get_param(0, `master_fx:fx${slotIndex + 1}:${key}`) || "";
    } catch (e) {
        return "";
    }
}

/* Get module ID loaded in a master FX slot (0-3) */
function getMasterFxSlotModule(slotIndex) {
    if (typeof shadow_get_param !== "function") return "";
    try {
        /* Query returns the module_id from the shim */
        return shadow_get_param(0, `master_fx:fx${slotIndex + 1}:name`) || "";
    } catch (e) {
        return "";
    }
}

/* Set module for a master FX slot */
function setMasterFxSlotModule(slotIndex, dspPath) {
    if (typeof shadow_set_param !== "function") return false;
    /* Clear warning tracking for this slot so warning can show again for new module */
    assetWarningShownForMasterFx.delete(slotIndex);
    try {
        return shadow_set_param(0, `master_fx:fx${slotIndex + 1}:module`, dspPath || "");
    } catch (e) {
        return false;
    }
}

/* Enter module selection for a Master FX slot */
function enterMasterFxModuleSelect(componentIndex) {
    const comp = MASTER_FX_CHAIN_COMPONENTS[componentIndex];
    if (!comp || comp.key === "settings") return;

    /* Set selection index to current module if any */
    const currentModule = masterFxConfig[comp.key]?.module || "";
    selectedMasterFxModuleIndex = MASTER_FX_OPTIONS.findIndex(o => o.id === currentModule);
    if (selectedMasterFxModuleIndex < 0) selectedMasterFxModuleIndex = 0;

    selectingMasterFxModule = true;
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = MASTER_FX_OPTIONS[selectedMasterFxModuleIndex]?.name || "None";
    announce(`Select ${comp.label}, ${moduleName}`);
}

/* Apply module selection for Master FX slot */
function applyMasterFxModuleSelection() {
    const comp = MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
    if (!comp || comp.key === "settings") return;

    const selected = MASTER_FX_OPTIONS[selectedMasterFxModuleIndex];
    if (selected) {
        if (selected.id === "__get_more__") {
            /* Open store picker for audio FX modules */
            selectingMasterFxModule = false;
            enterStorePicker('master_fx');
            return;
        }
        /* Load the module into the slot */
        setMasterFxSlotModule(selectedMasterFxComponent, selected.dspPath || "");
        masterFxConfig[comp.key].module = selected.id;
        /* Save to config */
        saveMasterFxChainConfig();
    }

    /* Exit module selection mode */
    selectingMasterFxModule = false;
    needsRedraw = true;
}

/* Save master FX chain configuration */
function saveMasterFxChainConfig() {
    /* The shim persists the state, but we also save to shadow config */
    try {
        const configPath = "/data/UserData/move-anything/shadow_config.json";
        let config = {};
        try {
            const content = host_read_file(configPath);
            if (content) config = JSON.parse(content);
        } catch (e) {}

        /* Save master FX chain - store dspPaths and plugin state for each slot */
        config.master_fx_chain = {
            preset_name: currentMasterPresetName || ""
        };
        for (let i = 1; i <= 4; i++) {
            const key = `fx${i}`;
            const slotIdx = i - 1;
            const moduleId = masterFxConfig[key]?.module || "";
            const stateFilePath = activeSlotStateDir + "/master_fx_" + slotIdx + ".json";

            if (!moduleId) {
                /* Empty slot - write empty marker */
                host_write_file(stateFilePath, "{}\n");
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
                        stateObj = JSON.parse(stateJson);
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

        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {
        /* Ignore errors */
    }
}

/* Save auto-update setting to shadow config */
function saveAutoUpdateConfig() {
    try {
        const configPath = "/data/UserData/move-anything/shadow_config.json";
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
        const configPath = "/data/UserData/move-anything/shadow_config.json";
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

/* Load master FX chain from config at startup.
 * The shim handles actual module loading + state restore from
 * slot_state/master_fx_N.json files at boot. This function just
 * syncs the JS-side masterFxConfig to reflect what the shim loaded. */
function loadMasterFxChainFromConfig() {
    try {
        const configPath = "/data/UserData/move-anything/shadow_config.json";
        const content = host_read_file(configPath);
        if (!content) return;

        const config = JSON.parse(content);

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

        if (!config.master_fx_chain) return;

        /* Restore loaded preset name */
        if (config.master_fx_chain.preset_name) {
            currentMasterPresetName = config.master_fx_chain.preset_name;
        }

        /* Sync masterFxConfig from state files (shim already loaded the modules) */
        for (let i = 0; i < 4; i++) {
            const key = `fx${i + 1}`;
            const stateFilePath = activeSlotStateDir + "/master_fx_" + i + ".json";
            try {
                const raw = host_read_file(stateFilePath);
                if (raw) {
                    const stateFile = JSON.parse(raw);
                    if (stateFile.module_id) {
                        masterFxConfig[key].module = stateFile.module_id;
                        debugLog(`MFX sync ${key}: module=${stateFile.module_id} (loaded by shim)`);
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
    selectedChainComponent = 0;  // Start at MIDI FX (scroll left for Chain/patch)
    /* Load current chain config from DSP */
    loadChainConfigFromSlot(slotIndex);
    setView(VIEWS.CHAIN_EDIT);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const slotName = slots[selectedSlot]?.name || "Unknown";
    const comp = CHAIN_COMPONENTS[selectedChainComponent];
    const cfg = chainConfigs[selectedSlot];
    const moduleData = cfg && cfg[comp.key];

    let info = "(empty)";
    if (moduleData && moduleData.module) {
        const prefix = comp.key === "midiFx" ? "midi_fx1" : comp.key;
        const displayName = getSlotParam(selectedSlot, `${prefix}:name`) || moduleData.module;
        info = displayName;
    }

    announce(`S${slotIndex + 1} ${slotName}, ${comp.label} ${info}`);
}

/* Scan modules directory for modules of a specific component type */
function scanModulesForType(componentType) {
    const MODULES_DIR = "/data/UserData/move-anything/modules";
    const result = [{ id: "", name: "None" }];

    /* Map component type to directory and expected component_type */
    let searchDirs = [];
    let expectedTypes = [];

    if (componentType === "synth") {
        searchDirs = [`${MODULES_DIR}/sound_generators`];
        expectedTypes = ["sound_generator"];
    } else if (componentType === "midiFx") {
        searchDirs = [`${MODULES_DIR}/midi_fx`];
        expectedTypes = ["midi_fx"];
    } else if (componentType === "fx1" || componentType === "fx2") {
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
                        /* Check if already in result to avoid duplicates */
                        const id = json.id || entry;
                        if (!result.find(m => m.id === id)) {
                            result.push({
                                id: id,
                                name: json.name || entry
                            });
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
        case 'fx1':
        case 'fx2':
        case 'master_fx': return 'audio_fx';
        case 'midiFx': return 'midi_fx';
        case 'overtake': return 'overtake';
        default: return null;
    }
}

/* Enter the store picker for a specific component type */
function enterStorePicker(componentKey) {
    const categoryId = componentKeyToCategoryId(componentKey);
    if (!categoryId) return;

    storePickerCategory = categoryId;
    storePickerSelectedIndex = 0;
    storePickerCurrentModule = null;
    storePickerActionIndex = 0;
    storePickerResultTitle = '';  /* Reset to default 'Module Store' */
    storePickerFromOvertake = (componentKey === 'overtake');
    storePickerFromMasterFx = (componentKey === 'master_fx');

    /* Check if we need to fetch catalog */
    if (!storeCatalog) {
        setView(VIEWS.STORE_PICKER_LOADING);
        storePickerLoadingTitle = 'Module Store';
        storePickerLoadingMessage = 'Loading catalog...';
        announce("Loading catalog");
        needsRedraw = true;

        /* Fetch catalog (blocking) */
        fetchStoreCatalogSync();
        /* Force display update after catalog load */
        needsRedraw = true;
    } else {
        /* Catalog already loaded, go to list */
        storePickerModules = getModulesForCategory(storeCatalog, categoryId);
        setView(VIEWS.STORE_PICKER_LIST);
        needsRedraw = true;
        /* Announce menu title + initial selection with status */
        if (storePickerModules.length > 0) {
            const module = storePickerModules[0];
            const statusLabel = getStoreModuleStatusLabel(module);
            announce(`Module Store, ${module.name}` + (statusLabel ? `, ${statusLabel}` : ""));
        } else {
            announce("Module Store, No modules available");
        }
    }
}

/* Fetch catalog synchronously (blocking) */
function fetchStoreCatalogSync() {
    if (storeFetchPending) return;
    storeFetchPending = true;

    const spinChars = ['-', '\\', '|', '/'];
    let spinIdx = 0;
    const onProgress = (title, message, current, total) => {
        storePickerLoadingTitle = title;
        if (current && total) {
            storePickerLoadingMessage = message + ' (' + current + '/' + total + ') ' + spinChars[spinIdx % 4];
        } else {
            storePickerLoadingMessage = message + ' ' + spinChars[spinIdx % 4];
        }
        spinIdx++;
        /* Update display during long fetch operations */
        drawStorePickerLoading();
        host_flush_display();
    };

    storeHostVersion = getHostVersion();
    storeInstalledModules = scanInstalledModules();

    const result = fetchCatalog(onProgress);
    storeFetchPending = false;

    if (result.success) {
        storeCatalog = result.catalog;
        storeHostVersion = getHostVersion();

        if (storePickerFromSettings) {
            /* Settings entry — caller will navigate to categories */
        } else {
            /* Single-category entry — go directly to module list */
            storePickerModules = getModulesForCategory(storeCatalog, storePickerCategory);
            /* Prepend core update if available */
            const hostUpdate = getHostUpdateModule();
            if (hostUpdate) {
                storePickerModules = [hostUpdate, ...storePickerModules];
            }
            setView(VIEWS.STORE_PICKER_LIST);
            /* Announce menu title + initial selection with status */
            if (storePickerModules.length > 0) {
                const module = storePickerModules[0];
                const statusLabel = getStoreModuleStatusLabel(module);
                announce(`Module Store, ${module.name}` + (statusLabel ? `, ${statusLabel}` : ""));
            } else {
                announce("Module Store, No modules available");
            }
        }
    } else {
        storePickerMessage = result.error || 'Failed to load catalog';
        setView(VIEWS.STORE_PICKER_RESULT);
        announce(storePickerMessage);
    }
    needsRedraw = true;
}

/* Enter the full module store from MFX settings (category browser) */
function enterStoreFromSettings() {
    storePickerFromSettings = true;
    storePickerFromOvertake = false;
    storePickerFromMasterFx = false;
    storeCategoryIndex = 0;
    storePickerCurrentModule = null;

    /* Fetch catalog if needed */
    if (!storeCatalog) {
        setView(VIEWS.STORE_PICKER_LOADING);
        storePickerLoadingTitle = 'Module Store';
        storePickerLoadingMessage = 'Loading catalog...';
        announce("Loading catalog");
        needsRedraw = true;

        fetchStoreCatalogSync();

        if (!storeCatalog) {
            /* fetchStoreCatalogSync already set error view */
            return;
        }
    }

    /* Build category list and enter categories view */
    buildStoreCategoryItems();
    setView(VIEWS.STORE_PICKER_CATEGORIES);
    if (storeCategoryItems.length > 0) {
        announce("Module Store, " + storeCategoryItems[0].label);
    }
    needsRedraw = true;
}

/* Build the category item list (with counts, host update, update-all) */
function buildStoreCategoryItems() {
    storeCategoryItems = [];
    storeHostVersion = getHostVersion();
    storeInstalledModules = scanInstalledModules();

    /* Core update at top if available */
    const hostUpdate = getHostUpdateModule();
    if (hostUpdate) {
        storeCategoryItems.push({
            id: '__host_update__',
            label: 'Update Host',
            value: storeHostVersion + ' -> ' + hostUpdate.latest_version,
            _hostUpdate: hostUpdate
        });
    }

    /* Module categories with counts */
    for (const cat of CATEGORIES) {
        const mods = getModulesForCategory(storeCatalog, cat.id);
        if (mods.length > 0) {
            storeCategoryItems.push({
                id: cat.id,
                label: cat.name,
                value: '(' + mods.length + ')'
            });
        }
    }

    /* "Update All" if any modules have updates */
    const updatable = getModulesWithUpdates();
    if (updatable.length > 0) {
        storeCategoryItems.push({
            id: '__update_all__',
            label: 'Update All',
            value: '(' + updatable.length + ')'
        });
    }
}

/* Get list of installed modules that have updates available */
function getModulesWithUpdates() {
    if (!storeCatalog || !storeCatalog.modules) return [];
    return storeCatalog.modules.filter(mod => {
        const status = getModuleStatus(mod, storeInstalledModules);
        return status.installed && status.hasUpdate;
    });
}

/* Handle jog wheel in store category browser */
function handleStoreCategoryJog(delta) {
    storeCategoryIndex += delta;
    if (storeCategoryIndex < 0) storeCategoryIndex = 0;
    if (storeCategoryIndex >= storeCategoryItems.length) {
        storeCategoryIndex = storeCategoryItems.length - 1;
    }
    if (storeCategoryIndex < 0) storeCategoryIndex = 0;
    if (storeCategoryItems.length > 0) {
        announceMenuItem("Store", storeCategoryItems[storeCategoryIndex].label);
    }
    needsRedraw = true;
}

/* Handle selection in store category browser */
function handleStoreCategorySelect() {
    if (storeCategoryItems.length === 0) return;

    const item = storeCategoryItems[storeCategoryIndex];

    if (item.id === '__host_update__') {
        /* Go directly to host update detail */
        storePickerCurrentModule = item._hostUpdate;
        storePickerActionIndex = 0;
        setView(VIEWS.STORE_PICKER_DETAIL);
        announce("Core Update, " + storeHostVersion + " to " + item._hostUpdate.latest_version);
        needsRedraw = true;
        return;
    }

    if (item.id === '__update_all__') {
        /* Process all pending updates */
        const updatable = getModulesWithUpdates();
        const hostUpdate = getHostUpdateModule();
        pendingUpdates = [];
        if (hostUpdate) {
            pendingUpdates.push(hostUpdate);
        }
        for (const mod of updatable) {
            pendingUpdates.push(mod);
        }
        pendingUpdateIndex = 0;
        setView(VIEWS.UPDATE_PROMPT);
        announce("Update All, " + pendingUpdates.length + " updates available");
        needsRedraw = true;
        return;
    }

    /* Regular category — enter module list */
    storePickerCategory = item.id;
    storePickerSelectedIndex = 0;
    storePickerModules = getModulesForCategory(storeCatalog, item.id);

    /* Prepend core update if browsing and it's available */
    /* (only at category level, not here — host update has its own top-level entry) */

    setView(VIEWS.STORE_PICKER_LIST);
    if (storePickerModules.length > 0) {
        const mod = storePickerModules[0];
        const statusLabel = getStoreModuleStatusLabel(mod);
        announce(item.label + ", " + mod.name + (statusLabel ? ", " + statusLabel : ""));
    } else {
        announce(item.label + ", No modules available");
    }
    needsRedraw = true;
}

/* Get accessible status label for a store module */
function getStoreModuleStatusLabel(mod) {
    if (mod._isHostUpdate) return "Update available";
    const status = getModuleStatus(mod, storeInstalledModules);
    if (!status.installed) return "";
    return status.hasUpdate ? "Update available" : "Installed";
}

/* Handle jog wheel in store picker list */
function handleStorePickerListJog(delta) {
    storePickerSelectedIndex += delta;
    if (storePickerSelectedIndex < 0) storePickerSelectedIndex = 0;
    if (storePickerSelectedIndex >= storePickerModules.length) {
        storePickerSelectedIndex = storePickerModules.length - 1;
    }
    if (storePickerSelectedIndex < 0) storePickerSelectedIndex = 0;
    if (storePickerModules.length > 0) {
        const mod = storePickerModules[storePickerSelectedIndex];
        const statusLabel = getStoreModuleStatusLabel(mod);
        announceMenuItem("Store", (mod.name || mod.id || "Module") + (statusLabel ? ", " + statusLabel : ""));
    }
    needsRedraw = true;
}

/* Handle jog wheel in store picker detail */
function handleStorePickerDetailJog(delta) {
    if (storeDetailScrollState) {
        handleScrollableTextJog(storeDetailScrollState, delta);
        needsRedraw = true;
    }
}

/* Handle selection in store picker list */
function handleStorePickerListSelect() {
    if (storePickerModules.length === 0) return;

    storePickerCurrentModule = storePickerModules[storePickerSelectedIndex];
    storePickerActionIndex = 0;
    setView(VIEWS.STORE_PICKER_DETAIL);
    needsRedraw = true;

    /* Announce module details: name, version, status, description */
    const mod = storePickerCurrentModule;
    let detailAnnounce = mod.name;
    if (mod._isHostUpdate) {
        detailAnnounce += `, version ${storeHostVersion} to ${mod.latest_version}`;
        detailAnnounce += ". Update Move Anything core framework. Restart required after update.";
    } else {
        const detailStatus = getModuleStatus(mod, storeInstalledModules);
        if (detailStatus.installed && detailStatus.hasUpdate) {
            detailAnnounce += `, version ${detailStatus.installedVersion} to ${mod.latest_version}`;
        } else if (mod.latest_version) {
            detailAnnounce += `, version ${mod.latest_version}`;
        }
        if (mod.description) detailAnnounce += ". " + mod.description;
        if (mod.author) detailAnnounce += ". By " + mod.author;
    }
    announce(detailAnnounce);
}

/* Handle selection in store picker detail */
function handleStorePickerDetailSelect() {
    /* Can't click until action is selected */
    if (!storeDetailScrollState || !isActionSelected(storeDetailScrollState)) {
        return;
    }

    const mod = storePickerCurrentModule;
    if (!mod) {
        return;
    }

    setView(VIEWS.STORE_PICKER_LOADING);
    storePickerLoadingTitle = 'Installing';
    storePickerLoadingMessage = mod.name;
    announce("Installing " + mod.name);

    /* Show loading screen before blocking operation */
    drawStorePickerLoading();
    host_flush_display();

    let result;
    if (mod._isHostUpdate) {
        /* Staged core update with verification and backup */
        result = performCoreUpdate(mod);
    } else {
        const installProgress = (phase, name) => {
            storePickerLoadingTitle = phase;
            storePickerLoadingMessage = name;
            drawStorePickerLoading();
            host_flush_display();
        };
        result = sharedInstallModule(mod, storeHostVersion, installProgress);
    }

    if (result.success) {
        storeInstalledModules = scanInstalledModules();
        if (mod._isHostUpdate) {
            /* Core update succeeded - show restart prompt */
            updateRestartFromVersion = storeHostVersion;
            updateRestartToVersion = mod.latest_version;
            view = VIEWS.UPDATE_RESTART;
        } else if (mod.post_install) {
            /* Check for post_install message */
            storePostInstallLines = wrapText(mod.post_install, 18);
            setView(VIEWS.STORE_PICKER_POST_INSTALL);
            announce("Install complete. " + mod.post_install);
        } else {
            storePickerMessage = `Installed ${mod.name}`;
            setView(VIEWS.STORE_PICKER_RESULT);
            announce(storePickerMessage);
        }
    } else {
        storePickerMessage = result.error || 'Install failed';
        setView(VIEWS.STORE_PICKER_RESULT);
        announce(storePickerMessage);
    }

    needsRedraw = true;
}

/* Handle selection in store picker result */
function handleStorePickerResultSelect() {
    /* Return to list */
    setView(VIEWS.STORE_PICKER_LIST);
    storePickerCurrentModule = null;
    needsRedraw = true;
}

/* Handle back in store picker */
function handleStorePickerBack() {
    switch (view) {
        case VIEWS.STORE_PICKER_CATEGORIES:
            /* Return to calling view */
            storePickerFromSettings = false;
            storeCatalog = null;
            storeCategoryItems = [];
            if (storeReturnView === VIEWS.GLOBAL_SETTINGS) {
                storeReturnView = null;
                enterGlobalSettings();
            } else {
                setView(VIEWS.MASTER_FX);
                announce("Settings");
            }
            break;
        case VIEWS.STORE_PICKER_LIST:
            if (storePickerFromSettings) {
                /* Came from category browser - go back to categories */
                buildStoreCategoryItems();
                setView(VIEWS.STORE_PICKER_CATEGORIES);
                if (storeCategoryItems.length > 0) {
                    announce("Module Store, " + storeCategoryItems[storeCategoryIndex].label);
                }
                storePickerCategory = null;
                storePickerModules = [];
            } else if (storePickerFromOvertake) {
                /* Came from overtake menu - rescan and go back there */
                overtakeModules = scanForOvertakeModules();
                setView(VIEWS.OVERTAKE_MENU);
                storePickerFromOvertake = false;
                storeCatalog = null;
                storePickerCategory = null;
                storePickerModules = [];
            } else if (storePickerFromMasterFx) {
                /* Came from master FX module select - rescan and go back */
                MASTER_FX_OPTIONS = scanForAudioFxModules();
                enterMasterFxModuleSelect(selectedMasterFxComponent);
                setView(VIEWS.MASTER_FX);
                storePickerFromMasterFx = false;
                storeCatalog = null;
                storePickerCategory = null;
                storePickerModules = [];
            } else {
                /* Came from component select - rescan modules and go back */
                availableModules = scanModulesForType(CHAIN_COMPONENTS[selectedChainComponent].key);
                setView(VIEWS.COMPONENT_SELECT);
                storeCatalog = null;
                storePickerCategory = null;
                storePickerModules = [];
            }
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            if (storePickerFromSettings && storePickerCurrentModule && storePickerCurrentModule._isHostUpdate) {
                /* Host update detail entered from categories - go back to categories */
                buildStoreCategoryItems();
                setView(VIEWS.STORE_PICKER_CATEGORIES);
                storePickerCurrentModule = null;
                storeDetailScrollState = null;
                if (storeCategoryItems.length > 0) {
                    announce("Module Store, " + storeCategoryItems[storeCategoryIndex].label);
                }
            } else {
                /* Return to list */
                setView(VIEWS.STORE_PICKER_LIST);
                storePickerCurrentModule = null;
                storeDetailScrollState = null;
            }
            break;
        case VIEWS.STORE_PICKER_RESULT:
            if (storePickerFromSettings && !storePickerCategory) {
                /* Result from category-level action (e.g. host update) - go to categories */
                buildStoreCategoryItems();
                setView(VIEWS.STORE_PICKER_CATEGORIES);
                storePickerCurrentModule = null;
                if (storeCategoryItems.length > 0) {
                    announce("Module Store, " + storeCategoryItems[storeCategoryIndex].label);
                }
            } else if (!storePickerFromSettings && storeReturnView === VIEWS.GLOBAL_SETTINGS) {
                /* Result from update-all via Global Settings → Update Prompt */
                storeReturnView = null;
                storePickerCurrentModule = null;
                enterGlobalSettings();
            } else {
                /* Return to list */
                setView(VIEWS.STORE_PICKER_LIST);
                storePickerCurrentModule = null;
            }
            break;
        case VIEWS.STORE_PICKER_POST_INSTALL:
            /* Dismiss post-install, go to result */
            storePickerMessage = `Installed ${storePickerCurrentModule.name}`;
            setView(VIEWS.STORE_PICKER_RESULT);
            announce(storePickerMessage);
            break;
        case VIEWS.STORE_PICKER_LOADING:
            /* Allow cancel during loading - return to where we came from */
            if (storePickerFromSettings) {
                storePickerFromSettings = false;
                storeCatalog = null;
                storeCategoryItems = [];
                if (storeReturnView === VIEWS.GLOBAL_SETTINGS) {
                    storeReturnView = null;
                    enterGlobalSettings();
                } else {
                    setView(VIEWS.MASTER_FX);
                }
            } else if (storePickerFromOvertake) {
                overtakeModules = scanForOvertakeModules();
                setView(VIEWS.OVERTAKE_MENU);
                storePickerFromOvertake = false;
            } else if (storePickerFromMasterFx) {
                MASTER_FX_OPTIONS = scanForAudioFxModules();
                enterMasterFxModuleSelect(selectedMasterFxComponent);
                setView(VIEWS.MASTER_FX);
                storePickerFromMasterFx = false;
            } else {
                availableModules = scanModulesForType(CHAIN_COMPONENTS[selectedChainComponent].key);
                setView(VIEWS.COMPONENT_SELECT);
            }
            storePickerCategory = null;
            storePickerModules = [];
            storeFetchPending = false;
            break;
    }
    needsRedraw = true;
}

/* Enter component module selection view */
function enterComponentSelect(slotIndex, componentIndex) {
    const comp = CHAIN_COMPONENTS[componentIndex];
    if (!comp || comp.key === "settings") return;

    selectedSlot = slotIndex;
    selectedChainComponent = componentIndex;

    /* Scan for available modules of this type */
    availableModules = scanModulesForType(comp.key);
    selectedModuleIndex = 0;

    /* Try to find current module in list */
    const cfg = chainConfigs[slotIndex];
    const current = cfg && cfg[comp.key];
    if (current && current.module) {
        const idx = availableModules.findIndex(m => m.id === current.module);
        if (idx >= 0) selectedModuleIndex = idx;
    }

    setView(VIEWS.COMPONENT_SELECT);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = availableModules[selectedModuleIndex]?.name || "None";
    announce(`Select ${comp.label}, ${moduleName}`);
}

/* Apply the selected module to the component - updates DSP in realtime */
function applyComponentSelection() {
    const comp = CHAIN_COMPONENTS[selectedChainComponent];
    const selected = availableModules[selectedModuleIndex];

    if (!comp || comp.key === "settings") {
        setView(VIEWS.CHAIN_EDIT);
        return;
    }

    /* Check if user selected "[Get more...]" - enter store picker */
    if (selected && selected.id === "__get_more__") {
        enterStorePicker(comp.key);
        return;
    }

    /* Update in-memory config */
    const cfg = chainConfigs[selectedSlot] || createEmptyChainConfig();
    if (selected && selected.id) {
        cfg[comp.key] = { module: selected.id, params: {} };
    } else {
        cfg[comp.key] = null;
    }
    chainConfigs[selectedSlot] = cfg;

    /* Apply to DSP - map component key to param key */
    const moduleId = selected && selected.id ? selected.id : "";
    let paramKey = "";
    switch (comp.key) {
        case "synth":
            paramKey = "synth:module";
            break;
        case "fx1":
            paramKey = "fx1:module";
            break;
        case "fx2":
            paramKey = "fx2:module";
            break;
        case "midiFx":
            paramKey = "midi_fx1:module";
            break;
    }

    if (paramKey) {
        if (typeof host_log === "function") host_log(`applyComponentSelection: slot=${selectedSlot} param=${paramKey} module=${moduleId}`);
        const success = setSlotParam(selectedSlot, paramKey, moduleId);
        if (typeof host_log === "function") host_log(`applyComponentSelection: setSlotParam returned ${success}`);
        if (!success) {
            print(2, 50, "Failed to apply", 1);
        }
    }

    /* Force sync chainConfigs from DSP and reset caches after module change.
     * Without this, the knob overlay can show the old module's name and params
     * because the periodic refreshSlotModuleSignature (every 30 ticks) hasn't
     * run yet to sync the in-memory state with DSP. */
    loadChainConfigFromSlot(selectedSlot);
    lastSlotModuleSignatures[selectedSlot] = getSlotModuleSignature(selectedSlot);
    invalidateKnobContextCache();
    setView(VIEWS.CHAIN_EDIT);
    needsRedraw = true;
}

/* Enter chain settings view */
function enterChainSettings(slotIndex) {
    selectedSlot = slotIndex;
    selectedChainSetting = 0;
    editingChainSettingValue = false;
    setView(VIEWS.CHAIN_SETTINGS);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const setting = SLOT_SETTINGS[0];
    const val = getSlotSettingValue(slotIndex, setting);
    announce(`Chain Settings, ${setting.label}: ${val}`);
}

/* Get current value for a chain setting */
function getChainSettingValue(slot, setting) {
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
    return String(val);
}

/* Adjust a chain setting value */
function adjustChainSetting(slot, setting, delta) {
    if (setting.type === "action") return;

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

    /* MIDI FX */
    if (cfg.midiFx && cfg.midiFx.module) {
        const name = getSlotParam(slot, "midi_fx1:name") || cfg.midiFx.module;
        targets.push({ id: "midi_fx1", name: `MIDI FX: ${name}` });
    }

    /* Synth */
    if (cfg.synth && cfg.synth.module) {
        const name = getSlotParam(slot, "synth:name") || cfg.synth.module;
        targets.push({ id: "synth", name: `Synth: ${name}` });
    }

    /* FX 1 */
    if (cfg.fx1 && cfg.fx1.module) {
        const name = getSlotParam(slot, "fx1:name") || cfg.fx1.module;
        targets.push({ id: "fx1", name: `FX1: ${name}` });
    }

    /* FX 2 */
    if (cfg.fx2 && cfg.fx2.module) {
        const name = getSlotParam(slot, "fx2:name") || cfg.fx2.module;
        targets.push({ id: "fx2", name: `FX2: ${name}` });
    }

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

    /* Add navigation items (levels) and param items */
    if (level.params && Array.isArray(level.params)) {
        for (const p of level.params) {
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
    const comp = CHAIN_COMPONENTS[selectedChainComponent];
    if (!comp || comp.key === "settings") return;

    /* Shift+click always goes to module chooser (for swapping) */
    enterComponentSelect(selectedSlot, selectedChainComponent);
}

/* Enter component edit mode - try hierarchy editor first, then module UI, then preset browser */
function enterComponentEdit(slotIndex, componentKey) {
    debugLog(`enterComponentEdit: slot=${slotIndex}, key=${componentKey}`);
    selectedSlot = slotIndex;
    editingComponentKey = componentKey;

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
    const cfg = chainConfigs[slotIndex];
    const moduleData = cfg && cfg[componentKey];
    const moduleId = moduleData ? moduleData.module : null;

    /* Try to load the module's UI */
    if (moduleId && loadModuleUi(slotIndex, componentKey, moduleId)) {
        /* Module UI loaded successfully */
        setView(VIEWS.COMPONENT_EDIT);
        needsRedraw = true;
        return;
    }

    /* Fall back to simple preset browser */
    const prefix = componentKey === "midiFx" ? "midi_fx1" : componentKey;

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

/* Enter hierarchy-based parameter editor for a component */
function enterHierarchyEditor(slotIndex, componentKey) {
    const hierarchy = getComponentHierarchy(slotIndex, componentKey);
    if (!hierarchy) {
        /* No hierarchy - fall back to simple preset browser */
        enterComponentEditFallback(slotIndex, componentKey);
        return;
    }

    /* Dismiss any active overlay and clear pending knob state */
    hideOverlay();
    pendingHierKnobIndex = -1;
    pendingHierKnobDelta = 0;

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
    hierEditorIsMasterFx = false;
    hierEditorMasterFxSlot = -1;

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
    const prefix = componentKey === "midiFx" ? "midi_fx1" : componentKey;
    const moduleName = getSlotParam(slotIndex, `${prefix}:name`) || componentKey;

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

/* Enter hierarchy-based parameter editor for a Master FX slot */
function enterMasterFxHierarchyEditor(fxSlot) {
    if (fxSlot < 0 || fxSlot >= 4) return;

    const hierarchy = getMasterFxHierarchy(fxSlot);
    if (!hierarchy) {
        /* No hierarchy - just return, module selection is available */
        return;
    }

    /* Dismiss any active overlay and clear pending knob state */
    hideOverlay();
    pendingHierKnobIndex = -1;
    pendingHierKnobDelta = 0;

    /* Set up hierarchy editor for Master FX
     * Use slot 0 (all Master FX params use slot 0)
     * Component key is "master_fx:fxN" so params become "master_fx:fxN:param" */
    const fxKey = `fx${fxSlot + 1}`;
    hierEditorSlot = 0;
    hierEditorComponent = `master_fx:${fxKey}`;
    hierEditorHierarchy = hierarchy;
    hierEditorLevel = hierarchy.modes ? null : "root";
    hierEditorPath = [];
    hierEditorChildIndex = -1;
    hierEditorChildCount = 0;
    hierEditorChildLabel = "";
    hierEditorSelectedIdx = 0;
    hierEditorEditMode = false;
    hierEditorIsMasterFx = true;
    hierEditorMasterFxSlot = fxSlot;

    /* Fetch chain_params metadata for this Master FX slot */
    hierEditorChainParams = getMasterFxChainParams(fxSlot);

    /* Set up param shims for Master FX component */
    setupModuleParamShims(0, `master_fx:${fxKey}`);

    /* Load current level's params and knobs */
    loadHierarchyLevel();

    setView(VIEWS.HIERARCHY_EDITOR);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = getSlotParam(0, `master_fx:${fxKey}:name`) || `FX ${fxSlot + 1}`;
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

/* Load params and knobs for current hierarchy level */
function loadHierarchyLevel() {
    if (!hierEditorHierarchy) return;

    const levels = hierEditorHierarchy.levels;
    const levelDef = hierEditorLevel ? levels[hierEditorLevel] : null;

    if (!levelDef) {
        /* At mode selection level - include swap module here */
        hierEditorParams = [...(hierEditorHierarchy.modes || []), SWAP_MODULE_ACTION];
        hierEditorKnobs = [];
        hierEditorIsPresetLevel = false;
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
        hierEditorPresetEditMode = false;
        hierEditorKnobs = [];
        hierEditorParams = [];
        const label = hierEditorChildLabel || "Child";
        for (let i = 0; i < hierEditorChildCount; i++) {
            hierEditorParams.push({
                isChild: true,
                childIndex: i,
                label: `${label} ${i + 1}`
            });
        }
        return;
    }

    /* Check if this is a preset browser level */
    if (levelDef.list_param && levelDef.count_param) {
        hierEditorIsPresetLevel = true;
        hierEditorIsDynamicItems = false;
        hierEditorPresetEditMode = false;  /* Reset edit mode when entering preset level */
        hierEditorKnobs = levelDef.knobs || [];

        /* Fetch preset count and current preset */
        const prefix = hierEditorComponent;
        const countStr = getSlotParam(hierEditorSlot, `${prefix}:${levelDef.count_param}`);
        hierEditorPresetCount = countStr ? parseInt(countStr) : 0;

        const presetStr = getSlotParam(hierEditorSlot, `${prefix}:${levelDef.list_param}`);
        hierEditorPresetIndex = presetStr ? parseInt(presetStr) : 0;

        /* Fetch preset name */
        const nameParam = levelDef.name_param || "preset_name";
        hierEditorPresetName = getSlotParam(hierEditorSlot, `${prefix}:${nameParam}`) || "";

        /* Also load params for preset edit mode (swap only at top level) */
        hierEditorParams = isTopLevel
            ? [...(levelDef.params || []), SWAP_MODULE_ACTION]
            : (levelDef.params || []);
    } else if (levelDef.items_param) {
        /* Dynamic items level - fetch items from plugin */
        hierEditorIsPresetLevel = false;
        hierEditorIsDynamicItems = true;
        hierEditorPresetEditMode = false;
        hierEditorSelectParam = levelDef.select_param || "";
        hierEditorNavigateTo = levelDef.navigate_to || "";
        hierEditorKnobs = levelDef.knobs || [];

        /* Fetch items list from plugin */
        const prefix = hierEditorComponent;
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
        hierEditorParams = items.map(item => ({
            isDynamicItem: true,
            label: item.label || item.name || `Item ${item.index}`,
            index: item.index
        }));
    } else {
        hierEditorIsPresetLevel = false;
        hierEditorIsDynamicItems = false;
        hierEditorPresetEditMode = false;
        /* Use hierarchy params for scrollable list, knobs for physical mapping */
        hierEditorParams = isTopLevel
            ? [...(levelDef.params || []), SWAP_MODULE_ACTION]
            : (levelDef.params || []);
        hierEditorKnobs = levelDef.knobs || [];
    }
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
    const prefix = hierEditorComponent;
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
    invalidateKnobContextCache();
}

/* Exit hierarchy editor */
function exitHierarchyEditor() {
    /* Clear pending knob state to prevent stale overlays */
    pendingHierKnobIndex = -1;
    pendingHierKnobDelta = 0;

    clearModuleParamShims();

    /* Determine return view based on whether we're editing Master FX */
    const returnToMasterFx = hierEditorIsMasterFx;

    hierEditorSlot = -1;
    hierEditorComponent = "";
    hierEditorHierarchy = null;
    hierEditorChainParams = [];
    hierEditorChildIndex = -1;
    hierEditorChildCount = 0;
    hierEditorChildLabel = "";
    hierEditorIsPresetLevel = false;
    hierEditorPresetEditMode = false;
    hierEditorIsMasterFx = false;
    hierEditorMasterFxSlot = -1;

    view = returnToMasterFx ? VIEWS.MASTER_FX : VIEWS.CHAIN_EDIT;
    needsRedraw = true;
}

/* Get param metadata from chain_params */
function getParamMetadata(key) {
    if (!hierEditorChainParams) return null;
    return hierEditorChainParams.find(p => p.key === key);
}

/* Format a param value for setting (respects type) */
function formatParamForSet(val, meta) {
    if (meta && meta.type === "int") {
        return Math.round(val).toString();
    }
    return val.toFixed(3);
}

/* Format a param value for overlay display (respects type and range) */
function formatParamForOverlay(val, meta) {
    if (meta && meta.type === "int") {
        return Math.round(val).toString();
    }
    /* Enum/bool: show value as-is (string) */
    if (meta && (meta.type === "enum" || meta.type === "bool")) {
        return String(val);
    }
    /* Float: show as percentage if 0-1 range */
    const min = meta && typeof meta.min === "number" ? meta.min : 0;
    const max = meta && typeof meta.max === "number" ? meta.max : 1;
    if (min === 0 && max === 1) {
        return Math.round(val * 100) + "%";
    }
    return val.toFixed(2);
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

/* Adjust selected param value via jog */
function adjustHierSelectedParam(delta) {
    if (hierEditorSelectedIdx >= hierEditorParams.length) return;

    const param = hierEditorParams[hierEditorSelectedIdx];
    if (param && typeof param === "object" && param.isChild) return;
    const key = typeof param === "string" ? param : param.key || param;

    /* Skip special actions */
    if (key === SWAP_MODULE_ACTION) return;
    const fullKey = buildHierarchyParamKey(key);

    const currentVal = getSlotParam(hierEditorSlot, fullKey);
    if (currentVal === null) return;

    const meta = getParamMetadata(key);

    /* Debug: log what we found */
    debugLog(`adjustHierSelectedParam: key=${key}, currentVal=${currentVal}, meta=${JSON.stringify(meta)}, chainParams=${JSON.stringify(hierEditorChainParams)}`);

    /* Handle enum type - cycle through options */
    if (meta && meta.type === "enum" && meta.options && meta.options.length > 0) {
        const currentIndex = meta.options.indexOf(currentVal);
        let newIndex = currentIndex + delta;
        if (newIndex < 0) newIndex = meta.options.length - 1;
        if (newIndex >= meta.options.length) newIndex = 0;
        setSlotParam(hierEditorSlot, fullKey, meta.options[newIndex]);
        return;
    }

    /* Handle numeric types */
    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    /* Get step from metadata - default 1 for int, 0.02 for float */
    const isInt = meta && meta.type === "int";
    const step = meta && meta.step ? meta.step : (isInt ? 1 : 0.02);
    const min = meta && typeof meta.min === "number" ? meta.min : 0;
    const max = meta && typeof meta.max === "number" ? meta.max : 1;

    const newVal = Math.max(min, Math.min(max, num + delta * step));
    setSlotParam(hierEditorSlot, fullKey, formatParamForSet(newVal, meta));
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
            title: `S${hierEditorSlot + 1}: ${pluginName} ${displayName}`
        };
    }

    /* Chain editor with component selected */
    if (view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0 && selectedChainComponent < CHAIN_COMPONENTS.length) {
            const comp = CHAIN_COMPONENTS[selectedChainComponent];
            if (comp && comp.key !== "settings") {
                const prefix = getComponentParamPrefix(comp.key);
                /* MIDI FX uses different param key format than synth/fx */
                const isMidiFx = comp.key === "midiFx";
                const moduleIdKey = isMidiFx ? "midi_fx1_module" : `${prefix}_module`;
                const moduleId = getSlotParam(selectedSlot, moduleIdKey) || "";
                const nameParamKey = isMidiFx ? null : `${prefix}:name`;
                const pluginName = (nameParamKey ? getSlotParam(selectedSlot, nameParamKey) : null) || moduleId || "";
                const hasModule = moduleId && moduleId.length > 0;
                debugLog(`buildKnobContext: slot=${selectedSlot}, comp=${comp.key}, prefix=${prefix}, nameParamKey=${nameParamKey}, pluginName=${pluginName}, hasModule=${hasModule}`);

            /* No module loaded in this slot */
            if (!hasModule) {
                return {
                    slot: selectedSlot,
                    key: null,
                    fullKey: null,
                    meta: null,
                    pluginName: comp.label,
                    displayName: `Knob ${knobIndex + 1}`,
                    title: `S${selectedSlot + 1} ${comp.label}`,
                    noModule: true
                };
            }

            const hierarchy = getComponentHierarchy(selectedSlot, comp.key);
            debugLog(`buildKnobContext: hierarchy=${hierarchy ? JSON.stringify(hierarchy).substring(0, 200) : 'null'}`);
            if (hierarchy && hierarchy.levels) {
                let levelDef = hierarchy.levels.root || hierarchy.levels[Object.keys(hierarchy.levels)[0]];
                debugLog(`buildKnobContext: levelDef=${levelDef ? JSON.stringify(levelDef) : 'null'}, knobIndex=${knobIndex}`);
                /* If root has no knobs but has children, use first child level for knob mapping */
                if (levelDef && (!levelDef.knobs || levelDef.knobs.length === 0) && levelDef.children) {
                    const childLevel = hierarchy.levels[levelDef.children];
                    if (childLevel && childLevel.knobs && childLevel.knobs.length > 0) {
                        levelDef = childLevel;
                    }
                }
                if (levelDef && levelDef.knobs && knobIndex < levelDef.knobs.length) {
                    const key = levelDef.knobs[knobIndex];
                    const fullKey = `${prefix}:${key}`;
                    const chainParams = getComponentChainParams(selectedSlot, comp.key);
                    debugLog(`buildKnobContext: found knob key=${key}, fullKey=${fullKey}, chainParams count=${chainParams.length}`);
                    const meta = chainParams.find(p => p.key === key);
                    const displayName = meta && meta.name ? meta.name : key.replace(/_/g, " ");
                    return {
                        slot: selectedSlot,
                        key,
                        fullKey,
                        meta,
                        pluginName,
                        displayName,
                        title: `S${selectedSlot + 1}: ${pluginName} ${displayName}`
                    };
                }
                debugLog(`buildKnobContext: no knob mapping for knobIndex=${knobIndex}, levelDef.knobs=${levelDef?.knobs ? JSON.stringify(levelDef.knobs) : 'undefined'}`);
            } else {
                debugLog(`buildKnobContext: no hierarchy or no levels`);
            }

            /* Fallback to chain_params if ui_hierarchy is missing */
            if (!hierarchy || !hierarchy.levels) {
                const chainParams = getComponentChainParams(selectedSlot, comp.key);
                if (chainParams && chainParams.length > 0 && knobIndex < chainParams.length) {
                    const param = chainParams[knobIndex];
                    const key = param.key;
                    const fullKey = `${prefix}:${key}`;
                    const displayName = param.name || key.replace(/_/g, " ");
                    return {
                        slot: selectedSlot,
                        key,
                        fullKey,
                        meta: param,
                        pluginName,
                        displayName,
                        title: `S${selectedSlot + 1}: ${pluginName} ${displayName}`
                    };
                }
            }
            /* Component selected but no knob mappings - return generic context */
            return {
                slot: selectedSlot,
                key: null,
                fullKey: null,
                meta: null,
                pluginName,
                displayName: `Knob ${knobIndex + 1}`,
                title: `S${selectedSlot + 1} ${pluginName}`,
                noMapping: true
            };
        }
    }

    /* Master FX view with FX slot selected */
    if (view === VIEWS.MASTER_FX && selectedMasterFxComponent >= 0 && selectedMasterFxComponent < 4) {
        const comp = MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
        if (comp && comp.key !== "settings") {
            const chainParams = getMasterFxChainParams(selectedMasterFxComponent);
            const pluginName = shadow_get_param(0, `master_fx:${comp.key}:name`) || "";
            const hasModule = pluginName && pluginName.length > 0;

            /* No module loaded in this slot */
            if (!hasModule) {
                return {
                    slot: 0,
                    key: null,
                    fullKey: null,
                    meta: null,
                    pluginName: comp.label,
                    displayName: `Knob ${knobIndex + 1}`,
                    title: `MFX ${comp.label}`,
                    noModule: true,
                    isMasterFx: true,
                    masterFxSlot: selectedMasterFxComponent
                };
            }

            /* Try ui_hierarchy first for explicit knob mappings */
            const hierarchy = getMasterFxHierarchy(selectedMasterFxComponent);
            if (hierarchy && hierarchy.levels) {
                let levelDef = hierarchy.levels.root || hierarchy.levels[Object.keys(hierarchy.levels)[0]];
                /* If root has no knobs but has children, use first child level for knob mapping */
                if (levelDef && (!levelDef.knobs || levelDef.knobs.length === 0) && levelDef.children) {
                    const childLevel = hierarchy.levels[levelDef.children];
                    if (childLevel && childLevel.knobs && childLevel.knobs.length > 0) {
                        levelDef = childLevel;
                    }
                }
                if (levelDef && levelDef.knobs && knobIndex < levelDef.knobs.length) {
                    const key = levelDef.knobs[knobIndex];
                    const fullKey = `master_fx:${comp.key}:${key}`;
                    const meta = chainParams.find(p => p.key === key);
                    const displayName = meta && meta.name ? meta.name : key.replace(/_/g, " ");
                    return {
                        slot: 0,  /* Master FX always uses slot 0 for param access */
                        key,
                        fullKey,
                        meta,
                        pluginName,
                        displayName,
                        title: `MFX ${pluginName} ${displayName}`,
                        isMasterFx: true,
                        masterFxSlot: selectedMasterFxComponent
                    };
                }
            }

            /* Fall back to chain_params: map first 8 params to knobs 1-8 */
            if (chainParams && chainParams.length > 0 && knobIndex < chainParams.length) {
                const param = chainParams[knobIndex];
                const key = param.key;
                const fullKey = `master_fx:${comp.key}:${key}`;
                const displayName = param.name || key.replace(/_/g, " ");
                return {
                    slot: 0,
                    key,
                    fullKey,
                    meta: param,
                    pluginName,
                    displayName,
                    title: `MFX ${pluginName} ${displayName}`,
                    isMasterFx: true,
                    masterFxSlot: selectedMasterFxComponent
                };
            }

            /* FX slot selected but no params available */
            return {
                slot: 0,
                key: null,
                fullKey: null,
                meta: null,
                pluginName,
                displayName: `Knob ${knobIndex + 1}`,
                title: `MFX ${pluginName}`,
                noMapping: true,
                isMasterFx: true,
                masterFxSlot: selectedMasterFxComponent
            };
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
    cachedKnobContextsComp = (view === VIEWS.HIERARCHY_EDITOR) ? -1 : selectedChainComponent;
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
    const currentComp = (view === VIEWS.HIERARCHY_EDITOR) ? -1 : selectedChainComponent;
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
            showOverlay(ctx.title, "No Module Selected");
        } else if (ctx.noMapping) {
            /* Show "not mapped" for unmapped knob */
            showOverlay(`Knob ${knobIndex + 1}`, "not mapped");
        } else if (ctx.fullKey) {
            /* Mapped knob - show value */
            let displayVal;
            const isEnum = ctx.meta && (ctx.meta.type === "enum" || ctx.meta.type === "bool");
            if (value !== undefined) {
                /* For enums, show string directly; for numbers, format */
                displayVal = isEnum ? String(value) : formatParamForOverlay(value, ctx.meta);
            } else {
                const currentVal = getSlotParam(ctx.slot, ctx.fullKey);
                /* For enums, show string directly; for numbers, parse and format */
                if (isEnum) {
                    if (isTriggerEnumMeta(ctx.meta)) {
                        displayVal = getTriggerEnumOverlayValue(knobIndex);
                    } else {
                        displayVal = currentVal || "-";
                    }
                } else {
                    const num = parseFloat(currentVal);
                    displayVal = !isNaN(num) ? formatParamForOverlay(num, ctx.meta) : (currentVal || "-");
                }
            }
            showOverlay(ctx.title, displayVal);
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
            showOverlay(ctx.title, "No Module Selected");
            needsRedraw = true;
            return true;
        }
        if (ctx.noMapping || !ctx.fullKey) {
            /* No mapping - show "not mapped" */
            debugLog(`adjustKnobAndShow: noMapping or no fullKey, showing not mapped`);
            showOverlay(`Knob ${knobIndex + 1}`, "not mapped");
            needsRedraw = true;
            return true;
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
 * Process pending hierarchy knob adjustment - called once per tick
 * This does the actual get/set/overlay work, throttled to avoid IPC overload
 */
function processPendingHierKnob() {
    /* Only log when there's actual work to do - reduces spam */
    if (pendingHierKnobIndex >= 0 && pendingHierKnobDelta !== 0) {
        debugLog(`processPendingHierKnob: index=${pendingHierKnobIndex}, delta=${pendingHierKnobDelta}`);
    }
    if (pendingHierKnobIndex < 0 || pendingHierKnobDelta === 0) {
        /* No pending adjustment, but still show overlay if knob active */
        if (pendingHierKnobIndex >= 0) {
            const ctx = getKnobContext(pendingHierKnobIndex);
            if (ctx && ctx.fullKey) {
                const currentVal = getSlotParam(ctx.slot, ctx.fullKey);
                if (currentVal !== null) {
                    /* For enums, pass string directly; for numbers, parse */
                    if (ctx.meta && (ctx.meta.type === "enum" || ctx.meta.type === "bool")) {
                        if (isTriggerEnumMeta(ctx.meta)) {
                            showOverlay(ctx.title, getTriggerEnumOverlayValue(pendingHierKnobIndex));
                        } else {
                            showOverlay(ctx.title, currentVal);
                        }
                    } else {
                        showKnobOverlay(pendingHierKnobIndex, parseFloat(currentVal));
                    }
                }
            }
        }
        return;
    }

    const knobIndex = pendingHierKnobIndex;
    const delta = pendingHierKnobDelta;
    pendingHierKnobDelta = 0;  /* Clear accumulated delta */

    const ctx = getKnobContext(knobIndex);
    debugLog(`processPendingHierKnob: ctx=${ctx ? JSON.stringify({slot: ctx.slot, key: ctx.key, fullKey: ctx.fullKey, noMapping: ctx.noMapping, meta: ctx.meta ? 'present' : 'null'}) : 'null'}`);
    if (!ctx || ctx.noMapping || !ctx.fullKey) return;

    /* Get current value */
    const currentVal = getSlotParam(ctx.slot, ctx.fullKey);
    debugLog(`processPendingHierKnob: currentVal=${currentVal}`);
    if (currentVal === null) return;

    /* Handle enum type - cycle through options (clamp at ends, don't wrap) */
    if (ctx.meta && ctx.meta.type === "enum" && ctx.meta.options && ctx.meta.options.length > 0) {
        if (isTriggerEnumMeta(ctx.meta)) {
            const shouldFire = updateTriggerEnumAccum(knobIndex, delta);
            if (shouldFire) {
                setSlotParam(ctx.slot, ctx.fullKey, "trigger");
                showOverlay(ctx.title, "Triggered");
            } else {
                showOverlay(ctx.title, getTriggerEnumOverlayValue(knobIndex));
            }
            return;
        }

        const currentIndex = ctx.meta.options.indexOf(currentVal);
        let newIndex = currentIndex + (delta > 0 ? 1 : -1);
        /* Clamp at ends instead of wrapping */
        if (newIndex < 0) newIndex = 0;
        if (newIndex >= ctx.meta.options.length) newIndex = ctx.meta.options.length - 1;
        const newVal = ctx.meta.options[newIndex];
        setSlotParam(ctx.slot, ctx.fullKey, newVal);
        showOverlay(ctx.title, newVal);
        return;
    }

    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    /* Calculate step and bounds from metadata */
    const isInt = ctx.meta && ctx.meta.type === "int";
    const defaultStep = isInt ? KNOB_BASE_STEP_INT : KNOB_BASE_STEP_FLOAT;
    const baseStep = ctx.meta && ctx.meta.step ? ctx.meta.step : defaultStep;
    const min = ctx.meta && typeof ctx.meta.min === "number" ? ctx.meta.min : 0;
    const max = ctx.meta && typeof ctx.meta.max === "number" ? ctx.meta.max : 1;

    /* Calculate acceleration based on turn speed */
    const accel = calcKnobAccel(knobIndex, isInt);

    /* Apply accumulated delta with acceleration and clamp */
    const step = baseStep * accel;
    const newVal = Math.max(min, Math.min(max, num + delta * step));

    /* Set the new value */
    setSlotParam(ctx.slot, ctx.fullKey, formatParamForSet(newVal, ctx.meta));

    /* Show overlay with new value */
    showKnobOverlay(knobIndex, newVal);
}

/* Format a value for display in hierarchy editor */
function formatHierDisplayValue(key, val) {
    const meta = getParamMetadata(key);

    /* For enums, always return the raw string value */
    if (meta && meta.type === "enum") {
        return val;
    }

    const num = parseFloat(val);
    if (isNaN(num)) return val;

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

/* Draw the hierarchy-based parameter editor */
function drawHierarchyEditor() {
    clear_screen();

    /* Get plugin info */
    const prefix = getComponentParamPrefix(hierEditorComponent);
    const cfg = chainConfigs[hierEditorSlot] || createEmptyChainConfig();
    const moduleData = cfg && cfg[hierEditorComponent];
    const abbrev = moduleData ? getModuleAbbrev(moduleData.module) : hierEditorComponent.toUpperCase();

    /* Get bank or preset name for header depending on view */
    let headerName;
    if (hierEditorIsPresetLevel && !hierEditorPresetEditMode) {
        /* Preset browser: show bank/soundfont name */
        headerName = getSlotParam(hierEditorSlot, `${prefix}:bank_name`) ||
                     getSlotParam(hierEditorSlot, `${prefix}:name`) || "";
    } else {
        /* Edit mode: show preset name */
        headerName = getSlotParam(hierEditorSlot, `${prefix}:preset_name`) ||
                     getSlotParam(hierEditorSlot, `${prefix}:name`) || "";
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
                const countStr = getSlotParam(hierEditorSlot, `${hierEditorComponent}:${levelDef.count_param}`);
                const newCount = countStr ? parseInt(countStr) : 0;
                if (newCount > 0) {
                    hierEditorPresetCount = newCount;
                    const presetStr = getSlotParam(hierEditorSlot, `${hierEditorComponent}:${levelDef.list_param}`);
                    hierEditorPresetIndex = presetStr ? parseInt(presetStr) : 0;
                    const nameParam = levelDef.name_param || "preset_name";
                    hierEditorPresetName = getSlotParam(hierEditorSlot, `${hierEditorComponent}:${nameParam}`) || "";
                }
            }
        }

        if (hierEditorPresetCount > 0) {
            /* Show preset number */
            const presetNum = `${hierEditorPresetIndex + 1} / ${hierEditorPresetCount}`;
            const numX = Math.floor((SCREEN_WIDTH - presetNum.length * 5) / 2);
            print(numX, centerY - 8, presetNum, 1);

            /* Show preset name */
            const name = truncateText(hierEditorPresetName || "(unnamed)", 22);
            const nameX = Math.floor((SCREEN_WIDTH - name.length * 5) / 2);
            print(nameX, centerY + 4, name, 1);

            /* Draw navigation arrows */
            print(4, centerY - 2, "<", 1);
            print(SCREEN_WIDTH - 10, centerY - 2, ">", 1);
        } else {
            print(4, centerY, "No presets available", 1);
        }

        /* Footer hints - always push to edit (for swap/params) */
        drawFooter({left: "Push: edit", right: "Jog: browse"});
    } else {
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

                /* Only fetch param value if this item is visible on screen */
                let displayVal = "";
                if (idx >= visibleStart && idx < visibleEnd) {
                    const val = getSlotParam(hierEditorSlot, buildHierarchyParamKey(key));
                    displayVal = val !== null ? formatHierDisplayValue(key, val) : "";
                }
                return { label, value: displayVal, key };
            });

            drawMenuList({
                items,
                selectedIndex: hierEditorSelectedIdx,
                listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
                getLabel: (item) => item.label,
                getValue: (item) => item.value,
                valueAlignRight: true,
                editMode: hierEditorEditMode
            });
        }

        /* Footer hints */
        const hint = hierEditorEditMode ? {left: "Push: done", right: "Jog: adjust"} : {left: "Push: edit", right: "Jog: scroll"};
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
    const prefix = editingComponentKey === "midiFx" ? "midi_fx1" : editingComponentKey;
    setSlotParam(selectedSlot, `${prefix}:preset`, String(newPreset));

    /* Update local state */
    editComponentPreset = newPreset;

    /* Fetch new preset name */
    editComponentPresetName = getSlotParam(selectedSlot, `${prefix}:preset_name`) || "";

    /* Announce preset change - name first for easier scrolling */
    const presetName = editComponentPresetName || `Preset ${editComponentPreset + 1}`;
    announce(`${presetName}, Preset ${editComponentPreset + 1} of ${editComponentPresetCount}`);
}

/* Get current value for a slot setting */
function getSlotSettingValue(slot, setting) {
    if (setting.key === "patch") {
        return slots[slot]?.name || "Unknown";
    }
    const val = getSlotParam(slot, setting.key);
    if (val === null) return "-";

    if (setting.key === "slot:volume") {
        const num = parseFloat(val);
        const pct = isNaN(num) ? 0 : Math.round(num * 100);
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
        return `Ch ${ch + 1}`;
    }
    if (setting.key === "slot:receive_channel") {
        const ch = parseInt(val);
        return ch === 0 ? "All" : `Ch ${val}`;
    }
    return val;
}

/* Adjust a slot setting by delta */
function adjustSlotSetting(slot, setting, delta) {
    if (setting.type === "action") return;

    const current = getSlotParam(slot, setting.key);
    let val;

    if (setting.type === "float") {
        val = parseFloat(current) || 0;
        val += delta * setting.step;
    } else {
        val = parseInt(current) || 0;
        val += delta * setting.step;
    }

    /* Clamp to range */
    val = Math.max(setting.min, Math.min(setting.max, val));

    /* Format and set */
    const newVal = setting.type === "float" ? val.toFixed(2) : String(Math.round(val));
    setSlotParam(slot, setting.key, newVal);
}

/* Get Master FX setting current value for display */
function getMasterFxSettingValue(setting) {
    if (setting.key === "master_volume") {
        const val = shadow_get_param(0, "master_fx:volume");
        if (!val) return "100%";
        const num = parseFloat(val);
        return isNaN(num) ? val : `${Math.round(num * 100)}%`;
    }
    if (setting.key === "link_audio_routing") {
        const val = shadow_get_param(0, "master_fx:link_audio_routing");
        return (val === "1") ? "On" : "Off";
    }
    if (setting.key === "resample_bridge") {
        const modeRaw = shadow_get_param(0, "master_fx:resample_bridge");
        const mode = parseResampleBridgeMode(modeRaw);
        return RESAMPLE_BRIDGE_LABEL_BY_MODE[mode] || "Off";
    }
    if (setting.key === "overlay_knobs") {
        const mode = typeof overlay_knobs_get_mode === "function" ? overlay_knobs_get_mode() : 0;
        return ["+Shift", "+Jog Touch", "Off", "Native"][mode] || "+Shift";
    }
    if (setting.key === "display_mirror") {
        return (typeof display_mirror_get === "function" && display_mirror_get()) ? "On" : "Off";
    }
    if (setting.key === "screen_reader_enabled") {
        return (typeof tts_get_enabled === "function" && tts_get_enabled()) ? "On" : "Off";
    }
    if (setting.key === "screen_reader_engine") {
        if (typeof tts_get_engine === "function") {
            const eng = tts_get_engine();
            return eng === "flite" ? "Flite" : "eSpeak-NG";
        }
        return "eSpeak-NG";
    }
    if (setting.key === "screen_reader_speed") {
        if (typeof tts_get_speed === "function") {
            return tts_get_speed().toFixed(1) + "x";
        }
        return "1.0x";
    }
    if (setting.key === "screen_reader_pitch") {
        if (typeof tts_get_pitch === "function") {
            return Math.round(tts_get_pitch()) + " Hz";
        }
        return "110 Hz";
    }
    if (setting.key === "screen_reader_volume") {
        if (typeof tts_get_volume === "function") {
            return tts_get_volume() + "%";
        }
        return "70%";
    }
    if (setting.key === "screen_reader_debounce") {
        if (typeof tts_get_debounce === "function") {
            return tts_get_debounce() + "ms";
        }
        return "300ms";
    }
    if (setting.key === "set_pages_enabled") {
        return (typeof set_pages_get === "function" && set_pages_get()) ? "On" : "Off";
    }
    if (setting.key === "auto_update_check") {
        return autoUpdateCheckEnabled ? "On" : "Off";
    }
    return "-";
}

/* Adjust Master FX setting value by delta */
function adjustMasterFxSetting(setting, delta) {
    if (setting.type === "action") return;

    if (setting.key === "master_volume") {
        let val = parseFloat(shadow_get_param(0, "master_fx:volume") || "1.0");
        val += delta * setting.step;
        val = Math.max(setting.min, Math.min(setting.max, val));
        shadow_set_param(0, "master_fx:volume", val.toFixed(2));
        return;
    }

    if (setting.key === "link_audio_routing") {
        const current = shadow_get_param(0, "master_fx:link_audio_routing");
        const newVal = (current === "1") ? "0" : "1";
        shadow_set_param(0, "master_fx:link_audio_routing", newVal);
        cachedLinkAudioRouting = (newVal === "1");
        saveMasterFxChainConfig();
        return;
    }
    if (setting.key === "resample_bridge") {
        const current = parseResampleBridgeMode(shadow_get_param(0, "master_fx:resample_bridge"));
        const values = (Array.isArray(setting.values) && setting.values.length > 0)
            ? setting.values
            : RESAMPLE_BRIDGE_VALUES;
        let idx = values.indexOf(current);
        if (idx < 0) idx = 0;
        const nextIdx = (idx + (delta > 0 ? 1 : values.length - 1)) % values.length;
        shadow_set_param(0, "master_fx:resample_bridge", String(values[nextIdx]));
        cachedResampleBridgeMode = values[nextIdx];
        saveMasterFxChainConfig();
        return;
    }

    if (setting.key === "overlay_knobs" && typeof overlay_knobs_set_mode === "function") {
        const current = typeof overlay_knobs_get_mode === "function" ? overlay_knobs_get_mode() : 0;
        const count = setting.values.length;
        const next = ((current + (delta > 0 ? 1 : count - 1)) % count);
        overlay_knobs_set_mode(next);
        saveMasterFxChainConfig();
        return;
    }

    if (setting.key === "display_mirror" && typeof display_mirror_set === "function") {
        /* Toggle boolean */
        const current = typeof display_mirror_get === "function" ? display_mirror_get() : false;
        display_mirror_set(!current ? 1 : 0);
        return;
    }

    if (setting.key === "screen_reader_enabled" && typeof tts_set_enabled === "function") {
        /* Toggle boolean */
        const current = typeof tts_get_enabled === "function" ? tts_get_enabled() : true;
        tts_set_enabled(!current);
        return;
    }

    if (setting.key === "screen_reader_engine" && typeof tts_set_engine === "function") {
        const current = typeof tts_get_engine === "function" ? tts_get_engine() : "espeak";
        const values = setting.values;
        let idx = values.indexOf(current);
        if (idx < 0) idx = 0;
        const nextIdx = (idx + (delta > 0 ? 1 : values.length - 1)) % values.length;
        tts_set_engine(values[nextIdx]);
        return;
    }

    if (setting.key === "screen_reader_speed" && typeof tts_set_speed === "function") {
        let val = typeof tts_get_speed === "function" ? tts_get_speed() : 1.0;
        val += delta * setting.step;
        val = Math.max(setting.min, Math.min(setting.max, val));
        tts_set_speed(val);
        return;
    }

    if (setting.key === "screen_reader_pitch" && typeof tts_set_pitch === "function") {
        let val = typeof tts_get_pitch === "function" ? tts_get_pitch() : 110.0;
        val += delta * setting.step;
        val = Math.max(setting.min, Math.min(setting.max, val));
        tts_set_pitch(val);
        return;
    }

    if (setting.key === "screen_reader_volume" && typeof tts_set_volume === "function") {
        let val = typeof tts_get_volume === "function" ? tts_get_volume() : 70;
        val += delta * setting.step;
        val = Math.max(setting.min, Math.min(setting.max, val));
        tts_set_volume(Math.round(val));
        return;
    }

    if (setting.key === "screen_reader_debounce" && typeof tts_set_debounce === "function") {
        let val = typeof tts_get_debounce === "function" ? tts_get_debounce() : 300;
        val += delta * setting.step;
        val = Math.max(setting.min, Math.min(setting.max, val));
        tts_set_debounce(Math.round(val));
        return;
    }

    if (setting.key === "set_pages_enabled" && typeof set_pages_set === "function") {
        const current = typeof set_pages_get === "function" ? set_pages_get() : true;
        set_pages_set(!current ? 1 : 0);
        return;
    }

    if (setting.key === "auto_update_check") {
        autoUpdateCheckEnabled = !autoUpdateCheckEnabled;
        saveAutoUpdateConfig();
        return;
    }
}

/* Update the focused slot in shared memory for knob CC routing */
function updateFocusedSlot(slot) {
    if (typeof shadow_set_focused_slot === "function") {
        shadow_set_focused_slot(slot);
    }

    /* Check for synth errors when selecting a chain slot (not Master FX) */
    if (slot >= 0 && slot < SHADOW_UI_SLOTS && !assetWarningShownForSlots.has(slot)) {
        const synthModule = getSlotParam(slot, "synth_module");
        if (synthModule && synthModule.length > 0) {
            /* Slot has a synth - check for errors */
            if (checkAndShowSynthError(slot)) {
                assetWarningShownForSlots.add(slot);
            }
        }
    }

    /* Check for Master FX errors when selecting the Master FX slot (slot 4) */
    if (slot === SHADOW_UI_SLOTS) {  /* Slot 4 = Master FX */
        for (let fx = 0; fx < 4; fx++) {
            if (assetWarningShownForMasterFx.has(fx)) continue;
            const fxModule = getMasterFxParam(fx, "module");
            if (fxModule && fxModule.length > 0) {
                /* FX slot has a module loaded - check for errors */
                if (checkAndShowMasterFxError(fx)) {
                    assetWarningShownForMasterFx.add(fx);
                    break;  /* Show one warning at a time */
                }
            }
        }
    }
}

function handleJog(delta) {
    hideOverlay();
    switch (view) {
        case VIEWS.SLOTS:
            /* 5 items: 4 slots + Master FX */
            selectedSlot = Math.max(0, Math.min(slots.length, selectedSlot + delta));
            /* Update focused slot: 0-3 for chain slots, 4 for Master FX */
            updateFocusedSlot(selectedSlot);
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
                /* Navigate module list */
                selectedMasterFxModuleIndex = Math.max(0, Math.min(MASTER_FX_OPTIONS.length - 1, selectedMasterFxModuleIndex + delta));
                const module = MASTER_FX_OPTIONS[selectedMasterFxModuleIndex];
                announceMenuItem("Module", module.name);
            } else {
                /* Navigate chain components (-1 = preset selection, like instrument slots)
                 * Preset picker is only accessible via click, not scroll */
                selectedMasterFxComponent = Math.max(-1, Math.min(MASTER_FX_CHAIN_COMPONENTS.length - 1, selectedMasterFxComponent + delta));
                if (selectedMasterFxComponent === -1) {
                    announce("Preset Selection");
                } else {
                    const comp = MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
                    const compKey = comp.key;
                    const moduleName = masterFxConfig?.[compKey]?.module || "Empty";
                    announceMenuItem(comp.label, moduleName);
                }
            }
            break;
        case VIEWS.SLOT_SETTINGS:
            if (editingSettingValue) {
                /* Adjust the setting value */
                const setting = SLOT_SETTINGS[selectedSetting];
                adjustSlotSetting(selectedSlot, setting, delta);
                /* Announce new value */
                const newVal = getSlotSettingValue(selectedSlot, setting);
                announceParameter(setting.label, newVal);
            } else {
                /* Navigate settings list */
                selectedSetting = Math.max(0, Math.min(SLOT_SETTINGS.length - 1, selectedSetting + delta));
                /* Announce selected setting */
                const setting = SLOT_SETTINGS[selectedSetting];
                const val = getSlotSettingValue(selectedSlot, setting);
                announceMenuItem(setting.label, val);
            }
            break;
        case VIEWS.PATCHES:
            selectedPatch = Math.max(0, Math.min(patches.length - 1, selectedPatch + delta));
            if (patches.length > 0) {
                const p = patches[selectedPatch];
                announceMenuItem("Patch", p.name || p);
            }
            break;
        case VIEWS.PATCH_DETAIL:
            const detailItems = getDetailItems();
            selectedDetailItem = Math.max(0, Math.min(detailItems.length - 1, selectedDetailItem + delta));
            if (detailItems.length > 0) {
                const di = detailItems[selectedDetailItem];
                announceMenuItem(di.label || di.component || "Item", di.value || "");
            }
            break;
        case VIEWS.COMPONENT_PARAMS:
            if (editingValue && componentParams.length > 0) {
                /* Adjusting value - modify the current param */
                const param = componentParams[selectedParam];
                const newVal = adjustParamValue(param, delta);
                param.value = newVal;
                /* Apply immediately */
                setSlotParam(selectedSlot, param.key, newVal);
                announceParameter(param.name || param.key, newVal);
            } else {
                /* Selecting param */
                selectedParam = Math.max(0, Math.min(componentParams.length - 1, selectedParam + delta));
                if (componentParams.length > 0) {
                    const cp = componentParams[selectedParam];
                    announceMenuItem(cp.name || cp.key, cp.value || "");
                }
            }
            break;
        case VIEWS.CHAIN_EDIT:
            /* Navigate horizontally through chain components (-1 = chain/patch selection) */
            selectedChainComponent = Math.max(-1, Math.min(CHAIN_COMPONENTS.length - 1, selectedChainComponent + delta));
            /* Announce component */
            if (selectedChainComponent === -1) {
                announce("Patch Selection");
            } else {
                const comp = CHAIN_COMPONENTS[selectedChainComponent];
                const compKey = comp.key;
                const moduleName = chainConfigs[selectedSlot]?.[compKey]?.module || "Empty";
                announceMenuItem(comp.label, moduleName);
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
        case VIEWS.STORE_PICKER_CATEGORIES:
            handleStoreCategoryJog(delta);
            break;
        case VIEWS.STORE_PICKER_LIST:
            handleStorePickerListJog(delta);
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            handleStorePickerDetailJog(delta);
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
                    const freshVal = getSlotParam(hierEditorSlot, fullKey);
                    const displayVal = freshVal !== null ? formatHierDisplayValue(key, freshVal) : "";
                    announceParameter(param.label || key, displayVal);
                }
            } else {
                /* Scroll param list (includes preset edit mode) */
                hierEditorSelectedIdx = Math.max(0, Math.min(hierEditorParams.length - 1, hierEditorSelectedIdx + delta));
                /* Announce selected parameter */
                if (hierEditorParams.length > 0 && hierEditorSelectedIdx >= 0 && hierEditorSelectedIdx < hierEditorParams.length) {
                    const param = hierEditorParams[hierEditorSelectedIdx];
                    announceMenuItem(param.label || param.key, param.value || "");
                }
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
        case VIEWS.UPDATE_PROMPT:
            /* +1 for the "Update All" item at the end */
            pendingUpdateIndex = Math.max(0, Math.min(pendingUpdates.length, pendingUpdateIndex + delta));
            if (pendingUpdateIndex === pendingUpdates.length) {
                announce("Update All");
            } else {
                const upd = pendingUpdates[pendingUpdateIndex];
                announce(upd.name + ", " + upd.from + " to " + upd.to);
            }
            break;
        case VIEWS.UPDATE_DETAIL:
            if (updateDetailScrollState) {
                handleScrollableTextJog(updateDetailScrollState, delta);
            }
            break;
        case VIEWS.UPDATE_RESTART:
            /* No jog navigation needed */
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
        case VIEWS.OVERTAKE_MODULE:
            /* Overtake module handles its own jog input */
            break;
        case VIEWS.GLOBAL_SETTINGS:
            if (helpDetailScrollState) {
                handleScrollableTextJog(helpDetailScrollState, delta);
            } else if (helpNavStack.length > 0) {
                const frame = helpNavStack[helpNavStack.length - 1];
                frame.selectedIndex = Math.max(0, Math.min(frame.items.length - 1, frame.selectedIndex + delta));
                announce(frame.items[frame.selectedIndex].title);
            } else if (globalSettingsInSection) {
                const section = GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex];
                if (globalSettingsEditing) {
                    /* Adjust value with jog */
                    const item = section.items[globalSettingsItemIndex];
                    adjustMasterFxSetting(item, delta);
                    const newVal = getMasterFxSettingValue(item);
                    announceParameter(item.label, newVal);
                } else {
                    /* Navigate items within section */
                    globalSettingsItemIndex = Math.max(0, Math.min(section.items.length - 1, globalSettingsItemIndex + delta));
                    const item = section.items[globalSettingsItemIndex];
                    const value = item.type === "action" ? "" : getMasterFxSettingValue(item);
                    announceMenuItem(item.label, value);
                }
            } else {
                /* Navigate sections at top level */
                globalSettingsSectionIndex = Math.max(0, Math.min(GLOBAL_SETTINGS_SECTIONS.length - 1, globalSettingsSectionIndex + delta));
                announce(GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex].label);
            }
            break;
    }
    needsRedraw = true;
}

function handleSelect() {
    hideOverlay();
    switch (view) {
        case VIEWS.SLOTS:
            if (selectedSlot < slots.length) {
                /* Go directly to chain editor */
                enterChainEdit(selectedSlot);
            } else {
                /* Master FX selected */
                enterMasterFxSettings();
            }
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
                const selectedComp = MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
                if (selectedComp.key === "settings") {
                    /* Enter settings submenu */
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
            const setting = SLOT_SETTINGS[selectedSetting];
            if (setting.type === "action") {
                if (setting.key === "patch") {
                    /* Patch action - go to patch browser */
                    enterPatchBrowser(selectedSlot);
                } else if (setting.key === "chain") {
                    /* Chain action - go to chain editor */
                    enterChainEdit(selectedSlot);
                }
            } else {
                /* Toggle editing mode for value settings */
                editingSettingValue = !editingSettingValue;
            }
            break;
        case VIEWS.PATCHES:
            if (patches.length > 0) {
                /* Load patch and return to chain edit */
                applyPatchSelection();
                /* Refresh chain config to show newly loaded synth/FX */
                loadChainConfigFromSlot(selectedSlot);
                setView(VIEWS.CHAIN_EDIT);
                needsRedraw = true;
            }
            break;
        case VIEWS.PATCH_DETAIL:
            const detailItems = getDetailItems();
            const item = detailItems[selectedDetailItem];
            if (item.component && item.editable) {
                /* Enter component param editor */
                enterComponentParams(selectedSlot, item.component);
            } else if (selectedDetailItem === detailItems.length - 1) {
                /* "Load Patch" selected - apply and return to chain edit */
                applyPatchSelection();
                /* Refresh chain config to show newly loaded synth/FX */
                loadChainConfigFromSlot(selectedSlot);
                setView(VIEWS.CHAIN_EDIT);
                announce("Patch loaded");
                needsRedraw = true;
            }
            break;
        case VIEWS.COMPONENT_PARAMS:
            if (componentParams.length > 0) {
                /* Toggle between selecting and editing */
                editingValue = !editingValue;
                const cp = componentParams[selectedParam];
                if (editingValue) {
                    announceParameter(cp.name || cp.key, cp.value || "");
                } else {
                    announce("Done editing");
                }
            }
            break;
        case VIEWS.CHAIN_EDIT:
            if (selectedChainComponent === -1) {
                /* Chain selected - open patch browser */
                enterPatchBrowser(selectedSlot);
            } else if (selectedChainComponent === CHAIN_COMPONENTS.length - 1) {
                /* Settings selected - go to chain settings */
                enterChainSettings(selectedSlot);
            } else {
                /* Component selected - check if populated or empty */
                const comp = CHAIN_COMPONENTS[selectedChainComponent];
                const cfg = chainConfigs[selectedSlot];
                const moduleData = cfg && cfg[comp.key];

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
        case VIEWS.STORE_PICKER_CATEGORIES:
            handleStoreCategorySelect();
            break;
        case VIEWS.STORE_PICKER_LIST:
            handleStorePickerListSelect();
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            handleStorePickerDetailSelect();
            break;
        case VIEWS.STORE_PICKER_RESULT:
            handleStorePickerResultSelect();
            break;
        case VIEWS.STORE_PICKER_POST_INSTALL:
            /* Dismiss post-install, go to result */
            storePickerMessage = `Installed ${storePickerCurrentModule.name}`;
            setView(VIEWS.STORE_PICKER_RESULT);
            announce(storePickerMessage);
            needsRedraw = true;
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
                    if (setting.key === "knobs") {
                        enterKnobEditor(selectedSlot);
                    } else if (setting.key === "save") {
                        /* Start save flow */
                        const currentName = slots[selectedSlot] ? slots[selectedSlot].name : "";
                        if (!currentName || currentName === "" || currentName === "Untitled") {
                            /* New - show name preview with Edit/OK */
                            pendingSaveName = generateSlotPresetName(selectedSlot);
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
                    } else if (setting.key === "save_as") {
                        /* Save As - show name preview with Edit/OK */
                        const currentName = slots[selectedSlot] ? slots[selectedSlot].name : "";
                        pendingSaveName = currentName && currentName !== "" && currentName !== "Untitled"
                            ? currentName
                            : generateSlotPresetName(selectedSlot);
                        showingNamePreview = true;
                        namePreviewIndex = 1;  /* Default to OK */
                        overwriteFromKeyboard = true;  /* Will use keyboard if Edit is selected */
                        announceSavePreview(pendingSaveName, namePreviewIndex);
                        needsRedraw = true;
                    } else if (setting.key === "delete") {
                        if (isExistingPreset(selectedSlot)) {
                            confirmingDelete = true;
                            confirmIndex = 0;
                            const patchName = slots[selectedSlot]?.name || "patch";
                            announce(`Delete ${patchName}?`);
                            needsRedraw = true;
                        }
                    }
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
                    const compIndex = CHAIN_COMPONENTS.findIndex(c => c.key === hierEditorComponent);
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
                        const prefix = hierEditorComponent;
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
                    loadHierarchyLevel();
                    invalidateKnobContextCache();
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
                        const compIndex = CHAIN_COMPONENTS.findIndex(c => c.key === hierEditorComponent);
                        const slotToSwap = hierEditorSlot;  /* Save before exit clears it */
                        if (compIndex >= 0) {
                            exitHierarchyEditor();
                            enterComponentSelect(slotToSwap, compIndex);
                        }
                    }
                } else {
                    /* Normal param - toggle edit mode */
                    hierEditorEditMode = !hierEditorEditMode;
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
        case VIEWS.UPDATE_PROMPT:
            if (pendingUpdateIndex === pendingUpdates.length) {
                /* "Update All" - install directly */
                announce("Installing all updates");
                processAllUpdates();
            } else if (pendingUpdateIndex >= 0 && pendingUpdateIndex < pendingUpdates.length) {
                const upd = pendingUpdates[pendingUpdateIndex];
                updateDetailModule = upd;

                announce("Loading details for " + upd.name);
                const repo = upd._isHostUpdate ? 'charlesvestal/move-anything' : upd.github_repo;
                const notes = repo ? fetchReleaseNotes(repo) : null;

                const lines = [];
                lines.push(upd.name);
                lines.push(upd.from + ' -> ' + upd.to);
                lines.push('');
                if (notes) {
                    lines.push(...buildReleaseNoteLines(notes));
                } else {
                    lines.push('No release notes');
                    lines.push('available.');
                }

                updateDetailScrollState = createScrollableText({
                    lines: lines,
                    actionLabel: 'Install',
                    visibleLines: 3
                });

                view = VIEWS.UPDATE_DETAIL;
                needsRedraw = true;
                announce(upd.name + ", " + upd.from + " to " + upd.to + ". Click to install");
            }
            break;
        case VIEWS.UPDATE_DETAIL:
            if (updateDetailScrollState && isActionSelected(updateDetailScrollState)) {
                const upd = updateDetailModule;
                announce("Installing " + upd.name);
                if (upd._isHostUpdate) {
                    const result = performCoreUpdate(upd);
                    if (result.success) {
                        pendingUpdates.splice(pendingUpdateIndex, 1);
                        if (pendingUpdateIndex >= pendingUpdates.length) {
                            pendingUpdateIndex = Math.max(0, pendingUpdates.length - 1);
                        }
                        updateRestartFromVersion = upd.from;
                        updateRestartToVersion = upd.to;
                        view = VIEWS.UPDATE_RESTART;
                        announce("Core updated to " + upd.to + ". Click to restart now, Back to restart later");
                    } else {
                        storePickerResultTitle = 'Updates';
                        storePickerMessage = result.error || 'Core update failed';
                        view = VIEWS.STORE_PICKER_RESULT;
                        announce(storePickerMessage);
                    }
                } else {
                    const result = sharedInstallModule(upd, storeHostVersion);
                    pendingUpdates.splice(pendingUpdateIndex, 1);
                    if (pendingUpdateIndex >= pendingUpdates.length) {
                        pendingUpdateIndex = Math.max(0, pendingUpdates.length - 1);
                    }
                    storeInstalledModules = scanInstalledModules();
                    storePickerResultTitle = 'Updates';
                    if (result.success) {
                        storePickerMessage = 'Updated ' + upd.name;
                    } else {
                        storePickerMessage = result.error || 'Update failed';
                    }
                    view = VIEWS.STORE_PICKER_RESULT;
                    announce(storePickerMessage);
                }
                needsRedraw = true;
            }
            break;
        case VIEWS.UPDATE_RESTART:
            /* Show restarting screen, then restart */
            clear_screen();
            drawStatusOverlay('Restarting', 'Please wait...');
            host_flush_display();
            if (typeof shadow_control_restart === "function") {
                shadow_control_restart();
            }
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
        case VIEWS.OVERTAKE_MODULE:
            /* Overtake module handles its own select input */
            break;
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
            } else if (globalSettingsInSection) {
                const section = GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex];
                const item = section.items[globalSettingsItemIndex];
                if (globalSettingsEditing) {
                    /* Exit editing */
                    globalSettingsEditing = false;
                } else if (item.type === "action") {
                    handleGlobalSettingsAction(item.key);
                } else if (item.type === "bool" || item.type === "enum") {
                    adjustMasterFxSetting(item, 1);
                    const newVal = getMasterFxSettingValue(item);
                    announceParameter(item.label, newVal);
                } else if (item.type === "float" || item.type === "int") {
                    globalSettingsEditing = !globalSettingsEditing;
                }
            } else {
                const section = GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex];
                if (section.isAction) {
                    /* Direct action section (Help) */
                    handleGlobalSettingsAction(section.id);
                } else {
                    /* Enter section */
                    globalSettingsInSection = true;
                    globalSettingsItemIndex = 0;
                    const firstItem = section.items[0];
                    const value = firstItem.type === "action" ? "" : getMasterFxSettingValue(firstItem);
                    announce(section.label + ", " + firstItem.label + (value ? ": " + value : ""));
                }
            }
            break;
    }
    needsRedraw = true;
}

function handleBack() {
    hideOverlay();
    switch (view) {
        case VIEWS.SLOTS:
            /* At root level - exit shadow mode and return to Move */
            if (typeof shadow_request_exit === "function") {
                shadow_request_exit();
            }
            break;
        case VIEWS.SLOT_SETTINGS:
            if (editingSettingValue) {
                /* Exit value editing mode */
                editingSettingValue = false;
                needsRedraw = true;
                announce("Slot Settings");
            } else {
                /* Return to slots list */
                setView(VIEWS.SLOTS);
                announce("Slots");
                needsRedraw = true;
            }
            break;
        case VIEWS.PATCHES:
            /* Return to chain editor */
            setView(VIEWS.CHAIN_EDIT);
            announce("Chain Editor");
            needsRedraw = true;
            break;
        case VIEWS.PATCH_DETAIL:
            setView(VIEWS.PATCHES);
            announce("Patch Browser");
            needsRedraw = true;
            break;
        case VIEWS.COMPONENT_PARAMS:
            if (editingValue) {
                /* Exit value editing mode */
                editingValue = false;
                needsRedraw = true;
                announce("Parameters");
            } else {
                /* Return to patch detail, refresh info */
                fetchPatchDetail(selectedSlot);
                setView(VIEWS.PATCH_DETAIL);
                announce("Patch Detail");
                needsRedraw = true;
            }
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
                /* Cancel module selection, return to chain view */
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
            /* Return to chain edit */
            setView(VIEWS.CHAIN_EDIT);
            announce("Chain Editor");
            needsRedraw = true;
            break;
        case VIEWS.STORE_PICKER_CATEGORIES:
        case VIEWS.STORE_PICKER_LIST:
        case VIEWS.STORE_PICKER_DETAIL:
        case VIEWS.STORE_PICKER_RESULT:
        case VIEWS.STORE_PICKER_POST_INSTALL:
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
        case VIEWS.HIERARCHY_EDITOR: {
            /* Helper: announce current hierarchy level label after navigation */
            const announceHierLevel = () => {
                const ld = getHierarchyLevelDef();
                announce(ld && ld.label ? ld.label : "Parameters");
            };
            if (hierEditorEditMode) {
                /* Exit param edit mode first */
                hierEditorEditMode = false;
                needsRedraw = true;
                announceHierLevel();
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
        case VIEWS.UPDATE_PROMPT:
            /* Skip updates - dismiss and return */
            pendingUpdates = [];
            if (storePickerFromSettings && storeCatalog) {
                /* Came from Module Store categories */
                buildStoreCategoryItems();
                view = VIEWS.STORE_PICKER_CATEGORIES;
            } else if (storeReturnView === VIEWS.GLOBAL_SETTINGS) {
                storeReturnView = null;
                enterGlobalSettings();
            } else {
                /* Startup auto-update or unknown origin — exit shadow mode */
                if (typeof shadow_request_exit === "function") {
                    shadow_request_exit();
                }
            }
            needsRedraw = true;
            break;
        case VIEWS.UPDATE_DETAIL:
            updateDetailScrollState = null;
            updateDetailModule = null;
            view = VIEWS.UPDATE_PROMPT;
            needsRedraw = true;
            if (pendingUpdates.length > 0) {
                const upd = pendingUpdates[pendingUpdateIndex];
                announce("Updates, " + upd.name);
            }
            break;
        case VIEWS.UPDATE_RESTART:
            /* Restart later - return to calling view */
            if (storeReturnView === VIEWS.GLOBAL_SETTINGS) {
                storeReturnView = null;
                enterGlobalSettings();
            } else {
                /* Exit shadow mode */
                if (typeof shadow_request_exit === "function") {
                    shadow_request_exit();
                }
            }
            needsRedraw = true;
            break;
        case VIEWS.OVERTAKE_MENU:
            /* Exit overtake menu and return to Move */
            debugLog("OVERTAKE_MENU back: exiting to Move");
            if (typeof shadow_set_overtake_mode === "function") {
                shadow_set_overtake_mode(0);
            }
            if (typeof shadow_request_exit === "function") {
                shadow_request_exit();
            }
            needsRedraw = true;
            break;
        case VIEWS.OVERTAKE_MODULE:
            /* Overtake module handles its own back input.
             * Use Shift+Vol+Jog Click to exit overtake mode. */
            break;
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
                } else {
                    announce("Settings, " + GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex].label);
                }
            } else if (globalSettingsEditing) {
                /* Exit editing mode */
                globalSettingsEditing = false;
                needsRedraw = true;
                const section = GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex];
                const item = section.items[globalSettingsItemIndex];
                const val = getMasterFxSettingValue(item);
                announce(item.label + ", " + val);
            } else if (globalSettingsInSection) {
                /* Return to section list */
                globalSettingsInSection = false;
                needsRedraw = true;
                announce("Settings, " + GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex].label);
            } else {
                /* Exit Global Settings → exit shadow mode */
                if (typeof shadow_request_exit === "function") {
                    shadow_request_exit();
                }
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

    /* Show overlay using shared overlay system */
    const mapping = knobMappings[pendingKnobIndex];
    if (mapping && mapping.name) {
        const displayName = `S${targetSlot + 1}: ${mapping.name}`;
        showOverlay(displayName, mapping.value);
    } else {
        /* No mapping for this knob */
        showOverlay(`Knob ${pendingKnobIndex + 1}`, "not mapped");
    }

    pendingKnobRefresh = false;
    pendingKnobIndex = -1;
}

function drawSlots() {
    clear_screen();
    drawHeader("Shadow Chains");

    /* Get the track-selected slot (for playback/knobs, set by track buttons) */
    let trackSelectedSlot = 0;
    if (typeof shadow_get_selected_slot === "function") {
        trackSelectedSlot = shadow_get_selected_slot();
    }

    /* Create items list: 4 slots + Master FX
     * Show asterisk (*) before patch name for track-selected slot (playing/knob control)
     * Use leading space for non-selected to maintain alignment */
    const items = [
        ...slots.map((s, i) => {
            const muted = getSlotParam(i, "slot:muted") === "1";
            const soloed = getSlotParam(i, "slot:soloed") === "1";
            const flags = (muted ? "M" : "") + (soloed ? "S" : "");
            const prefix = (i === trackSelectedSlot ? "*" : " ") + (slotDirtyCache[i] ? "*" : "");
            return {
                label: prefix + (s.name || "Unknown Patch"),
                value: flags || (s.channel === 0 ? "All" : `Ch${s.channel}`),
                isSlot: true
            };
        }),
        { label: " Master FX", value: getMasterFxDisplayName(), isSlot: false }
    ];

    drawMenuList({
        items,
        selectedIndex: selectedSlot,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        getLabel: (item) => item.label,
        getValue: (item) => item.value,
        valueAlignRight: true
    });
    /* Debug: show flags value in footer */
    const debugInfo = typeof globalThis._debugFlags !== "undefined"
        ? `F:${globalThis._debugFlags}` : "";
    /* Also show current shift/vol state if available */
    let stateInfo = "";
    if (typeof shadow_get_debug_state === "function") {
        stateInfo = shadow_get_debug_state();
    }
    drawFooter(`${debugInfo} ${stateInfo}`);
}

function getMasterFxDisplayName() {
    const opt = MASTER_FX_OPTIONS.find(o => o.id === currentMasterFxId);
    return opt ? opt.name : "None";
}

function drawSlotSettings() {
    clear_screen();
    const slotName = slots[selectedSlot]?.name || "Unknown";
    drawHeader(`Slot ${selectedSlot + 1}`);

    const listY = LIST_TOP_Y;
    const lineHeight = LIST_LINE_HEIGHT;

    /* Calculate visible items accounting for footer */
    const maxVisible = Math.max(1, Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / lineHeight));
    let startIdx = 0;
    const maxSelectedRow = maxVisible - 1;
    if (selectedSetting > maxSelectedRow) {
        startIdx = selectedSetting - maxSelectedRow;
    }
    const endIdx = Math.min(startIdx + maxVisible, SLOT_SETTINGS.length);

    for (let i = startIdx; i < endIdx; i++) {
        const y = listY + (i - startIdx) * lineHeight;
        const setting = SLOT_SETTINGS[i];
        const isSelected = i === selectedSetting;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }

        const color = isSelected ? 0 : 1;
        let prefix = "  ";
        if (isSelected) {
            prefix = editingSettingValue ? "* " : "> ";
        }

        const value = getSlotSettingValue(selectedSlot, setting);
        let valueStr = truncateText(value, 10);
        if (isSelected && editingSettingValue && setting.type !== "action") {
            valueStr = `[${valueStr}]`;
        }

        print(LIST_LABEL_X, y, `${prefix}${setting.label}:`, color);
        print(LIST_VALUE_X - 8, y, valueStr, color);
    }

    if (editingSettingValue) {
        drawFooter({left: "Click: done", right: "Jog: adjust"});
    } else {
        drawFooter({left: "Back: slots", right: "Click: edit"});
    }
}

function drawPatches() {
    clear_screen();
    const rawCh = slots[selectedSlot]?.channel;
    const channel = (typeof rawCh === "number") ? rawCh : (DEFAULT_SLOTS[selectedSlot]?.channel ?? 1 + selectedSlot);
    drawHeader(`${channel === 0 ? "All" : "Ch" + channel} Patch`);
    if (patches.length === 0) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No patches found", 1);
        drawFooter("Back: settings");
    } else {
        const loadedName = slots[selectedSlot]?.name;
        drawMenuList({
            items: patches,
            selectedIndex: selectedPatch,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => {
                const isCurrent = loadedName && item.name === loadedName;
                return isCurrent ? `* ${item.name}` : item.name;
            }
        });
        drawFooter({left: "Back: settings", right: "Click: load"});
    }
}

function drawPatchDetail() {
    clear_screen();
    const patch = patches[selectedPatch];
    const patchName = patch ? patch.name : "Unknown";
    drawHeader(truncateText(patchName, 18));

    const items = getDetailItems();
    const listY = LIST_TOP_Y;
    const lineHeight = 12;

    for (let i = 0; i < items.length; i++) {
        const y = listY + i * lineHeight;
        const item = items[i];
        const isSelected = i === selectedDetailItem;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, lineHeight, 1);
        }

        const color = isSelected ? 0 : 1;
        const prefix = isSelected ? "> " : "  ";

        if (item.value) {
            /* Label: Value (subvalue) format */
            print(LIST_LABEL_X, y, `${prefix}${item.label}:`, color);
            let valueStr = item.value;
            if (item.subvalue && item.subvalue !== "-") {
                valueStr = truncateText(valueStr, 8);
                print(LIST_VALUE_X - 24, y, valueStr, color);
                print(LIST_VALUE_X + 4, y, `(${item.subvalue})`, color);
            } else {
                print(LIST_VALUE_X - 24, y, truncateText(valueStr, 12), color);
            }
        } else {
            /* Just label (for "Load Patch") */
            print(LIST_LABEL_X, y, `${prefix}${item.label}`, color);
        }
    }

    drawFooter({left: "Back: list", right: "Click: edit"});
}

function drawComponentParams() {
    clear_screen();

    /* Live-refresh read-only param values (e.g. detected_key) from DSP */
    for (const param of componentParams) {
        const freshVal = getSlotParam(selectedSlot, param.key);
        if (freshVal !== null) param.value = freshVal;
    }

    /* Header shows component name */
    const componentTitle = editingComponent.charAt(0).toUpperCase() + editingComponent.slice(1);
    drawHeader(`Edit ${componentTitle}`);

    if (componentParams.length === 0) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No parameters", 1);
        drawFooter("Back: return");
        return;
    }

    const listY = LIST_TOP_Y;
    const lineHeight = 12;

    for (let i = 0; i < componentParams.length; i++) {
        const y = listY + i * lineHeight;
        const param = componentParams[i];
        const isSelected = i === selectedParam;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, lineHeight, 1);
        }

        const color = isSelected ? 0 : 1;
        let prefix = "  ";
        if (isSelected) {
            prefix = editingValue ? "* " : "> ";
        }

        print(LIST_LABEL_X, y, `${prefix}${param.label}:`, color);

        /* Format value display */
        let valueStr = formatParamValue(param);
        if (isSelected && editingValue) {
            /* Show brackets when editing */
            valueStr = `[${valueStr}]`;
        }
        print(LIST_VALUE_X - 8, y, valueStr, color);
    }

    if (editingValue) {
        drawFooter({left: "Click: done", right: "Jog: adjust"});
    } else {
        drawFooter({left: "Back: detail", right: "Click: edit"});
    }
}

/* Draw horizontal chain editor with boxed icons */
function drawChainEdit() {
    clear_screen();
    /* Slot view: show slot patch name in header */
    const slotName = slots[selectedSlot]?.name || "Unknown";
    const dirtyMark = slotDirtyCache[selectedSlot] ? "*" : "";
    const headerText = truncateText(`S${selectedSlot + 1} ${dirtyMark}${slotName}`, 24);
    drawHeader(headerText);

    /* Refresh chain config from DSP each render to ensure display matches actual state.
     * Without this, the cached chainConfigs can be stale if the slot was loaded
     * externally (e.g. patch restore) and the periodic signature refresh hasn't run yet. */
    loadChainConfigFromSlot(selectedSlot);
    const cfg = chainConfigs[selectedSlot] || createEmptyChainConfig();
    const chainSelected = selectedChainComponent === -1;

    /* Calculate box layout - 5 components across 128px
     * Box size: 22px wide, with 2px gaps, centered */
    const BOX_W = 22;
    const BOX_H = 16;
    const GAP = 2;
    const TOTAL_W = 5 * BOX_W + 4 * GAP;  // 118px
    const START_X = Math.floor((SCREEN_WIDTH - TOTAL_W) / 2);  // center it
    const BOX_Y = 20;  // Below header

    /* Draw each component box */
    for (let i = 0; i < CHAIN_COMPONENTS.length; i++) {
        const comp = CHAIN_COMPONENTS[i];
        const x = START_X + i * (BOX_W + GAP);
        const isSelected = i === selectedChainComponent;

        /* Get abbreviation for this component */
        let abbrev = "--";
        if (comp.key === "settings") {
            abbrev = "*";
        } else {
            const moduleData = cfg[comp.key];
            abbrev = moduleData ? getModuleAbbrev(moduleData.module) : "--";
        }

        /* Draw box:
         * - If chain selected (position -1): all boxes filled (inverted)
         * - If individual component selected: that box filled
         * - Otherwise: outlined box */
        const fillBox = chainSelected || isSelected;
        if (fillBox) {
            fill_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        } else {
            draw_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        }

        /* Draw abbreviation centered in box */
        const textColor = fillBox ? 0 : 1;
        const textX = x + Math.floor((BOX_W - abbrev.length * 5) / 2) + 1;
        const textY = BOX_Y + 5;
        print(textX, textY, abbrev, textColor);
    }

    /* Draw component label below boxes */
    const selectedComp = chainSelected ? null : CHAIN_COMPONENTS[selectedChainComponent];
    const labelY = BOX_Y + BOX_H + 4;
    const label = chainSelected ? "Chain" : (selectedComp ? selectedComp.label : "");
    const labelX = Math.floor((SCREEN_WIDTH - label.length * 5) / 2);
    print(labelX, labelY, label, 1);

    /* Draw current module name/preset below label */
    const infoY = labelY + 12;
    let infoLine = "";
    if (chainSelected) {
        /* Show patch name when chain is selected */
        infoLine = slots[selectedSlot]?.name || "(no patch)";
    } else if (selectedComp && selectedComp.key !== "settings") {
        const moduleData = cfg[selectedComp.key];
        if (moduleData) {
            /* Get display name from DSP if available */
            const prefix = selectedComp.key === "midiFx" ? "midi_fx1" : selectedComp.key;
            const displayName = getSlotParam(selectedSlot, `${prefix}:name`) || moduleData.module;
            const preset = getSlotParam(selectedSlot, `${prefix}:preset_name`) ||
                          getSlotParam(selectedSlot, `${prefix}:preset`) || "";
            infoLine = preset ? `${displayName} (${truncateText(preset, 8)})` : displayName;
        } else {
            infoLine = "(empty)";
        }
    } else if (selectedComp && selectedComp.key === "settings") {
        infoLine = "Configure slot";
    }
    infoLine = truncateText(infoLine, 24);
    const infoX = Math.floor((SCREEN_WIDTH - infoLine.length * 5) / 2);
    print(infoX, infoY, infoLine, 1);
}

/* Draw component module selection list */
function drawComponentSelect() {
    clear_screen();
    const comp = CHAIN_COMPONENTS[selectedChainComponent];
    drawHeader(`Select ${comp ? comp.label : "Module"}`);

    if (availableModules.length === 0) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No modules available", 1);
        return;
    }

    drawMenuList({
        items: availableModules,
        selectedIndex: selectedModuleIndex,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        lineHeight: 9,  /* Smaller to fit 4 items */
        getLabel: (item) => item.name || item.id || "Unknown",
        getValue: (item) => {
            const cfg = chainConfigs[selectedSlot];
            const compKey = CHAIN_COMPONENTS[selectedChainComponent]?.key;
            const current = cfg && cfg[compKey];
            const currentId = current ? current.module : null;
            return currentId === item.id ? "*" : "";
        }
    });
}

/* ===== Store Picker Drawing Functions ===== */

/* Draw store category browser */
function drawStorePickerCategories() {
    clear_screen();
    drawHeader('Module Store');

    if (storeCategoryItems.length === 0) {
        print(2, 28, "No modules available", 1);
        drawFooter('Back: return');
        return;
    }

    drawMenuList({
        items: storeCategoryItems,
        selectedIndex: storeCategoryIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.label,
        getValue: (item) => item.value || ''
    });

    drawFooter({left: "Back: return", right: "Jog: browse"});
}

/* Draw store picker module list */
function drawStorePickerList() {
    clear_screen();

    /* Find category name */
    const cat = CATEGORIES.find(c => c.id === storePickerCategory);
    const catName = cat ? cat.name : 'Modules';

    /* Header shows "Store: <category>" for context */
    drawHeader('Store: ' + catName);

    if (storePickerModules.length === 0) {
        print(2, 28, "No modules available", 1);
        drawFooter('Back: return');
        return;
    }

    /* Build items for drawMenuList */
    const items = storePickerModules.map(mod => {
        let statusIcon = '';
        if (mod._isHostUpdate) {
            /* Core update always shows update available icon */
            statusIcon = '^';
        } else {
            const status = getModuleStatus(mod, storeInstalledModules);
            if (status.installed) {
                statusIcon = status.hasUpdate ? '^' : '*';
            }
        }
        return { ...mod, statusIcon };
    });

    drawMenuList({
        items,
        selectedIndex: storePickerSelectedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.name,
        getValue: (item) => item.statusIcon
    });

    drawFooter({left: "Back: return", right: "Jog: browse"});
}

/* Draw store picker loading screen */
function drawStorePickerLoading() {
    clear_screen();
    const title = storePickerLoadingTitle || 'Module Store';
    const msg = storePickerLoadingMessage || 'Loading...';
    drawStatusOverlay(title, msg);
}

/* Draw store picker result screen */
function drawStorePickerResult() {
    clear_screen();
    drawHeader(storePickerResultTitle || 'Module Store');

    /* Message centered vertically */
    const msg = storePickerMessage || 'Done';
    print(2, 28, msg, 1);

    drawFooter('Press to continue');
}

/* Draw store picker module detail */
/* Build scrollable lines from release notes text */
function buildReleaseNoteLines(notesText) {
    const lines = [];
    const noteLines = notesText.split('\n');
    for (const line of noteLines) {
        if (line.trim() === '') {
            lines.push('');
        } else {
            /* Strip markdown: headers, bold, italic */
            const cleaned = line.trim()
                .replace(/^#+\s*/, '')
                .replace(/\*\*/g, '')
                .replace(/\*/g, '');
            const wrapped = wrapText(cleaned, 20);
            lines.push(...wrapped);
        }
    }
    return lines;
}

function drawStorePickerDetail() {
    clear_screen();

    const mod = storePickerCurrentModule;
    if (!mod) return;

    /* Special handling for core update */
    if (mod._isHostUpdate) {
        const title = 'Core Update';
        const versionStr = `${storeHostVersion}->${mod.latest_version}`;
        drawHeader(title, versionStr);

        if (!storeDetailScrollState || storeDetailScrollState.moduleId !== mod.id) {
            const notes = fetchReleaseNotes('charlesvestal/move-anything');
            const descLines = [];
            descLines.push(`${storeHostVersion} -> ${mod.latest_version}`);
            descLines.push('');
            if (notes) {
                descLines.push(...buildReleaseNoteLines(notes));
            } else {
                descLines.push('Update Move Anything');
                descLines.push('core framework.');
                descLines.push('');
                descLines.push('Restart required');
                descLines.push('after update.');
            }
            storeDetailScrollState = createScrollableText({
                lines: descLines,
                actionLabel: 'Update',
                visibleLines: 3,
                onActionSelected: (label) => announce(label)
            });
            storeDetailScrollState.moduleId = mod.id;
        }

        drawScrollableText({
            state: storeDetailScrollState,
            topY: 16,
            bottomY: 40,
            actionY: 52
        });
        return;
    }

    const status = getModuleStatus(mod, storeInstalledModules);

    /* Header with name and version */
    let title = mod.name;
    let versionStr = `v${mod.latest_version}`;
    if (status.installed && status.hasUpdate) {
        versionStr = `${status.installedVersion}->${mod.latest_version}`;
        if (title.length > 8) title = title.substring(0, 7) + '~';
    } else {
        if (title.length > 12) title = title.substring(0, 11) + '~';
    }
    drawHeader(title, versionStr);

    /* Initialize scroll state if needed */
    if (!storeDetailScrollState || storeDetailScrollState.moduleId !== mod.id) {
        const descLines = wrapText(mod.description || 'No description available.', 20);

        /* Add author */
        descLines.push('');
        descLines.push(`by ${mod.author || 'Unknown'}`);

        /* Add requires line if present */
        if (mod.requires) {
            descLines.push('');
            descLines.push('Requires:');
            const reqLines = wrapText(mod.requires, 18);
            descLines.push(...reqLines);
        }

        /* Fetch and append release notes */
        if (mod.github_repo) {
            const notes = fetchReleaseNotes(mod.github_repo);
            if (notes) {
                descLines.push('');
                descLines.push('What\'s New:');
                descLines.push(...buildReleaseNoteLines(notes));
            }
        }

        /* Determine action label */
        let actionLabel;
        if (status.installed) {
            actionLabel = status.hasUpdate ? 'Update' : 'Reinstall';
        } else {
            actionLabel = 'Install';
        }

        storeDetailScrollState = createScrollableText({
            lines: descLines,
            actionLabel,
            visibleLines: 3,
            onActionSelected: (label) => announce(label)
        });
        storeDetailScrollState.moduleId = mod.id;
    }

    /* Draw scrollable content */
    drawScrollableText({
        state: storeDetailScrollState,
        topY: 16,
        bottomY: 40,
        actionY: 52
    });
}

/* Draw update detail view (release notes + install action) */
function drawUpdateDetail() {
    clear_screen();
    const title = updateDetailModule ? updateDetailModule.name : 'Update';
    drawHeader(title);

    if (updateDetailScrollState) {
        drawScrollableText({
            state: updateDetailScrollState,
            topY: 16,
            bottomY: 40,
            actionY: 52
        });
    }

    /* 1px border */
    fill_rect(0, 0, 128, 1, 1);
    fill_rect(0, 63, 128, 1, 1);
    fill_rect(0, 0, 1, 64, 1);
    fill_rect(127, 0, 1, 64, 1);
}

/* Draw update prompt view (shown on startup when updates are available) */
function drawUpdatePrompt() {
    clear_screen();
    drawHeader('Updates Available');

    const items = pendingUpdates.map(upd => ({
        label: upd.name,
        subLabel: upd.from + ' -> ' + upd.to
    }));
    /* Add "Update All" as the last item */
    items.push({ label: '[Update All]', subLabel: '' });

    drawMenuList({
        items: items,
        selectedIndex: pendingUpdateIndex,
        getLabel: (item) => item.label,
        getSubLabel: (item) => item.subLabel,
        subLabelOffset: 7,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y }
    });

    drawFooter('Back: cancel');

    /* 1px border around entire screen to distinguish from normal UI */
    fill_rect(0, 0, 128, 1, 1);
    fill_rect(0, 63, 128, 1, 1);
    fill_rect(0, 0, 1, 64, 1);
    fill_rect(127, 0, 1, 64, 1);
}

/* Draw restart prompt after core update */
function drawUpdateRestart() {
    clear_screen();
    drawHeader('Core Updated!');

    const versionStr = updateRestartFromVersion + ' -> ' + updateRestartToVersion;
    print(2, 20, versionStr, 1);
    print(2, 36, 'Click: Restart now', 1);

    drawFooter("Back: Later");
}

/* Draw component edit view (presets, params) */
function drawComponentEdit() {
    clear_screen();

    /* Get component info */
    const cfg = chainConfigs[selectedSlot];
    const moduleData = cfg && cfg[editingComponentKey];
    const moduleName = moduleData ? moduleData.module.toUpperCase() : "Unknown";

    /* Get display name from DSP if available */
    const prefix = editingComponentKey === "midiFx" ? "midi_fx1" : editingComponentKey;
    const displayName = getSlotParam(selectedSlot, `${prefix}:name`) || moduleName;

    /* Build header: S#: Module: Bank (preset selection shows bank/soundfont) */
    const abbrev = moduleData ? getModuleAbbrev(moduleData.module) : moduleName;
    const bankName = getSlotParam(selectedSlot, `${prefix}:bank_name`) || displayName;
    const headerText = truncateText(`S${selectedSlot + 1}: ${abbrev}: ${bankName}`, 24);
    drawHeader(headerText);

    const centerY = 32;

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
    drawFooter({left: "Back: done", right: "Jog: preset"});
}

/* Draw chain settings view */
function drawChainSettings() {
    clear_screen();

    /* Handle name preview view */
    if (showingNamePreview) {
        drawHeader("Save As");
        const name = truncateText(pendingSaveName, 20);
        print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

        const listY = LIST_TOP_Y + 16;
        for (let i = 0; i < 2; i++) {
            const y = listY + i * LIST_LINE_HEIGHT;
            const isSelected = i === namePreviewIndex;
            if (isSelected) {
                fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
            }
            print(LIST_LABEL_X, y, i === 0 ? "Edit" : "OK", isSelected ? 0 : 1);
        }
        drawFooter("Back: cancel");
        return;
    }

    /* Handle confirmation views */
    if (confirmingOverwrite) {
        drawHeader("Overwrite?");
        const name = truncateText(pendingSaveName, 20);
        print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

        const listY = LIST_TOP_Y + 16;
        for (let i = 0; i < 2; i++) {
            const y = listY + i * LIST_LINE_HEIGHT;
            const isSelected = i === confirmIndex;
            if (isSelected) {
                fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
            }
            print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
        }
        drawFooter("Back: cancel");
        return;
    }

    if (confirmingDelete) {
        drawHeader("Delete?");
        const name = truncateText(slots[selectedSlot] ? slots[selectedSlot].name : "Unknown", 20);
        print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

        const listY = LIST_TOP_Y + 16;
        for (let i = 0; i < 2; i++) {
            const y = listY + i * LIST_LINE_HEIGHT;
            const isSelected = i === confirmIndex;
            if (isSelected) {
                fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
            }
            print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
        }
        drawFooter("Back: cancel");
        return;
    }

    drawHeader("S" + (selectedSlot + 1) + " Settings");

    const items = getChainSettingsItems(selectedSlot);
    const listY = LIST_TOP_Y;
    const lineHeight = 9;
    const maxVisible = Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / lineHeight);

    let scrollOffset = 0;
    if (selectedChainSetting >= maxVisible) {
        scrollOffset = selectedChainSetting - maxVisible + 1;
    }

    for (let i = 0; i < maxVisible && (i + scrollOffset) < items.length; i++) {
        const itemIdx = i + scrollOffset;
        const y = listY + i * lineHeight;
        const setting = items[itemIdx];
        const isSelected = itemIdx === selectedChainSetting;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }

        const labelColor = isSelected ? 0 : 1;
        print(LIST_LABEL_X, y, setting.label, labelColor);

        if (setting.type !== "action") {
            const value = getChainSettingValue(selectedSlot, setting);
            if (value) {
                const valueX = SCREEN_WIDTH - value.length * 5 - 4;
                if (isSelected && editingChainSettingValue) {
                    print(valueX - 8, y, "<", 0);
                    print(valueX, y, value, 0);
                    print(valueX + value.length * 5 + 2, y, ">", 0);
                } else {
                    print(valueX, y, value, labelColor);
                }
            }
        }
    }
}

/* Draw knob assignment editor - list of 8 knobs with their assignments */
function drawKnobEditor() {
    clear_screen();
    drawHeader(`S${knobEditorSlot + 1} Knobs`);

    const listY = LIST_TOP_Y;
    const lineHeight = 10;
    const maxVisible = Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / lineHeight);

    /* List all 8 knobs */
    const items = [];
    for (let i = 0; i < NUM_KNOBS; i++) {
        items.push({
            type: "knob",
            label: `Knob ${i + 1}`,
            assignment: knobEditorAssignments[i]
        });
    }

    /* Calculate scroll offset */
    let scrollOffset = 0;
    if (knobEditorIndex >= maxVisible) {
        scrollOffset = knobEditorIndex - maxVisible + 1;
    }

    for (let i = 0; i < maxVisible && (i + scrollOffset) < items.length; i++) {
        const itemIdx = i + scrollOffset;
        const item = items[itemIdx];
        const y = listY + i * lineHeight;
        const isSelected = itemIdx === knobEditorIndex;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }

        const labelColor = isSelected ? 0 : 1;
        print(LIST_LABEL_X, y, item.label, labelColor);

        /* Show assignment on the right */
        if (item.type === "knob") {
            const value = getKnobAssignmentLabel(item.assignment);
            const truncValue = truncateText(value, 12);
            const valueX = SCREEN_WIDTH - truncValue.length * 5 - 4;
            print(valueX, y, truncValue, labelColor);
        }
    }

    drawFooter({left: "Back: cancel", right: "Click: edit"});
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

        drawFooter({left: "Back: cancel", right: "Click: select"});
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
        drawFooter(hasNav ? {left: "Back: up", right: "Click: select"} : {left: "Back: up", right: "Click: assign"});
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

        drawFooter({left: "Back: targets", right: "Click: assign"});
    }
}

/* ========== Master Preset Draw Functions ========== */

function drawMasterNamePreview() {
    drawHeader("Save As");

    const name = truncateText(masterPendingSaveName, 20);
    print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < 2; i++) {
        const y = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === masterNamePreviewIndex;
        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, y, i === 0 ? "Edit" : "OK", isSelected ? 0 : 1);
    }

    drawFooter("Back: cancel");
}

function drawMasterConfirmOverwrite() {
    drawHeader("Overwrite?");

    const name = truncateText(masterPendingSaveName, 20);
    print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < 2; i++) {
        const y = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === masterConfirmIndex;
        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
    }

    drawFooter("Back: cancel");
}

function drawMasterConfirmDelete() {
    drawHeader("Delete?");

    const name = truncateText(currentMasterPresetName, 20);
    print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < 2; i++) {
        const y = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === masterConfirmIndex;
        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
    }

    drawFooter("Back: cancel");
}

function drawMasterFxSettingsMenu() {
    const title = currentMasterPresetName || "Master FX";
    drawHeader(truncateText(title, 18));

    const items = getMasterFxSettingsItems();

    drawMenuList({
        items: items,
        selectedIndex: selectedMasterFxSetting,
        getLabel: (item) => item.label,
        getValue: (item) => {
            if (item.type === "action") return "";
            return getMasterFxSettingValue(item);
        },
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true
    });

    drawFooter("Back: FX chain");
}

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

function drawGlobalSettings() {
    clear_screen();

    /* Help viewer takes over display */
    if (helpDetailScrollState) {
        drawHelpDetail();
        return;
    }
    if (helpNavStack.length > 0) {
        drawHelpList();
        return;
    }

    if (globalSettingsInSection) {
        /* Show items within selected section */
        const section = GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex];
        drawHeader(truncateText(section.label, 18));

        drawMenuList({
            items: section.items,
            selectedIndex: globalSettingsItemIndex,
            getLabel: (item) => {
                if (globalSettingsEditing && section.items[globalSettingsItemIndex] === item) {
                    return "[" + item.label + "]";
                }
                return item.label;
            },
            getValue: (item) => {
                if (item.type === "action") return "";
                return getMasterFxSettingValue(item);
            },
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            valueAlignRight: true
        });

        drawFooter("Back: Settings");
    } else {
        /* Show section list */
        drawHeader("Settings");

        drawMenuList({
            items: GLOBAL_SETTINGS_SECTIONS,
            selectedIndex: globalSettingsSectionIndex,
            getLabel: (section) => section.label,
            getValue: () => "",
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            valueAlignRight: false
        });

        drawFooter("Back: return");
    }
}

function drawMasterFx() {
    clear_screen();

    /* Handle text entry (keyboard) */
    if (isTextEntryActive()) {
        drawTextEntry();
        return;
    }

    /* Check if we're showing name preview */
    if (masterShowingNamePreview) {
        drawMasterNamePreview();
        return;
    }

    /* Check if we're confirming overwrite */
    if (masterConfirmingOverwrite) {
        drawMasterConfirmOverwrite();
        return;
    }

    /* Check if we're confirming delete */
    if (masterConfirmingDelete) {
        drawMasterConfirmDelete();
        return;
    }

    /* Check if we're in help viewer */
    if (helpDetailScrollState) {
        drawHelpDetail();
        return;
    }
    if (helpNavStack.length > 0) {
        drawHelpList();
        return;
    }

    /* Check if we're in preset picker mode */
    if (inMasterPresetPicker) {
        drawMasterPresetPicker();
        return;
    }

    /* Check if we're in settings submenu */
    if (inMasterFxSettingsMenu) {
        drawMasterFxSettingsMenu();
        return;
    }

    /* Check if we're in module selection mode */
    if (selectingMasterFxModule) {
        drawMasterFxModuleSelect();
        return;
    }

    drawHeader("Master FX");

    /* Calculate box layout - 5 components (4 FX + settings) across 128px */
    const BOX_W = 22;
    const BOX_H = 16;
    const GAP = 2;
    const TOTAL_W = 5 * BOX_W + 4 * GAP;  // 118px
    const START_X = Math.floor((SCREEN_WIDTH - TOTAL_W) / 2);
    const BOX_Y = 20;

    const presetSelected = selectedMasterFxComponent === -1;

    /* Draw each component box */
    for (let i = 0; i < MASTER_FX_CHAIN_COMPONENTS.length; i++) {
        const comp = MASTER_FX_CHAIN_COMPONENTS[i];
        const x = START_X + i * (BOX_W + GAP);
        const isSelected = i === selectedMasterFxComponent;

        /* Get abbreviation for this component */
        let abbrev = "--";
        if (comp.key === "settings") {
            abbrev = "*";
        } else {
            const moduleData = masterFxConfig[comp.key];
            abbrev = moduleData ? getModuleAbbrev(moduleData.module) : "--";
        }

        /* Draw box:
         * - If preset selected (position -1): all boxes filled (inverted)
         * - If individual component selected: that box filled
         * - Otherwise: outlined box */
        const fillBox = presetSelected || isSelected;
        if (fillBox) {
            fill_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        } else {
            draw_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        }

        /* Draw abbreviation centered in box */
        const textColor = fillBox ? 0 : 1;
        const textX = x + Math.floor((BOX_W - abbrev.length * 5) / 2) + 1;
        const textY = BOX_Y + 5;
        print(textX, textY, abbrev, textColor);
    }

    /* Draw component label below boxes */
    const selectedComp = presetSelected ? null : MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
    const labelY = BOX_Y + BOX_H + 4;
    const label = presetSelected ? "Preset" : (selectedComp ? selectedComp.label : "");
    const labelX = Math.floor((SCREEN_WIDTH - label.length * 5) / 2);
    print(labelX, labelY, label, 1);

    /* Draw current module name/preset below label */
    const infoY = labelY + 12;
    let infoLine = "";
    if (presetSelected) {
        /* Show preset name when preset is selected */
        infoLine = currentMasterPresetName || "(no preset)";
    } else if (selectedComp && selectedComp.key !== "settings") {
        const moduleData = masterFxConfig[selectedComp.key];
        if (moduleData && moduleData.module) {
            /* Get display name from MASTER_FX_OPTIONS */
            const opt = MASTER_FX_OPTIONS.find(o => o.id === moduleData.module);
            const displayName = opt ? opt.name : moduleData.module;
            const preset = getMasterFxParam(selectedMasterFxComponent, "preset_name") ||
                          getMasterFxParam(selectedMasterFxComponent, "preset") || "";
            infoLine = preset ? `${displayName} (${truncateText(preset, 8)})` : displayName;
        } else {
            infoLine = "(empty)";
        }
    } else if (selectedComp && selectedComp.key === "settings") {
        infoLine = "Configure master FX";
    }
    infoLine = truncateText(infoLine, 24);
    const infoX = Math.floor((SCREEN_WIDTH - infoLine.length * 5) / 2);
    print(infoX, infoY, infoLine, 1);
}

/* Draw module selection list for Master FX */
function drawMasterFxModuleSelect() {
    const comp = MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
    drawHeader(`Select ${comp ? comp.label : "FX"}`);

    if (MASTER_FX_OPTIONS.length === 0) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No FX modules available", 1);
        return;
    }

    drawMenuList({
        items: MASTER_FX_OPTIONS,
        selectedIndex: selectedMasterFxModuleIndex,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        getLabel: (item) => item.name,
        getValue: (item) => {
            /* Show * if this is the currently loaded module */
            const currentModule = masterFxConfig[comp.key]?.module || "";
            return item.id === currentModule ? "*" : "";
        }
    });
    drawFooter({left: "Back: cancel", right: "Click: apply"});
}

globalThis.init = function() {
    debugLog("Shadow UI init");
    refreshSlots();
    loadPatchList();
    initChainConfigs();
    updateFocusedSlot(selectedSlot);
    fetchKnobMappings(selectedSlot);

    /* Scan for audio FX modules so display names are available */
    MASTER_FX_OPTIONS = scanForAudioFxModules();

    /* Load and apply master FX chain from config */
    loadMasterFxChainFromConfig();

    /* Load auto-update preference */
    loadAutoUpdateConfig();

    /* Legacy: migrate old single master_fx config to slot 1 */
    const savedMasterFx = loadMasterFxFromConfig();
    if (savedMasterFx.path && !masterFxConfig.fx1.module) {
        setMasterFxSlotModule(0, savedMasterFx.path);
        masterFxConfig.fx1.module = savedMasterFx.id;
    }
    /* Note: Jump-to-slot check moved to first tick() to avoid race condition */

    /* Auto-update check is manual only (Settings → Updates → Check Updates) */

    /* Process any HTML left by a prior background download, then kick off
     * a new background download if the cache is stale. Both are non-blocking. */
    try { processDownloadedHtml(); } catch (e) { debugLog("Manual process: " + e); }
    try { refreshManualBackground(); } catch (e) { debugLog("Manual refresh: " + e); }

    /* Read active set UUID to point autosave at the correct per-set directory.
     * File format: line 1 = UUID, line 2 = set name */
    {
        const raw = host_read_file("/data/UserData/move-anything/active_set.txt");
        if (raw) {
            const lines = raw.split("\n");
            const uuid = lines[0] ? lines[0].trim() : "";
            if (uuid) {
                const setDir = "/data/UserData/move-anything/set_state/" + uuid;
                if (host_file_exists(setDir + "/slot_0.json") || host_file_exists(setDir + "/shadow_chain_config.json")) {
                    activeSlotStateDir = setDir;
                    debugLog("Init: using per-set state dir " + setDir);
                }
            }
        }
    }

    /* Sync dirty cache and slot names from autosave files (shim loaded them on startup) */
    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
        const dirty = getSlotParam(i, "dirty");
        slotDirtyCache[i] = (dirty === "1");
        /* Sync slot names from autosave if present */
        const path = activeSlotStateDir + "/slot_" + i + ".json";
        if (host_file_exists(path)) {
            const raw = host_read_file(path);
            if (raw) {
                const m = raw.match(/"name"\s*:\s*"([^"]+)"/);
                if (m && m[1] && !slots[i].name) {
                    slots[i].name = m[1];
                }
            }
        }
    }
    saveSlotsToConfig(slots);

    /* Announce initial view + selection */
    const slotName = slots[selectedSlot]?.name || "Unknown";
    announce(`Slots Menu, S${selectedSlot + 1} ${slotName}`);
};

/* Called by shadow_ui.c during controlled exits (restart/shutdown paths)
 * to guarantee one final persistence flush before process termination. */
globalThis.shadow_save_state_now = function() {
    autosaveAllSlots();
    saveMasterFxChainConfig();
    debugLog("shadow_save_state_now: flushed set state before exit");
    return true;
};

globalThis.tick = function() {
    /* Check for jump-to-slot flag on EVERY tick (flag can be set while UI is running) */
    if (typeof shadow_get_ui_flags === "function") {
        const flags = shadow_get_ui_flags();
        globalThis._debugFlags = flags;  /* Debug: store for display */

        /* If showing update prompt/restart, allow navigation flags to override */
        if (view === VIEWS.UPDATE_PROMPT || view === VIEWS.UPDATE_RESTART) {
            /* Let jump flags through so shortcuts still work */
            const navFlags = flags & (SHADOW_UI_FLAG_JUMP_TO_SLOT | SHADOW_UI_FLAG_JUMP_TO_MASTER_FX |
                              SHADOW_UI_FLAG_JUMP_TO_OVERTAKE | SHADOW_UI_FLAG_JUMP_TO_SETTINGS |
                              SHADOW_UI_FLAG_JUMP_TO_SCREENREADER);
            const otherFlags = flags & ~navFlags;
            if (otherFlags && typeof shadow_clear_ui_flags === "function") {
                shadow_clear_ui_flags(otherFlags);
            }
            /* Fall through to process nav flags */
        }
        {
            /* Settings/Screenreader flags take priority (clear conflicting SLOT flag) */
            if (flags & SHADOW_UI_FLAG_JUMP_TO_SETTINGS) {
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
                debugLog("OVERTAKE flag detected, view=" + view);
                /* Toggle overtake mode */
                if (view === VIEWS.OVERTAKE_MODULE || view === VIEWS.OVERTAKE_MENU) {
                    /* Already in overtake mode - exit back to Move */
                    debugLog("exiting overtake mode");
                    exitOvertakeMode();
                } else {
                    /* Enter overtake menu */
                    debugLog("entering overtake menu");
                    enterOvertakeMenu();
                }
                /* Clear the flag */
                if (typeof shadow_clear_ui_flags === "function") {
                    shadow_clear_ui_flags(SHADOW_UI_FLAG_JUMP_TO_OVERTAKE);
                }
            }
        }
        if (flags & SHADOW_UI_FLAG_SAVE_STATE) {
            debugLog("SAVE_STATE flag detected — shutdown imminent, saving all state");
            autosaveAllSlots();
            saveMasterFxChainConfig();
            if (typeof shadow_clear_ui_flags === "function") {
                shadow_clear_ui_flags(SHADOW_UI_FLAG_SAVE_STATE);
            }
        }
        if (flags & SHADOW_UI_FLAG_SET_CHANGED) {
            debugLog("SET_CHANGED flag detected — switching slot state directory");

            /* 1. Save current state to outgoing directory */
            autosaveAllSlots();
            saveMasterFxChainConfig();

            /* 2. Read new UUID and set name from active_set.txt
             *    Format: line 1 = UUID, line 2 = set name */
            const activeSetRaw = host_read_file("/data/UserData/move-anything/active_set.txt");
            const activeSetLines = activeSetRaw ? activeSetRaw.split("\n") : [];
            const uuid = activeSetLines[0] ? activeSetLines[0].trim() : "";
            const setName = activeSetLines[1] ? activeSetLines[1].trim() : "";

            /* 3. Determine new directory */
            const newDir = uuid
                ? "/data/UserData/move-anything/set_state/" + uuid
                : SLOT_STATE_DIR_DEFAULT;

            if (uuid && typeof host_ensure_dir === "function") {
                host_ensure_dir("/data/UserData/move-anything/set_state");
                host_ensure_dir(newDir);
            }

            /* 4. First visit to this set: seed its state directory.
             *    Batch migration (shim boot) already copied default state to all existing sets.
             *    Here we only handle: (a) duplicated sets, (b) newly created sets (start clean). */
            if (uuid && !host_file_exists(newDir + "/slot_0.json")) {
                const copySourceUuid = host_read_file(newDir + "/copy_source.txt");
                if (copySourceUuid && copySourceUuid.trim()) {
                    /* Set was duplicated — copy from the source set */
                    const srcDir = "/data/UserData/move-anything/set_state/" + copySourceUuid.trim();
                    if (host_file_exists(srcDir + "/slot_0.json")) {
                        debugLog("SET_CHANGED: duplicated set, copying from " + srcDir);
                        for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                            const src = host_read_file(srcDir + "/slot_" + i + ".json");
                            if (src) host_write_file(newDir + "/slot_" + i + ".json", src);
                            const mfx = host_read_file(srcDir + "/master_fx_" + i + ".json");
                            if (mfx) host_write_file(newDir + "/master_fx_" + i + ".json", mfx);
                        }
                    }
                } else {
                    /* New set created after migration — start with empty slots */
                    debugLog("SET_CHANGED: new set, starting with empty slots");
                    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
                        host_write_file(newDir + "/slot_" + i + ".json", "{}\n");
                        host_write_file(newDir + "/master_fx_" + i + ".json", "{}\n");
                    }
                }
            }

            /* 5. Switch directory */
            const oldDir = activeSlotStateDir;
            activeSlotStateDir = newDir;
            debugLog("SET_CHANGED: " + oldDir + " -> " + newDir);

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
            }

            /* Suppress autosave briefly so async DSP settling doesn't
             * overwrite the freshly-written slot files */
            autosaveSuppressUntil = 150; /* ~5 seconds at 30fps */

            /* 7. Reload master FX modules from per-set state files */
            for (let mfxi = 0; mfxi < 4; mfxi++) {
                const mfxPath = activeSlotStateDir + "/master_fx_" + mfxi + ".json";
                let mfxDspPath = "";
                let mfxModuleId = "";
                if (host_file_exists(mfxPath)) {
                    try {
                        const mfxRaw = host_read_file(mfxPath);
                        if (mfxRaw && mfxRaw.length > 10) {
                            const mfxData = JSON.parse(mfxRaw);
                            mfxDspPath = mfxData.module_path || "";
                            mfxModuleId = mfxData.module_id || "";
                        }
                    } catch (e) {}
                }
                /* Load or unload the module */
                setMasterFxSlotModule(mfxi, mfxDspPath);
                const key = `fx${mfxi + 1}`;
                masterFxConfig[key].module = mfxModuleId;

                /* Restore plugin state/params if available */
                if (mfxDspPath && host_file_exists(mfxPath)) {
                    try {
                        const mfxRaw = host_read_file(mfxPath);
                        const mfxData = JSON.parse(mfxRaw);
                        if (mfxData.state) {
                            shadow_set_param(0, `master_fx:fx${mfxi + 1}:state`, JSON.stringify(mfxData.state));
                        }
                        if (mfxData.params) {
                            for (const [pk, pv] of Object.entries(mfxData.params)) {
                                shadow_set_param(0, `master_fx:fx${mfxi + 1}:${pk}`, String(pv));
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

            /* 10. Clear flag */
            if (typeof shadow_clear_ui_flags === "function") {
                shadow_clear_ui_flags(SHADOW_UI_FLAG_SET_CHANGED);
            }
            debugLog("SET_CHANGED: reload complete");
        }
        /* SETTINGS and SCREENREADER flags are handled earlier with SLOT/MFX/OVERTAKE */
    }


    refreshCounter++;
    if (refreshCounter % 120 === 0) {
        refreshSlots();
    }

    /* Periodic autosave (suppressed briefly after set change) */
    if (autosaveSuppressUntil > 0) {
        autosaveSuppressUntil--;
        autosaveCounter = 0;
    } else {
        autosaveCounter++;
        if (autosaveCounter >= AUTOSAVE_INTERVAL) {
            autosaveCounter = 0;
            autosaveAllSlots();
            saveMasterFxChainConfig();
        }
    }
    /* Refresh dirty cache frequently for responsive UI */
    if (refreshCounter % 15 === 0) {
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
    if (refreshCounter % 30 === 0) {
        const fxComponents = ["fx1", "fx2"];
        /* Per-slot FX */
        for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
            const cfg = chainConfigs[i];
            if (!cfg) continue;
            for (const comp of fxComponents) {
                if (!cfg[comp] || !cfg[comp].module) continue;
                const cacheKey = `${i}:${comp}`;
                const name = getSlotParam(i, `${comp}:display_name`);
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
        const masterFxKeys = ["fx1", "fx2", "fx3", "fx4"];
        for (const key of masterFxKeys) {
            if (!masterFxConfig[key] || !masterFxConfig[key].module) continue;
            const cacheKey = `master:${key}`;
            const name = getSlotParam(0, `master_fx:${key}:display_name`);
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
    if (refreshCounter % 30 === 0) {
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

    /* Throttled hierarchy knob adjustment - once per frame */
    processPendingHierKnob();

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
        case VIEWS.KNOB_EDITOR:
            drawKnobEditor();
            break;
        case VIEWS.KNOB_PARAM_PICKER:
            drawKnobParamPicker();
            break;
        case VIEWS.STORE_PICKER_CATEGORIES:
            drawStorePickerCategories();
            break;
        case VIEWS.STORE_PICKER_LIST:
            drawStorePickerList();
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            drawStorePickerDetail();
            break;
        case VIEWS.STORE_PICKER_LOADING:
            drawStorePickerLoading();
            break;
        case VIEWS.STORE_PICKER_RESULT:
            drawStorePickerResult();
            break;
        case VIEWS.STORE_PICKER_POST_INSTALL:
            drawMessageOverlay('Install Complete', storePostInstallLines);
            break;
        case VIEWS.UPDATE_PROMPT:
            drawUpdatePrompt();
            break;
        case VIEWS.UPDATE_DETAIL:
            drawUpdateDetail();
            break;
        case VIEWS.UPDATE_RESTART:
            drawUpdateRestart();
            break;
        case VIEWS.OVERTAKE_MENU:
            drawOvertakeMenu();
            break;
        case VIEWS.GLOBAL_SETTINGS:
            drawGlobalSettings();
            break;
        case VIEWS.OVERTAKE_MODULE:
            try {
                /* Handle exit - progressively clear LEDs then return to Move */
                if (overtakeExitPending) {
                    debugLog("OVERTAKE tick: exit phase");
                    /* Show exiting screen while clearing LEDs */
                    clear_screen();
                    print(40, 28, "Exiting...", 1);

                    /* Clear LEDs in batches */
                    const exitLedsCleared = clearLedBatch();

                    /* After LEDs cleared, complete the exit */
                    if (exitLedsCleared) {
                        ledClearIndex = 0;
                        completeOvertakeExit();
                    }
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

                    /* After LEDs cleared and delay passed, call init */
                    if (ledsCleared && overtakeInitTicks >= OVERTAKE_INIT_DELAY_TICKS) {
                        overtakeInitPending = false;
                        ledClearIndex = 0;  /* Reset for next time */
                        debugLog("loadOvertakeModule: init delay complete, calling init()");
                        if (overtakeModuleCallbacks && overtakeModuleCallbacks.init) {
                            try {
                                overtakeModuleCallbacks.init();
                                debugLog("loadOvertakeModule: init() returned successfully");
                            } catch (e) {
                                debugLog("loadOvertakeModule: init() threw exception: " + e);
                                /* Exit overtake on init error */
                                exitOvertakeMode();
                            }
                        }
                        flushLedQueue();  /* Drain any LEDs set during init() */
                    }
                } else {
                    /* Call the overtake module's tick() function */
                    if (overtakeModuleCallbacks && overtakeModuleCallbacks.tick) {
                        try {
                            overtakeModuleCallbacks.tick();
                        } catch (e) {
                            debugLog("OVERTAKE tick() exception: " + e);
                            /* Exit overtake on tick error */
                            exitOvertakeMode();
                        }
                    }
                    flushLedQueue();  /* Drain queued LED updates to SHM after module tick */
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

    /* Draw text entry on top if active */
    if (isTextEntryActive()) {
        drawTextEntry();
    }

    /* Draw asset warning overlay if active */
    if (assetWarningActive) {
        drawMessageOverlay(assetWarningTitle, assetWarningLines);
    }

    /* Draw overlay on top of main view (uses shared overlay system) */
    drawOverlay();
};

let debugMidiCounter = 0;
let lastCC = { cc: 0, val: 0 };
globalThis.onMidiMessageInternal = function(data) {
    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    /* Debug: log all MIDI when in overtake mode to diagnose escape issues */
    if (view === VIEWS.OVERTAKE_MODULE || view === VIEWS.OVERTAKE_MENU) {
        debugLog(`MIDI_IN: view=${view} status=${status} d1=${d1} d2=${d2} loaded=${overtakeModuleLoaded} callbacks=${!!overtakeModuleCallbacks}`);
    }

    /* Handle text entry MIDI if active */
    if (isTextEntryActive()) {
        if (handleTextEntryMidi(data)) {
            needsRedraw = true;
            return;  /* Consumed by text entry */
        }
    }

    /* Dismiss asset warning overlay on any button press */
    if (assetWarningActive && (status & 0xF0) === 0xB0 && d2 > 0) {
        dismissAssetWarning();
        return;  /* Consumed - don't process further */
    }

    /* Debug: track last CC for display (only for CC messages) */
    if ((status & 0xF0) === 0xB0) {
        lastCC = { cc: d1, val: d2 };
        needsRedraw = true;
    }

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
        debugLog(`OVERTAKE MIDI: status=${status} d1=${d1} d2=${d2} hostShift=${hostShiftHeld} volTouch=${hostVolumeKnobTouched}`);

        /* HOST-LEVEL ESCAPE: Shift+Vol+Jog Click always exits overtake mode
         * This runs BEFORE passing MIDI to the module, ensuring escape always works */
        if ((status & 0xF0) === 0xB0 && d1 === MoveMainButton && d2 > 0) {
            debugLog(`JOG CLICK: hostShift=${hostShiftHeld} volTouch=${hostVolumeKnobTouched}`);
            if (hostShiftHeld && hostVolumeKnobTouched) {
                debugLog("HOST: Shift+Vol+Jog detected, exiting overtake mode");
                exitOvertakeMode();
                return;
            }
        }

        /* Route to the overtake module's onMidiMessageInternal if it exists */
        if (overtakeModuleCallbacks.onMidiMessageInternal) {
            try {
                overtakeModuleCallbacks.onMidiMessageInternal(data);
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
            /* Shift+Click in chain edit enters component edit mode */
            if (isShiftHeld() && view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0) {
                handleShiftSelect();
            } else if (isShiftHeld() && view === VIEWS.MASTER_FX && selectedMasterFxComponent >= 0 && selectedMasterFxComponent < 4) {
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
     * This ensures accumulated deltas are processed before next touch */
    if ((status & 0xF0) === MidiNoteOn && d2 === 0) {
        if (d1 >= MoveKnob1Touch && d1 <= MoveKnob8Touch) {
            const knobIndex = d1 - MoveKnob1Touch;
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
            return;
        }
    }
};

globalThis.onMidiMessageExternal = function(_data) {
    /* ignore */
};
