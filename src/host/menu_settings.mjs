/*
 * Settings screen using shared menu components.
 */

import { createValue, createEnum, createToggle, createBack, createSubmenu, createInfo, capitalize } from '/data/UserData/move-anything/shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '/data/UserData/move-anything/shared/menu_nav.mjs';
import { createMenuStack } from '/data/UserData/move-anything/shared/menu_stack.mjs';
import { drawStackMenu } from '/data/UserData/move-anything/shared/menu_render.mjs';

import * as std from 'std';

const VELOCITY_CURVES = ['linear', 'soft', 'hard', 'full'];
const PAD_LAYOUTS = ['chromatic', 'fourth'];
const CLOCK_MODES = ['off', 'internal', 'external'];
const HOST_VERSION_FILE = '/data/UserData/move-anything/host/version.txt';

/**
 * Get host version from version.txt
 */
function getHostVersion() {
    try {
        const versionStr = std.loadFile(HOST_VERSION_FILE);
        if (versionStr) {
            return versionStr.trim();
        }
    } catch (e) {
        /* Fall through */
    }
    return 'unknown';
}

/**
 * Build About submenu items
 */
function getAboutItems() {
    const items = [];

    /* Host version */
    items.push(createInfo('Host', `v${getHostVersion()}`));

    /* Get installed modules */
    const modules = host_list_modules();
    if (modules && modules.length > 0) {
        /* Sort by name */
        modules.sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));

        for (const mod of modules) {
            const name = mod.name || mod.id;
            const version = mod.version || '?';
            items.push(createInfo(name, `v${version}`));
        }
    }

    items.push(createBack());
    return items;
}

/* Settings menu state */
let settingsState = createMenuState();
let settingsStack = null;
let settingsItems = null;

/**
 * Get current settings values
 */
export function getSettings() {
    return {
        velocity_curve: host_get_setting('velocity_curve') || 'linear',
        aftertouch_enabled: host_get_setting('aftertouch_enabled') ?? 1,
        aftertouch_deadzone: host_get_setting('aftertouch_deadzone') ?? 0,
        pad_layout: host_get_setting('pad_layout') || 'chromatic',
        clock_mode: host_get_setting('clock_mode') || 'internal',
        tempo_bpm: host_get_setting('tempo_bpm') ?? 120
    };
}

/**
 * Build settings menu items
 */
function getSettingsItems() {
    return [
        createEnum('Velocity', {
            get: () => host_get_setting('velocity_curve') || 'linear',
            set: (v) => {
                host_set_setting('velocity_curve', v);
                host_save_settings();
            },
            options: VELOCITY_CURVES,
            format: capitalize
        }),
        createToggle('Aftertouch', {
            get: () => !!(host_get_setting('aftertouch_enabled') ?? 1),
            set: (v) => {
                host_set_setting('aftertouch_enabled', v ? 1 : 0);
                host_save_settings();
            }
        }),
        createValue('AT Deadzone', {
            get: () => host_get_setting('aftertouch_deadzone') ?? 0,
            set: (v) => {
                host_set_setting('aftertouch_deadzone', v);
                host_save_settings();
            },
            min: 0,
            max: 50,
            step: 5,
            fineStep: 1
        }),
        createEnum('Pad Layout', {
            get: () => host_get_setting('pad_layout') || 'chromatic',
            set: (v) => {
                host_set_setting('pad_layout', v);
                host_save_settings();
            },
            options: PAD_LAYOUTS,
            format: capitalize
        }),
        createEnum('MIDI Clock', {
            get: () => host_get_setting('clock_mode') || 'internal',
            set: (v) => {
                host_set_setting('clock_mode', v);
                host_save_settings();
            },
            options: CLOCK_MODES,
            format: formatClockMode
        }),
        createValue('Tempo BPM', {
            get: () => host_get_setting('tempo_bpm') ?? 120,
            set: (v) => {
                host_set_setting('tempo_bpm', v);
                host_save_settings();
            },
            min: 20,
            max: 300,
            step: 5,
            fineStep: 1
        }),
        createSubmenu('About', getAboutItems),
        createBack()
    ];
}

/**
 * Initialize settings menu
 */
export function initSettings() {
    settingsState = createMenuState();
    settingsItems = getSettingsItems();
    settingsStack = createMenuStack();
    settingsStack.push({
        title: 'Settings',
        items: settingsItems,
        selectedIndex: 0
    });
}

/**
 * Get current settings state (for external access)
 */
export function getSettingsState() {
    return settingsState;
}

/**
 * Check if currently editing a value
 */
export function isEditing() {
    return settingsState.editing;
}

/**
 * Draw the settings screen
 */
export function drawSettings() {
    if (!settingsStack || settingsStack.depth() === 0) {
        initSettings();
    }

    const footer = settingsState.editing
        ? 'Clk:Save Bck:Cancel'
        : 'Clk:Edit </>:Change';

    drawStackMenu({
        stack: settingsStack,
        state: settingsState,
        footer
    });
}

/**
 * Handle settings input
 * @returns {Object} Result with needsRedraw and shouldExit flags
 */
export function handleSettingsCC({ cc, value, shiftHeld }) {
    if (!settingsStack || settingsStack.depth() === 0) {
        initSettings();
    }

    const current = settingsStack.current();
    const items = current ? current.items : settingsItems;

    const result = handleMenuInput({
        cc,
        value,
        items,
        state: settingsState,
        stack: settingsStack,
        onBack: null, /* Let caller handle back from settings */
        shiftHeld
    });

    /* Check if user clicked [Back] item at root level */
    const item = items[settingsState.selectedIndex];
    let shouldExit = false;
    if (item && item.type === 'back' && cc === 3 && value > 0 && settingsStack.depth() <= 1) {
        shouldExit = true;
    }

    return {
        needsRedraw: result.needsRedraw,
        shouldExit
    };
}

/* Helpers */
function formatClockMode(mode) {
    if (mode === 'internal') return 'INT';
    if (mode === 'external') return 'EXT';
    return 'OFF';
}
