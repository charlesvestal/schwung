import * as os from 'os';
import * as std from 'std';

const MoveMainKnob = 14;
const MoveMainButton = 3;
const MoveBack = 51;

/* Knob CC range for parameter control */
const KNOB_CC_START = 71;
const KNOB_CC_END = 78;
const NUM_KNOBS = 8;

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const TITLE_Y = 2;
const TITLE_RULE_Y = 12;
const LIST_TOP_Y = 15;
const LIST_LINE_HEIGHT = 11;
const LIST_HIGHLIGHT_HEIGHT = LIST_LINE_HEIGHT + 2;
const LIST_LABEL_X = 4;
const LIST_VALUE_X = 92;
const FOOTER_TEXT_Y = SCREEN_HEIGHT - 11;
const FOOTER_RULE_Y = FOOTER_TEXT_Y - 1;

function decodeDelta(value) {
    if (value === 0) return 0;
    if (value >= 1 && value <= 63) return 1;
    if (value >= 65 && value <= 127) return -1;
    return 0;
}

function truncateText(text, maxChars) {
    if (text.length <= maxChars) return text;
    if (maxChars <= 3) return text.slice(0, maxChars);
    return `${text.slice(0, maxChars - 3)}...`;
}

function drawHeader(title) {
    print(2, TITLE_Y, title, 1);
    fill_rect(0, TITLE_RULE_Y, SCREEN_WIDTH, 1, 1);
}

function drawFooter(text) {
    if (!text) return;
    fill_rect(0, FOOTER_RULE_Y, SCREEN_WIDTH, 1, 1);
    print(2, FOOTER_TEXT_Y, text, 1);
}

function drawList(items, selectedIndex, getLabel, getValue) {
    const maxVisible = Math.max(1, Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / LIST_LINE_HEIGHT));
    let startIdx = 0;
    const maxSelectedRow = maxVisible - 2;
    if (selectedIndex > maxSelectedRow) {
        startIdx = selectedIndex - maxSelectedRow;
    }
    const endIdx = Math.min(startIdx + maxVisible, items.length);
    const maxLabelChars = Math.floor((LIST_VALUE_X - LIST_LABEL_X - 6) / 6);

    for (let i = startIdx; i < endIdx; i++) {
        const y = LIST_TOP_Y + (i - startIdx) * LIST_LINE_HEIGHT;
        const item = items[i];
        const label = truncateText(getLabel(item, i), maxLabelChars);
        const value = getValue ? getValue(item, i) : "";
        if (i === selectedIndex) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
            print(LIST_LABEL_X, y, `> ${label}`, 0);
            if (value) {
                print(LIST_VALUE_X, y, value, 0);
            }
        } else {
            print(LIST_LABEL_X, y, `  ${label}`, 1);
            if (value) {
                print(LIST_VALUE_X, y, value, 1);
            }
        }
    }
}

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
    SLOTS: "slots",           // List of 4 chain slots
    SLOT_SETTINGS: "settings", // Per-slot settings (volume, channels)
    PATCHES: "patches",       // Patch list for selected slot
    PATCH_DETAIL: "detail",   // Show synth/fx info for selected patch
    COMPONENT_PARAMS: "params" // Edit component params (Phase 3)
};

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

/* Knob overlay state */
let knobMappings = [];       // {cc, name, value} for each knob
let knobOverlayActive = false;
let knobOverlayTimeout = 0;
let knobOverlayName = "";
let knobOverlayValue = "";
let lastKnobSlot = -1;       // Track slot changes to refresh mappings

/* Slot settings definitions */
const SLOT_SETTINGS = [
    { key: "patch", label: "Patch", type: "action" },  // Opens patch browser
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 1, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -1, max: 16, step: 1 },  // -1 = none
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
    const byChannel = new Map();
    for (const entry of data.patches) {
        if (!entry || typeof entry.channel !== "number") continue;
        if (typeof entry.name !== "string") continue;
        byChannel.set(entry.channel, entry.name);
    }
    return DEFAULT_SLOTS.map((slot) => ({
        channel: slot.channel,
        name: byChannel.get(slot.channel) || slot.name
    }));
}

function saveSlotsToConfig(nextSlots) {
    const payload = {
        patches: nextSlots.map((slot) => ({
            name: slot.name,
            channel: slot.channel
        }))
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
        /* -1 means "auto" (same as receive channel), otherwise show specific channel */
        return ch < 0 ? "Auto" : `Ch ${ch + 1}`;
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
    switch (view) {
        case VIEWS.SLOTS:
            selectedSlot = Math.max(0, Math.min(slots.length - 1, selectedSlot + delta));
            updateFocusedSlot(selectedSlot);
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
    }
    needsRedraw = true;
}

function handleSelect() {
    switch (view) {
        case VIEWS.SLOTS:
            enterSlotSettings(selectedSlot);
            break;
        case VIEWS.SLOT_SETTINGS:
            const setting = SLOT_SETTINGS[selectedSetting];
            if (setting.type === "action") {
                /* Patch action - go to patch browser */
                enterPatchBrowser(selectedSlot);
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
    }
    needsRedraw = true;
}

function handleBack() {
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
            view = VIEWS.SLOT_SETTINGS;
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
    }
}

/* Handle knob turn - show overlay with parameter name and value */
function handleKnobTurn(knobIndex, value) {
    /* Refresh knob mappings if slot changed */
    if (lastKnobSlot !== selectedSlot) {
        fetchKnobMappings(selectedSlot);
    }

    /* Refresh value from DSP (it processes the CC) */
    const newValue = getSlotParam(selectedSlot, `knob_${knobIndex + 1}_value`);
    if (knobMappings[knobIndex]) {
        knobMappings[knobIndex].value = newValue || "-";
    }

    /* Show overlay */
    const mapping = knobMappings[knobIndex];
    if (mapping) {
        knobOverlayActive = true;
        knobOverlayTimeout = 30;  /* ~500ms at 60fps tick rate */
        knobOverlayName = mapping.name;
        knobOverlayValue = mapping.value;
    }
    needsRedraw = true;
}

function drawSlots() {
    clear_screen();
    drawHeader("Shadow Chains");
    drawList(
        slots,
        selectedSlot,
        (item) => item.name || "Unknown Patch",
        (item) => `Ch${item.channel}`
    );
    drawFooter("Click: settings");
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
        drawList(
            patches,
            selectedPatch,
            (item) => item.name
        );
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

/* Draw knob parameter overlay centered on screen */
function drawKnobOverlay() {
    if (!knobOverlayActive) return;

    const boxW = 100, boxH = 24;
    const boxX = (SCREEN_WIDTH - boxW) / 2;
    const boxY = (SCREEN_HEIGHT - boxH) / 2;

    /* Draw box with black background and white border */
    fill_rect(boxX, boxY, boxW, boxH, 0);
    draw_rect(boxX, boxY, boxW, boxH, 1);

    /* Draw parameter name and value */
    print(boxX + 4, boxY + 4, truncateText(knobOverlayName, 14), 1);
    print(boxX + 4, boxY + 14, knobOverlayValue, 1);
}

globalThis.init = function() {
    refreshSlots();
    loadPatchList();
    updateFocusedSlot(selectedSlot);
    fetchKnobMappings(selectedSlot);
};

globalThis.tick = function() {
    refreshCounter++;
    if (refreshCounter % 120 === 0) {
        refreshSlots();
    }

    /* Update knob overlay timeout */
    if (knobOverlayTimeout > 0) {
        knobOverlayTimeout--;
        if (knobOverlayTimeout === 0) {
            knobOverlayActive = false;
            needsRedraw = true;
        }
    }

    /* Refresh knob mappings if slot changed */
    if (lastKnobSlot !== selectedSlot) {
        fetchKnobMappings(selectedSlot);
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
        default:
            drawSlots();
    }

    /* Draw knob overlay on top of main view */
    drawKnobOverlay();

    /* Debug: show frame counter and last CC to prove display updates */
    print(100, 2, `${redrawCounter % 1000}`, 1);
    print(2, SCREEN_HEIGHT - 2, `CC${lastCC.cc}:${lastCC.val}`, 1);
};

let debugMidiCounter = 0;
let lastCC = { cc: 0, val: 0 };
globalThis.onMidiMessageInternal = function(data) {
    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    /* Debug: log all CC messages to help diagnose input issues */
    if ((status & 0xF0) === 0xB0) {
        debugMidiCounter++;
        lastCC = { cc: d1, val: d2 };
        needsRedraw = true;

        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                handleJog(delta);
            }
            return;
        }
        if (d1 === MoveMainButton && d2 > 0) {
            handleSelect();
            return;
        }
        if (d1 === MoveBack && d2 > 0) {
            handleBack();
            return;
        }

        /* Handle knob CCs (71-78) for parameter control overlay */
        if (d1 >= KNOB_CC_START && d1 <= KNOB_CC_END) {
            const knobIndex = d1 - KNOB_CC_START;
            handleKnobTurn(knobIndex, d2);
            return;
        }
    }
};

globalThis.onMidiMessageExternal = function(_data) {
    /* ignore */
};
