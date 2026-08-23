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
    FOOTER_RULE_Y
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
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

/*
 * VIEWS.GLOBAL_SETTINGS is the HELP VIEWER'S HOST and nothing else.
 *
 * The settings themselves are a synthesised module contract drawn by the page
 * chrome (shadow_ui_global_grid.mjs + enterGlobalSettingsGrid), so the section
 * list and the in-section list that used to live here are gone along with the
 * four globalSettings* state vars and the three switch arms that drove them.
 *
 * The view survives because the help stack has to be drawn somewhere and it is
 * already drawn here and under VIEWS.MASTER_FX; giving it a view of its own is
 * a separate change. runGlobalActionFromGrid sets this view when [Help...] is
 * chosen and maybeReturnToGlobalGrid takes the page back when the stack empties.
 *
 * Empty stack + this view is a state nothing should be able to reach, so it
 * draws nothing rather than pretending: the reconcile runs before the draw.
 */
export function drawGlobalSettings() {
    const { helpDetailScrollState, helpNavStack,
            drawHelpDetail, drawHelpList } = ctx;

    clear_screen();

    if (helpDetailScrollState) {
        drawHelpDetail();
        return;
    }
    if (helpNavStack.length > 0) {
        drawHelpList();
    }
}
