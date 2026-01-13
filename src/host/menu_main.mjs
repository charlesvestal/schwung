/*
 * Main menu screen renderer and input handler.
 *
 * Menu organization:
 * 1. Featured modules (Signal Chain, always first)
 * 2. Categories (Sound Generators, Audio FX, MIDI FX, Utilities) - sorted alphabetically
 * 3. System modules (Module Store, near end)
 * 4. Settings
 * 5. Return to Move
 *
 * Categories are read from each module's component_type field in module.json.
 */

import { drawMenuHeader, drawMenuList, menuLayoutDefaults } from '../shared/menu_layout.mjs';

const MENU_LABEL = "Move Anything";

/* Category display order and names - modules provide their own component_type */
const CATEGORY_ORDER = [
    { id: 'featured', name: null },           /* No header, just Signal Chain */
    { id: 'sound_generator', name: 'Sound Generators' },
    { id: 'audio_fx', name: 'Audio FX' },
    { id: 'midi_fx', name: 'MIDI FX' },
    { id: 'midi_source', name: 'MIDI Sources' },
    { id: 'utility', name: 'Utilities' },
    { id: 'system', name: null },             /* No header, Module Store */
];

/* Build organized menu items from module list */
function organizeModules(modules) {
    const items = [];

    /* Group modules by category (read from module's component_type) */
    const byCategory = {};
    for (const cat of CATEGORY_ORDER) {
        byCategory[cat.id] = [];
    }
    byCategory['other'] = [];  /* Uncategorized */

    for (const mod of modules) {
        /* Use component_type from module.json, fall back to 'other' if not set */
        const category = mod.component_type || 'other';
        if (byCategory[category]) {
            byCategory[category].push(mod);
        } else {
            byCategory['other'].push(mod);
        }
    }

    /* Sort each category alphabetically by name */
    for (const cat of Object.keys(byCategory)) {
        byCategory[cat].sort((a, b) => a.name.localeCompare(b.name));
    }

    /* Build items list in order */
    for (const cat of CATEGORY_ORDER) {
        const mods = byCategory[cat.id];
        if (mods.length === 0) continue;

        /* Add category header if it has a name */
        if (cat.name) {
            items.push({ type: 'header', label: cat.name });
        }

        /* Add modules in this category */
        for (const mod of mods) {
            items.push({ type: 'module', module: mod, label: mod.name });
        }
    }

    /* Add any uncategorized modules */
    if (byCategory['other'].length > 0) {
        items.push({ type: 'header', label: 'Other' });
        for (const mod of byCategory['other']) {
            items.push({ type: 'module', module: mod, label: mod.name });
        }
    }

    /* Add Settings and Return to Move */
    items.push({ type: 'settings', label: 'Settings' });
    items.push({ type: 'exit', label: 'Return to Move' });

    return items;
}

/* Get selectable items (skip headers) */
function getSelectableIndices(items) {
    const indices = [];
    for (let i = 0; i < items.length; i++) {
        if (items[i].type !== 'header') {
            indices.push(i);
        }
    }
    return indices;
}

/* Exported state for menu items */
let menuItems = [];
let selectableIndices = [];

export function drawMainMenu({ modules, selectedIndex, volume }) {
    /* Rebuild menu items (could cache this) */
    menuItems = organizeModules(modules);
    selectableIndices = getSelectableIndices(menuItems);

    drawMenuHeader(MENU_LABEL, `Vol:${volume}`);

    if (modules.length === 0) {
        print(2, 24, "No modules found", 1);
        print(2, 36, "Check modules/ dir", 1);
        return;
    }

    /* Map selectedIndex to actual item index */
    const actualIndex = selectableIndices[selectedIndex] || 0;

    drawMenuList({
        items: menuItems,
        selectedIndex: actualIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => item.label,
        getValue: () => "",
        isHeader: (item) => item.type === 'header',
        isSelectable: (item) => item.type !== 'header'
    });
}

export function handleMainMenuCC({ cc, value, selectedIndex, totalItems }) {
    const isDown = value > 0;
    let nextIndex = selectedIndex;
    let didSelect = false;

    /* totalItems should be number of selectable items */
    const maxIndex = selectableIndices.length - 1;

    if (cc === 14) {
        const delta = value < 64 ? value : value - 128;
        if (delta > 0) {
            nextIndex = Math.min(selectedIndex + 1, maxIndex);
        } else if (delta < 0) {
            nextIndex = Math.max(selectedIndex - 1, 0);
        }
    } else if (cc === 54 && isDown) {
        nextIndex = Math.min(selectedIndex + 1, maxIndex);
    } else if (cc === 55 && isDown) {
        nextIndex = Math.max(selectedIndex - 1, 0);
    } else if (cc === 3 && isDown) {
        didSelect = true;
    }

    return { nextIndex, didSelect };
}

/* Get what was selected */
export function getSelectedItem(selectedIndex) {
    const actualIndex = selectableIndices[selectedIndex];
    return menuItems[actualIndex];
}

/* Get total selectable items */
export function getSelectableCount() {
    return selectableIndices.length;
}
