/*
 * Shadow UI - Settings views (chain settings, global settings).
 *
 * Extracted from shadow_ui.js to allow forks to modify settings
 * presentation without touching core.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    /* Row geometry is gone from this file: nothing here draws a row any more,
     * drawMenuList / drawNamePreview do. */
    LIST_TOP_Y,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList,
    drawConfirmModal,
    drawNamePreview
} from '/data/UserData/schwung/shared/menu_layout.mjs';

/* ---- Draw --------------------------------------------------------------- */

export function drawChainSettings() {
    const { showingNamePreview, pendingSaveName, namePreviewIndex,
            confirmingOverwrite, confirmingDelete, confirmIndex,
            selectedSlot, slots, selectedChainSetting,
            editingChainSettingValue,
            getChainSettingsItems, getChainSettingValue } = ctx;

    clear_screen();

    if (showingNamePreview) {
        drawNamePreview({ name: pendingSaveName, selectedIndex: namePreviewIndex });
        return;
    }

    if (confirmingOverwrite) {
        drawConfirmModal({ title: "Overwrite?", name: pendingSaveName, selectedIndex: confirmIndex });
        return;
    }

    if (confirmingDelete) {
        drawConfirmModal({
            title: "Delete?",
            name: slots[selectedSlot] ? slots[selectedSlot].name : "Unknown",
            selectedIndex: confirmIndex
        });
        return;
    }

    drawHeader("S" + (selectedSlot + 1) + " Settings");

    const items = getChainSettingsItems(selectedSlot);

    drawMenuList({
        items,
        selectedIndex: selectedChainSetting,
        getLabel: (item) => item.label,
        getValue: (item) => item.type === "action"
            ? ""
            : (getChainSettingValue(selectedSlot, item) || ""),
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true,
        editMode: editingChainSettingValue
    });
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
            getLabel: (item) => item.label,
            getValue: (item) => {
                if (item.type === "action") return "";
                return getMasterFxSettingValue(item);
            },
            listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
            valueAlignRight: true,
            prioritizeSelectedValue: true,
            /* The fourth spelling of "you are editing this row" was
             * bracket-the-LABEL. editMode brackets the VALUE, which is what
             * every other list on the device does. */
            editMode: globalSettingsEditing
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
