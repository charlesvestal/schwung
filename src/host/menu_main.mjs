/*
 * Main menu screen renderer and input handler.
 */

import { drawMenuHeader, drawMenuList, menuLayoutDefaults } from '../shared/menu_layout.mjs';

const MENU_LABEL = "Move Anything";

export function drawMainMenu({ modules, selectedIndex, volume }) {
    drawMenuHeader(MENU_LABEL, `Vol:${volume}`);

    const totalItems = modules.length + 2;
    if (totalItems === 1 && modules.length === 0) {
        print(2, 24, "No modules found", 1);
        print(2, 36, "Check modules/ dir", 1);
        print(2, 48, "> Settings", 1);
        return;
    }

    drawMenuList({
        items: Array.from({ length: totalItems }, (_, i) => i),
        selectedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (index) => {
            if (index < modules.length) return modules[index].name;
            if (index === modules.length) return "Settings";
            return "Return to Move";
        },
        getValue: () => ""
    });
}

export function handleMainMenuCC({ cc, value, selectedIndex, totalItems }) {
    const isDown = value > 0;
    let nextIndex = selectedIndex;
    let didSelect = false;

    if (cc === 14) {
        const delta = value < 64 ? value : value - 128;
        if (delta > 0) {
            nextIndex = Math.min(selectedIndex + 1, totalItems - 1);
        } else if (delta < 0) {
            nextIndex = Math.max(selectedIndex - 1, 0);
        }
    } else if (cc === 54 && isDown) {
        nextIndex = Math.min(selectedIndex + 1, totalItems - 1);
    } else if (cc === 55 && isDown) {
        nextIndex = Math.max(selectedIndex - 1, 0);
    } else if (cc === 3 && isDown) {
        didSelect = true;
    }

    return { nextIndex, didSelect };
}
