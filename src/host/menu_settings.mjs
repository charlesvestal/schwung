/*
 * Settings screen using shared menu components.
 */

import { createValue, createEnum, createToggle, createBack } from '../shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '../shared/menu_nav.mjs';
import { drawHierarchicalMenu } from '../shared/menu_render.mjs';

const VELOCITY_CURVES = ['linear', 'soft', 'hard', 'full'];
const PAD_LAYOUTS = ['chromatic', 'fourth'];
const CLOCK_MODES = ['off', 'internal', 'external'];

/* Settings menu state */
let settingsState = createMenuState();
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
        createBack()
    ];
}

/**
 * Initialize settings menu
 */
export function initSettings() {
    settingsState = createMenuState();
    settingsItems = getSettingsItems();
}

/**
 * Get current settings state (for external access)
 */
export function getSettingsState() {
    return settingsState;
}

/**
 * Draw the settings screen
 */
export function drawSettings() {
    if (!settingsItems) {
        settingsItems = getSettingsItems();
    }

    drawHierarchicalMenu({
        title: 'Settings',
        items: settingsItems,
        state: settingsState,
        footer: 'Click:edit  </>:adjust  Back:exit'
    });
}

/**
 * Handle settings input
 * @returns {Object} Result with needsRedraw and shouldExit flags
 */
export function handleSettingsCC({ cc, value, shiftHeld }) {
    if (!settingsItems) {
        settingsItems = getSettingsItems();
    }

    const result = handleMenuInput({
        cc,
        value,
        items: settingsItems,
        state: settingsState,
        stack: null,  /* Settings is flat, no stack needed */
        onBack: null, /* Let caller handle back from settings */
        shiftHeld
    });

    /* Check if user clicked [Back] item */
    const item = settingsItems[settingsState.selectedIndex];
    let shouldExit = false;
    if (item && item.type === 'back' && cc === 3 && value > 0) {
        shouldExit = true;
    }

    return {
        needsRedraw: result.needsRedraw,
        shouldExit
    };
}

/* Helpers */
function capitalize(s) {
    if (!s) return '';
    return s.charAt(0).toUpperCase() + s.slice(1);
}

function formatClockMode(mode) {
    if (mode === 'internal') return 'INT';
    if (mode === 'external') return 'EXT';
    return 'OFF';
}
