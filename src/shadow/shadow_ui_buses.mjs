/*
 * Shadow UI — the SLOT BUS screens: the bus list, one bus's actions, the voice
 * multi-select, and a bus's 8-position insert chain.
 *
 * A sibling .mjs rather than more of shadow_ui.js, which is already ~24k lines.
 *
 * WHERE THESE SCREENS HANG. The chain row is horizontal and buses hang BELOW
 * the synth box: Down on the synth opens the list, Down on a bus row opens that
 * bus's inserts. Nothing else in the chain editor changes.
 *
 * DRAWING ONLY. Every rule these screens draw — what the rows are, what a row
 * says, which actions a row has — is in shared/bus_model.mjs, where tests/host
 * can run it; this file is the pictures, like every other shadow_ui_*.mjs.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList,
    drawConfirmModal
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    LIST_TOP_Y,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawChainDiagram
} from '/data/UserData/schwung/shared/chain_diagram.mjs';
import { drawChainEditorBands, drawChainPicker }
    from '/data/UserData/schwung/shared/chain_editor_chrome.mjs';
import {
    busListRows, busRowLabel, busRowValue, busActionItems, busSendValue,
    voiceRows, voiceRowValue, busChainComponents
} from '/data/UserData/schwung/shared/bus_model.mjs';

/* ==========================================================================
 * THE SCREENS
 *
 * Full-screen, so each clears — the same thing every sibling shadow view module
 * does through the device globals. (The rule that a renderer must not clear is
 * param_pages', where a page has to be able to sit inside a caller's chrome.)
 * ========================================================================== */

/* The band a screen prints while a read has not answered. It says WAITING, and
 * it is deliberately not an empty list: an empty list is a claim about the
 * slot, and a failed read makes no claim at all. */
function drawWaiting(title) {
    ctx.clearScreen();
    drawHeader(title, "");
    ctx.print(4, LIST_TOP_Y + 8, "Reading...", 1);
    drawFooter(["Back: out"]);
}

export function drawBusList() {
    const { busConfig, busListIndex, getModuleAbbrev, slotLabel } = ctx;
    if (!busConfig || busConfig.unresolved) { drawWaiting("Buses"); return; }
    ctx.clearScreen();
    drawHeader("Buses", slotLabel());
    const rows = busListRows(busConfig, getModuleAbbrev);
    drawMenuList({
        items: rows,
        selectedIndex: busListIndex,
        getLabel: busRowLabel,
        getValue: busRowValue,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        /* The value column starts far left of the default 92: this row carries
         * an insert summary AND two send levels, and at the default it fits
         * about five characters — the render showed "TA..." with both levels
         * gone. The label floor (eight characters, MEASURED) still protects the
         * bus name, so a short name costs the value nothing. */
        valueX: 52,
    });
    /* The verb of the row under the CURSOR, and never more than three pairs:
     * drawFooter drops a pair that does not fit AND every pair after it, so the
     * primary action goes first and the count is kept where it fits. */
    const row = rows[busListIndex];
    if (row && row.kind === "new") drawFooter(["Clk: create", "Back: out"]);
    /* "Dn: fx", not "Dn: inserts": drawFooter pins BACK and drops the MIDDLE
     * pairs that do not fit, silently, so the longer word cost this row the
     * hint for its own second gesture. Measured in the render, not guessed. */
    else if (row && row.kind === "bus") drawFooter(["Clk: edit", "Dn: fx", "Back: out"]);
    else drawFooter(["Clk: edit", "Back: out"]);
}

export function drawBusActions() {
    const { busConfig, busActionsRow, busActionsIndex, busActionsEditing,
            busConfirmingDelete, busConfirmIndex, getModuleAbbrev } = ctx;
    if (!busConfig || busConfig.unresolved) { drawWaiting("Bus"); return; }
    const rows = busListRows(busConfig, getModuleAbbrev);
    const row = rows[busActionsRow];
    if (!row) { drawWaiting("Bus"); return; }
    if (busConfirmingDelete) {
        ctx.clearScreen();
        drawConfirmModal({
            title: "Delete Bus?", name: row.name, selectedIndex: busConfirmIndex,
            footer: "Back: cancel",
        });
        return;
    }
    ctx.clearScreen();
    /* The orphan count is in the HEADER, where it is on screen for every row of
     * this menu rather than only while the cursor happens to be on Voices. */
    drawHeader(truncateText(row.name, 14), row.orphans > 0 ? `!${row.orphans}` : "");
    const items = busActionItems(row);
    drawMenuList({
        items,
        selectedIndex: busActionsIndex,
        getLabel: (it) => it.label,
        getValue: (it) => (it.type === "int" ? String(busSendValue(row, it.id)) : ""),
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        editMode: busActionsEditing,
    });
    /* The verb of the row under the CURSOR — branching on `type === "int"`
     * alone left every OTHER row (Voices, Inserts, Rename, Delete) reading
     * "Clk: open", which is wrong for three of the four and actively
     * misleading on Delete/Rename. One verb per id, matching what Click
     * actually does (see handleSelect's VIEWS.BUS_ACTIONS case). */
    const item = items[busActionsIndex];
    let verb = "open";
    if (item) {
        if (item.type === "int") verb = "edit";
        else if (item.id === "voices") verb = "edit";
        else if (item.id === "chain") verb = "fx";
        else if (item.id === "rename") verb = "rename";
        else if (item.id === "delete") verb = "delete";
    }
    if (busActionsEditing) drawFooter(["Jog: level", "Clk: done"]);
    else drawFooter([`Clk: ${verb}`, "Back: out"]);
}

export function drawBusVoices() {
    const { busConfig, busVoices, busVoicesIndex, busVoicesBus } = ctx;
    if (!busConfig || busConfig.unresolved || !busVoices || busVoices.unresolved) {
        drawWaiting("Voices"); return;
    }
    const bus = busConfig.buses[busVoicesBus];
    ctx.clearScreen();
    drawHeader("Voices", truncateText(bus ? bus.name : "", 8));
    const rows = voiceRows(busConfig, busVoices.voices, busVoicesBus);
    drawMenuList({
        items: rows,
        selectedIndex: busVoicesIndex,
        getLabel: (r) => r.label,
        getValue: (r) => voiceRowValue(r, busConfig),
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
    });
    const row = rows[busVoicesIndex];
    if (row && row.kind === "orphan") drawFooter(["Clk: clear", "Back: out"]);
    else if (row && row.mine) drawFooter(["Clk: remove", "Back: out"]);
    else drawFooter(["Clk: add", "Back: out"]);
}

export function drawBusChain() {
    const { busConfig, busChainBus, busChainPos, getModuleAbbrev,
            movyCtx, selectingBusModule } = ctx;
    if (selectingBusModule) { drawBusModuleSelect(); return; }
    if (!busConfig || busConfig.unresolved) { drawWaiting("Inserts"); return; }
    const bus = busConfig.buses[busChainBus];
    if (!bus) { drawWaiting("Inserts"); return; }
    ctx.clearScreen();
    const movy = movyCtx();
    const comps = busChainComponents(bus.fx);
    drawChainDiagram(movy, comps, busChainPos, {
        allSelected: false,
        abbrev: (comp) => {
            if (comp.kind === "add") return "+";
            return comp.module ? getModuleAbbrev(comp.module) : "--";
        },
        /* Bypass only. A bus chain has no LFOs — the chain host serves no
         * bus<N>:lfoN key at all — so asking for one would be an IPC round trip
         * that can only answer "". */
        marks: (comp) => {
            const e = bus.fx[comp.index];
            return (e && e.bypassed) ? { bypassed: true, lfo1: false, lfo2: false } : null;
        },
    });
    const comp = comps[busChainPos];
    let info = "(empty)";
    if (comp && comp.kind === "add") info = "New effect";
    else if (comp && comp.module) info = comp.module;
    drawChainEditorBands(movy, {
        headerLeft: truncateText(bus.name, 14),
        headerRight: "BUS",
        label: comp ? comp.label : "",
        info,
        /* The verb of the box under the CURSOR: the `+` ADDS and a loaded
         * position SWAPS. One fixed pair said SWAP on both, which named a
         * different action than the one the click performs. */
        hints: [["JOG", "SEL"],
                ["CLK", comp && comp.kind === "add" ? "ADD" : "SWAP"],
                ["BACK", "OUT"]],
    });
}

export function drawBusModuleSelect() {
    const { busPickerItems, busPickerIndex, busChainBus, busChainPos,
            busConfig, movyCtx } = ctx;
    ctx.clearScreen();
    const bus = busConfig && !busConfig.unresolved ? busConfig.buses[busChainBus] : null;
    const comps = busChainComponents(bus ? bus.fx : []);
    const comp = comps[busChainPos];
    drawChainPicker(movyCtx(), {
        headerLeft: `${truncateText(bus ? bus.name : "Bus", 8)} > ${comp ? comp.label : ""}`,
        entries: busPickerItems,
        index: busPickerIndex,
        currentId: comp ? comp.module : "",
        emptyMessage: "No FX modules available",
    });
}
