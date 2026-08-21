/*
 * Shadow UI - Master FX views (chain, settings, module select, presets).
 *
 * Extracted from shadow_ui.js to allow forks to modify Master FX
 * presentation without touching core.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawChainDiagram, DIAGRAM_W, BOX_H as DIAGRAM_BOX_H
} from '/data/UserData/schwung/shared/chain_diagram.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList,
    drawConfirmModal
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    announce, announceMenuItem
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ---- Enter -------------------------------------------------------------- */

export function enterMasterFxSettings() {
    const { scanForAudioFxModules, loadMasterFxChainConfig,
            getMasterFxSlotModule, MASTER_FX_CHAIN_COMPONENTS,
            setView, VIEWS } = ctx;

    ctx.MASTER_FX_OPTIONS = scanForAudioFxModules();
    loadMasterFxChainConfig();
    ctx.selectedMasterFxComponent = 0;
    ctx.selectingMasterFxModule = false;
    setView(VIEWS.MASTER_FX);
    ctx.needsRedraw = true;

    const comp = MASTER_FX_CHAIN_COMPONENTS[0];
    const moduleName = getMasterFxSlotModule(0) || "Empty";
    announce(`Master FX, ${comp.label} ${moduleName}`);
}

/* ---- Display name (used in slot list) ----------------------------------- */

export function getMasterFxDisplayName() {
    /* MASTER_FX_SLOTS comes across on ctx, mirroring the constant in
     * shadow_ui.js (itself a mirror of shadow_chain_mgmt.h). Never re-derive
     * the cap here. */
    const { masterFxConfig, MASTER_FX_SLOTS } = ctx;
    const parts = [];
    for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
        const key = `fx${i}`;
        if (masterFxConfig[key]?.module) {
            parts.push(masterFxConfig[key].module);
        }
    }
    return parts.length > 0 ? parts.join("+") : "None";
}

/* ---- Draw --------------------------------------------------------------- */

export function drawMasterFx() {
    const { masterShowingNamePreview, masterConfirmingOverwrite,
            masterConfirmingDelete, helpDetailScrollState, helpNavStack,
            inMasterPresetPicker, inMasterFxSettingsMenu,
            selectingMasterFxModule, selectedMasterFxComponent,
            masterFxConfig, MASTER_FX_CHAIN_COMPONENTS, MASTER_FX_OPTIONS,
            currentMasterPresetName, getMasterFxParam,
            getModuleAbbrev, isTextEntryActive, drawTextEntry,
            drawHelpDetail, drawHelpList } = ctx;

    clear_screen();

    if (isTextEntryActive()) {
        drawTextEntry();
        return;
    }

    if (masterShowingNamePreview) {
        drawMasterNamePreview();
        return;
    }

    if (masterConfirmingOverwrite) {
        drawMasterConfirmOverwrite();
        return;
    }

    if (masterConfirmingDelete) {
        drawMasterConfirmDelete();
        return;
    }

    if (helpDetailScrollState) {
        drawHelpDetail();
        return;
    }
    if (helpNavStack.length > 0) {
        drawHelpList();
        return;
    }

    if (inMasterPresetPicker) {
        drawMasterPresetPicker();
        return;
    }

    if (inMasterFxSettingsMenu) {
        drawMasterFxSettingsMenu();
        return;
    }

    if (selectingMasterFxModule) {
        drawMasterFxModuleSelect();
        return;
    }

    drawHeader("Master FX");

    /*
     * The row of boxes is shared/chain_diagram.mjs — the same renderer the slot
     * chain editor draws, so the two screens stop looking like different
     * products.
     *
     * What it replaces was a fixed row: `TOTAL_W = 5 * BOX_W + 4 * GAP`, five
     * 22px boxes filling 118 of the 128px screen exactly. A fixed row cannot
     * report that it overflowed. At the 8-slot cap the nine boxes are 214px
     * wide, and boxes 6..9 — together with their bypass "B" and LFO "~"
     * markers, which were drawn by two more hand-rolled loops over the FULL
     * component list — would have gone off the right edge with no clipping and
     * no error. The shared diagram windows the row around the selection
     * (scrollWindow in chain_model.mjs) and puts a dotted rail in the margin on
     * whichever side has more chain, so the count can grow without the layout
     * having an opinion about it.
     */
    const BOX_Y = 20;
    /* Centred, unlike the slot editor, which shifts right to clear its
     * slot-indicator column. Master FX has no such column, so the strip keeps
     * the x it has always had. */
    const START_X = Math.floor((SCREEN_WIDTH - DIAGRAM_W) / 2);

    const presetSelected = selectedMasterFxComponent === -1;

    /* The device hands the renderers these four primitives and nothing else. */
    const dctx = {
        fillRect: fill_rect,
        print: print,
        textWidth: typeof text_width === "function" ? text_width : undefined,
        setPixel: set_pixel,
    };

    /*
     * Which components an LFO is pointed at. Four IPC reads, FIXED — the
     * question is asked of the two LFOs, never of each box, so it does not grow
     * with the slot cap.
     */
    const mfxLfoTargets = {};
    if (typeof shadow_get_param === "function") {
        for (let li = 1; li <= 2; li++) {
            if (shadow_get_param(0, "master_fx:lfo" + li + ":enabled") !== "1") continue;
            const t = shadow_get_param(0, "master_fx:lfo" + li + ":target") || "";
            if (!t) continue;
            if (!mfxLfoTargets[t]) mfxLfoTargets[t] = {};
            mfxLfoTargets[t]["lfo" + li] = true;
        }
    }

    drawChainDiagram(dctx, MASTER_FX_CHAIN_COMPONENTS, selectedMasterFxComponent, {
        x: START_X,
        y: BOX_Y,
        /* Selecting the preset row lights the whole chain, as it always has. */
        allSelected: presetSelected,
        abbrev: (comp) => {
            if (comp.kind === "settings") return "*";
            /* masterFxConfig is the cached copy the editor already holds — no
             * IPC here. Nothing in this list is `kind: "synth"`, so no box gets
             * the synth band: Master FX has no synth to landmark. */
            const moduleData = masterFxConfig[comp.key];
            return moduleData ? getModuleAbbrev(moduleData.module) : "--";
        },
        marks: (comp) => {
            /* The settings box is not an FX slot: it has no bypass parameter
             * and cannot be an LFO target, so it must never be asked. */
            if (comp.kind === "settings") return null;
            const lfo = mfxLfoTargets[comp.key];
            /* One read per DRAWN box — at most the diagram capacity, five —
             * rather than one per slot. The loop this came from ran the whole
             * component list, so the 4 -> 8 raise would otherwise have doubled
             * the per-frame IPC cost of a screen that redraws every frame, at
             * ~2.8ms a read against a 1.68ms whole-page render. */
            const bypassed = typeof shadow_get_param === "function" &&
                parseInt(shadow_get_param(0, `master_fx:${comp.key}:bypassed`) || "0", 10) === 1;
            if (!lfo && !bypassed) return null;
            return { bypassed, lfo1: lfo && lfo.lfo1, lfo2: lfo && lfo.lfo2 };
        },
    });

    const selectedComp = presetSelected ? null : MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
    const labelY = BOX_Y + DIAGRAM_BOX_H + 4;
    const label = presetSelected ? "Preset" : (selectedComp ? selectedComp.label : "");
    const labelX = Math.floor((SCREEN_WIDTH - label.length * 5) / 2);
    print(labelX, labelY, label, 1);

    const infoY = labelY + 12;
    let infoLine = "";
    if (presetSelected) {
        infoLine = currentMasterPresetName || "(no preset)";
    } else if (selectedComp && selectedComp.key !== "settings") {
        const moduleData = masterFxConfig[selectedComp.key];
        if (moduleData && moduleData.module) {
            const opt = MASTER_FX_OPTIONS.find(o => o.id === moduleData.module);
            const displayName = opt ? opt.name : moduleData.module;
            const preset = getMasterFxParam(selectedMasterFxComponent, "preset_name") ||
                          getMasterFxParam(selectedMasterFxComponent, "preset") || "";
            infoLine = preset ? `${displayName} (${truncateText(preset, 8)})` : displayName;
        } else {
            infoLine = "(empty)";
        }
    } else if (selectedComp && selectedComp.key === "settings") {
        infoLine = "Configure master FX";
    }
    infoLine = truncateText(infoLine, 24);
    const infoX = Math.floor((SCREEN_WIDTH - infoLine.length * 5) / 2);
    print(infoX, infoY, infoLine, 1);
}

function drawMasterFxSettingsMenu() {
    const { currentMasterPresetName, selectedMasterFxSetting,
            getMasterFxSettingsItems, getMasterFxSettingValue } = ctx;

    const title = currentMasterPresetName || "Master FX";
    drawHeader(truncateText(title, 18));

    const items = getMasterFxSettingsItems();

    drawMenuList({
        items: items,
        selectedIndex: selectedMasterFxSetting,
        getLabel: (item) => item.label,
        getValue: (item) => {
            if (item.type === "action") return "";
            return getMasterFxSettingValue(item);
        },
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true
    });

    drawFooter("Back: FX chain");
}

function drawMasterFxModuleSelect() {
    const { selectedMasterFxComponent, MASTER_FX_CHAIN_COMPONENTS,
            MASTER_FX_OPTIONS, selectedMasterFxModuleIndex,
            masterFxConfig } = ctx;

    const comp = MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
    drawHeader(`Select ${comp ? comp.label : "FX"}`);

    if (MASTER_FX_OPTIONS.length === 0) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No FX modules available", 1);
        return;
    }

    drawMenuList({
        items: MASTER_FX_OPTIONS,
        selectedIndex: selectedMasterFxModuleIndex,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        getLabel: (item) => item.name,
        getValue: (item) => {
            const currentModule = masterFxConfig[comp.key]?.module || "";
            return item.id === currentModule ? "*" : "";
        }
    });
    drawFooter({left: "Back: cancel", right: "Click: apply"});
}

function drawMasterPresetPicker() {
    const { masterPresets, selectedMasterPresetIndex,
            currentMasterPresetName } = ctx;

    drawHeader("Master Presets");

    const items = [{ name: "[New]", index: -1 }];
    for (let i = 0; i < masterPresets.length; i++) {
        items.push(masterPresets[i]);
    }

    drawMenuList({
        items: items,
        selectedIndex: selectedMasterPresetIndex,
        getLabel: (item) => {
            const isCurrent = item.index >= 0 && masterPresets[item.index]?.name === currentMasterPresetName;
            return isCurrent ? `* ${item.name}` : item.name;
        },
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y }
    });

    drawFooter("Back: cancel");
}

function drawMasterNamePreview() {
    const { masterPendingSaveName, masterNamePreviewIndex } = ctx;

    drawHeader("Save As");

    const name = truncateText(masterPendingSaveName, 20);
    print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < 2; i++) {
        const y = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === masterNamePreviewIndex;
        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, y, i === 0 ? "Edit" : "OK", isSelected ? 0 : 1);
    }

    drawFooter("Back: cancel");
}

function drawMasterConfirmOverwrite() {
    const { masterPendingSaveName, masterConfirmIndex } = ctx;
    drawConfirmModal({ title: "Overwrite?", name: masterPendingSaveName, selectedIndex: masterConfirmIndex });
}

function drawMasterConfirmDelete() {
    const { currentMasterPresetName, masterConfirmIndex } = ctx;
    drawConfirmModal({ title: "Delete?", name: currentMasterPresetName, selectedIndex: masterConfirmIndex });
}
