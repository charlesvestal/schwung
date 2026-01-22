/*
 * Main menu screen renderer and input handler.
 *
 * Menu organization:
 * - Featured modules (Signal Chain) - directly selectable
 * - Categories (Sound Generators >, Audio FX >, etc.) - navigable submenus
 * - System modules (Module Store) - directly selectable
 * - Settings
 * - Return to Move
 *
 * Categories are read from each module's component_type field in module.json.
 */

import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/move-anything/shared/menu_layout.mjs';

const MENU_LABEL = "Move Anything";

/* Category display order and names - modules provide their own component_type */
const CATEGORY_ORDER = [
    { id: 'featured', name: null },           /* No category entry, modules listed directly */
    { id: 'sound_generator', name: 'Sound Generators' },
    { id: 'audio_fx', name: 'Audio FX' },
    { id: 'midi_fx', name: 'MIDI FX' },
    { id: 'midi_source', name: 'MIDI Sources' },
    { id: 'utility', name: 'Utilities' },
    /* 'system' modules (Module Store) are listed directly at the end */
];

/* Current view state */
let currentView = 'main';      /* 'main' or category id */
let menuItems = [];
let selectableIndices = [];
let modulesByCategory = {};    /* Cache of modules grouped by category */

/* Group modules by category */
function groupModulesByCategory(modules) {
    const byCategory = {};
    for (const cat of CATEGORY_ORDER) {
        byCategory[cat.id] = [];
    }
    byCategory['system'] = [];
    byCategory['other'] = [];

    for (const mod of modules) {
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

    return byCategory;
}

/* Build main menu items */
function buildMainMenuItems(byCategory) {
    const items = [];

    /* Featured modules (listed directly, no category entry) */
    for (const mod of byCategory['featured']) {
        items.push({ type: 'module', module: mod, label: mod.name });
    }

    /* Category entries (navigable with >) */
    for (const cat of CATEGORY_ORDER) {
        if (!cat.name) continue;  /* Skip featured */
        const mods = byCategory[cat.id];
        if (mods.length === 0) continue;

        items.push({
            type: 'category',
            categoryId: cat.id,
            label: cat.name
        });
    }

    /* Other (uncategorized) as a category if any exist */
    if (byCategory['other'].length > 0) {
        items.push({
            type: 'category',
            categoryId: 'other',
            label: 'Other'
        });
    }

    /* System modules (listed directly) */
    for (const mod of byCategory['system']) {
        items.push({ type: 'module', module: mod, label: mod.name });
    }

    /* Settings and Return to Move */
    items.push({ type: 'settings', label: 'Settings' });
    items.push({ type: 'exit', label: 'Return to Move' });

    return items;
}

/* Build category submenu items */
function buildCategoryMenuItems(categoryId, byCategory) {
    const items = [];
    const mods = byCategory[categoryId] || [];

    for (const mod of mods) {
        items.push({ type: 'module', module: mod, label: mod.name });
    }

    return items;
}

/* Get selectable items (all items are selectable in new design) */
function getSelectableIndices(items) {
    const indices = [];
    for (let i = 0; i < items.length; i++) {
        indices.push(i);
    }
    return indices;
}

/* Get category name for display */
function getCategoryName(categoryId) {
    for (const cat of CATEGORY_ORDER) {
        if (cat.id === categoryId) return cat.name;
    }
    if (categoryId === 'other') return 'Other';
    return categoryId;
}

export function drawMainMenu({ modules, selectedIndex, volume }) {
    /* Group modules by category */
    modulesByCategory = groupModulesByCategory(modules);

    /* Build items based on current view */
    if (currentView === 'main') {
        menuItems = buildMainMenuItems(modulesByCategory);
    } else {
        menuItems = buildCategoryMenuItems(currentView, modulesByCategory);
    }
    selectableIndices = getSelectableIndices(menuItems);

    /* Header */
    if (currentView === 'main') {
        drawMenuHeader(MENU_LABEL, `Vol:${volume}`);
    } else {
        drawMenuHeader(getCategoryName(currentView), `Vol:${volume}`);
    }

    if (menuItems.length === 0) {
        if (currentView === 'main') {
            print(2, 24, "No modules found", 1);
            print(2, 36, "Check modules/ dir", 1);
        } else {
            print(2, 24, "No modules in category", 1);
        }
        return;
    }

    /* Clamp selectedIndex */
    const maxIndex = selectableIndices.length - 1;
    const clampedIndex = Math.min(selectedIndex, maxIndex);

    /* Draw list */
    const hasFooter = currentView !== 'main';
    drawMenuList({
        items: menuItems,
        selectedIndex: clampedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: hasFooter ? menuLayoutDefaults.listBottomWithFooter : menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => item.label,
        getValue: (item) => item.type === 'category' ? '>' : '',
        valueAlignRight: true
    });

    /* Footer for category view */
    if (currentView !== 'main') {
        drawMenuFooter("Back: return");
    }
}

export function handleMainMenuCC({ cc, value, selectedIndex, totalItems }) {
    const isDown = value > 0;
    let nextIndex = selectedIndex;
    let didSelect = false;
    let didBack = false;

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
    } else if (cc === 51 && isDown) {
        /* Back button (MoveBack) */
        didBack = true;
    }

    return { nextIndex, didSelect, didBack };
}

/* Get what was selected */
export function getSelectedItem(selectedIndex) {
    return menuItems[selectedIndex];
}

/* Get total selectable items */
export function getSelectableCount() {
    return selectableIndices.length;
}

/* Navigation: enter a category */
export function enterCategory(categoryId) {
    currentView = categoryId;
}

/* Navigation: return to main menu */
export function exitCategory() {
    currentView = 'main';
}

/* Check if in category view */
export function isInCategory() {
    return currentView !== 'main';
}

/* Get current view */
export function getCurrentView() {
    return currentView;
}

/* Reset to main view (called on module load/unload) */
export function resetToMain() {
    currentView = 'main';
}
