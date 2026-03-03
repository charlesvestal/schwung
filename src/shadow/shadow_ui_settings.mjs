/*
 * Shadow UI - Settings views (chain settings, global settings).
 *
 * Extracted from shadow_ui.js to allow forks to modify settings
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

/* ---- Draw --------------------------------------------------------------- */

export function drawChainSettings() {
    const { showingNamePreview, pendingSaveName, namePreviewIndex,
            confirmingOverwrite, confirmingDelete, confirmIndex,
            selectedSlot, slots, selectedChainSetting,
            editingChainSettingValue,
            getChainSettingsItems, getChainSettingValue } = ctx;

    clear_screen();

    if (showingNamePreview) {
        drawHeader("Save As");
        const name = truncateText(pendingSaveName, 20);
        print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

        const listY = LIST_TOP_Y + 16;
        for (let i = 0; i < 2; i++) {
            const y = listY + i * LIST_LINE_HEIGHT;
            const isSelected = i === namePreviewIndex;
            if (isSelected) {
                fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
            }
            print(LIST_LABEL_X, y, i === 0 ? "Edit" : "OK", isSelected ? 0 : 1);
        }
        drawFooter("Back: cancel");
        return;
    }

    if (confirmingOverwrite) {
        drawHeader("Overwrite?");
        const name = truncateText(pendingSaveName, 20);
        print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

        const listY = LIST_TOP_Y + 16;
        for (let i = 0; i < 2; i++) {
            const y = listY + i * LIST_LINE_HEIGHT;
            const isSelected = i === confirmIndex;
            if (isSelected) {
                fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
            }
            print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
        }
        drawFooter("Back: cancel");
        return;
    }

    if (confirmingDelete) {
        drawHeader("Delete?");
        const name = truncateText(
            slots[selectedSlot] ? slots[selectedSlot].name : "Unknown", 20);
        print(LIST_LABEL_X, LIST_TOP_Y, '"' + name + '"', 1);

        const listY = LIST_TOP_Y + 16;
        for (let i = 0; i < 2; i++) {
            const y = listY + i * LIST_LINE_HEIGHT;
            const isSelected = i === confirmIndex;
            if (isSelected) {
                fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
            }
            print(LIST_LABEL_X, y, i === 0 ? "No" : "Yes", isSelected ? 0 : 1);
        }
        drawFooter("Back: cancel");
        return;
    }

    drawHeader("S" + (selectedSlot + 1) + " Settings");

    const items = getChainSettingsItems(selectedSlot);
    const listY = LIST_TOP_Y;
    const lineHeight = 9;
    const maxVisible = Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / lineHeight);

    let scrollOffset = 0;
    if (selectedChainSetting >= maxVisible) {
        scrollOffset = selectedChainSetting - maxVisible + 1;
    }

    for (let i = 0; i < maxVisible && (i + scrollOffset) < items.length; i++) {
        const itemIdx = i + scrollOffset;
        const y = listY + i * lineHeight;
        const setting = items[itemIdx];
        const isSelected = itemIdx === selectedChainSetting;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }

        const labelColor = isSelected ? 0 : 1;
        print(LIST_LABEL_X, y, setting.label, labelColor);

        if (setting.type !== "action") {
            const value = getChainSettingValue(selectedSlot, setting);
            if (value) {
                const valueX = SCREEN_WIDTH - value.length * 5 - 4;
                if (isSelected && editingChainSettingValue) {
                    print(valueX - 8, y, "<", 0);
                    print(valueX, y, value, 0);
                    print(valueX + value.length * 5 + 2, y, ">", 0);
                } else {
                    print(valueX, y, value, labelColor);
                }
            }
        }
    }
}

export function drawGlobalSettings() {
    const { helpDetailScrollState, helpNavStack,
            drawHelpDetail, drawHelpList,
            globalSettingsInSection, globalSettingsSectionIndex,
            globalSettingsItemIndex, globalSettingsEditing,
            GLOBAL_SETTINGS_SECTIONS,
            getMasterFxSettingValue } = ctx;

    clear_screen();

    if (helpDetailScrollState) {
        drawHelpDetail();
        return;
    }
    if (helpNavStack.length > 0) {
        drawHelpList();
        return;
    }

    if (globalSettingsInSection) {
        const section = GLOBAL_SETTINGS_SECTIONS[globalSettingsSectionIndex];
        drawHeader(truncateText(section.label, 18));

        drawMenuList({
            items: section.items,
            selectedIndex: globalSettingsItemIndex,
            getLabel: (item) => {
                if (globalSettingsEditing &&
                    section.items[globalSettingsItemIndex] === item) {
                    return "[" + item.label + "]";
                }
                return item.label;
            },
            getValue: (item) => {
                if (item.type === "action") return "";
                return getMasterFxSettingValue(item);
            },
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            valueAlignRight: true
        });

        drawFooter("Back: Settings");
    } else {
        drawHeader("Settings");

        drawMenuList({
            items: GLOBAL_SETTINGS_SECTIONS,
            selectedIndex: globalSettingsSectionIndex,
            getLabel: (section) => section.label,
            getValue: () => "",
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            valueAlignRight: false
        });

        drawFooter("Back: return");
    }
}
