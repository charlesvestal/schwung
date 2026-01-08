/*
 * Host Menu UI - Module selection and management
 *
 * This is the main host script that provides a menu for selecting
 * and loading DSP modules, plus host settings.
 */

import {
    MoveMainKnob, MoveMainButton, MoveMainTouch,
    MoveShift, MoveMenu,
    MoveLeft, MoveRight, MoveUp, MoveDown
} from '../shared/constants.mjs';

import { isCapacitiveTouchMessage } from '../shared/input_filter.mjs';

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
const SETTINGS_COUNT = 5;

/* Alias constants for clarity */
const CC_JOG_WHEEL = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_SHIFT = MoveShift;
const CC_MENU = MoveMenu;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;
const NOTE_JOG_TOUCH = MoveMainTouch;

let shiftHeld = false;

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const LINE_HEIGHT = 10;
const MAX_VISIBLE_ITEMS = 5;

/* Velocity curve options */
const VELOCITY_CURVES = ['linear', 'soft', 'hard', 'full'];

/* Clock mode options */
const CLOCK_MODES = ['off', 'internal', 'external'];

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

/* Get current settings from host */
function getSettings() {
    return {
        velocity_curve: host_get_setting('velocity_curve') || 'linear',
        aftertouch_enabled: host_get_setting('aftertouch_enabled') ?? 1,
        aftertouch_deadzone: host_get_setting('aftertouch_deadzone') ?? 0,
        clock_mode: host_get_setting('clock_mode') || 'internal',
        tempo_bpm: host_get_setting('tempo_bpm') ?? 120
    };
}

/* Capitalize first letter */
function capitalize(s) {
    return s.charAt(0).toUpperCase() + s.slice(1);
}

/* Draw the settings screen */
function drawSettings() {
    clear_screen();

    /* Title */
    print(2, 2, "Settings", 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    const settings = getSettings();

    /* Draw settings */
    const items = [
        { label: "Velocity", value: capitalize(settings.velocity_curve) },
        { label: "Aftertouch", value: settings.aftertouch_enabled ? "On" : "Off" },
        { label: "AT Deadzone", value: String(settings.aftertouch_deadzone) },
        { label: "MIDI Clock", value: capitalize(settings.clock_mode) },
        { label: "Tempo BPM", value: String(settings.tempo_bpm) }
    ];

    /* Use smaller line height for settings to fit all items */
    const settingsLineHeight = 9;

    for (let i = 0; i < items.length; i++) {
        const y = 15 + i * settingsLineHeight;
        const isSelected = (i === settingsIndex);

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, settingsLineHeight, 1);
            print(4, y, `>${items[i].label}:`, 0);
            print(75, y, items[i].value, 0);
        } else {
            print(4, y, ` ${items[i].label}:`, 1);
            print(75, y, items[i].value, 1);
        }
    }

    /* Instructions at bottom */
    print(2, 56, "</>:change jog:save", 1);
}

/* Draw the menu screen */
function drawMenu() {
    clear_screen();

    /* Title and volume */
    print(2, 2, "Move Anything", 1);
    const vol = host_get_volume();
    print(90, 2, `Vol:${vol}`, 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    /* Total items = modules + Settings */
    const totalItems = modules.length + 1;

    if (totalItems === 1 && modules.length === 0) {
        print(2, 24, "No modules found", 1);
        print(2, 36, "Check modules/ dir", 1);
        print(2, 48, "> Settings", 1);
        return;
    }

    /* Calculate visible range */
    let startIdx = 0;
    if (selectedIndex >= MAX_VISIBLE_ITEMS) {
        startIdx = selectedIndex - MAX_VISIBLE_ITEMS + 1;
    }
    let endIdx = Math.min(startIdx + MAX_VISIBLE_ITEMS, totalItems);

    /* Draw item list (modules + Settings) */
    for (let i = startIdx; i < endIdx; i++) {
        const y = 16 + (i - startIdx) * LINE_HEIGHT;
        const isSelected = (i === selectedIndex);
        let itemName;

        if (i < modules.length) {
            itemName = modules[i].name;
        } else {
            itemName = "Settings";
        }

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LINE_HEIGHT, 1);
            print(4, y, `> ${itemName}`, 0);
        } else {
            print(4, y, `  ${itemName}`, 1);
        }
    }

    /* Scroll indicators */
    if (startIdx > 0) {
        print(120, 16, "^", 1);
    }
    if (endIdx < totalItems) {
        print(120, SCREEN_HEIGHT - 10, "v", 1);
    }

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

/* Change a setting value */
function changeSettingValue(delta) {
    const settings = getSettings();

    if (settingsIndex === 0) {
        /* Velocity curve */
        let idx = VELOCITY_CURVES.indexOf(settings.velocity_curve);
        idx = (idx + delta + VELOCITY_CURVES.length) % VELOCITY_CURVES.length;
        host_set_setting('velocity_curve', VELOCITY_CURVES[idx]);
    } else if (settingsIndex === 1) {
        /* Aftertouch enabled */
        host_set_setting('aftertouch_enabled', settings.aftertouch_enabled ? 0 : 1);
    } else if (settingsIndex === 2) {
        /* Aftertouch deadzone */
        let dz = settings.aftertouch_deadzone;
        const step = shiftHeld ? 1 : 5;
        dz = dz + (delta * step);
        if (dz < 0) dz = 0;
        if (dz > 50) dz = 50;
        host_set_setting('aftertouch_deadzone', dz);
    } else if (settingsIndex === 3) {
        /* Clock mode */
        let idx = CLOCK_MODES.indexOf(settings.clock_mode);
        idx = (idx + delta + CLOCK_MODES.length) % CLOCK_MODES.length;
        host_set_setting('clock_mode', CLOCK_MODES[idx]);
    } else if (settingsIndex === 4) {
        /* Tempo BPM */
        let bpm = settings.tempo_bpm;
        const step = shiftHeld ? 1 : 5;
        bpm = bpm + (delta * step);
        if (bpm < 20) bpm = 20;
        if (bpm > 300) bpm = 300;
        host_set_setting('tempo_bpm', bpm);
    }
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

    /* Settings screen navigation */
    if (settingsVisible) {
        /* Jog wheel navigation */
        if (cc === CC_JOG_WHEEL) {
            if (value === 1) {
                settingsIndex = Math.min(settingsIndex + 1, SETTINGS_COUNT - 1);
            } else if (value === 127 || value === 65) {
                settingsIndex = Math.max(settingsIndex - 1, 0);
            }
            return;
        }

        /* Arrow up/down navigation */
        if (cc === CC_DOWN && value > 0) {
            settingsIndex = Math.min(settingsIndex + 1, SETTINGS_COUNT - 1);
            return;
        }
        if (cc === CC_UP && value > 0) {
            settingsIndex = Math.max(settingsIndex - 1, 0);
            return;
        }

        /* Left/right change value */
        if (cc === CC_LEFT && value > 0) {
            changeSettingValue(-1);
            return;
        }
        if (cc === CC_RIGHT && value > 0) {
            changeSettingValue(1);
            return;
        }

        /* Jog wheel click saves and exits */
        if (cc === CC_JOG_CLICK && value > 0) {
            host_save_settings();
            settingsVisible = false;
            showStatus("Settings saved");
            return;
        }

        return;
    }

    /* Menu navigation (only when menu visible) */
    if (!menuVisible) return;

    const totalItems = modules.length + 1;  /* +1 for Settings */

    /* Jog wheel navigation */
    if (cc === CC_JOG_WHEEL) {
        if (value === 1) {
            /* Clockwise */
            selectedIndex = Math.min(selectedIndex + 1, totalItems - 1);
        } else if (value === 127 || value === 65) {
            /* Counter-clockwise */
            selectedIndex = Math.max(selectedIndex - 1, 0);
        }
        return;
    }

    /* Arrow navigation */
    if (cc === CC_DOWN && value > 0) {
        selectedIndex = Math.min(selectedIndex + 1, totalItems - 1);
        return;
    }
    if (cc === CC_UP && value > 0) {
        selectedIndex = Math.max(selectedIndex - 1, 0);
        return;
    }

    /* Jog wheel click (CC 3) selects item */
    if (cc === CC_JOG_CLICK && value > 0) {
        if (selectedIndex === modules.length) {
            /* Settings selected */
            settingsVisible = true;
            settingsIndex = 0;
        } else {
            loadSelectedModule();
        }
        return;
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
