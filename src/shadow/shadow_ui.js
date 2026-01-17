import * as os from 'os';
import * as std from 'std';

const MoveMainKnob = 14;
const MoveMainButton = 3;
const MoveBack = 51;

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

let slots = [];
let patches = [];
let selectedSlot = 0;
let selectedPatch = 0;
let view = "slots";
let needsRedraw = true;
let refreshCounter = 0;
let redrawCounter = 0;
const REDRAW_INTERVAL = 2; // ~30fps at 16ms tick

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
        const match = raw.match(/\"name\"\\s*:\\s*\"([^\"]+)\"/);
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
    patches = entries;
}

function findPatchIndexByName(name) {
    if (!name) return 0;
    const match = patches.findIndex((patch) => patch.name === name);
    return match >= 0 ? match : 0;
}

function enterPatchBrowser(slotIndex) {
    loadPatchList();
    if (patches.length === 0) {
        return;
    }
    selectedSlot = slotIndex;
    selectedPatch = findPatchIndexByName(slots[slotIndex]?.name);
    view = "patches";
    needsRedraw = true;
}

function applyPatchSelection() {
    const patch = patches[selectedPatch];
    const slot = slots[selectedSlot];
    if (!patch || !slot) return;
    slot.name = patch.name;
    saveSlotsToConfig(slots);
    if (typeof shadow_request_patch === "function") {
        try {
            shadow_request_patch(selectedSlot, selectedPatch);
        } catch (e) {
            /* ignore */
        }
    }
    view = "slots";
    needsRedraw = true;
}

function handleJog(delta) {
    if (view === "slots") {
        selectedSlot = Math.max(0, Math.min(slots.length - 1, selectedSlot + delta));
    } else {
        selectedPatch = Math.max(0, Math.min(patches.length - 1, selectedPatch + delta));
    }
    needsRedraw = true;
}

function handleSelect() {
    if (view === "slots") {
        enterPatchBrowser(selectedSlot);
        return;
    }
    applyPatchSelection();
}

function handleBack() {
    if (view === "patches") {
        view = "slots";
        needsRedraw = true;
    }
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
    drawFooter("Click: browse");
}

function drawPatches() {
    clear_screen();
    const channel = slots[selectedSlot]?.channel || (DEFAULT_SLOTS[selectedSlot]?.channel ?? 5 + selectedSlot);
    drawHeader(`Ch${channel} Patch`);
    drawList(
        patches,
        selectedPatch,
        (item) => item.name
    );
    drawFooter("Click: load  Back: slots");
}

globalThis.init = function() {
    refreshSlots();
    loadPatchList();
};

globalThis.tick = function() {
    refreshCounter++;
    if (refreshCounter % 120 === 0) {
        refreshSlots();
    }
    redrawCounter++;
    if (!needsRedraw && (redrawCounter % REDRAW_INTERVAL !== 0)) {
        return;
    }
    needsRedraw = false;
    if (view === "slots") {
        drawSlots();
    } else {
        drawPatches();
    }
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    if ((status & 0xF0) === 0xB0) {
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
    }
};

globalThis.onMidiMessageExternal = function(_data) {
    /* ignore */
};
