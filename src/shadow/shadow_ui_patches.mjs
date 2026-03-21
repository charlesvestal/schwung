/*
 * Shadow UI - Patch views (PATCHES list, PATCH_DETAIL, COMPONENT_PARAMS).
 *
 * Extracted from shadow_ui.js to allow forks to modify patch
 * presentation without touching core.
 */
import * as os from 'os';
import * as std from 'std';
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    announce, announceMenuItem, announceParameter
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ---- Constants ---------------------------------------------------------- */

const PATCH_DIR = "/data/UserData/schwung/patches";

const SYNTH_PARAMS = [
    { key: "preset", label: "Preset", type: "int", min: 0, max: 127 },
    { key: "volume", label: "Volume", type: "float", min: 0, max: 1 },
];

const FX_PARAMS = [
    { key: "wet", label: "Wet", type: "float", min: 0, max: 1 },
    { key: "dry", label: "Dry", type: "float", min: 0, max: 1 },
    { key: "room_size", label: "Size", type: "float", min: 0, max: 1 },
    { key: "damping", label: "Damp", type: "float", min: 0, max: 1 },
];

/* Special patch index value meaning clear the slot - must match shim */
export const PATCH_INDEX_NONE = 65535;

/* ---- Module-local state ------------------------------------------------- */

let selectedDetailItem = 0;
let patchDetail = {
    synthName: "",
    synthPreset: "",
    fx1Name: "",
    fx1Wet: "",
    fx2Name: "",
    fx2Wet: ""
};

/* Component parameter editing state */
let editingComponent = "";
let componentParams = [];
let selectedParam = 0;
let editingValue = false;

/* ---- Helpers ------------------------------------------------------------ */

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

export function loadPatchList() {
    const entries = [];
    let dir = [];
    try {
        dir = os.readdir(PATCH_DIR) || [];
    } catch (e) {
        dir = [];
    }
    const names = dir[0];
    if (!Array.isArray(names)) {
        ctx.patches = entries;
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
    ctx.patches = [{ name: "[New Slot Preset]", file: null }, ...entries];
}

export function findPatchIndexByName(name) {
    const { patches } = ctx;
    if (!name) return 0;
    const match = patches.findIndex((patch) => patch.name === name);
    return match >= 0 ? match : 0;
}

export function findPatchByName(name) {
    loadPatchList();
    const { patches } = ctx;
    for (let i = 1; i < patches.length; i++) {
        if (patches[i].name === name) {
            return i - 1;
        }
    }
    return -1;
}

function fetchPatchDetail(slot) {
    const { getSlotParam } = ctx;
    patchDetail.synthName = getSlotParam(slot, "synth:name") || "Unknown";
    patchDetail.synthPreset = getSlotParam(slot, "synth:preset_name") || getSlotParam(slot, "synth:preset") || "-";
    patchDetail.fx1Name = getSlotParam(slot, "fx1:name") || "None";
    patchDetail.fx1Wet = getSlotParam(slot, "fx1:wet") || "-";
    patchDetail.fx2Name = getSlotParam(slot, "fx2:name") || "None";
    patchDetail.fx2Wet = getSlotParam(slot, "fx2:wet") || "-";
}

function getDetailItems() {
    return [
        { label: "Synth", value: patchDetail.synthName, subvalue: patchDetail.synthPreset, editable: true, component: "synth" },
        { label: "FX1", value: patchDetail.fx1Name, subvalue: patchDetail.fx1Wet, editable: true, component: "fx1" },
        { label: "FX2", value: patchDetail.fx2Name, subvalue: patchDetail.fx2Wet, editable: true, component: "fx2" },
        { label: "Load Patch", value: "", subvalue: "", editable: false, component: "" }
    ];
}

function fetchComponentParams(slot, component) {
    const { getSlotParam } = ctx;
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

function formatParamValue(param) {
    if (param.type === "float") {
        const num = parseFloat(param.value);
        if (isNaN(num)) return param.value;
        return num.toFixed(2);
    }
    return param.value;
}

function adjustParamValue(param, delta) {
    const { KNOB_BASE_STEP_FLOAT } = ctx;
    let val;
    if (param.type === "float") {
        val = parseFloat(param.value) || 0;
        const step = (param.step > 0) ? param.step : KNOB_BASE_STEP_FLOAT;
        val += delta * step;
    } else {
        val = parseInt(param.value) || 0;
        val += delta;
    }

    val = Math.max(param.min, Math.min(param.max, val));

    if (param.type === "float") {
        return val.toFixed(4);
    }
    return String(Math.round(val));
}

export function applyPatchSelection() {
    const { selectedSlot, slots, patches, selectedPatch,
            saveSlotsToConfig, fetchKnobMappings,
            invalidateKnobContextCache, setView, VIEWS } = ctx;
    const patch = patches[selectedPatch];
    const slot = slots[selectedSlot];
    if (!patch || !slot) return;
    const isNewSlot = patch.name === "[New Slot Preset]";
    slot.name = isNewSlot ? "Untitled" : patch.name;
    saveSlotsToConfig(slots);
    if (typeof shadow_request_patch === "function") {
        try {
            const patchIndex = isNewSlot ? PATCH_INDEX_NONE : selectedPatch - 1;
            shadow_request_patch(selectedSlot, patchIndex);
        } catch (e) {
            /* ignore */
        }
    }
    fetchPatchDetail(selectedSlot);
    fetchKnobMappings(selectedSlot);
    invalidateKnobContextCache();
    setView(VIEWS.SLOTS);
    ctx.needsRedraw = true;
}

/* ---- Enter -------------------------------------------------------------- */

export function enterPatchBrowser(slotIndex) {
    const { slots, setView, updateFocusedSlot, VIEWS } = ctx;
    loadPatchList();
    ctx.selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    const patches = ctx.patches;
    if (patches.length === 0) {
        ctx.selectedPatch = 0;
    } else {
        ctx.selectedPatch = findPatchIndexByName(slots[slotIndex]?.name);
    }
    setView(VIEWS.PATCHES);
    ctx.needsRedraw = true;

    if (patches.length === 0) {
        announce("Patch Browser, No patches found");
    } else {
        const patchName = patches[ctx.selectedPatch]?.name || "Unknown";
        announce(`Patch Browser, ${patchName}`);
    }
}

export function enterPatchDetail(slotIndex, patchIndex) {
    const { setView, updateFocusedSlot, VIEWS } = ctx;
    ctx.selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    ctx.selectedPatch = patchIndex;
    selectedDetailItem = 0;
    fetchPatchDetail(slotIndex);
    setView(VIEWS.PATCH_DETAIL);
    ctx.needsRedraw = true;

    const patches = ctx.patches;
    const patchName = patches[patchIndex]?.name || "Unknown";
    const items = getDetailItems();
    if (items.length > 0) {
        const item = items[0];
        const value = item.value || "Empty";
        announce(`${patchName}, ${item.label}: ${value}`);
    }
}

export function enterComponentParams(slot, component) {
    const { setView, VIEWS } = ctx;
    editingComponent = component;
    componentParams = fetchComponentParams(slot, component);
    selectedParam = 0;
    editingValue = false;
    setView(VIEWS.COMPONENT_PARAMS);
    ctx.needsRedraw = true;

    if (componentParams.length > 0) {
        const param = componentParams[0];
        const label = param.label || param.key;
        const value = formatParamValue(param);
        announce(`Component Parameters, ${label}: ${value}`);
    } else {
        announce("Component Parameters, No parameters");
    }
}

/* ---- Draw --------------------------------------------------------------- */

export function drawPatches() {
    const { slots, selectedSlot, patches, selectedPatch, DEFAULT_SLOTS } = ctx;

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

export function drawPatchDetail() {
    const { patches, selectedPatch } = ctx;

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
            print(LIST_LABEL_X, y, `${prefix}${item.label}`, color);
        }
    }

    drawFooter({left: "Back: list", right: "Click: edit"});
}

export function drawComponentParams() {
    const { selectedSlot, getSlotParam } = ctx;

    clear_screen();

    /* Live-refresh read-only param values from DSP */
    for (const param of componentParams) {
        const freshVal = getSlotParam(selectedSlot, param.key);
        if (freshVal !== null) param.value = freshVal;
    }

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

        let valueStr = formatParamValue(param);
        if (isSelected && editingValue) {
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

/* ---- Jog ---------------------------------------------------------------- */

export function handlePatchesJog(delta) {
    const { patches, selectedPatch } = ctx;
    ctx.selectedPatch = Math.max(0, Math.min(patches.length - 1, selectedPatch + delta));
    if (patches.length > 0) {
        const p = patches[ctx.selectedPatch];
        announceMenuItem("Patch", p.name || p);
    }
}

export function handlePatchDetailJog(delta) {
    const items = getDetailItems();
    selectedDetailItem = Math.max(0, Math.min(items.length - 1, selectedDetailItem + delta));
    if (items.length > 0) {
        const di = items[selectedDetailItem];
        announceMenuItem(di.label || di.component || "Item", di.value || "");
    }
}

export function handleComponentParamsJog(delta) {
    const { selectedSlot, setSlotParam } = ctx;
    if (editingValue && componentParams.length > 0) {
        const param = componentParams[selectedParam];
        const newVal = adjustParamValue(param, delta);
        param.value = newVal;
        setSlotParam(selectedSlot, param.key, newVal);
        announceParameter(param.name || param.key, newVal);
    } else {
        selectedParam = Math.max(0, Math.min(componentParams.length - 1, selectedParam + delta));
        if (componentParams.length > 0) {
            const cp = componentParams[selectedParam];
            announceMenuItem(cp.name || cp.key, cp.value || "");
        }
    }
}

/* ---- Select ------------------------------------------------------------- */

export function handlePatchesSelect() {
    const { patches, selectedSlot,
            loadChainConfigFromSlot, setView, VIEWS } = ctx;
    if (patches.length > 0) {
        applyPatchSelection();
        loadChainConfigFromSlot(selectedSlot);
        setView(VIEWS.CHAIN_EDIT);
        ctx.needsRedraw = true;
    }
}

export function handlePatchDetailSelect() {
    const { selectedSlot, loadChainConfigFromSlot, setView, VIEWS } = ctx;
    const items = getDetailItems();
    const item = items[selectedDetailItem];
    if (item.component && item.editable) {
        enterComponentParams(selectedSlot, item.component);
    } else if (selectedDetailItem === items.length - 1) {
        applyPatchSelection();
        loadChainConfigFromSlot(selectedSlot);
        setView(VIEWS.CHAIN_EDIT);
        announce("Patch loaded");
        ctx.needsRedraw = true;
    }
}

export function handleComponentParamsSelect() {
    if (componentParams.length > 0) {
        editingValue = !editingValue;
        const cp = componentParams[selectedParam];
        if (editingValue) {
            announceParameter(cp.name || cp.key, cp.value || "");
        } else {
            announce("Done editing");
        }
    }
}

/* ---- Back --------------------------------------------------------------- */

export function handlePatchesBack() {
    const { setView, VIEWS } = ctx;
    setView(VIEWS.CHAIN_EDIT);
    announce("Chain Editor");
    ctx.needsRedraw = true;
}

export function handlePatchDetailBack() {
    const { setView, VIEWS } = ctx;
    setView(VIEWS.PATCHES);
    announce("Patch Browser");
    ctx.needsRedraw = true;
}

export function handleComponentParamsBack() {
    const { selectedSlot, setView, VIEWS } = ctx;
    if (editingValue) {
        editingValue = false;
        ctx.needsRedraw = true;
        announce("Parameters");
    } else {
        fetchPatchDetail(selectedSlot);
        setView(VIEWS.PATCH_DETAIL);
        announce("Patch Detail");
        ctx.needsRedraw = true;
    }
}
