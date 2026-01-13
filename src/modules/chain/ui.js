/*
 * Signal Chain Module UI
 *
 * Phase 2: Patch browser with synth display
 */

import * as std from 'std';
import * as os from 'os';
import { isCapacitiveTouchMessage } from '../../shared/input_filter.mjs';
import { MoveBack, MoveMenu, MoveSteps, MoveMainButton, MoveMainKnob } from '../../shared/constants.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults, showOverlay, tickOverlay, drawOverlay, isOverlayActive } from '../../shared/menu_layout.mjs';
import { midiFxRegistry } from './midi_fx/index.mjs';

/* State */
let patchName = "";
let patchCount = 0;
let currentPatch = 0;
let patchNames = [];
let selectedPatch = -1;  /* -1 = New Chain item */
let viewMode = "list";
let synthModule = "";
let presetName = "";
let polyphony = 0;
let octaveTranspose = -2;
let octaveInitialized = false;
let midiFxJsSpec = "";
let midiFxJsChain = [];
let rawMidi = false;
let midiInput = "both";
let midiSourceModule = "";
let midiSourceModuleLoaded = "";
let sourceUiActive = false;
let sourceUiReady = false;
let sourceUi = null;
let sourceUiLoadError = false;

/* Editor state */
let editorMode = false;
let editorState = null;
let editorError = "";  /* Validation error message */
let editorErrorTimeout = 0;
let availableComponents = {
    sound_generators: [],
    audio_fx: [],
    midi_fx: [],
    midi_sources: []
};

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* Knob feedback state */
let currentKnobMappings = [];  /* Array of {cc, target, param, currentValue, type, min, max} for current patch */
const KNOB_STEP_FLOAT = 0.05;  /* Step size for float params (must match DSP) */
const KNOB_STEP_INT = 1;       /* Step size for int params */

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* MIDI CCs */
const CC_JOG = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_UP = 55;
const CC_DOWN = 54;
const CC_MENU = MoveMenu;
const CC_BACK = MoveBack;

const hostFns = {
    setParam: host_module_set_param,
    getParam: host_module_get_param,
    sendMidi: host_module_send_midi
};

let modulesRoot = "";
let pendingSourceUi = null;

function sourceKey(key) {
    if (typeof key !== "string") return key;
    if (key.startsWith("source:")) return key;
    return `source:${key}`;
}

let sourceHostActive = false;

function enableSourceHostRouting() {
    if (sourceHostActive) return;
    globalThis.host_module_set_param = (key, val) => hostFns.setParam(sourceKey(key), val);
    globalThis.host_module_get_param = (key) => hostFns.getParam(sourceKey(key));
    globalThis.host_module_send_midi = hostFns.sendMidi;
    sourceHostActive = true;
}

function disableSourceHostRouting() {
    if (!sourceHostActive) return;
    globalThis.host_module_set_param = hostFns.setParam;
    globalThis.host_module_get_param = hostFns.getParam;
    globalThis.host_module_send_midi = hostFns.sendMidi;
    sourceHostActive = false;
}

function getModulesRoot() {
    if (modulesRoot) return modulesRoot;
    const current = host_get_current_module();
    if (!current || !current.ui_script) return "";
    const uiPath = current.ui_script;
    const chainDir = uiPath.slice(0, uiPath.lastIndexOf("/"));
    const rootIdx = chainDir.lastIndexOf("/");
    if (rootIdx < 0) return "";
    modulesRoot = chainDir.slice(0, rootIdx);
    return modulesRoot;
}

function getChainUiPath(moduleId) {
    const root = getModulesRoot();
    if (!root || !moduleId) return "";
    const moduleDir = `${root}/${moduleId}`;
    let chainUiPath = `${moduleDir}/ui_chain.js`;

    let moduleJson = null;
    try {
        moduleJson = std.loadFile(`${moduleDir}/module.json`);
    } catch (e) {
        moduleJson = null;
    }
    if (moduleJson) {
        const match = moduleJson.match(/\"ui_chain\"\\s*:\\s*\"([^\"]+)\"/);
        if (match && match[1]) {
            chainUiPath = `${moduleDir}/${match[1]}`;
        }
    }

    return chainUiPath;
}

/* Built-in JS MIDI FX (not DSP modules) */
const BUILTIN_MIDI_FX = [
    { id: "chord", name: "Chord", builtin: true, params: [
        { key: "type", name: "Chord Type", type: "enum", options: ["none", "major", "minor", "power", "octave"], default: "none" }
    ]},
    { id: "arp", name: "Arpeggiator", builtin: true, params: [
        { key: "mode", name: "Mode", type: "enum", options: ["off", "up", "down", "up_down", "random"], default: "off" },
        { key: "bpm", name: "BPM", type: "int", min: 40, max: 240, default: 120, step: 1 },
        { key: "division", name: "Division", type: "enum", options: ["1/4", "1/8", "1/16"], default: "1/16" }
    ]}
];

function parseModuleJson(jsonStr) {
    try {
        return JSON.parse(jsonStr);
    } catch (e) {
        return null;
    }
}

function scanModuleDir(path) {
    /* Read module.json from a directory */
    try {
        const jsonStr = std.loadFile(`${path}/module.json`);
        if (!jsonStr) return null;
        const mod = parseModuleJson(jsonStr);
        if (!mod) return null;
        if (!mod.capabilities || !mod.capabilities.chainable) return null;

        return {
            id: mod.id,
            name: mod.name || mod.id,
            component_type: mod.capabilities.component_type,
            params: mod.capabilities.chain_params || [],
            builtin: mod.builtin || false
        };
    } catch (e) {
        return null;
    }
}

function getInstalledModules() {
    /* Use host_list_modules() to get actually installed modules */
    const moduleIds = [];
    if (typeof host_list_modules === "function") {
        try {
            const modules = host_list_modules();
            if (Array.isArray(modules)) {
                for (const m of modules) {
                    const id = m.id || m.name;
                    if (id && !moduleIds.includes(id)) {
                        moduleIds.push(id);
                    }
                }
            }
        } catch (e) {
            console.log("Error listing modules: " + e);
        }
    }
    return moduleIds;
}

function scanChainSubdir(basePath, subdir) {
    /* Scan chain subdirectory for components (audio_fx, sound_generators, midi_fx) */
    const results = [];
    const dirPath = `${basePath}/${subdir}`;

    /* List directory contents dynamically */
    /* os.readdir returns [names_array, error_code] */
    let readdirResult = [];
    try {
        readdirResult = os.readdir(dirPath) || [];
    } catch (e) {
        return results;
    }

    /* Get the names array (first element of result) */
    const names = readdirResult[0];
    if (!names || !Array.isArray(names)) {
        return results;
    }

    /* Filter out . and .. and try to load each as a module */
    for (const name of names) {
        if (name === "." || name === "..") continue;
        const mod = scanModuleDir(`${dirPath}/${name}`);
        if (mod) {
            results.push(mod);
        }
    }
    return results;
}

function scanChainableModules() {
    const root = getModulesRoot();
    if (!root) return;

    availableComponents = {
        sound_generators: [],
        audio_fx: [],
        midi_fx: [],
        midi_sources: []
    };

    /* Scan installed modules using host_list_modules() */
    const installedModules = getInstalledModules();
    for (const moduleId of installedModules) {
        if (moduleId === "chain") continue; /* Skip chain module itself */

        const mod = scanModuleDir(`${root}/${moduleId}`);
        if (!mod) continue;

        switch (mod.component_type) {
            case "sound_generator":
                availableComponents.sound_generators.push(mod);
                break;
            case "audio_fx":
                availableComponents.audio_fx.push(mod);
                break;
            case "midi_fx":
                availableComponents.midi_fx.push(mod);
                break;
            case "midi_source":
                availableComponents.midi_sources.push(mod);
                break;
        }
    }

    /* Scan chain subdirectories for embedded components */
    const chainDir = `${root}/chain`;

    const soundGens = scanChainSubdir(chainDir, "sound_generators");
    availableComponents.sound_generators.push(...soundGens);

    const audioFx = scanChainSubdir(chainDir, "audio_fx");
    availableComponents.audio_fx.push(...audioFx);

    /* Add built-in JS MIDI FX */
    availableComponents.midi_fx.push(...BUILTIN_MIDI_FX);

    /* Sort by name for consistent display */
    availableComponents.sound_generators.sort((a, b) => a.name.localeCompare(b.name));
    availableComponents.audio_fx.sort((a, b) => a.name.localeCompare(b.name));
    availableComponents.midi_fx.sort((a, b) => a.name.localeCompare(b.name));
    availableComponents.midi_sources.sort((a, b) => a.name.localeCompare(b.name));

    console.log(`Scanned modules: ${availableComponents.sound_generators.length} synths, ` +
                `${availableComponents.audio_fx.length} audio fx, ` +
                `${availableComponents.midi_fx.length} midi fx`);
}

/* Editor view modes */
const EDITOR_VIEW = {
    OVERVIEW: "overview",
    SLOT_MENU: "slot_menu",
    COMPONENT_PICKER: "component_picker",
    PARAM_EDITOR: "param_editor",
    KNOB_EDITOR: "knob_editor",
    KNOB_PARAM_PICKER: "knob_param_picker",
    CONFIRM_DELETE: "confirm_delete"
};

/* Editor slot types - source combines input routing + MIDI generator modules */
const SLOT_TYPES = ["source", "midi_fx", "synth", "fx1", "fx2"];

/* Knob constants - Move has 8 knobs (CC 71-78) */
const NUM_KNOBS = 8;
const KNOB_CC_START = 71;

/* Source options - input routing options that are NOT MIDI generator modules */
const INPUT_SOURCE_OPTIONS = [
    { id: "both", name: "Pads + External", isInputOption: true },
    { id: "pads", name: "Pads Only", isInputOption: true },
    { id: "external", name: "External Only", isInputOption: true }
];

function createEmptyKnobs() {
    const knobs = [];
    for (let i = 0; i < NUM_KNOBS; i++) {
        knobs.push({ slot: null, param: null });
    }
    return knobs;
}

function createEditorState(existingPatch = null) {
    if (existingPatch) {
        /* Source can be an input option string or a MIDI source module ID */
        let source = existingPatch.midi_source_module || existingPatch.input || "both";
        return {
            isNew: false,
            originalPath: existingPatch.path || "",
            view: EDITOR_VIEW.OVERVIEW,
            selectedSlot: 0,
            slotMenuIndex: 0,
            componentPickerIndex: 0,
            paramIndex: 0,
            confirmIndex: 0,
            knobIndex: 0,
            knobParamIndex: 0,
            chain: {
                source: source,
                source_config: {},
                midi_fx: existingPatch.chord_type || existingPatch.arp_mode ? "chord" : null,
                midi_fx_config: {},
                synth: existingPatch.synth_module || "sf2",
                synth_config: { preset: existingPatch.synth_preset || 0 },
                fx1: existingPatch.audio_fx?.[0] || null,
                fx1_config: {},
                fx2: existingPatch.audio_fx?.[1] || null,
                fx2_config: {},
                knobs: existingPatch.knobs || createEmptyKnobs()
            }
        };
    }
    return {
        isNew: true,
        originalPath: "",
        view: EDITOR_VIEW.COMPONENT_PICKER,
        selectedSlot: 2, /* Start at synth slot (index 2: source, midi_fx, synth) */
        slotMenuIndex: 0,
        componentPickerIndex: 0,
        paramIndex: 0,
        confirmIndex: 0,
        knobIndex: 0,
        knobParamIndex: 0,
        chain: {
            source: "both",
            source_config: {},
            midi_fx: null,
            midi_fx_config: {},
            synth: null,
            synth_config: {},
            fx1: null,
            fx1_config: {},
            fx2: null,
            fx2_config: {},
            knobs: createEmptyKnobs()
        }
    };
}

function enterEditor(patchIndex = -1) {
    if (patchIndex >= 0 && patchIndex < patchCount) {
        /* Edit existing patch - load config from DSP */
        const configJson = host_module_get_param(`patch_config_${patchIndex}`);
        let config = null;
        try {
            config = JSON.parse(configJson || "{}");
        } catch (e) {
            console.log("Failed to parse patch config: " + e);
            config = {};
        }

        editorState = {
            isNew: false,
            originalPath: "",
            editIndex: patchIndex,
            view: EDITOR_VIEW.OVERVIEW,
            selectedSlot: 0,
            slotMenuIndex: 0,
            componentPickerIndex: 0,
            paramIndex: 0,
            confirmIndex: 0,
            knobIndex: 0,
            knobParamIndex: 0,
            chain: {
                source: config.source || null,
                midi_fx: null,
                midi_fx_config: {},
                synth: config.synth || "sf2",
                synth_config: { preset: config.preset || 0 },
                fx1: null,
                fx1_config: {},
                fx2: null,
                fx2_config: {},
                input: config.input || "both",
                knobs: config.knobs || createEmptyKnobs()
            }
        };

        /* Parse MIDI FX */
        if (config.chord && config.chord !== "none") {
            editorState.chain.midi_fx = "chord";
            editorState.chain.midi_fx_config = { type: config.chord };
        } else if (config.arp && config.arp !== "off") {
            editorState.chain.midi_fx = "arp";
            const divStr = config.arp_div === 1 ? "1/4" : config.arp_div === 2 ? "1/8" : "1/16";
            editorState.chain.midi_fx_config = {
                mode: config.arp,
                bpm: config.arp_bpm || 120,
                division: divStr
            };
        }

        /* Parse Audio FX */
        if (Array.isArray(config.audio_fx)) {
            if (config.audio_fx[0]) {
                editorState.chain.fx1 = config.audio_fx[0];
            }
            if (config.audio_fx[1]) {
                editorState.chain.fx2 = config.audio_fx[1];
            }
        }

        /* Parse Knob Mappings */
        if (Array.isArray(config.knob_mappings)) {
            for (const mapping of config.knob_mappings) {
                const knobIndex = mapping.cc - KNOB_CC_START;
                if (knobIndex >= 0 && knobIndex < NUM_KNOBS) {
                    editorState.chain.knobs[knobIndex] = {
                        slot: mapping.target,
                        param: mapping.param
                    };
                }
            }
        }
    } else {
        /* New chain */
        editorState = createEditorState();
    }
    editorMode = true;
    needsRedraw = true;
}

function exitEditor() {
    editorMode = false;
    editorState = null;
    editorError = "";
    editorErrorTimeout = 0;
    needsRedraw = true;
}

function getSlotLabel(slotType) {
    switch (slotType) {
        case "source": return "Source";
        case "midi_fx": return "MIDI FX";
        case "synth": return "Synth";
        case "fx1": return "FX 1";
        case "fx2": return "FX 2";
        default: return slotType;
    }
}

function isInputSourceOption(value) {
    return INPUT_SOURCE_OPTIONS.some(o => o.id === value);
}

function getSlotValue(slotType) {
    if (!editorState) return "[none]";
    const chain = editorState.chain;
    switch (slotType) {
        case "source": {
            const value = chain.source;
            /* Check if it's an input option (pads/external/both) */
            const inputOpt = INPUT_SOURCE_OPTIONS.find(o => o.id === value);
            if (inputOpt) return inputOpt.name;
            /* Otherwise it's a MIDI source module */
            return value ? getComponentName("source", value) : "Pads + External";
        }
        case "midi_fx": return chain.midi_fx ? getComponentName("midi_fx", chain.midi_fx) : "[none]";
        case "synth": return chain.synth ? getComponentName("synth", chain.synth) : "[none]";
        case "fx1": return chain.fx1 ? getComponentName("fx1", chain.fx1) : "[none]";
        case "fx2": return chain.fx2 ? getComponentName("fx2", chain.fx2) : "[none]";
        default: return "[none]";
    }
}

function getComponentName(slotType, componentId) {
    const component = findComponent(slotType, componentId);
    return component ? component.name : componentId;
}

function getAssignedKnobCount() {
    if (!editorState || !editorState.chain.knobs) return 0;
    return editorState.chain.knobs.filter(k => k.slot && k.param).length;
}

function drawEditorOverview() {
    const title = editorState.isNew ? "New Chain" : "Edit Chain";
    drawMenuHeader(title);

    const items = [
        ...SLOT_TYPES.map(slot => ({ type: "slot", slot })),
        { type: "knobs", label: "Knobs" },
        { type: "action", action: "save", label: "[Save]" },
        { type: "action", action: "cancel", label: "[Cancel]" }
    ];

    if (!editorState.isNew) {
        items.push({ type: "action", action: "delete", label: "[Delete]" });
    }

    drawMenuList({
        items,
        selectedIndex: editorState.selectedSlot,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: editorError ? menuLayoutDefaults.listBottomWithFooter : menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => {
            if (item.type === "slot") return getSlotLabel(item.slot);
            if (item.type === "knobs") return "Knobs";
            return item.label;
        },
        getValue: (item) => {
            if (item.type === "slot") return getSlotValue(item.slot);
            if (item.type === "knobs") {
                const count = getAssignedKnobCount();
                return count > 0 ? `${count} assigned` : "[none]";
            }
            return "";
        },
        valueAlignRight: true
    });

    /* Show validation error if present */
    if (editorError) {
        drawMenuFooter(editorError);
    }
}

function drawSlotMenu() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    drawMenuHeader(getSlotLabel(slotType));

    const items = [
        { action: "change", label: "Change..." }
    ];

    /* Source slot: only show Configure for MIDI source modules, not input options */
    const sourceValue = editorState.chain.source;
    const isSourceInputOption = slotType === "source" && isInputSourceOption(sourceValue);

    /* Show Configure for slots that have configurable params (not input options) */
    if (!isSourceInputOption) {
        items.push({ action: "configure", label: "Configure..." });
    }

    /* Can't clear synth or source (source always has a value) */
    if (slotType !== "synth" && slotType !== "source") {
        items.push({ action: "clear", label: "[Clear]" });
    }

    drawMenuList({
        items,
        selectedIndex: editorState.slotMenuIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.label
    });

    drawMenuFooter("Click:select Back:cancel");
}

function getComponentsForSlot(slotType) {
    switch (slotType) {
        case "source":
            /* Input options first, then MIDI source modules */
            return [...INPUT_SOURCE_OPTIONS, ...availableComponents.midi_sources];
        case "synth":
            return availableComponents.sound_generators;
        case "fx1":
        case "fx2":
            return [{ id: null, name: "[none]" }, ...availableComponents.audio_fx];
        case "midi_fx":
            return [{ id: null, name: "[none]" }, ...availableComponents.midi_fx];
        default:
            return [];
    }
}

function drawComponentPicker() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const components = getComponentsForSlot(slotType);
    let title = "";

    switch (slotType) {
        case "source": title = "Source"; break;
        case "synth": title = "Sound Generator"; break;
        case "fx1":
        case "fx2": title = "Audio FX"; break;
        case "midi_fx": title = "MIDI FX"; break;
        default: title = "Select"; break;
    }

    drawMenuHeader(title);

    drawMenuList({
        items: components,
        selectedIndex: editorState.componentPickerIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.name
    });

    drawMenuFooter("Click:select Back:cancel");
}

function findComponent(slotType, componentId) {
    if (!componentId) return null;

    let list = [];
    switch (slotType) {
        case "synth":
            list = availableComponents.sound_generators;
            break;
        case "fx1":
        case "fx2":
            list = availableComponents.audio_fx;
            break;
        case "midi_fx":
            list = availableComponents.midi_fx;
            break;
        case "source":
            /* Check input options first, then MIDI source modules */
            const inputOpt = INPUT_SOURCE_OPTIONS.find(o => o.id === componentId);
            if (inputOpt) return inputOpt;
            list = availableComponents.midi_sources;
            break;
    }
    return list.find(c => c.id === componentId);
}

function drawParamEditor() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const componentId = editorState.chain[slotType];
    const component = findComponent(slotType, componentId);

    if (!component || !component.params || component.params.length === 0) {
        drawMenuHeader(componentId || "Unknown");
        print(4, 28, "No configurable", 1);
        print(4, 40, "parameters", 1);
        drawMenuFooter("Back:return");
        return;
    }

    drawMenuHeader(component.name);

    const configKey = slotType + "_config";
    const config = editorState.chain[configKey] || {};

    const items = [
        ...component.params.map(p => ({
            type: "param",
            param: p,
            value: config[p.key] !== undefined ? config[p.key] : p.default
        })),
        { type: "action", action: "done", label: "[Done]" }
    ];

    drawMenuList({
        items,
        selectedIndex: editorState.paramIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => {
            if (item.type === "param") return item.param.name;
            return item.label;
        },
        getValue: (item) => {
            if (item.type === "param") {
                const val = item.value;
                if (item.param.type === "float") {
                    return val.toFixed(2);
                }
                return String(val);
            }
            return "";
        },
        valueAlignRight: true
    });

    drawMenuFooter("Jog:value Up/Dn:nav");
}

function drawConfirmDelete() {
    drawMenuHeader("Delete Chain?");

    const patchName = host_module_get_param(`patch_name_${editorState.editIndex}`) || "Unknown";
    print(4, 24, `"${patchName}"`, 1);

    const items = [
        { action: "cancel", label: "Cancel" },
        { action: "confirm", label: "Delete" }
    ];

    drawMenuList({
        items,
        selectedIndex: editorState.confirmIndex,
        listArea: {
            topY: 36,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => item.label
    });
}

function getKnobAssignmentLabel(knobAssignment) {
    if (!knobAssignment || !knobAssignment.slot || !knobAssignment.param) {
        return "[none]";
    }
    const slotLabel = getSlotLabel(knobAssignment.slot);
    return `${slotLabel}: ${knobAssignment.param}`;
}

function getAllAssignableParams() {
    /* Get all parameters from all configured components */
    const params = [];
    const chain = editorState.chain;

    /* Add synth params */
    if (chain.synth) {
        const component = findComponent("synth", chain.synth);
        if (component && component.params) {
            for (const p of component.params) {
                params.push({ slot: "synth", param: p.key, name: `${component.name}: ${p.name}` });
            }
        }
    }

    /* Add MIDI FX params */
    if (chain.midi_fx) {
        const component = findComponent("midi_fx", chain.midi_fx);
        if (component && component.params) {
            for (const p of component.params) {
                params.push({ slot: "midi_fx", param: p.key, name: `${component.name}: ${p.name}` });
            }
        }
    }

    /* Add FX1 params */
    if (chain.fx1) {
        const component = findComponent("fx1", chain.fx1);
        if (component && component.params) {
            for (const p of component.params) {
                params.push({ slot: "fx1", param: p.key, name: `FX1 ${component.name}: ${p.name}` });
            }
        }
    }

    /* Add FX2 params */
    if (chain.fx2) {
        const component = findComponent("fx2", chain.fx2);
        if (component && component.params) {
            for (const p of component.params) {
                params.push({ slot: "fx2", param: p.key, name: `FX2 ${component.name}: ${p.name}` });
            }
        }
    }

    return params;
}

function drawKnobEditor() {
    drawMenuHeader("Knob Assignment");

    const items = [];
    for (let i = 0; i < NUM_KNOBS; i++) {
        const knob = editorState.chain.knobs[i];
        items.push({
            type: "knob",
            index: i,
            label: `Knob ${i + 1}`,
            assignment: knob
        });
    }
    items.push({ type: "action", action: "done", label: "[Done]" });

    drawMenuList({
        items,
        selectedIndex: editorState.knobIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => {
            if (item.type === "knob") return item.label;
            return item.label;
        },
        getValue: (item) => {
            if (item.type === "knob") return getKnobAssignmentLabel(item.assignment);
            return "";
        },
        valueAlignRight: true
    });

    drawMenuFooter("Click:assign Back:return");
}

function drawKnobParamPicker() {
    const knobNum = editorState.knobIndex + 1;
    drawMenuHeader(`Knob ${knobNum} Param`);

    const params = getAllAssignableParams();
    const items = [
        { type: "clear", name: "[Clear]" },
        ...params.map(p => ({ type: "param", ...p }))
    ];

    drawMenuList({
        items,
        selectedIndex: editorState.knobParamIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.name
    });

    drawMenuFooter("Click:select Back:cancel");
}

function handleEditorJog(delta) {
    switch (editorState.view) {
        case EDITOR_VIEW.OVERVIEW: {
            /* SLOT_TYPES + Knobs + Save + Cancel + (Delete if editing) */
            const maxItems = SLOT_TYPES.length + 1 + 2 + (editorState.isNew ? 0 : 1);
            editorState.selectedSlot = Math.max(0, Math.min(maxItems - 1, editorState.selectedSlot + delta));
            break;
        }
        case EDITOR_VIEW.SLOT_MENU: {
            const slotType = SLOT_TYPES[editorState.selectedSlot];
            /* input: 1 item (Change), synth: 2 items (Change, Configure), others: 3 items */
            const maxItems = slotType === "input" ? 1 : slotType === "synth" ? 2 : 3;
            editorState.slotMenuIndex = Math.max(0, Math.min(maxItems - 1, editorState.slotMenuIndex + delta));
            break;
        }
        case EDITOR_VIEW.COMPONENT_PICKER: {
            const components = getComponentsForSlot(SLOT_TYPES[editorState.selectedSlot]);
            editorState.componentPickerIndex = Math.max(0, Math.min(components.length - 1, editorState.componentPickerIndex + delta));
            break;
        }
        case EDITOR_VIEW.PARAM_EDITOR: {
            handleParamJog(delta);
            break;
        }
        case EDITOR_VIEW.KNOB_EDITOR: {
            /* 8 knobs + Done button */
            const maxItems = NUM_KNOBS + 1;
            editorState.knobIndex = Math.max(0, Math.min(maxItems - 1, editorState.knobIndex + delta));
            break;
        }
        case EDITOR_VIEW.KNOB_PARAM_PICKER: {
            /* Clear + all assignable params */
            const params = getAllAssignableParams();
            const maxItems = 1 + params.length;
            editorState.knobParamIndex = Math.max(0, Math.min(maxItems - 1, editorState.knobParamIndex + delta));
            break;
        }
        case EDITOR_VIEW.CONFIRM_DELETE: {
            editorState.confirmIndex = editorState.confirmIndex === 0 ? 1 : 0;
            break;
        }
    }
}

function handleParamJog(delta) {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const componentId = editorState.chain[slotType];
    const component = findComponent(slotType, componentId);

    if (!component || !component.params) {
        return;
    }

    /* If on [Done] button, jog navigates */
    if (editorState.paramIndex >= component.params.length) {
        return;
    }

    /* Adjust the selected parameter value */
    const param = component.params[editorState.paramIndex];
    const configKey = slotType + "_config";
    if (!editorState.chain[configKey]) {
        editorState.chain[configKey] = {};
    }
    const config = editorState.chain[configKey];
    let value = config[param.key] !== undefined ? config[param.key] : param.default;

    switch (param.type) {
        case "int": {
            const step = param.step || 1;
            value = Math.max(param.min, Math.min(param.max || 127, value + delta * step));
            break;
        }
        case "float": {
            const step = param.step || 0.05;
            value = Math.max(param.min, Math.min(param.max || 1.0, value + delta * step));
            /* Round to avoid floating point errors */
            value = Math.round(value * 100) / 100;
            break;
        }
        case "enum": {
            const options = param.options || [];
            if (options.length > 0) {
                let idx = options.indexOf(value);
                if (idx < 0) idx = 0;
                idx = Math.max(0, Math.min(options.length - 1, idx + delta));
                value = options[idx];
            }
            break;
        }
    }

    config[param.key] = value;
}

function handleParamNavigate(delta) {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const componentId = editorState.chain[slotType];
    const component = findComponent(slotType, componentId);

    if (!component || !component.params) {
        editorState.paramIndex = 0;
        return;
    }

    const maxItems = component.params.length + 1; /* +1 for Done */
    editorState.paramIndex = Math.max(0, Math.min(maxItems - 1, editorState.paramIndex + delta));
}

function handleEditorSelect() {
    switch (editorState.view) {
        case EDITOR_VIEW.OVERVIEW: {
            if (editorState.selectedSlot < SLOT_TYPES.length) {
                /* Slot selection */
                editorState.view = EDITOR_VIEW.SLOT_MENU;
                editorState.slotMenuIndex = 0;
            } else if (editorState.selectedSlot === SLOT_TYPES.length) {
                /* Knobs row */
                editorState.view = EDITOR_VIEW.KNOB_EDITOR;
                editorState.knobIndex = 0;
            } else {
                /* Actions: Save, Cancel, Delete */
                const actionIndex = editorState.selectedSlot - SLOT_TYPES.length - 1;
                if (actionIndex === 0) {
                    saveChain();
                } else if (actionIndex === 1) {
                    exitEditor();
                } else if (actionIndex === 2) {
                    editorState.view = EDITOR_VIEW.CONFIRM_DELETE;
                    editorState.confirmIndex = 0;
                }
            }
            break;
        }
        case EDITOR_VIEW.SLOT_MENU: {
            const slotType = SLOT_TYPES[editorState.selectedSlot];
            /* Source slot with input option only has Change available */
            const sourceValue = editorState.chain.source;
            const isSourceInputOption = slotType === "source" && isInputSourceOption(sourceValue);

            if (isSourceInputOption) {
                /* Only Change is available for source with input option */
                if (editorState.slotMenuIndex === 0) {
                    editorState.view = EDITOR_VIEW.COMPONENT_PICKER;
                    editorState.componentPickerIndex = 0;
                }
            } else if (slotType === "source" || slotType === "synth") {
                /* Source (with module) or synth: Change and Configure, no Clear */
                if (editorState.slotMenuIndex === 0) {
                    editorState.view = EDITOR_VIEW.COMPONENT_PICKER;
                    editorState.componentPickerIndex = 0;
                } else if (editorState.slotMenuIndex === 1) {
                    if (editorState.chain[slotType]) {
                        editorState.view = EDITOR_VIEW.PARAM_EDITOR;
                        editorState.paramIndex = 0;
                    }
                }
            } else {
                /* Other slots: Change, Configure, Clear */
                if (editorState.slotMenuIndex === 0) {
                    editorState.view = EDITOR_VIEW.COMPONENT_PICKER;
                    editorState.componentPickerIndex = 0;
                } else if (editorState.slotMenuIndex === 1) {
                    if (editorState.chain[slotType]) {
                        editorState.view = EDITOR_VIEW.PARAM_EDITOR;
                        editorState.paramIndex = 0;
                    }
                } else if (editorState.slotMenuIndex === 2) {
                    editorState.chain[slotType] = null;
                    editorState.chain[slotType + "_config"] = {};
                    editorState.view = EDITOR_VIEW.OVERVIEW;
                }
            }
            break;
        }
        case EDITOR_VIEW.COMPONENT_PICKER: {
            const slotType = SLOT_TYPES[editorState.selectedSlot];
            const components = getComponentsForSlot(slotType);
            const selected = components[editorState.componentPickerIndex];

            /* Source must always have a value - default to "both" */
            if (slotType === "source") {
                editorState.chain.source = selected?.id || "both";
            } else {
                editorState.chain[slotType] = selected?.id || null;
            }
            editorState.chain[slotType + "_config"] = {};

            if (editorState.isNew && slotType === "synth") {
                editorState.view = EDITOR_VIEW.OVERVIEW;
                editorState.selectedSlot = 0;
            } else {
                editorState.view = EDITOR_VIEW.OVERVIEW;
            }
            break;
        }
        case EDITOR_VIEW.PARAM_EDITOR: {
            handleParamSelect();
            break;
        }
        case EDITOR_VIEW.KNOB_EDITOR: {
            if (editorState.knobIndex >= NUM_KNOBS) {
                /* Done button */
                editorState.view = EDITOR_VIEW.OVERVIEW;
            } else {
                /* Select a knob to assign */
                editorState.view = EDITOR_VIEW.KNOB_PARAM_PICKER;
                editorState.knobParamIndex = 0;
            }
            break;
        }
        case EDITOR_VIEW.KNOB_PARAM_PICKER: {
            const params = getAllAssignableParams();
            if (editorState.knobParamIndex === 0) {
                /* Clear */
                editorState.chain.knobs[editorState.knobIndex] = { slot: null, param: null };
            } else {
                /* Assign param */
                const selected = params[editorState.knobParamIndex - 1];
                editorState.chain.knobs[editorState.knobIndex] = {
                    slot: selected.slot,
                    param: selected.param
                };
            }
            editorState.view = EDITOR_VIEW.KNOB_EDITOR;
            break;
        }
        case EDITOR_VIEW.CONFIRM_DELETE: {
            if (editorState.confirmIndex === 0) {
                editorState.view = EDITOR_VIEW.OVERVIEW;
            } else {
                deleteChain();
            }
            break;
        }
    }
}

function handleParamSelect() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const componentId = editorState.chain[slotType];
    const component = findComponent(slotType, componentId);

    if (!component || !component.params) {
        editorState.view = EDITOR_VIEW.OVERVIEW;
        return;
    }

    /* If on [Done], return to overview */
    if (editorState.paramIndex >= component.params.length) {
        editorState.view = EDITOR_VIEW.OVERVIEW;
        return;
    }

    /* Click moves to next parameter (or Done) */
    editorState.paramIndex++;
}

function handleEditorCC(cc, val) {
    if (!editorState) return false;

    if (cc === CC_BACK && val === 127) {
        switch (editorState.view) {
            case EDITOR_VIEW.OVERVIEW:
                exitEditor();
                break;
            case EDITOR_VIEW.SLOT_MENU:
            case EDITOR_VIEW.COMPONENT_PICKER:
            case EDITOR_VIEW.PARAM_EDITOR:
            case EDITOR_VIEW.KNOB_EDITOR:
            case EDITOR_VIEW.CONFIRM_DELETE:
                editorState.view = EDITOR_VIEW.OVERVIEW;
                break;
            case EDITOR_VIEW.KNOB_PARAM_PICKER:
                editorState.view = EDITOR_VIEW.KNOB_EDITOR;
                break;
        }
        needsRedraw = true;
        return true;
    }

    if (cc === CC_JOG) {
        const delta = val < 64 ? 1 : -1;
        handleEditorJog(delta);
        needsRedraw = true;
        return true;
    }

    if (cc === CC_JOG_CLICK && val === 127) {
        handleEditorSelect();
        needsRedraw = true;
        return true;
    }

    /* Up/Down for param navigation in param editor */
    if (editorState.view === EDITOR_VIEW.PARAM_EDITOR) {
        if (cc === CC_UP && val === 127) {
            handleParamNavigate(-1);
            needsRedraw = true;
            return true;
        }
        if (cc === CC_DOWN && val === 127) {
            handleParamNavigate(1);
            needsRedraw = true;
            return true;
        }
    }

    return false;
}

function generateChainName() {
    const chain = editorState.chain;
    let parts = [];

    /* Synth name */
    const synth = findComponent("synth", chain.synth);
    if (synth) {
        parts.push(synth.name);
        const preset = chain.synth_config?.preset;
        if (preset !== undefined && preset > 0) {
            parts.push(String(preset).padStart(2, '0'));
        }
    }

    /* MIDI FX */
    if (chain.midi_fx === "chord") {
        const type = chain.midi_fx_config?.type;
        if (type && type !== "none") {
            parts.push("Chord");
        }
    }
    if (chain.midi_fx === "arp") {
        const mode = chain.midi_fx_config?.mode;
        if (mode && mode !== "off") {
            parts.push("Arp");
        }
    }

    /* Audio FX */
    if (chain.fx1) {
        const fx = findComponent("fx1", chain.fx1);
        if (fx) parts.push(fx.name);
    }
    if (chain.fx2) {
        const fx = findComponent("fx2", chain.fx2);
        if (fx) parts.push(fx.name);
    }

    return parts.join(" + ") || "New Chain";
}

function buildChainJson() {
    const chain = editorState.chain;
    const name = generateChainName();

    const patch = {
        name: name,
        version: 1,
        chain: {
            synth: {
                module: chain.synth || "sf2",
                config: {}
            },
            audio_fx: []
        }
    };

    /* Source can be an input option (pads/external/both) or a MIDI source module */
    const sourceValue = chain.source || "both";
    if (isInputSourceOption(sourceValue)) {
        /* Input routing option - store as "input" for backwards compat */
        patch.chain.input = sourceValue;
    } else {
        /* MIDI source module - default input to "both" and set module */
        patch.chain.input = "both";
        patch.chain.midi_source_module = sourceValue;
    }

    /* Synth config */
    if (chain.synth_config) {
        patch.chain.synth.config = { ...chain.synth_config };
    }

    /* MIDI FX - array of MIDI effect objects */
    const midiFxArray = [];
    if (chain.midi_fx === "chord") {
        const type = chain.midi_fx_config?.type || "none";
        if (type !== "none") {
            midiFxArray.push({ type: "chord", chord: type });
        }
    }
    if (chain.midi_fx === "arp") {
        const mode = chain.midi_fx_config?.mode || "off";
        if (mode !== "off") {
            const div = chain.midi_fx_config?.division || "1/16";
            const divNum = div === "1/4" ? 1 : div === "1/8" ? 2 : 4;
            midiFxArray.push({
                type: "arp",
                mode: mode,
                bpm: chain.midi_fx_config?.bpm || 120,
                division: divNum
            });
        }
    }
    if (midiFxArray.length > 0) {
        patch.chain.midi_fx = midiFxArray;
    }

    /* Audio FX */
    if (chain.fx1) {
        const fx = { type: chain.fx1 };
        if (chain.fx1_config && Object.keys(chain.fx1_config).length > 0) {
            fx.params = chain.fx1_config;
        }
        patch.chain.audio_fx.push(fx);
    }
    if (chain.fx2) {
        const fx = { type: chain.fx2 };
        if (chain.fx2_config && Object.keys(chain.fx2_config).length > 0) {
            fx.params = chain.fx2_config;
        }
        patch.chain.audio_fx.push(fx);
    }

    /* Knob assignments */
    if (chain.knobs) {
        const knobs = [];
        for (let i = 0; i < NUM_KNOBS; i++) {
            const knob = chain.knobs[i];
            if (knob && knob.slot && knob.param) {
                knobs.push({
                    cc: KNOB_CC_START + i,
                    target: knob.slot,
                    param: knob.param
                });
            }
        }
        if (knobs.length > 0) {
            patch.chain.knob_mappings = knobs;
        }
    }

    /* Return only the chain content - save_patch wraps with name/version */
    return JSON.stringify(patch.chain, null, 4);
}

function showEditorError(msg) {
    editorError = msg;
    editorErrorTimeout = 30; /* Show for ~30 ticks (~0.5 sec) */
    needsRedraw = true;
}

function saveChain() {
    if (!editorState.chain.synth) {
        showEditorError("Select a synth first");
        return;
    }

    const chainJson = buildChainJson();
    host_module_set_param("save_patch", chainJson);

    exitEditor();
}

function deleteChain() {
    if (editorState.isNew || editorState.editIndex === undefined) {
        console.log("Chain editor: Cannot delete new chain");
        exitEditor();
        return;
    }

    host_module_set_param("delete_patch", String(editorState.editIndex));

    exitEditor();
}

function loadSourceUi(moduleId) {
    if (!moduleId) {
        sourceUiLoadError = false;
        return null;
    }
    const path = getChainUiPath(moduleId);
    if (!path) {
        sourceUiLoadError = true;
        return null;
    }
    globalThis.chain_ui = null;
    if (typeof host_load_ui_module !== "function") {
        sourceUiLoadError = true;
        return null;
    }
    const ok = host_load_ui_module(path);
    if (!ok) {
        sourceUiLoadError = true;
        return null;
    }
    const ui = globalThis.chain_ui;
    globalThis.chain_ui = null;
    if (!ui || typeof ui.init !== "function") return null;
    sourceUiLoadError = false;
    return ui;
}

function refreshSourceUi(patchChanged) {
    if (midiSourceModule !== midiSourceModuleLoaded) {
        sourceUi = loadSourceUi(midiSourceModule);
        midiSourceModuleLoaded = midiSourceModule;
        sourceUiReady = false;
        sourceUiActive = false;
        disableSourceHostRouting();
    }

    if (patchChanged) {
        sourceUiActive = false;
        hostFns.setParam("source_ui_active", "0");
        disableSourceHostRouting();
    }
}

function enterSourceUi() {
    if (!sourceUi) return;
    hostFns.setParam("source_ui_active", "1");
    enableSourceHostRouting();
    if (!sourceUiReady && typeof sourceUi.init === "function") {
        sourceUi.init();
        sourceUiReady = true;
    }
    sourceUiActive = true;
    needsRedraw = true;
}

function exitSourceUi() {
    if (!sourceUiActive) return;
    sourceUiActive = false;
    hostFns.setParam("source_ui_active", "0");
    disableSourceHostRouting();
    needsRedraw = true;
}

function handleCC(cc, val) {
    /* Handle editor mode first */
    if (editorMode) {
        return handleEditorCC(cc, val);
    }

    if (cc === CC_BACK && val === 127) {
        if (sourceUiActive) {
            exitSourceUi();
            return true;
        }
        if (viewMode === "patch") {
            selectedPatch = currentPatch;
            viewMode = "list";
            host_module_set_param("patch", "-1");
            needsRedraw = true;
            return true;
        }
        host_return_to_menu();
        return true;
    }

    if (cc === CC_MENU && val === 127) {
        if (viewMode === "list" && selectedPatch >= 0 && selectedPatch < patchCount) {
            enterEditor(selectedPatch);
            return true;
        }
        if (viewMode === "patch" && !sourceUiActive && sourceUi) {
            enterSourceUi();
            return true;
        }
    }

    /* Jog wheel for patch navigation */
    if (cc === CC_JOG) {
        if (viewMode === "list") {
            const delta = val < 64 ? val : val - 128;
            if (patchCount >= 0 && delta !== 0) {
                const totalItems = patchCount + 1; /* +1 for New Chain */
                const next = selectedPatch + (delta > 0 ? 1 : -1);
                if (next < -1) {
                    selectedPatch = patchCount - 1;
                } else if (next >= patchCount) {
                    selectedPatch = -1; /* New Chain is at index -1 */
                } else {
                    selectedPatch = next;
                }
                needsRedraw = true;
            }
            return true;
        }
        return false;
    }

    if (cc === CC_JOG_CLICK && val === 127) {
        if (viewMode === "list") {
            if (selectedPatch === -1) {
                /* New Chain selected */
                enterEditor(-1);
                return true;
            } else if (selectedPatch >= 0 && selectedPatch < patchCount) {
                host_module_set_param("patch", String(selectedPatch));
                viewMode = "patch";
                needsRedraw = true;
                return true;
            }
        }
        return false;
    }

    /* Up/Down for octave (on button press) */
    if (cc === CC_UP && val === 127) {
        if (viewMode === "patch") {
            octaveUp();
            return true;
        }
    }
    if (cc === CC_DOWN && val === 127) {
        if (viewMode === "patch") {
            octaveDown();
            return true;
        }
    }

    /* Handle knob CCs for parameter feedback (CC 71-78) */
    if (cc >= KNOB_CC_START && cc <= KNOB_CC_START + NUM_KNOBS - 1) {
        if (viewMode === "patch" && !sourceUiActive) {
            handleKnobFeedback(cc, val);
        }
    }

    return false;
}

function loadMidiFxChain(spec) {
    midiFxJsSpec = spec || "";
    midiFxJsChain = [];

    const names = midiFxJsSpec
        .split(',')
        .map((name) => name.trim())
        .filter((name) => name.length > 0);

    for (const name of names) {
        const fx = midiFxRegistry[name];
        if (!fx || typeof fx.processMidi !== "function") {
            console.log(`Chain UI: unknown MIDI FX '${name}'`);
            continue;
        }
        midiFxJsChain.push(fx);
    }
}

function processMidiFx(msg, source) {
    if (midiFxJsChain.length === 0) return;

    let messages = [msg];
    for (const fx of midiFxJsChain) {
        const next = [];
        for (const current of messages) {
            const out = fx.processMidi(current, source);
            if (Array.isArray(out)) {
                for (const o of out) {
                    if (o && o.length >= 3) next.push(o);
                }
            } else if (out && out.length >= 3) {
                next.push(out);
            }
        }
        messages = next;
        if (messages.length === 0) break;
    }

    for (const out of messages) {
        host_module_send_midi(out, "host");
    }
}

function midiSourceAllowed(source) {
    if (midiInput === "pads") {
        return source === "internal";
    }
    if (midiInput === "external") {
        return source === "external";
    }
    return true;
}

/* Draw the UI */
function drawUI() {
    clear_screen();

    if (editorMode && editorState) {
        switch (editorState.view) {
            case EDITOR_VIEW.OVERVIEW:
                drawEditorOverview();
                break;
            case EDITOR_VIEW.SLOT_MENU:
                drawSlotMenu();
                break;
            case EDITOR_VIEW.COMPONENT_PICKER:
                drawComponentPicker();
                break;
            case EDITOR_VIEW.PARAM_EDITOR:
                drawParamEditor();
                break;
            case EDITOR_VIEW.KNOB_EDITOR:
                drawKnobEditor();
                break;
            case EDITOR_VIEW.KNOB_PARAM_PICKER:
                drawKnobParamPicker();
                break;
            case EDITOR_VIEW.CONFIRM_DELETE:
                drawConfirmDelete();
                break;
        }
        needsRedraw = false;
        return;
    }

    if (viewMode === "list") {
        drawMenuHeader("Signal Chain");

        /* Build list with New Chain at top */
        const listItems = [
            { type: "new", name: "[+ New Chain]" },
            ...patchNames.map((name, i) => ({ type: "patch", name, index: i }))
        ];

        drawMenuList({
            items: listItems,
            selectedIndex: selectedPatch + 1, /* +1 for New Chain item */
            listArea: {
                topY: menuLayoutDefaults.listTopY,
                bottomY: menuLayoutDefaults.listBottomWithFooter
            },
            getLabel: (item) => item.name
        });
        drawMenuFooter("Click:load Menu:edit");
        needsRedraw = false;
        return;
    }

    /* Title bar with patch name */
    const title = patchName || "Signal Chain";
    print(2, 2, title, 1);
    fill_rect(0, 14, SCREEN_WIDTH, 1, 1);

    /* Synth module info */
    print(2, 20, "Synth:", 1);
    print(40, 20, synthModule || "SF2", 1);

    /* Preset from synth */
    const pn = host_module_get_param("preset_name");
    if (pn) {
        presetName = pn.substring(0, 16);
    }
    print(2, 32, presetName, 1);

    /* Octave and polyphony */
    const octStr = octaveTranspose >= 0 ? `+${octaveTranspose}` : `${octaveTranspose}`;
    print(2, 44, `Oct:${octStr}  Voices:${polyphony}`, 1);

    /* Hint for navigation */
    const hint = sourceUi ? "Menu:src Back:list" : "Up/Dn:oct Back:list";
    if (sourceUiLoadError) {
        print(2, 54, "No ui_chain.js", 1);
    } else {
        print(2, 54, hint, 1);
    }

    /* Draw shared overlay if active */
    drawOverlay();

    needsRedraw = false;
}

/* Load knob mappings for current patch from DSP */
function loadCurrentPatchKnobMappings() {
    currentKnobMappings = [];

    if (currentPatch < 0 || currentPatch >= patchCount) {
        return;
    }

    const configJson = host_module_get_param(`patch_config_${currentPatch}`);
    if (!configJson) return;

    try {
        const config = JSON.parse(configJson);
        /* Config has: synth, preset, source, input, chord, arp, arp_bpm, arp_div, audio_fx, knob_mappings */
        if (Array.isArray(config.knob_mappings)) {
            /* Add currentValue field to each mapping, with type-appropriate defaults */
            currentKnobMappings = config.knob_mappings.map(m => {
                const type = m.type || "float";  /* Default to float */
                const min = m.min !== undefined ? m.min : (type === "int" ? 0 : 0.0);
                const max = m.max !== undefined ? m.max : (type === "int" ? 127 : 1.0);
                const defaultVal = type === "int" ? Math.floor((min + max) / 2) : 0.5;
                return {
                    ...m,
                    type,
                    min,
                    max,
                    currentValue: defaultVal
                };
            });
        }
    } catch (e) {
        /* Ignore parse errors */
    }
}

/* Get display name for a knob mapping */
function getKnobMappingDisplayName(mapping) {
    if (!mapping) return "";
    const slotNames = {
        "synth": synthModule || "Synth",
        "fx1": "FX1",
        "fx2": "FX2",
        "midi_fx": "MIDI FX"
    };
    const slotName = slotNames[mapping.target] || mapping.target;
    return `${slotName}: ${mapping.param}`;
}

/* Handle knob CC for feedback display */
function handleKnobFeedback(cc, value) {
    const knobIndex = cc - KNOB_CC_START;
    if (knobIndex < 0 || knobIndex >= NUM_KNOBS) return false;

    /* Find mapping for this knob in current patch */
    const mappingIndex = currentKnobMappings.findIndex(m => m.cc === cc);
    if (mappingIndex < 0) return false;

    const mapping = currentKnobMappings[mappingIndex];
    const isInt = mapping.type === "int";

    /* Relative encoder: 1 = increment, 127 = decrement */
    let delta = 0;
    if (value === 1) {
        delta = isInt ? KNOB_STEP_INT : KNOB_STEP_FLOAT;
    } else if (value === 127) {
        delta = isInt ? -KNOB_STEP_INT : -KNOB_STEP_FLOAT;
    } else {
        return false;  /* Ignore other values */
    }

    /* Update current value with clamping to min/max */
    let newValue = mapping.currentValue + delta;
    if (newValue < mapping.min) newValue = mapping.min;
    if (newValue > mapping.max) newValue = mapping.max;
    if (isInt) newValue = Math.round(newValue);
    currentKnobMappings[mappingIndex].currentValue = newValue;

    /* Format display value based on type */
    const displayName = getKnobMappingDisplayName(mapping);
    let displayValue;
    if (isInt) {
        displayValue = String(Math.round(newValue));
    } else {
        displayValue = `${Math.round(newValue * 100)}%`;
    }

    /* Show feedback using shared overlay */
    showOverlay(displayName, displayValue);
    needsRedraw = true;

    return true;
}

/* Update state from DSP */
function updateState() {
    let patchChanged = false;
    const pc = host_module_get_param("patch_count");
    if (pc) {
        const nextCount = parseInt(pc) || 0;
        if (nextCount !== patchCount) {
            patchCount = nextCount;
            patchNames = [];
            for (let i = 0; i < patchCount; i++) {
                const name = host_module_get_param(`patch_name_${i}`);
                patchNames.push(name || `Patch ${i + 1}`);
            }
            if (selectedPatch >= patchCount) {
                selectedPatch = Math.max(0, patchCount - 1);
            }
        }
    }

    const cp = host_module_get_param("current_patch");
    if (cp) {
        const nextPatch = parseInt(cp) || 0;
        patchChanged = nextPatch !== currentPatch;
        currentPatch = nextPatch;
        if (patchChanged && viewMode === "list") {
            selectedPatch = currentPatch;
        }
    }

    const pn = host_module_get_param("patch_name");
    if (pn) patchName = pn;

    const sm = host_module_get_param("synth_module");
    if (sm) synthModule = sm;

    const ms = host_module_get_param("midi_source_module");
    if (ms !== null && ms !== undefined) {
        midiSourceModule = ms;
    }

    const poly = host_module_get_param("polyphony");
    if (poly) polyphony = parseInt(poly) || 0;

    const oct = host_module_get_param("octave_transpose");
    if (oct !== null && oct !== undefined) {
        octaveTranspose = parseInt(oct) || 0;
    }

    const rm = host_module_get_param("raw_midi");
    if (rm !== null && rm !== undefined) {
        rawMidi = parseInt(rm) === 1;
    }

    const mi = host_module_get_param("midi_input");
    if (mi) midiInput = mi;

    const fxSpec = host_module_get_param("midi_fx_js") || "";
    if (fxSpec !== midiFxJsSpec) {
        loadMidiFxChain(fxSpec);
    }

    refreshSourceUi(patchChanged);

    /* Load knob mappings when patch changes */
    if (patchChanged) {
        loadCurrentPatchKnobMappings();
    }

    if (patchChanged && viewMode === "patch" && sourceUi && !sourceUiActive) {
        enterSourceUi();
    }
}

/* Adjust octave */
function octaveUp() {
    if (octaveTranspose < 4) {
        octaveTranspose++;
        host_module_set_param("octave_transpose", String(octaveTranspose));
        needsRedraw = true;
    }
}

function octaveDown() {
    if (octaveTranspose > -4) {
        octaveTranspose--;
        host_module_set_param("octave_transpose", String(octaveTranspose));
        needsRedraw = true;
    }
}

/* === Required module UI callbacks === */

globalThis.init = function() {
    console.log("Signal Chain UI initializing...");
    scanChainableModules();
    needsRedraw = true;
    console.log("Signal Chain UI ready");
};

globalThis.tick = function() {
    if (sourceUiActive) {
        if (typeof sourceUi.tick === "function") {
            sourceUi.tick();
        }
        return;
    }

    /* Clear editor error after timeout */
    if (editorErrorTimeout > 0) {
        editorErrorTimeout--;
        if (editorErrorTimeout === 0) {
            editorError = "";
            needsRedraw = true;
        }
    }

    /* Tick the shared overlay timer */
    if (tickOverlay()) {
        needsRedraw = true;
    }

    /* Set default octave on first tick */
    if (!octaveInitialized) {
        host_module_set_param("octave_transpose", "-2");
        octaveTranspose = -2;
        octaveInitialized = true;
    }

    /* Update state from DSP */
    updateState();

    /* Rate-limited redraw */
    tickCount++;
    if (needsRedraw || tickCount >= REDRAW_INTERVAL) {
        drawUI();
        tickCount = 0;
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    const useJsMidiFx = midiFxJsChain.length > 0;
    const isCap = isCapacitiveTouchMessage(data);
    const status = data[0] & 0xF0;
    if (status === 0xB0 && data[1] === CC_BACK && data[2] === 127) {
        handleCC(data[1], data[2]);
        return;
    }
    if (sourceUiActive) {
        if (typeof sourceUi.onMidiMessageInternal === "function") {
            sourceUi.onMidiMessageInternal(data);
        }
        return;
    }
    if ((status === 0x90 || status === 0x80) && MoveSteps.includes(data[1])) {
        return;
    }
    if (!useJsMidiFx && isCap) return;
    if (useJsMidiFx && isCap && !rawMidi) return;

    if (status === 0xB0) {
        handleCC(data[1], data[2]);
        return;
    }

    if (useJsMidiFx) {
        if (!midiSourceAllowed("internal")) return;
        processMidiFx([data[0], data[1], data[2]], "internal");
    }

    /* Handle CC messages */
    /* Note activity triggers redraw for visual feedback */
    if (status === 0x90 || status === 0x80) {
        needsRedraw = true;
    }
};

globalThis.onMidiMessageExternal = function(data) {
    if (sourceUiActive) {
        if (typeof sourceUi.onMidiMessageExternal === "function") {
            sourceUi.onMidiMessageExternal(data);
        }
        return;
    }

    if (midiFxJsChain.length === 0) return;
    if (!midiSourceAllowed("external")) return;
    processMidiFx([data[0], data[1], data[2]], "external");
};
