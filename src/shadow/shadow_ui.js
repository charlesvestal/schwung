import * as os from 'os';
import * as std from 'std';
import { MoveMainKnob, MoveMainButton, MoveBack } from '../shared/constants.mjs';
import { decodeDelta } from '../shared/input_filter.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '../shared/menu_layout.mjs';

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
    drawMenuHeader("Shadow Chains");
    drawMenuList({
        items: slots,
        selectedIndex: selectedSlot,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.name || "Unknown Patch",
        getValue: (item) => `Ch${item.channel}`
    });
    drawMenuFooter("Click: browse");
}

function drawPatches() {
    clear_screen();
    const channel = slots[selectedSlot]?.channel || (DEFAULT_SLOTS[selectedSlot]?.channel ?? 5 + selectedSlot);
    drawMenuHeader(`Ch${channel} Patch`);
    drawMenuList({
        items: patches,
        selectedIndex: selectedPatch,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.name
    });
    drawMenuFooter("Click: load  Back: slots");
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
    if (!needsRedraw) return;
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
