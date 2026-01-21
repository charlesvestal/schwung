import * as os from 'os';
import * as std from 'std';

/* Debug logging to file */
const DEBUG_LOG_FILE = '/tmp/shadow_debug.log';
function debugLog(msg) {
    try {
        const f = std.open(DEBUG_LOG_FILE, 'a');
        if (f) {
            f.puts(`[${Date.now()}] ${msg}\n`);
            f.close();
        }
    } catch (e) {
        /* Ignore logging errors */
    }
}

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
} from '../shared/constants.mjs';

import {
    SCREEN_WIDTH, SCREEN_HEIGHT,
    TITLE_Y, TITLE_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_TEXT_Y, FOOTER_RULE_Y,
    truncateText
} from '../shared/chain_ui_views.mjs';

import { decodeDelta } from '../shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList,
    showOverlay,
    hideOverlay,
    tickOverlay,
    drawOverlay,
    menuLayoutDefaults
} from '../shared/menu_layout.mjs';

/* Track buttons - derive from imported constants */
const TRACK_CC_START = MoveRow4;  // CC 40
const TRACK_CC_END = MoveRow1;    // CC 43
const SHADOW_UI_SLOTS = 4;

/* UI flags from shim (must match SHADOW_UI_FLAG_* in shim) */
const SHADOW_UI_FLAG_JUMP_TO_SLOT = 0x01;

/* Knob CC range for parameter control */
const KNOB_CC_START = MoveKnob1;  // CC 71
const KNOB_CC_END = MoveKnob8;    // CC 78
const NUM_KNOBS = 8;

const CONFIG_PATH = "/data/UserData/move-anything/shadow_chain_config.json";
const PATCH_DIR = "/data/UserData/move-anything/modules/chain/patches";
const DEFAULT_SLOTS = [
    { channel: 5, name: "SF2 + Freeverb (Preset 1)" },
    { channel: 6, name: "DX7 + Freeverb" },
    { channel: 7, name: "OB-Xd + Freeverb" },
    { channel: 8, name: "JV-880 + Freeverb" }
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
    HIERARCHY_EDITOR: "hierarch" // Hierarchy-based parameter editor
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
    "none": "--"
};

/* In-memory chain configuration (for future save/load) */
function createEmptyChainConfig() {
    return {
        midiFx: null,    // { module: "chord", params: {} } or null
        synth: null,     // { module: "dx7", params: {} } or null
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
let redrawCounter = 0;
const REDRAW_INTERVAL = 2; // ~30fps at 16ms tick

/* Knob mapping state (overlay uses shared menu_layout.mjs) */
let knobMappings = [];       // {cc, name, value} for each knob
let lastKnobSlot = -1;       // Track slot changes to refresh mappings

/* Throttled knob overlay - only refresh value once per frame to avoid display lag */
let pendingKnobRefresh = false;  // True if we need to refresh overlay value
let pendingKnobIndex = -1;       // Which knob to refresh (-1 = none)

/* Throttled hierarchy knob adjustment - accumulate deltas, apply once per frame */
let pendingHierKnobIndex = -1;   // Which knob is being turned (-1 = none)
let pendingHierKnobDelta = 0;    // Accumulated delta to apply

/* Cached knob contexts - avoid IPC calls on every CC message */
let cachedKnobContexts = [];     // Array of 8 contexts (one per knob)
let cachedKnobContextsView = ""; // View when cache was built
let cachedKnobContextsSlot = -1; // Slot when cache was built
let cachedKnobContextsComp = -1; // Component when cache was built
let cachedKnobContextsLevel = ""; // Hierarchy level when cache was built

/* Master FX state */
let selectedMasterFx = 0;    // Index into MASTER_FX_OPTIONS
let currentMasterFxId = "";  // Currently loaded master FX module ID
let currentMasterFxPath = ""; // Full path to currently loaded DSP

/* Slot settings definitions */
const SLOT_SETTINGS = [
    { key: "patch", label: "Patch", type: "action" },  // Opens patch browser
    { key: "chain", label: "Edit Chain", type: "action" },  // Opens chain editor
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 1, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -1, max: 15, step: 1 },  // -1 = none, 0-15 = ch 1-16
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

/* Chain settings (shown when Settings component is selected) */
const CHAIN_SETTINGS_ITEMS = [
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 1, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -1, max: 15, step: 1 }  // -1 = none, 0-15 = ch 1-16
];
let selectedChainSetting = 0;
let editingChainSettingValue = false;

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
let hierEditorParams = [];        // current level's params
let hierEditorKnobs = [];         // current level's knob-mapped params
let hierEditorSelectedIdx = 0;
let hierEditorEditMode = false;   // true when editing a param value
let hierEditorChainParams = [];   // metadata from chain_params

/* Preset browser state (for preset_browser type levels) */
let hierEditorIsPresetLevel = false;  // true when current level is a preset browser
let hierEditorPresetCount = 0;
let hierEditorPresetIndex = 0;
let hierEditorPresetName = "";
let hierEditorPresetEditMode = false; // true when editing params within a preset browser level

/* Loaded module UI state */
let loadedModuleUi = null;       // The chain_ui object from loaded module
let loadedModuleSlot = -1;       // Which slot the module UI is for
let loadedModuleComponent = "";  // "synth", "fx1", "fx2"
let moduleUiLoadError = false;   // True if load failed

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
            if (stat && stat[0] === 0) {
                return uiPath;
            }
        } catch (e) {
            /* File doesn't exist */
        }

        /* Fall back to ui.js (standard module UI) */
        uiPath = `${moduleDir}/ui.js`;
        try {
            const stat = os.stat(uiPath);
            if (stat && stat[0] === 0) {
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
        `${MODULES_ROOT}/chain/sound_generators/${moduleId}`, /* Chain synths */
        `${MODULES_ROOT}/chain/audio_fx/${moduleId}`,        /* Chain FX */
        `${MODULES_ROOT}/chain/midi_fx/${moduleId}`          /* Chain MIDI FX */
    ];

    for (const dir of searchDirs) {
        const result = checkDir(dir);
        if (result) return result;
    }

    return null;
}

/* Set up shims for host_module_get_param and host_module_set_param
 * These route to the correct slot and component in shadow mode */
function setupModuleParamShims(slot, componentKey) {
    const prefix = componentKey === "midiFx" ? "midi_fx" : componentKey;

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

/* Initialize chain configs for all slots */
function initChainConfigs() {
    chainConfigs = [];
    for (let i = 0; i < SHADOW_UI_SLOTS; i++) {
        chainConfigs.push(createEmptyChainConfig());
    }
}

/* Load chain config from current patch info */
function loadChainConfigFromSlot(slotIndex) {
    const cfg = chainConfigs[slotIndex] || createEmptyChainConfig();

    /* Read current patch configuration from DSP
     * Note: get_param uses underscores (synth_module), set_param uses colons (synth:module) */
    const synthModule = getSlotParam(slotIndex, "synth_module");
    const midiFxModule = getSlotParam(slotIndex, "midi_fx_type");
    const fx1Module = getSlotParam(slotIndex, "fx1_module");
    const fx2Module = getSlotParam(slotIndex, "fx2_module");

    /* Debug: track what DSP returned */
    globalThis._debugLoad = `syn:${synthModule || '-'} fx1:${fx1Module || '-'}`;

    cfg.synth = synthModule && synthModule !== "" ? { module: synthModule.toLowerCase(), params: {} } : null;
    cfg.midiFx = midiFxModule && midiFxModule !== "" ? { module: midiFxModule.toLowerCase(), params: {} } : null;
    cfg.fx1 = fx1Module && fx1Module !== "" ? { module: fx1Module.toLowerCase(), params: {} } : null;
    cfg.fx2 = fx2Module && fx2Module !== "" ? { module: fx2Module.toLowerCase(), params: {} } : null;

    chainConfigs[slotIndex] = cfg;
    return cfg;
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

/* Scan modules directory for audio_fx modules */
function scanForAudioFxModules() {
    const MODULES_DIR = "/data/UserData/move-anything/modules";
    const CHAIN_AUDIO_FX_DIR = `${MODULES_DIR}/chain/audio_fx`;
    const result = [{ id: "", name: "None", dspPath: "" }];

    /* Helper to scan a directory for audio_fx modules */
    function scanDir(dirPath, pathPrefix) {
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
                        /* Build DSP path - chain audio_fx use {id}.so, top-level use dsp.so */
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

    /* Scan top-level modules */
    scanDir(MODULES_DIR, "");

    /* Scan chain/audio_fx for built-in and installed chain effects */
    scanDir(CHAIN_AUDIO_FX_DIR, "chain/audio_fx/");

    return result;
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
                componentKey === "fx2" ? "fx2:chain_params" : null;
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
                componentKey === "fx2" ? "fx2:ui_hierarchy" : null;
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
    view = VIEWS.COMPONENT_PARAMS;
    needsRedraw = true;
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
        val += delta * 0.05;  // 5% step for floats
    } else {
        val = parseInt(param.value) || 0;
        val += delta;
    }

    /* Clamp to range */
    val = Math.max(param.min, Math.min(param.max, val));

    if (param.type === "float") {
        return val.toFixed(2);
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
    return data.patches.map((entry, idx) => ({
        channel: (typeof entry.channel === "number") ? entry.channel : (DEFAULT_SLOTS[idx]?.channel ?? 5 + idx),
        name: (typeof entry.name === "string") ? entry.name : (DEFAULT_SLOTS[idx]?.name ?? "Unknown")
    }));
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
        patches: nextSlots.map((slot) => ({
            name: slot.name,
            channel: slot.channel
        })),
        master_fx: currentMasterFxId || ""
    };
    try {
        const file = std.open(CONFIG_PATH, "w");
        if (!file) return;
        file.puts(JSON.stringify(payload, null, 2));
        file.puts("\n");
        file.close();
    } catch (e) {
        /* ignore */
    }
}

function saveConfigMasterFx() {
    /* Save just the master FX setting to config */
    const data = safeLoadJson(CONFIG_PATH);
    const payload = {
        patches: data && Array.isArray(data.patches) ? data.patches : slots.map((slot) => ({
            name: slot.name,
            channel: slot.channel
        })),
        master_fx: currentMasterFxId || "",
        master_fx_path: currentMasterFxPath || ""
    };
    try {
        const file = std.open(CONFIG_PATH, "w");
        if (!file) return;
        file.puts(JSON.stringify(payload, null, 2));
        file.puts("\n");
        file.close();
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
    if (Array.isArray(hostSlots) && hostSlots.length) {
        slots = hostSlots.map((slot, idx) => ({
            channel: slot.channel || (DEFAULT_SLOTS[idx] ? DEFAULT_SLOTS[idx].channel : 5 + idx),
            name: slot.name || (DEFAULT_SLOTS[idx] ? DEFAULT_SLOTS[idx].name : "Unknown Patch")
        }));
    } else {
        slots = loadSlotsFromConfig();
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
    /* Add "none" as first option to clear a slot */
    patches = [{ name: "none", file: null }, ...entries];
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
    view = VIEWS.PATCHES;
    needsRedraw = true;
}

function enterPatchDetail(slotIndex, patchIndex) {
    selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    selectedPatch = patchIndex;
    selectedDetailItem = 0;
    fetchPatchDetail(slotIndex);
    view = VIEWS.PATCH_DETAIL;
    needsRedraw = true;
}

/* Special patch index value meaning "none" / clear the slot - must match shim */
const PATCH_INDEX_NONE = 65535;

function applyPatchSelection() {
    const patch = patches[selectedPatch];
    const slot = slots[selectedSlot];
    if (!patch || !slot) return;
    slot.name = patch.name;
    saveSlotsToConfig(slots);
    if (typeof shadow_request_patch === "function") {
        try {
            /* "none" is at index 0 in patches array, use special value 65535
             * Real patches start at index 1, so subtract 1 for shim's index */
            const patchIndex = patch.name === "none" ? PATCH_INDEX_NONE : selectedPatch - 1;
            shadow_request_patch(selectedSlot, patchIndex);
        } catch (e) {
            /* ignore */
        }
    }
    /* Refresh detail info after loading patch */
    fetchPatchDetail(selectedSlot);
    view = VIEWS.SLOTS;
    needsRedraw = true;
}

/* Enter slot settings view */
function enterSlotSettings(slotIndex) {
    selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    selectedSetting = 0;
    editingSettingValue = false;
    view = VIEWS.SLOT_SETTINGS;
    needsRedraw = true;
}

function enterMasterFxSettings() {
    /* Scan for available audio_fx modules */
    MASTER_FX_OPTIONS = scanForAudioFxModules();
    /* Load current master FX selection */
    currentMasterFxId = getMasterFxModule();
    /* Find index in options */
    selectedMasterFx = MASTER_FX_OPTIONS.findIndex(o => o.id === currentMasterFxId);
    if (selectedMasterFx < 0) selectedMasterFx = 0;
    view = VIEWS.MASTER_FX;
    needsRedraw = true;
}

function applyMasterFxSelection() {
    const selected = MASTER_FX_OPTIONS[selectedMasterFx];
    if (selected) {
        /* Pass the full DSP path to the shim */
        setMasterFxModule(selected.dspPath || "");
        currentMasterFxId = selected.id;
        /* Save to config - save the dspPath for persistence */
        currentMasterFxPath = selected.dspPath || "";
        saveConfigMasterFx();
    }
    /* Return to slots view */
    view = VIEWS.SLOTS;
    needsRedraw = true;
}

/* Enter chain editing view for a slot */
function enterChainEdit(slotIndex) {
    selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    selectedChainComponent = 0;  // Start at MIDI FX (scroll left for Chain/patch)
    /* Load current chain config from DSP */
    loadChainConfigFromSlot(slotIndex);
    view = VIEWS.CHAIN_EDIT;
    needsRedraw = true;
}

/* Scan modules directory for modules of a specific component type */
function scanModulesForType(componentType) {
    const MODULES_DIR = "/data/UserData/move-anything/modules";
    const CHAIN_SUBDIR = `${MODULES_DIR}/chain`;
    const result = [{ id: "", name: "None" }];

    /* Map component type to directory and expected component_type */
    let searchDirs = [];
    let expectedTypes = [];

    if (componentType === "synth") {
        searchDirs = [MODULES_DIR, `${CHAIN_SUBDIR}/sound_generators`];
        expectedTypes = ["sound_generator"];
    } else if (componentType === "midiFx") {
        searchDirs = [`${CHAIN_SUBDIR}/midi_fx`];
        expectedTypes = ["midi_fx"];
    } else if (componentType === "fx1" || componentType === "fx2") {
        searchDirs = [MODULES_DIR, `${CHAIN_SUBDIR}/audio_fx`];
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

    return result;
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

    view = VIEWS.COMPONENT_SELECT;
    needsRedraw = true;
}

/* Apply the selected module to the component - updates DSP in realtime */
function applyComponentSelection() {
    const comp = CHAIN_COMPONENTS[selectedChainComponent];
    const selected = availableModules[selectedModuleIndex];

    if (!comp || comp.key === "settings") {
        view = VIEWS.CHAIN_EDIT;
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
            /* MIDI FX handled differently - uses chord/arp type params */
            /* TODO: implement midi_fx:type or similar */
            break;
    }

    if (paramKey) {
        const success = setSlotParam(selectedSlot, paramKey, moduleId);
        if (!success) {
            print(2, 50, "Failed to apply", 1);
        }
    }

    view = VIEWS.CHAIN_EDIT;
    needsRedraw = true;
}

/* Enter chain settings view */
function enterChainSettings(slotIndex) {
    selectedSlot = slotIndex;
    selectedChainSetting = 0;
    editingChainSettingValue = false;
    view = VIEWS.CHAIN_SETTINGS;
    needsRedraw = true;
}

/* Get current value for a chain setting */
function getChainSettingValue(slot, setting) {
    const val = getSlotParam(slot, setting.key);
    if (val === null) return "-";

    if (setting.key === "slot:volume") {
        const pct = Math.round(parseFloat(val) * 100);
        return `${pct}%`;
    }
    if (setting.key === "slot:forward_channel") {
        const ch = parseInt(val);
        return ch < 0 ? "Off" : `Ch ${ch + 1}`;  // Internal 0-15 → display 1-16
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
        view = VIEWS.COMPONENT_EDIT;
        needsRedraw = true;
        return;
    }

    /* Fall back to simple preset browser */
    const prefix = componentKey === "midiFx" ? "midi_fx" : componentKey;

    /* Fetch preset count and current preset */
    const countStr = getSlotParam(slotIndex, `${prefix}:preset_count`);
    editComponentPresetCount = countStr ? parseInt(countStr) : 0;

    const presetStr = getSlotParam(slotIndex, `${prefix}:preset`);
    editComponentPreset = presetStr ? parseInt(presetStr) : 0;

    /* Fetch preset name */
    editComponentPresetName = getSlotParam(slotIndex, `${prefix}:preset_name`) || "";

    view = VIEWS.COMPONENT_EDIT;
    needsRedraw = true;
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
    hierEditorSelectedIdx = 0;
    hierEditorEditMode = false;

    /* Fetch chain_params metadata for this component */
    hierEditorChainParams = getComponentChainParams(slotIndex, componentKey);

    /* Set up param shims for this component */
    setupModuleParamShims(slotIndex, componentKey);

    /* Load current level's params and knobs */
    loadHierarchyLevel();

    view = VIEWS.HIERARCHY_EDITOR;
    needsRedraw = true;
}

/* Load params and knobs for current hierarchy level */
function loadHierarchyLevel() {
    if (!hierEditorHierarchy) return;

    const levels = hierEditorHierarchy.levels;
    const levelDef = hierEditorLevel ? levels[hierEditorLevel] : null;

    if (!levelDef) {
        /* At mode selection level */
        hierEditorParams = hierEditorHierarchy.modes || [];
        hierEditorKnobs = [];
        hierEditorIsPresetLevel = false;
        return;
    }

    /* Check if this is a preset browser level */
    if (levelDef.list_param && levelDef.count_param) {
        hierEditorIsPresetLevel = true;
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

        /* Also load params for preset edit mode (append swap action at end) */
        hierEditorParams = [...(levelDef.params || []), SWAP_MODULE_ACTION];
    } else {
        hierEditorIsPresetLevel = false;
        hierEditorPresetEditMode = false;
        /* Append swap module action to params list */
        hierEditorParams = [...(levelDef.params || []), SWAP_MODULE_ACTION];
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
}

/* Exit hierarchy editor */
function exitHierarchyEditor() {
    /* Clear pending knob state to prevent stale overlays */
    pendingHierKnobIndex = -1;
    pendingHierKnobDelta = 0;

    clearModuleParamShims();
    hierEditorSlot = -1;
    hierEditorComponent = "";
    hierEditorHierarchy = null;
    hierEditorChainParams = [];
    hierEditorIsPresetLevel = false;
    hierEditorPresetEditMode = false;
    view = VIEWS.CHAIN_EDIT;
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
    /* Float: show as percentage if 0-1 range */
    const min = meta && typeof meta.min === "number" ? meta.min : 0;
    const max = meta && typeof meta.max === "number" ? meta.max : 1;
    if (min === 0 && max === 1) {
        return Math.round(val * 100) + "%";
    }
    return val.toFixed(2);
}

/* Adjust selected param value via jog */
function adjustHierSelectedParam(delta) {
    if (hierEditorSelectedIdx >= hierEditorParams.length) return;

    const param = hierEditorParams[hierEditorSelectedIdx];
    const key = typeof param === "string" ? param : param.key || param;

    /* Skip special actions */
    if (key === SWAP_MODULE_ACTION) return;
    const fullKey = `${hierEditorComponent}:${key}`;

    const currentVal = getSlotParam(hierEditorSlot, fullKey);
    if (currentVal === null) return;

    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    /* Get step from metadata - default 1 for int, 0.02 for float */
    const meta = getParamMetadata(key);
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
}

/*
 * Build knob context for a single knob - internal, called by rebuildKnobContextCache
 */
function buildKnobContextForKnob(knobIndex) {
    /* Hierarchy editor context */
    if (view === VIEWS.HIERARCHY_EDITOR && knobIndex < hierEditorKnobs.length) {
        const key = hierEditorKnobs[knobIndex];
        const fullKey = `${hierEditorComponent}:${key}`;
        const meta = getParamMetadata(key);
        const pluginName = getSlotParam(hierEditorSlot, `${hierEditorComponent}:name`) || "";
        const displayName = meta && meta.name ? meta.name : key.replace(/_/g, " ");
        return {
            slot: hierEditorSlot,
            key,
            fullKey,
            meta,
            pluginName,
            displayName,
            title: `${pluginName} ${displayName}`
        };
    }

    /* Chain editor with component selected */
    if (view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0 && selectedChainComponent < CHAIN_COMPONENTS.length) {
        const comp = CHAIN_COMPONENTS[selectedChainComponent];
        if (comp && comp.key !== "settings") {
            const hierarchy = getComponentHierarchy(selectedSlot, comp.key);
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
                    const fullKey = `${comp.key}:${key}`;
                    const chainParams = getComponentChainParams(selectedSlot, comp.key);
                    const meta = chainParams.find(p => p.key === key);
                    const pluginName = getSlotParam(selectedSlot, `${comp.key}:name`) || comp.label;
                    const displayName = meta && meta.name ? meta.name : key.replace(/_/g, " ");
                    return {
                        slot: selectedSlot,
                        key,
                        fullKey,
                        meta,
                        pluginName,
                        displayName,
                        title: `${pluginName} ${displayName}`
                    };
                }
            }
            /* Component selected but no knob mappings - return generic context */
            const pluginName = getSlotParam(selectedSlot, `${comp.key}:name`) || comp.label;
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
}

/*
 * Unified knob context resolution - used by both touch (peek) and turn (adjust)
 * Returns context object or null if no mapping exists for this knob
 * Uses caching to avoid IPC calls on every CC message
 */
function getKnobContext(knobIndex) {
    /* Check if cache is valid */
    const currentSlot = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorSlot : selectedSlot;
    const currentComp = (view === VIEWS.HIERARCHY_EDITOR) ? -1 : selectedChainComponent;
    const currentLevel = (view === VIEWS.HIERARCHY_EDITOR) ? hierEditorLevel : "";

    const cacheValid = (
        cachedKnobContexts.length === NUM_KNOBS &&
        cachedKnobContextsView === view &&
        cachedKnobContextsSlot === currentSlot &&
        cachedKnobContextsComp === currentComp &&
        cachedKnobContextsLevel === currentLevel
    );

    if (!cacheValid) {
        rebuildKnobContextCache();
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
        if (ctx.noMapping) {
            /* Generic label for unmapped knob */
            showOverlay(ctx.title, ctx.displayName);
        } else if (ctx.fullKey) {
            /* Mapped knob - show value */
            let displayVal;
            if (value !== undefined) {
                displayVal = formatParamForOverlay(value, ctx.meta);
            } else {
                const currentVal = getSlotParam(ctx.slot, ctx.fullKey);
                const num = parseFloat(currentVal);
                displayVal = !isNaN(num) ? formatParamForOverlay(num, ctx.meta) : (currentVal || "-");
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
    const ctx = getKnobContext(knobIndex);

    if (ctx) {
        if (ctx.noMapping || !ctx.fullKey) {
            /* No mapping - just show generic overlay */
            showOverlay(ctx.title, ctx.displayName);
            needsRedraw = true;
            return true;
        }

        /* Accumulate delta for throttled processing */
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
    if (pendingHierKnobIndex < 0 || pendingHierKnobDelta === 0) {
        /* No pending adjustment, but still show overlay if knob active */
        if (pendingHierKnobIndex >= 0) {
            const ctx = getKnobContext(pendingHierKnobIndex);
            if (ctx && ctx.fullKey) {
                const currentVal = getSlotParam(ctx.slot, ctx.fullKey);
                if (currentVal !== null) {
                    showKnobOverlay(pendingHierKnobIndex, parseFloat(currentVal));
                }
            }
        }
        return;
    }

    const knobIndex = pendingHierKnobIndex;
    const delta = pendingHierKnobDelta;
    pendingHierKnobDelta = 0;  /* Clear accumulated delta */

    const ctx = getKnobContext(knobIndex);
    if (!ctx || ctx.noMapping || !ctx.fullKey) return;

    /* Get current value */
    const currentVal = getSlotParam(ctx.slot, ctx.fullKey);
    if (currentVal === null) return;

    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    /* Calculate step and bounds from metadata */
    const isInt = ctx.meta && ctx.meta.type === "int";
    const step = ctx.meta && ctx.meta.step ? ctx.meta.step : (isInt ? 1 : 0.02);
    const min = ctx.meta && typeof ctx.meta.min === "number" ? ctx.meta.min : 0;
    const max = ctx.meta && typeof ctx.meta.max === "number" ? ctx.meta.max : 1;

    /* Apply accumulated delta and clamp */
    const newVal = Math.max(min, Math.min(max, num + delta * step));

    /* Set the new value */
    setSlotParam(ctx.slot, ctx.fullKey, formatParamForSet(newVal, ctx.meta));

    /* Show overlay with new value */
    showKnobOverlay(knobIndex, newVal);
}

/* Format a value for display in hierarchy editor */
function formatHierDisplayValue(key, val) {
    const meta = getParamMetadata(key);
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

    /* Build breadcrumb header with plugin name */
    const componentName = hierEditorComponent === "synth" ? "Synth" :
                          hierEditorComponent === "fx1" ? "FX1" :
                          hierEditorComponent === "fx2" ? "FX2" : hierEditorComponent;

    /* Get plugin display name */
    const pluginName = getSlotParam(hierEditorSlot, `${hierEditorComponent}:name`) || "";

    /* Check for mode indicator - show * for performance mode */
    let modeIndicator = "";
    if (hierEditorHierarchy && hierEditorHierarchy.modes && hierEditorHierarchy.mode_param) {
        const modeVal = getSlotParam(hierEditorSlot, `${hierEditorComponent}:${hierEditorHierarchy.mode_param}`);
        const modeIndex = modeVal !== null ? parseInt(modeVal) : 0;
        /* modes[1] is typically "performance" - show * indicator */
        if (modeIndex === 1) {
            modeIndicator = "*";
        }
    }

    let breadcrumb = `S${hierEditorSlot + 1} ${componentName}`;
    if (pluginName) {
        breadcrumb += ` ${pluginName}${modeIndicator}`;
    }
    if (hierEditorPath.length > 0) {
        breadcrumb += " > " + hierEditorPath.join(" > ");
    }

    drawHeader(truncateText(breadcrumb, 24));

    /* Check if this is a preset browser level (and not in edit mode) */
    if (hierEditorIsPresetLevel && !hierEditorPresetEditMode) {
        /* Draw preset browser UI */
        const centerY = 32;

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
        drawFooter("Jog:browse  Push:edit");
    } else {
        /* Draw param list */
        if (hierEditorParams.length === 0) {
            print(4, 24, "No parameters", 1);
        } else {
            /* Build items with labels and values */
            const items = hierEditorParams.map(param => {
                const key = typeof param === "string" ? param : param.key || param;

                /* Handle special swap module action */
                if (key === SWAP_MODULE_ACTION) {
                    return { label: "[Swap module...]", value: "", key, isAction: true };
                }

                const meta = getParamMetadata(key);
                const label = meta && meta.name ? meta.name : key.replace(/_/g, " ");
                const val = getSlotParam(hierEditorSlot, `${hierEditorComponent}:${key}`);
                const displayVal = val !== null ? formatHierDisplayValue(key, val) : "";
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
        const hint = hierEditorEditMode ? "Jog:adjust  Push:done" : "Jog:scroll  Push:edit";
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
    const prefix = editingComponentKey === "midiFx" ? "midi_fx" : editingComponentKey;
    setSlotParam(selectedSlot, `${prefix}:preset`, String(newPreset));

    /* Update local state */
    editComponentPreset = newPreset;

    /* Fetch new preset name */
    editComponentPresetName = getSlotParam(selectedSlot, `${prefix}:preset_name`) || "";
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
        return isNaN(num) ? val : `${Math.round(num * 100)}%`;
    }
    if (setting.key === "slot:forward_channel") {
        const ch = parseInt(val);
        /* -1 means no remapping, otherwise forward to specific channel */
        return ch < 0 ? "Off" : `Ch ${ch + 1}`;
    }
    if (setting.key === "slot:receive_channel") {
        return `Ch ${val}`;
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

/* Update the focused slot in shared memory for knob CC routing */
function updateFocusedSlot(slot) {
    if (typeof shadow_set_focused_slot === "function") {
        shadow_set_focused_slot(slot);
    }
}

function handleJog(delta) {
    hideOverlay();
    switch (view) {
        case VIEWS.SLOTS:
            /* 5 items: 4 slots + Master FX */
            selectedSlot = Math.max(0, Math.min(slots.length, selectedSlot + delta));
            /* Only update focused slot for actual slots (0-3), not Master FX (4) */
            if (selectedSlot < slots.length) {
                updateFocusedSlot(selectedSlot);
            }
            break;
        case VIEWS.MASTER_FX:
            selectedMasterFx = Math.max(0, Math.min(MASTER_FX_OPTIONS.length - 1, selectedMasterFx + delta));
            break;
        case VIEWS.SLOT_SETTINGS:
            if (editingSettingValue) {
                /* Adjust the setting value */
                const setting = SLOT_SETTINGS[selectedSetting];
                adjustSlotSetting(selectedSlot, setting, delta);
            } else {
                /* Navigate settings list */
                selectedSetting = Math.max(0, Math.min(SLOT_SETTINGS.length - 1, selectedSetting + delta));
            }
            break;
        case VIEWS.PATCHES:
            selectedPatch = Math.max(0, Math.min(patches.length - 1, selectedPatch + delta));
            break;
        case VIEWS.PATCH_DETAIL:
            const detailItems = getDetailItems();
            selectedDetailItem = Math.max(0, Math.min(detailItems.length - 1, selectedDetailItem + delta));
            break;
        case VIEWS.COMPONENT_PARAMS:
            if (editingValue && componentParams.length > 0) {
                /* Adjusting value - modify the current param */
                const param = componentParams[selectedParam];
                const newVal = adjustParamValue(param, delta);
                param.value = newVal;
                /* Apply immediately */
                setSlotParam(selectedSlot, param.key, newVal);
            } else {
                /* Selecting param */
                selectedParam = Math.max(0, Math.min(componentParams.length - 1, selectedParam + delta));
            }
            break;
        case VIEWS.CHAIN_EDIT:
            /* Navigate horizontally through chain components (-1 = chain/patch selection) */
            selectedChainComponent = Math.max(-1, Math.min(CHAIN_COMPONENTS.length - 1, selectedChainComponent + delta));
            break;
        case VIEWS.COMPONENT_SELECT:
            /* Navigate available modules list */
            selectedModuleIndex = Math.max(0, Math.min(availableModules.length - 1, selectedModuleIndex + delta));
            break;
        case VIEWS.CHAIN_SETTINGS:
            if (editingChainSettingValue) {
                /* Adjust the setting value */
                const setting = CHAIN_SETTINGS_ITEMS[selectedChainSetting];
                adjustChainSetting(selectedSlot, setting, delta);
            } else {
                /* Navigate settings list */
                selectedChainSetting = Math.max(0, Math.min(CHAIN_SETTINGS_ITEMS.length - 1, selectedChainSetting + delta));
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
            } else if (hierEditorEditMode) {
                /* Adjust selected param value */
                adjustHierSelectedParam(delta);
            } else {
                /* Scroll param list (includes preset edit mode) */
                hierEditorSelectedIdx = Math.max(0, Math.min(hierEditorParams.length - 1, hierEditorSelectedIdx + delta));
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
            /* Apply master FX selection */
            applyMasterFxSelection();
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
                /* Load patch directly and return to slots */
                applyPatchSelection();
            }
            break;
        case VIEWS.PATCH_DETAIL:
            const detailItems = getDetailItems();
            const item = detailItems[selectedDetailItem];
            if (item.component && item.editable) {
                /* Enter component param editor */
                enterComponentParams(selectedSlot, item.component);
            } else if (selectedDetailItem === detailItems.length - 1) {
                /* "Load Patch" selected - apply and return to slots */
                applyPatchSelection();
            }
            break;
        case VIEWS.COMPONENT_PARAMS:
            if (componentParams.length > 0) {
                /* Toggle between selecting and editing */
                editingValue = !editingValue;
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
            applyComponentSelection();
            break;
        case VIEWS.CHAIN_SETTINGS:
            /* Toggle editing mode for value settings */
            editingChainSettingValue = !editingChainSettingValue;
            break;
        case VIEWS.HIERARCHY_EDITOR:
            /* Check for mode selection (hierEditorLevel is null when modes exist) */
            if (!hierEditorLevel && hierEditorHierarchy.modes) {
                /* Select mode and navigate into it */
                const selectedMode = hierEditorParams[hierEditorSelectedIdx];
                if (selectedMode && hierEditorHierarchy.levels[selectedMode]) {
                    /* If hierarchy specifies mode_param, set it to the mode index */
                    if (hierEditorHierarchy.mode_param) {
                        const modeIndex = hierEditorHierarchy.modes.indexOf(selectedMode);
                        if (modeIndex >= 0) {
                            setSlotParam(hierEditorSlot, `${hierEditorComponent}:${hierEditorHierarchy.mode_param}`, String(modeIndex));
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
                    hierEditorLevel = levelDef.children;
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                } else {
                    /* No children - enter preset edit mode to show params/swap */
                    hierEditorPresetEditMode = true;
                    hierEditorSelectedIdx = 0;
                }
            } else if (hierEditorPresetEditMode || !hierEditorIsPresetLevel) {
                /* On params level - check for special actions */
                const selectedParam = hierEditorParams[hierEditorSelectedIdx];
                if (selectedParam === SWAP_MODULE_ACTION) {
                    /* Swap module - find component index and enter module select */
                    const compIndex = CHAIN_COMPONENTS.findIndex(c => c.key === hierEditorComponent);
                    const slotToSwap = hierEditorSlot;  /* Save before exit clears it */
                    if (compIndex >= 0) {
                        exitHierarchyEditor();
                        enterComponentSelect(slotToSwap, compIndex);
                    }
                } else {
                    /* Normal param - toggle edit mode */
                    hierEditorEditMode = !hierEditorEditMode;
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
            } else {
                /* Return to slots list */
                view = VIEWS.SLOTS;
                needsRedraw = true;
            }
            break;
        case VIEWS.PATCHES:
            /* Return to chain editor */
            view = VIEWS.CHAIN_EDIT;
            needsRedraw = true;
            break;
        case VIEWS.PATCH_DETAIL:
            view = VIEWS.PATCHES;
            needsRedraw = true;
            break;
        case VIEWS.COMPONENT_PARAMS:
            if (editingValue) {
                /* Exit value editing mode */
                editingValue = false;
                needsRedraw = true;
            } else {
                /* Return to patch detail, refresh info */
                fetchPatchDetail(selectedSlot);
                view = VIEWS.PATCH_DETAIL;
                needsRedraw = true;
            }
            break;
        case VIEWS.MASTER_FX:
            /* Return to slots list */
            view = VIEWS.SLOTS;
            needsRedraw = true;
            break;
        case VIEWS.CHAIN_EDIT:
            /* Return to slot list */
            view = VIEWS.SLOTS;
            needsRedraw = true;
            break;
        case VIEWS.COMPONENT_SELECT:
            /* Return to chain edit */
            view = VIEWS.CHAIN_EDIT;
            needsRedraw = true;
            break;
        case VIEWS.CHAIN_SETTINGS:
            if (editingChainSettingValue) {
                /* Exit value editing mode */
                editingChainSettingValue = false;
                needsRedraw = true;
            } else {
                /* Return to chain edit */
                view = VIEWS.CHAIN_EDIT;
                needsRedraw = true;
            }
            break;
        case VIEWS.COMPONENT_EDIT:
            /* Unload module UI and return to chain edit */
            unloadModuleUi();
            view = VIEWS.CHAIN_EDIT;
            needsRedraw = true;
            break;
        case VIEWS.HIERARCHY_EDITOR:
            if (hierEditorEditMode) {
                /* Exit param edit mode first */
                hierEditorEditMode = false;
                needsRedraw = true;
            } else if (hierEditorPresetEditMode) {
                /* Exit preset edit mode - return to preset browser */
                hierEditorPresetEditMode = false;
                needsRedraw = true;
            } else if (hierEditorPath.length > 0) {
                /* Go back to parent level */
                hierEditorPath.pop();

                /* Check if current level is a mode (top-level) - go back to mode selection */
                if (hierEditorHierarchy.modes && hierEditorHierarchy.modes.includes(hierEditorLevel)) {
                    hierEditorLevel = null;  // Return to mode selection
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                    needsRedraw = true;
                } else {
                    /* Find the parent level that has children pointing to current level */
                    const levels = hierEditorHierarchy.levels;
                    let parentLevel = "root";
                    for (const [name, def] of Object.entries(levels)) {
                        if (def.children === hierEditorLevel) {
                            parentLevel = name;
                            break;
                        }
                    }
                    hierEditorLevel = parentLevel;
                    hierEditorSelectedIdx = 0;
                    loadHierarchyLevel();
                    needsRedraw = true;
                }
            } else {
                /* At root level - exit hierarchy editor */
                exitHierarchyEditor();
            }
            break;
    }
}

/* Handle knob turn - mark for throttled overlay refresh
 * CC messages still flow to DSP normally; we just throttle the display update
 * to once per frame to avoid lag when turning knobs quickly */
function handleKnobTurn(knobIndex, value) {
    pendingKnobRefresh = true;
    pendingKnobIndex = knobIndex;
    needsRedraw = true;
}

/* Refresh knob overlay value - called once per tick to avoid display lag */
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
    }

    /* Get current value from DSP (only once per frame) */
    const newValue = getSlotParam(targetSlot, `knob_${pendingKnobIndex + 1}_value`);
    if (knobMappings[pendingKnobIndex]) {
        knobMappings[pendingKnobIndex].value = newValue || "-";
    }

    /* Show overlay using shared overlay system */
    const mapping = knobMappings[pendingKnobIndex];
    if (mapping) {
        const displayName = `S${targetSlot + 1}: ${mapping.name}`;
        showOverlay(displayName, mapping.value);
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
        ...slots.map((s, i) => ({
            label: (i === trackSelectedSlot ? "*" : " ") + (s.name || "Unknown Patch"),
            value: `Ch${s.channel}`,
            isSlot: true
        })),
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
        drawFooter("Jog: adjust  Click: done");
    } else {
        drawFooter("Click: edit  Back: slots");
    }
}

function drawPatches() {
    clear_screen();
    const channel = slots[selectedSlot]?.channel || (DEFAULT_SLOTS[selectedSlot]?.channel ?? 5 + selectedSlot);
    drawHeader(`Ch${channel} Patch`);
    if (patches.length === 0) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No patches found", 1);
        drawFooter("Back: settings");
    } else {
        drawMenuList({
            items: patches,
            selectedIndex: selectedPatch,
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            getLabel: (item) => item.name
        });
        drawFooter("Click: load  Back: settings");
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

    drawFooter("Click: edit  Back: list");
}

function drawComponentParams() {
    clear_screen();

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
        drawFooter("Jog: adjust  Click: done");
    } else {
        drawFooter("Click: edit  Back: detail");
    }
}

/* Draw horizontal chain editor with boxed icons */
function drawChainEdit() {
    clear_screen();
    const slotName = slots[selectedSlot]?.name || "Unknown";
    /* Show slot number and preset name in header */
    const headerText = truncateText(`S${selectedSlot + 1} ${slotName}`, 24);
    drawHeader(headerText);

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
            const prefix = selectedComp.key === "midiFx" ? "midi_fx" : selectedComp.key;
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

/* Draw component edit view (presets, params) */
function drawComponentEdit() {
    clear_screen();

    /* Get component info */
    const cfg = chainConfigs[selectedSlot];
    const moduleData = cfg && cfg[editingComponentKey];
    const moduleName = moduleData ? moduleData.module.toUpperCase() : "Unknown";

    /* Get display name from DSP if available */
    const prefix = editingComponentKey === "midiFx" ? "midi_fx" : editingComponentKey;
    const displayName = getSlotParam(selectedSlot, `${prefix}:name`) || moduleName;

    drawHeader(truncateText(displayName, 20));

    const centerY = 32;

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
    const hint = "Jog: preset  Back: done";
    const hintX = Math.floor((SCREEN_WIDTH - hint.length * 5) / 2);
    print(hintX, 56, hint, 1);
}

/* Draw chain settings view */
function drawChainSettings() {
    clear_screen();
    drawHeader(`S${selectedSlot + 1} Settings`);

    const listY = LIST_TOP_Y;
    const lineHeight = 10;
    const maxVisible = Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / lineHeight);

    for (let i = 0; i < CHAIN_SETTINGS_ITEMS.length && i < maxVisible; i++) {
        const y = listY + i * lineHeight;
        const setting = CHAIN_SETTINGS_ITEMS[i];
        const isSelected = i === selectedChainSetting;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }

        const labelColor = isSelected ? 0 : 1;
        print(LIST_LABEL_X, y, setting.label, labelColor);

        /* Show value on the right */
        const value = getChainSettingValue(selectedSlot, setting);
        if (value) {
            const valueX = SCREEN_WIDTH - value.length * 5 - 4;
            if (isSelected && editingChainSettingValue) {
                /* Show editing indicator */
                print(valueX - 8, y, "<", 0);
                print(valueX, y, value, 0);
                print(valueX + value.length * 5 + 2, y, ">", 0);
            } else {
                print(valueX, y, value, labelColor);
            }
        }
    }
}

function drawMasterFx() {
    clear_screen();
    drawHeader("Master FX");
    drawMenuList({
        items: MASTER_FX_OPTIONS,
        selectedIndex: selectedMasterFx,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        getLabel: (item) => item.name,
        getValue: (item) => item.id === currentMasterFxId ? "*" : ""
    });
    drawFooter("Click: apply  Back: slots");
}

globalThis.init = function() {
    refreshSlots();
    loadPatchList();
    initChainConfigs();
    updateFocusedSlot(selectedSlot);
    fetchKnobMappings(selectedSlot);

    /* Load and apply master FX from config */
    const savedMasterFx = loadMasterFxFromConfig();
    if (savedMasterFx.path) {
        setMasterFxModule(savedMasterFx.path);
        currentMasterFxId = savedMasterFx.id;
        currentMasterFxPath = savedMasterFx.path;
    }
    /* Note: Jump-to-slot check moved to first tick() to avoid race condition */
};

globalThis.tick = function() {
    /* Check for jump-to-slot flag on EVERY tick (flag can be set while UI is running) */
    if (typeof shadow_get_ui_flags === "function") {
        const flags = shadow_get_ui_flags();
        globalThis._debugFlags = flags;  /* Debug: store for display */
        if (flags & SHADOW_UI_FLAG_JUMP_TO_SLOT) {
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
    }

    refreshCounter++;
    if (refreshCounter % 120 === 0) {
        refreshSlots();
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
    let currentTargetSlot = 0;
    if (typeof shadow_get_selected_slot === "function") {
        currentTargetSlot = shadow_get_selected_slot();
    }
    if (lastKnobSlot !== currentTargetSlot) {
        fetchKnobMappings(currentTargetSlot);
    }

    redrawCounter++;
    if (!needsRedraw && (redrawCounter % REDRAW_INTERVAL !== 0)) {
        return;
    }
    needsRedraw = false;
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
        default:
            drawSlots();
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

            /* Default (chain selected or settings): show overlay for slot's knob mapping */
            handleKnobTurn(knobIndex, d2);
            return;
        }

        /* Handle track button CCs (40-43) for slot selection
         * Track 1 (top) = CC 43 → slot 0, Track 4 (bottom) = CC 40 → slot 3 */
        if (d1 >= TRACK_CC_START && d1 <= TRACK_CC_END && d2 > 0) {
            const slotIndex = TRACK_CC_END - d1;
            if (slotIndex >= 0 && slotIndex < SHADOW_UI_SLOTS) {
                selectedSlot = slotIndex;
                updateFocusedSlot(slotIndex);
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
            if (pendingHierKnobIndex === knobIndex) {
                /* Process any remaining delta before clearing */
                processPendingHierKnob();
                pendingHierKnobIndex = -1;
                pendingHierKnobDelta = 0;
            }
            return;
        }
    }
};

globalThis.onMidiMessageExternal = function(_data) {
    /* ignore */
};
