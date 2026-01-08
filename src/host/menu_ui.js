/*
 * Host Menu UI - Module selection and management
 *
 * This is the main host script that provides a menu for selecting
 * and loading DSP modules, plus host settings.
 */

import {
    MoveMainKnob, MoveMainButton, MoveMainTouch,
    MoveShift, MoveMenu, MoveBack,
    MoveLeft, MoveRight, MoveUp, MoveDown
} from '../shared/constants.mjs';

import { isCapacitiveTouchMessage } from '../shared/input_filter.mjs';
import { drawMainMenu, handleMainMenuCC } from './menu_main.mjs';
import { drawSettings as renderSettings, handleSettingsCC } from './menu_settings.mjs';

/* State */
let modules = [];
let selectedIndex = 0;
let currentModuleUI = null;
let menuVisible = true;
let statusMessage = '';
let statusTimeout = 0;

/* Settings state */
let settingsVisible = false;
let settingsIndex = 0;  /* 0=velocity, 1=aftertouch on/off, 2=deadzone, 3=clock, 4=tempo */

/* Alias constants for clarity */
const CC_JOG_WHEEL = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_SHIFT = MoveShift;
const CC_MENU = MoveMenu;
const CC_BACK = MoveBack;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;
const NOTE_JOG_TOUCH = MoveMainTouch;

let shiftHeld = false;

/* Display constants */
const SCREEN_HEIGHT = 64;

/* Refresh module list from host */
function refreshModules() {
    modules = host_list_modules();
    /* Add 1 for Settings entry */
    if (selectedIndex >= modules.length + 1) {
        selectedIndex = Math.max(0, modules.length);
    }
    console.log(`Found ${modules.length} modules`);
}

/* Show a temporary status message */
function showStatus(msg, duration = 2000) {
    statusMessage = msg;
    statusTimeout = Date.now() + duration;
}

/* Draw the settings screen */
function drawSettings() {
    clear_screen();
    renderSettings({ settingsIndex });
}

/* Draw the menu screen */
function drawMenu() {
    clear_screen();
    drawMainMenu({
        modules,
        selectedIndex,
        volume: host_get_volume()
    });

    /* Status bar */
    if (statusMessage && Date.now() < statusTimeout) {
        fill_rect(0, SCREEN_HEIGHT - 9, SCREEN_WIDTH, 9, 1);
        print(2, SCREEN_HEIGHT - 8, statusMessage, 0);
    }
}

/* Draw the loaded module info (when module is active) */
function drawModuleInfo() {
    const mod = host_get_current_module();
    if (!mod) {
        menuVisible = true;
        return;
    }

    clear_screen();
    print(2, 2, mod.name, 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    print(2, 20, "Module active", 1);
    print(2, 32, "Shift+Menu: back", 1);
}

/* Load the selected module */
function loadSelectedModule() {
    if (selectedIndex < 0 || selectedIndex >= modules.length) {
        showStatus("No module selected");
        return;
    }

    const mod = modules[selectedIndex];
    console.log(`Loading module: ${mod.id}`);
    showStatus(`Loading ${mod.name}...`);

    const success = host_load_module(mod.id);
    if (success) {
        showStatus(`Loaded: ${mod.name}`);
        menuVisible = false;
        console.log(`Module ${mod.id} loaded successfully`);
    } else {
        showStatus(`Failed to load!`);
        console.log(`Failed to load module ${mod.id}`);
    }
}

/* Unload current module and return to menu */
function returnToMenu() {
    host_unload_module();
    menuVisible = true;
    settingsVisible = false;
    refreshModules();
    showStatus("Module unloaded");
}

/* Handle MIDI CC */
function handleCC(cc, value) {
    /* Track shift state */
    if (cc === CC_SHIFT) {
        shiftHeld = (value > 0);
        return;
    }

    /* Note: Shift+Wheel exit is handled at host level (C code) */

    /* Menu button returns to menu if shift held */
    if (cc === CC_MENU && value > 0 && shiftHeld) {
        if (!menuVisible || settingsVisible) {
            if (settingsVisible) {
                settingsVisible = false;
            } else {
                returnToMenu();
            }
        }
        return;
    }

    /* Back button goes back from settings */
    if (settingsVisible && cc === CC_BACK && value > 0) {
        settingsVisible = false;
        return;
    }

    /* Settings screen navigation */
    if (settingsVisible) {
        const result = handleSettingsCC({
            cc,
            value,
            settingsIndex,
            shiftHeld
        });

        settingsIndex = result.nextIndex;
        return;
    }

    /* Menu navigation (only when menu visible) */
    if (!menuVisible) return;

    const totalItems = modules.length + 1;  /* +1 for Settings */
    const result = handleMainMenuCC({
        cc,
        value,
        selectedIndex,
        totalItems
    });
    selectedIndex = result.nextIndex;
    if (result.didSelect) {
        if (selectedIndex === modules.length) {
            settingsVisible = true;
            settingsIndex = 0;
        } else {
            loadSelectedModule();
        }
    }
}

/* Handle MIDI note */
function handleNote(note, velocity) {
    if (!menuVisible) return;
    /* Note 9 is jog wheel capacitive touch, not click - don't use for selection */
}

/* === Required JS callbacks === */

globalThis.init = function() {
    console.log("Menu UI initializing...");
    refreshModules();

    if (modules.length > 0) {
        console.log("Available modules:");
        for (const mod of modules) {
            console.log(`  - ${mod.name} (${mod.id}) v${mod.version}`);
        }
    }

    clear_screen();
    print(2, 24, "Move Anything", 1);
    print(2, 36, "Host Ready", 1);
};

globalThis.tick = function() {
    if (settingsVisible) {
        drawSettings();
    } else if (menuVisible) {
        drawMenu();
    } else {
        /* If a module is loaded, its UI should handle drawing.
         * We just show a minimal indicator here. */
        if (!host_is_module_loaded()) {
            menuVisible = true;
        }
    }
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;

    const status = data[0] & 0xF0;
    const isNote = status === 0x90 || status === 0x80;

    if (status === 0xB0) {
        /* Control Change */
        handleCC(data[1], data[2]);
    } else if (status === 0x90) {
        /* Note On */
        handleNote(data[1], data[2]);
    } else if (status === 0x80) {
        /* Note Off */
        handleNote(data[1], 0);
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI is passed through to DSP plugin by host.
     * UI can optionally react to it here. */
};
