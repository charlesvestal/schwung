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
} from '/data/UserData/move-anything/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/move-anything/shared/menu_layout.mjs';
import {
    announce, announceMenuItem
} from '/data/UserData/move-anything/shared/screen_reader.mjs';

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
    const { masterFxConfig } = ctx;
    const parts = [];
    for (let i = 1; i <= 4; i++) {
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

    const BOX_W = 22;
    const BOX_H = 16;
    const GAP = 2;
    const TOTAL_W = 5 * BOX_W + 4 * GAP;
    const START_X = Math.floor((SCREEN_WIDTH - TOTAL_W) / 2);
    const BOX_Y = 20;

    const presetSelected = selectedMasterFxComponent === -1;

    for (let i = 0; i < MASTER_FX_CHAIN_COMPONENTS.length; i++) {
        const comp = MASTER_FX_CHAIN_COMPONENTS[i];
        const x = START_X + i * (BOX_W + GAP);
        const isSelected = i === selectedMasterFxComponent;

        let abbrev = "--";
        if (comp.key === "settings") {
            abbrev = "*";
        } else {
            const moduleData = masterFxConfig[comp.key];
            abbrev = moduleData ? getModuleAbbrev(moduleData.module) : "--";
        }

        const fillBox = presetSelected || isSelected;
        if (fillBox) {
            fill_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        } else {
            draw_rect(x, BOX_Y, BOX_W, BOX_H, 1);
        }

        const textColor = fillBox ? 0 : 1;
        const textX = x + Math.floor((BOX_W - abbrev.length * 5) / 2) + 1;
        const textY = BOX_Y + 5;
        print(textX, textY, abbrev, textColor);
    }

    const selectedComp = presetSelected ? null : MASTER_FX_CHAIN_COMPONENTS[selectedMasterFxComponent];
    const labelY = BOX_Y + BOX_H + 4;
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

    drawHeader("Overwrite?");

    const name = truncateText(masterPendingSaveName, 20);
    print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < 2; i++) {
        const y = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === masterConfirmIndex;
        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
    }

    drawFooter("Back: cancel");
}

function drawMasterConfirmDelete() {
    const { currentMasterPresetName, masterConfirmIndex } = ctx;

    drawHeader("Delete?");

    const name = truncateText(currentMasterPresetName, 20);
    print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < 2; i++) {
        const y = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === masterConfirmIndex;
        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
    }

    drawFooter("Back: cancel");
}
