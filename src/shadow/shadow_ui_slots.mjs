/*
 * Shadow UI - Slot views (SLOTS list + SLOT_SETTINGS).
 *
 * Extracted from shadow_ui.js to allow forks to modify slot
 * presentation without touching core.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    /* Row geometry (SCREEN_WIDTH, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
     * LIST_LABEL_X, LIST_VALUE_X) is gone from this file: nothing here draws a
     * row any more, drawMenuList does. */
    LIST_TOP_Y,
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

/* ---- Slot settings definition ------------------------------------------- */

export const SLOT_SETTINGS = [
    { key: "patch", label: "Patch", type: "action" },
    { key: "chain", label: "Edit Chain", type: "action" },
    /* The other door onto a slot's split-voice buses. This list and
     * CHAIN_SETTINGS_ITEMS overlap heavily already (Volume through MPE Mode are
     * in both); the duplication is pre-existing and matched here rather than
     * left as an asymmetry — a row that exists on one of the two ways into a
     * slot and not the other is worse than either. Hidden when the synth
     * publishes no `split_voices` — see slotSettingsItems. */
    { key: "buses", label: "Buses", type: "action" },
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 4, step: 0.05 },
    /* THE SLOT SEND — the whole slot into global Send A / Send B, needing no
     * bus and therefore nothing at all from the module. A per-bus send is only
     * reachable through `split_voices`, which almost no module publishes, so
     * without these two rows the send buses have no feed on an ordinary synth.
     * Post-fader and post-slot-FX; drained by chain_drain_main_send.
     *
     * Step 4 rather than 1: this is a 0..127 level on a JOG, and 127 detents to
     * cross it is not an edit. Both ends stay reachable — the adjust clamps, so
     * 124 + 4 lands on 127. The knob grid keeps step 1, which is what a knob is
     * for. */
    { key: "buses:main_send1", label: "Send A", type: "int", min: 0, max: 127, step: 4 },
    { key: "buses:main_send2", label: "Send B", type: "int", min: 0, max: 127, step: 4 },
    { key: "slot:muted", label: "Muted", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:soloed", label: "Soloed", type: "int", min: 0, max: 1, step: 1 },
    { key: "slot:receive_channel", label: "Recv Ch", type: "int", min: 0, max: 16, step: 1 },
    { key: "slot:forward_channel", label: "Fwd Ch", type: "int", min: -2, max: 15, step: 1 },
    { key: "slot:transpose", label: "Transpose", type: "int", min: -12, max: 12, step: 1 },
    { key: "midi_fx_pre_mode", label: "MIDI FX", type: "int", min: 0, max: 1, step: 1 },
    { key: "mpe_mode", label: "MPE Mode", type: "int", min: 0, max: 1, step: 1 },
    /* Automation lanes. The third form of this screen (CHAIN_SETTINGS_ITEMS)
     * and the knob grid's Actions menu carry the same row and reach the same
     * implementation -- a row on two of the three is the asymmetry the `buses`
     * comment above calls worse than either. */
    { key: "clear_lanes", label: "Clear Lanes", type: "action" },
];

/*
 * The rows this slot actually shows.
 *
 * ONE list for the draw, the jog and the click. Three sites indexed
 * SLOT_SETTINGS directly and a conditional row makes that a click acting on a
 * row that was not drawn, which is the bug the bus list's own `ctx.busRows()`
 * exists to prevent.
 *
 * `chainSynthSplits` is the host's — cached, and false for a read that did not
 * complete, so a stalled channel costs one draw without the row.
 */
export function slotSettingsItems(slot) {
    const splits = ctx.chainSynthSplits ? ctx.chainSynthSplits(slot) : false;
    return SLOT_SETTINGS.filter((item) => item.key !== "buses" || splits);
}

/* ---- Module-local state ------------------------------------------------- */

let selectedSetting = 0;
let editingSettingValue = false;

/* ---- Helpers ------------------------------------------------------------ */

/* Check if a slot is in MPE mode (Recv=All + Fwd=THRU) */
function isSlotMpe(slot) {
    const { getSlotParam } = ctx;
    const recv = parseInt(getSlotParam(slot, "slot:receive_channel")) || 0;
    const fwd = parseInt(getSlotParam(slot, "slot:forward_channel"));
    return recv === 0 && fwd === -2;
}

export function getSlotSettingValue(slot, setting) {
    const { slots, getSlotParam } = ctx;
    if (setting.key === "patch") {
        return slots[slot]?.name || "Unknown";
    }
    if (setting.key === "mpe_mode") {
        return isSlotMpe(slot) ? "On" : "Off";
    }
    /* Host-side, because it is the same count the other settings list prints
     * and one cache serves both. */
    if (setting.key === "buses") {
        return ctx.slotBusCountLabel ? ctx.slotBusCountLabel(slot) : "";
    }
    /* Nothing to show, and answered HERE so the fallback below does not spend
     * a ~2.8 ms IPC round trip per draw reading a key no slot serves. */
    if (setting.key === "clear_lanes") return "";
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
    if (setting.key === "slot:transpose") {
        const n = parseInt(val) || 0;
        if (n === 0) return "0 st";
        return `${n > 0 ? "+" : ""}${n} st`;
    }
    if (setting.key === "midi_fx_pre_mode") {
        return parseInt(val) ? "Schw+Move" : "Schw";
    }
    return val;
}

/* State to restore when MPE mode is turned off */
const preMpeState = [null, null, null, null];

function adjustSlotSetting(slot, setting, delta) {
    if (setting.type === "action") return;

    const { getSlotParam, setSlotParam } = ctx;

    /* MPE Mode toggle: sets recv/fwd/synth MPE in one action */
    if (setting.key === "mpe_mode") {
        const mpeOn = isSlotMpe(slot);
        if (delta > 0 && !mpeOn) {
            /* Save current recv/fwd for restore */
            preMpeState[slot] = {
                recv: getSlotParam(slot, "slot:receive_channel"),
                fwd: getSlotParam(slot, "slot:forward_channel"),
            };
            setSlotParam(slot, "slot:receive_channel", "0");    /* All */
            setSlotParam(slot, "slot:forward_channel", "-2");   /* THRU */
            setSlotParam(slot, "synth:mpe_enabled", "1");
        } else if (delta < 0 && mpeOn) {
            /* Restore previous settings or defaults */
            const prev = preMpeState[slot];
            setSlotParam(slot, "slot:receive_channel", prev?.recv || String(slot + 1));
            setSlotParam(slot, "slot:forward_channel", prev?.fwd || "-1");
            setSlotParam(slot, "synth:mpe_enabled", "0");
            preMpeState[slot] = null;
        }
        return;
    }

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

    const setting = slotSettingsItems(slotIndex)[0];
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

    /* Mute/solo for every slot in ONE shared-memory read (~490ns), instead of
     * two get_param calls per slot inside the map below — which was eight
     * synchronous round trips at ~2.9ms each, about a whole 22.7ms frame, to
     * decide whether to draw an "M" or an "S". Measured on device, those were
     * 81% of every parameter read the UI made.
     *
     * It has to happen here in the DRAW rather than in refreshSlots(), which
     * only runs every 120 ticks (~2.7s) — a mute glyph lagging the button
     * press by that long is worse than the cost it saves.
     *
     * null means a v1 shim that does not mirror these (possible mid-upgrade,
     * since shim and shadow_ui are separate files); fall back to the round
     * trip rather than silently draw every slot as unmuted. */
    let slotFlags = null;
    if (typeof shadow_get_slot_flags === "function") {
        try { slotFlags = shadow_get_slot_flags(); } catch (e) { slotFlags = null; }
    }

    const items = [
        ...slots.map((s, i) => {
            const f = slotFlags ? slotFlags[i] : null;
            const muted = (typeof f === "number")
                ? (f & 1) !== 0 : getSlotParam(i, "slot:muted") === "1";
            const soloed = (typeof f === "number")
                ? (f & 2) !== 0 : getSlotParam(i, "slot:soloed") === "1";
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

    /* The hand-drawn "> " / "* " prefix is gone: drawMenuList supplies the
     * caret and editMode carries the editing marker. */
    drawMenuList({
        items: slotSettingsItems(selectedSlot),
        selectedIndex: selectedSetting,
        getLabel: (setting) => `${setting.label}:`,
        getValue: (setting) => truncateText(
            getSlotSettingValue(selectedSlot, setting), 10),
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true,
        editMode: editingSettingValue
    });

    if (editingSettingValue) {
        drawFooter(["Click: done", "Jog: adjust"]);
    } else {
        drawFooter(["Back: slots", "Click: edit"]);
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
    const items = slotSettingsItems(selectedSlot);
    if (editingSettingValue) {
        const setting = items[selectedSetting];
        if (!setting) return;
        adjustSlotSetting(selectedSlot, setting, delta);
        const newVal = getSlotSettingValue(selectedSlot, setting);
        announceParameter(setting.label, newVal);
    } else {
        selectedSetting = Math.max(0, Math.min(items.length - 1, selectedSetting + delta));
        const setting = items[selectedSetting];
        if (!setting) return;
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
    const { selectedSlot, enterPatchBrowser, enterChainEdit, enterBusList } = ctx;
    const setting = slotSettingsItems(selectedSlot)[selectedSetting];
    if (!setting) return;
    if (setting.type === "action") {
        if (setting.key === "patch") {
            enterPatchBrowser(selectedSlot);
        } else if (setting.key === "chain") {
            enterChainEdit(selectedSlot);
        } else if (setting.key === "clear_lanes") {
            /* Acts and announces; it opens nothing, so this screen stays up
             * and the announcement is the whole feedback. */
            if (ctx.clearSlotLanes) ctx.clearSlotLanes(selectedSlot);
        } else if (setting.key === "buses") {
            /* Back comes back to THIS screen, not to the other slot settings
             * list — the thunk is what carries that, and it announces itself,
             * so the destination and the announcement cannot disagree. */
            enterBusList(selectedSlot, () => enterSlotSettings(selectedSlot));
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
