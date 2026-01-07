/*
 * Host Menu UI - Module selection and management
 *
 * This is the main host script that provides a menu for selecting
 * and loading DSP modules.
 */

/* State */
let modules = [];
let selectedIndex = 0;
let currentModuleUI = null;
let menuVisible = true;
let statusMessage = '';
let statusTimeout = 0;

/* Move hardware constants */
const CC_JOG_WHEEL = 14;   /* Jog wheel rotation (1=CW, 127/65=CCW) */
const CC_JOG_CLICK = 3;    /* Jog wheel press/click */
const CC_SHIFT = 49;
const CC_MENU = 50;
const CC_LEFT = 62;
const CC_RIGHT = 63;
const CC_UP = 55;
const CC_DOWN = 54;
const NOTE_JOG_TOUCH = 9;  /* Jog wheel capacitive touch (not click) */

let shiftHeld = false;

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const LINE_HEIGHT = 10;
const MAX_VISIBLE_ITEMS = 5;

/* Refresh module list from host */
function refreshModules() {
    modules = host_list_modules();
    if (selectedIndex >= modules.length) {
        selectedIndex = Math.max(0, modules.length - 1);
    }
    console.log(`Found ${modules.length} modules`);
}

/* Show a temporary status message */
function showStatus(msg, duration = 2000) {
    statusMessage = msg;
    statusTimeout = Date.now() + duration;
}

/* Draw the menu screen */
function drawMenu() {
    clear_screen();

    /* Title */
    print(2, 2, "Move Anything", 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    if (modules.length === 0) {
        print(2, 24, "No modules found", 1);
        print(2, 36, "Check modules/ dir", 1);
        return;
    }

    /* Calculate visible range */
    let startIdx = 0;
    if (selectedIndex >= MAX_VISIBLE_ITEMS) {
        startIdx = selectedIndex - MAX_VISIBLE_ITEMS + 1;
    }
    let endIdx = Math.min(startIdx + MAX_VISIBLE_ITEMS, modules.length);

    /* Draw module list */
    for (let i = startIdx; i < endIdx; i++) {
        const y = 16 + (i - startIdx) * LINE_HEIGHT;
        const mod = modules[i];
        const isSelected = (i === selectedIndex);

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, LINE_HEIGHT, 1);
            print(4, y, `> ${mod.name}`, 0);
        } else {
            print(4, y, `  ${mod.name}`, 1);
        }
    }

    /* Scroll indicators */
    if (startIdx > 0) {
        print(120, 16, "^", 1);
    }
    if (endIdx < modules.length) {
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

    /* Shift+Jog click exits Move Anything */
    if (cc === CC_JOG_CLICK && shiftHeld) {
        console.log("Shift+Wheel - exit");
        exit();
        return;
    }

    /* Menu button returns to menu if shift held */
    if (cc === CC_MENU && value > 0 && shiftHeld) {
        if (!menuVisible) {
            returnToMenu();
        }
        return;
    }

    /* Menu navigation (only when menu visible) */
    if (!menuVisible) return;

    /* Jog wheel navigation */
    if (cc === CC_JOG_WHEEL) {
        if (value === 1) {
            /* Clockwise */
            selectedIndex = Math.min(selectedIndex + 1, modules.length - 1);
        } else if (value === 127 || value === 65) {
            /* Counter-clockwise */
            selectedIndex = Math.max(selectedIndex - 1, 0);
        }
        return;
    }

    /* Arrow navigation */
    if (cc === CC_DOWN && value > 0) {
        selectedIndex = Math.min(selectedIndex + 1, modules.length - 1);
        return;
    }
    if (cc === CC_UP && value > 0) {
        selectedIndex = Math.max(selectedIndex - 1, 0);
        return;
    }

    /* Jog wheel click (CC 3) selects module */
    if (cc === CC_JOG_CLICK && value > 0) {
        loadSelectedModule();
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
    if (menuVisible) {
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
    const status = data[0] & 0xF0;
    const channel = data[0] & 0x0F;
    const isNote = status === 0x90 || status === 0x80;

    /* Filter capacitive touch events from knobs (notes 0-9) */
    if (isNote && data[1] < 10) {
        return; /* Ignore capacitive touch */
    }

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
