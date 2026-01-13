/*
 * Menu Render - Hierarchical menu rendering
 *
 * Combines menu_layout drawing functions with menu_items formatting
 * for rendering hierarchical menus.
 */

import {
    drawMenuHeader,
    drawMenuList,
    drawMenuFooter,
    menuLayoutDefaults
} from './menu_layout.mjs';

import { formatItemValue } from './menu_items.mjs';

/**
 * Draw a hierarchical menu with proper value formatting
 * @param {Object} options
 * @param {string} options.title - Menu title
 * @param {Array} options.items - Menu items (from menu_items.mjs)
 * @param {Object} options.state - Menu state (from menu_nav.mjs createMenuState)
 * @param {string} [options.footer] - Optional footer text
 */
export function drawHierarchicalMenu({ title, items, state, footer }) {
    drawMenuHeader(title);

    const bottomY = footer
        ? menuLayoutDefaults.listBottomWithFooter
        : menuLayoutDefaults.listBottomNoFooter;

    drawMenuList({
        items,
        selectedIndex: state.selectedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY
        },
        valueAlignRight: true,
        getLabel: (item) => {
            if (!item) return '';
            return item.label || '';
        },
        getValue: (item, index) => {
            if (!item) return '';
            const isEditing = state.editing && index === state.selectedIndex;
            return formatItemValue(item, isEditing, state.editValue);
        }
    });

    if (footer) {
        drawMenuFooter(footer);
    }
}

/**
 * Draw a menu from a menu stack
 * @param {Object} options
 * @param {Object} options.stack - Menu stack (from createMenuStack)
 * @param {Object} options.state - Menu state (from createMenuState)
 * @param {string} [options.footer] - Optional footer text
 */
export function drawStackMenu({ stack, state, footer }) {
    const current = stack.current();
    if (!current) return;

    drawHierarchicalMenu({
        title: current.title,
        items: current.items,
        state,
        footer
    });
}
