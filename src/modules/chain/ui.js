/*
 * Signal Chain Module UI
 *
 * Phase 2: Patch browser with synth display
 */

import * as std from 'std';
import { isCapacitiveTouchMessage } from '../../shared/input_filter.mjs';
import { MoveBack, MoveMenu, MoveSteps, MoveMainButton, MoveMainKnob } from '../../shared/constants.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '../../shared/menu_layout.mjs';
import { midiFxRegistry } from './midi_fx/index.mjs';

/* State */
let patchName = "";
let patchCount = 0;
let currentPatch = 0;
let patchNames = [];
let selectedPatch = 0;
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
let availableComponents = {
    sound_generators: [],
    audio_fx: [],
    midi_fx: [],
    midi_sources: []
};

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

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

function scanChainableModules() {
    const root = getModulesRoot();
    if (!root) return;

    availableComponents = {
        sound_generators: [],
        audio_fx: [],
        midi_fx: [],
        midi_sources: []
    };

    /* For now, use known modules - will be replaced with dynamic scan later */
    availableComponents.sound_generators = [
        { id: "sf2", name: "SF2 Synth", params: [{ key: "preset", name: "Preset", type: "int", min: 0, max_param: "preset_count", default: 0 }] },
        { id: "dx7", name: "DX7 Synth", params: [{ key: "preset", name: "Preset", type: "int", min: 0, max_param: "preset_count", default: 0 }] },
        { id: "linein", name: "Line In", params: [] }
    ];

    availableComponents.audio_fx = [
        { id: "freeverb", name: "Freeverb", params: [
            { key: "room_size", name: "Room Size", type: "float", min: 0, max: 1, default: 0.7, step: 0.05 },
            { key: "damping", name: "Damping", type: "float", min: 0, max: 1, default: 0.5, step: 0.05 },
            { key: "wet", name: "Wet", type: "float", min: 0, max: 1, default: 0.35, step: 0.05 },
            { key: "dry", name: "Dry", type: "float", min: 0, max: 1, default: 0.65, step: 0.05 }
        ]}
    ];

    availableComponents.midi_fx = [
        { id: "chord", name: "Chord", params: [
            { key: "type", name: "Chord Type", type: "enum", options: ["none", "major", "minor", "power", "octave"], default: "none" }
        ]},
        { id: "arp", name: "Arpeggiator", params: [
            { key: "mode", name: "Mode", type: "enum", options: ["off", "up", "down", "up_down", "random"], default: "off" },
            { key: "bpm", name: "BPM", type: "int", min: 40, max: 240, default: 120, step: 1 },
            { key: "division", name: "Division", type: "enum", options: ["1/4", "1/8", "1/16"], default: "1/16" }
        ]}
    ];
}

/* Editor view modes */
const EDITOR_VIEW = {
    OVERVIEW: "overview",
    SLOT_MENU: "slot_menu",
    COMPONENT_PICKER: "component_picker",
    PARAM_EDITOR: "param_editor",
    CONFIRM_DELETE: "confirm_delete"
};

/* Editor slot types */
const SLOT_TYPES = ["source", "midi_fx", "synth", "fx1", "fx2"];

function createEditorState(existingPatch = null) {
    if (existingPatch) {
        return {
            isNew: false,
            originalPath: existingPatch.path || "",
            view: EDITOR_VIEW.OVERVIEW,
            selectedSlot: 0,
            slotMenuIndex: 0,
            componentPickerIndex: 0,
            paramIndex: 0,
            confirmIndex: 0,
            chain: {
                source: existingPatch.midi_source_module || null,
                midi_fx: existingPatch.chord_type || existingPatch.arp_mode ? "chord" : null,
                midi_fx_config: {},
                synth: existingPatch.synth_module || "sf2",
                synth_config: { preset: existingPatch.synth_preset || 0 },
                fx1: existingPatch.audio_fx?.[0] || null,
                fx1_config: {},
                fx2: existingPatch.audio_fx?.[1] || null,
                fx2_config: {}
            }
        };
    }
    return {
        isNew: true,
        originalPath: "",
        view: EDITOR_VIEW.COMPONENT_PICKER,
        selectedSlot: 2, /* Start at synth slot */
        slotMenuIndex: 0,
        componentPickerIndex: 0,
        paramIndex: 0,
        confirmIndex: 0,
        chain: {
            source: null,
            midi_fx: null,
            midi_fx_config: {},
            synth: null,
            synth_config: {},
            fx1: null,
            fx1_config: {},
            fx2: null,
            fx2_config: {}
        }
    };
}

function enterEditor(patchIndex = -1) {
    if (patchIndex >= 0 && patchIndex < patchCount) {
        /* Edit existing patch */
        const patchData = {
            path: "",
            midi_source_module: "",
            synth_module: "",
            synth_preset: 0,
            audio_fx: [],
            chord_type: null,
            arp_mode: null
        };
        const name = host_module_get_param(`patch_name_${patchIndex}`);
        patchData.name = name;
        editorState = createEditorState(patchData);
        editorState.editIndex = patchIndex;
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
    needsRedraw = true;
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
        if (viewMode === "patch" && !sourceUiActive && sourceUi) {
            enterSourceUi();
            return true;
        }
    }

    /* Jog wheel for patch navigation */
    if (cc === CC_JOG) {
        if (viewMode === "list") {
            const delta = val < 64 ? val : val - 128;
            if (patchCount > 0 && delta !== 0) {
                const next = selectedPatch + (delta > 0 ? 1 : -1);
                if (next < 0) {
                    selectedPatch = patchCount - 1;
                } else if (next >= patchCount) {
                    selectedPatch = 0;
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
        if (viewMode === "list" && patchCount > 0) {
            host_module_set_param("patch", String(selectedPatch));
            viewMode = "patch";
            needsRedraw = true;
            return true;
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

    if (viewMode === "list") {
        drawMenuHeader("Signal Chain");
        drawMenuList({
            items: patchNames,
            selectedIndex: selectedPatch,
            listArea: {
                topY: menuLayoutDefaults.listTopY,
                bottomY: menuLayoutDefaults.listBottomWithFooter
            },
            getLabel: (name) => name
        });
        drawMenuFooter("Click:load Back:menu");
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

    needsRedraw = false;
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
