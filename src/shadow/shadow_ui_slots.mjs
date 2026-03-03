/*
 * Shadow UI - Slot views (SLOTS list + SLOT_SETTINGS).
 *
 * Extracted from shadow_ui.js to allow forks to modify slot
 * presentation without touching core.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/move-anything/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/move-anything/shared/menu_layout.mjs';
import {
    announce, announceMenuItem, announceParameter
} from '/data/UserData/move-anything/shared/screen_reader.mjs';

/* ---- Slot settings definition ------------------------------------------- */

export const SLOT_SETTINGS = [
    { key: "patch", label: "Patch", type: "action" },
    { key: "chain", label: "Edit Chain", type: "action" },
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "slot:muted", label: "Muted", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:soloed", label: "Soloed", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 0, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -2, max: 15, step: 1 },
];

/* ---- Module-local state ------------------------------------------------- */

let selectedSetting = 0;
let editingSettingValue = false;

/* ---- Helpers ------------------------------------------------------------ */

export function getSlotSettingValue(slot, setting) {
    const { slots, getSlotParam } = ctx;
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

function adjustSlotSetting(slot, setting, delta) {
    if (setting.type === "action") return;

    const { getSlotParam, setSlotParam } = ctx;
    const current = getSlotParam(slot, setting.key);
    let val;

    if (setting.type === "float") {
        val = parseFloat(current) || 0;
        val += delta * setting.step;
    } else {
        val = parseInt(current) || 0;
        val += delta * setting.step;
    }

    val = Math.max(setting.min, Math.min(setting.max, val));
    const newVal = setting.type === "float" ? val.toFixed(2) : String(Math.round(val));
    setSlotParam(slot, setting.key, newVal);
}

/* ---- Enter -------------------------------------------------------------- */

export function enterSlotSettings(slotIndex) {
    const { setView, updateFocusedSlot, VIEWS } = ctx;
    ctx.selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);
    selectedSetting = 0;
    editingSettingValue = false;
    setView(VIEWS.SLOT_SETTINGS);
    ctx.needsRedraw = true;

    const setting = SLOT_SETTINGS[0];
    const val = getSlotSettingValue(slotIndex, setting);
    announceMenuItem(`Slot Settings, ${setting.label}`, val);
}

/* ---- Draw --------------------------------------------------------------- */

export function drawSlots() {
    const { slots, selectedSlot, slotDirtyCache,
            getSlotParam, getMasterFxDisplayName } = ctx;

    clear_screen();
    drawHeader("Shadow Chains");

    let trackSelectedSlot = 0;
    if (typeof shadow_get_selected_slot === "function") {
        trackSelectedSlot = shadow_get_selected_slot();
    }

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

    const debugInfo = typeof globalThis._debugFlags !== "undefined"
        ? `F:${globalThis._debugFlags}` : "";
    let stateInfo = "";
    if (typeof shadow_get_debug_state === "function") {
        stateInfo = shadow_get_debug_state();
    }
    drawFooter(`${debugInfo} ${stateInfo}`);
}

export function drawSlotSettings() {
    const { slots, selectedSlot, getSlotParam } = ctx;

    clear_screen();
    drawHeader(`Slot ${selectedSlot + 1}`);

    const listY = LIST_TOP_Y;
    const lineHeight = LIST_LINE_HEIGHT;
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

/* ---- Jog ---------------------------------------------------------------- */

export function handleSlotsJog(delta) {
    const { slots, updateFocusedSlot } = ctx;
    ctx.selectedSlot = Math.max(0, Math.min(slots.length, ctx.selectedSlot + delta));
    updateFocusedSlot(ctx.selectedSlot);
}

export function handleSlotSettingsJog(delta) {
    const { selectedSlot } = ctx;
    if (editingSettingValue) {
        const setting = SLOT_SETTINGS[selectedSetting];
        adjustSlotSetting(selectedSlot, setting, delta);
        const newVal = getSlotSettingValue(selectedSlot, setting);
        announceParameter(setting.label, newVal);
    } else {
        selectedSetting = Math.max(0, Math.min(SLOT_SETTINGS.length - 1, selectedSetting + delta));
        const setting = SLOT_SETTINGS[selectedSetting];
        const val = getSlotSettingValue(selectedSlot, setting);
        announceMenuItem(setting.label, val);
    }
}

/* ---- Select ------------------------------------------------------------- */

export function handleSlotsSelect() {
    const { selectedSlot, slots, enterChainEdit, enterMasterFxSettings } = ctx;
    if (selectedSlot < slots.length) {
        enterChainEdit(selectedSlot);
    } else {
        enterMasterFxSettings();
    }
}

export function handleSlotSettingsSelect() {
    const { selectedSlot, enterPatchBrowser, enterChainEdit } = ctx;
    const setting = SLOT_SETTINGS[selectedSetting];
    if (setting.type === "action") {
        if (setting.key === "patch") {
            enterPatchBrowser(selectedSlot);
        } else if (setting.key === "chain") {
            enterChainEdit(selectedSlot);
        }
    } else {
        editingSettingValue = !editingSettingValue;
    }
}

/* ---- Back --------------------------------------------------------------- */

export function handleSlotsBack() {
    if (typeof shadow_request_exit === "function") {
        shadow_request_exit();
    }
}

export function handleSlotSettingsBack() {
    const { setView, VIEWS } = ctx;
    if (editingSettingValue) {
        editingSettingValue = false;
        ctx.needsRedraw = true;
        announce("Slot Settings");
    } else {
        setView(VIEWS.SLOTS);
        announce("Slots");
        ctx.needsRedraw = true;
    }
}
