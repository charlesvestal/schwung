/*
 * Set View
 * Select which set (song) to work on
 * 32 sets displayed on pads, current set highlighted
 */

import {
    Black, White, Cyan, BrightGreen, BrightRed, VividYellow,
    MoveSteps, MovePads, MoveTracks, MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack, MoveCopy,
    MoveStep1UI, MoveStep2UI, MoveStep3UI, MoveStep4UI, MoveStep5UI, MoveStep6UI, MoveStep7UI, MoveStep8UI,
    MoveStep9UI, MoveStep10UI, MoveStep11UI, MoveStep12UI, MoveStep13UI, MoveStep14UI, MoveStep15UI, MoveStep16UI,
    MoveDelete, MoveMainButton,
    OrangeRed, Bright, LightYellow, Ochre, BurntOrange, Mustard,
    ForestGreen, NeonGreen, TealGreen, Lime,
    AzureBlue, RoyalBlue, Navy, SkyBlue, MutedBlue,
    BlueViolet, Violet, Purple, ElectricViolet, HotMagenta, NeonPink, Rose, BrightPink,
    LightGrey, DarkGrey
} from "../lib/shared-constants.js";

import { setLED, setButtonLED } from "../lib/shared-input.js";

import { NUM_SETS, NUM_STEPS, TRACK_COLORS } from '../lib/constants.js';
import { state, displayMessage, displayTemporaryMessage, enterTrackView } from '../lib/state.js';
import * as trackView from './track.js';
import { setParam, syncAllTracksToDSP } from '../lib/helpers.js';
import { markDirty, flushDirty, loadSetToTracks, setHasContent, deleteSetFile, loadSetFromDisk, saveSetToDisk } from '../lib/persistence.js';

/* ============ Color Palette ============ */

/* 32 colors for the color picker - arranged visually on the 4x8 pad grid */
const COLOR_PALETTE = [
    /* Row 1 (bottom): Reds/Oranges/Yellows */
    BrightRed, OrangeRed, Bright, VividYellow, LightYellow, Ochre, BurntOrange, Mustard,
    /* Row 2: Greens */
    BrightGreen, Lime, ForestGreen, NeonGreen, TealGreen, Cyan, AzureBlue, SkyBlue,
    /* Row 3: Blues/Purples */
    RoyalBlue, Navy, BlueViolet, Violet, Purple, ElectricViolet, HotMagenta, NeonPink,
    /* Row 4 (top): Pinks/Neutrals */
    Rose, BrightPink, MutedBlue, LightGrey, DarkGrey, White, Black, Cyan
];

/* ============ Local State ============ */

let deleteHeld = false;
let deleteConfirmSetIdx = -1;  // Set pending deletion confirmation (-1 = none)
let copyHeld = false;
let copiedSetData = null;  // Copied set data
let copiedSetIdx = -1;  // Source set index for display (-1 = none)

/* Color picker state */
let colorPickerActive = false;
let colorPickerSetIdx = -1;  // Which set is being colored (-1 = none)
let colorPickerCallback = null;  // Optional callback for reusable picker

/* ============ View Interface ============ */

/**
 * Called when entering set view
 */
export function onEnter() {
    deleteHeld = false;
    deleteConfirmSetIdx = -1;
    copyHeld = false;
    copiedSetData = null;
    copiedSetIdx = -1;
    colorPickerActive = false;
    colorPickerSetIdx = -1;
    colorPickerCallback = null;
    updateDisplayContent();
}

/**
 * Called when exiting set view
 */
export function onExit() {
    deleteHeld = false;
    deleteConfirmSetIdx = -1;
    copyHeld = false;
    copiedSetData = null;
    copiedSetIdx = -1;
    colorPickerActive = false;
    colorPickerSetIdx = -1;
    colorPickerCallback = null;
}

/**
 * Handle MIDI input for set view
 * Returns true if handled, false to let router handle
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Delete button (CC 119) - track held state */
    if (isCC && note === MoveDelete) {
        deleteHeld = velocity > 0;
        if (!deleteHeld && deleteConfirmSetIdx < 0) {
            /* Released without selecting a set - just update display */
            updateDisplayContent();
        }
        updateLEDs();  // Update Delete button LED
        return true;
    }

    /* Copy button (CC 60) - track held state */
    if (isCC && note === MoveCopy) {
        copyHeld = velocity > 0;
        if (!copyHeld && copiedSetIdx < 0) {
            /* Released without copying - just update display */
            updateDisplayContent();
        }
        return true;
    }

    /* Main button (jog click) - confirm deletion */
    if (isCC && note === MoveMainButton && velocity > 0) {
        if (deleteConfirmSetIdx >= 0) {
            /* Prevent deleting the currently loaded set */
            if (deleteConfirmSetIdx === state.currentSet) {
                const setIdx = deleteConfirmSetIdx;
                deleteConfirmSetIdx = -1;
                deleteHeld = false;
                updateDisplayContent();  // Set normal display
                displayTemporaryMessage(
                    "CANNOT DELETE",
                    `Set ${setIdx + 1} is loaded`,
                    "Switch to another set first",
                    "",
                    1500,
                    updateLEDs
                );
                updateLEDs();  // Show message LEDs
                return true;
            }

            /* Confirm deletion */
            const setIdx = deleteConfirmSetIdx;
            deleteSetFile(setIdx);

            /* Clear in-memory cache if it exists */
            if (state.sets[setIdx]) {
                state.sets[setIdx] = null;
            }

            /* Clear confirmation state */
            deleteConfirmSetIdx = -1;
            deleteHeld = false;

            updateDisplayContent();  // Set normal display
            displayTemporaryMessage(
                "SET DELETED",
                `Set ${setIdx + 1} deleted`,
                "",
                "",
                1000,
                updateLEDs
            );

            updateLEDs();  // Show message LEDs
            return true;
        }
    }

    /* Back button - cancel deletion or exit color picker */
    if (isCC && note === MoveBack && velocity > 0) {
        if (colorPickerActive) {
            /* Cancel color picker */
            colorPickerActive = false;
            colorPickerSetIdx = -1;
            updateDisplayContent();
            updateLEDs();
            return true;
        }
        if (deleteConfirmSetIdx >= 0) {
            /* Cancel deletion */
            deleteConfirmSetIdx = -1;
            deleteHeld = false;
            updateDisplayContent();
            updateLEDs();
            return true;
        }
        /* Otherwise let router handle (return to track view) */
    }

    /* Pads - select a set or request deletion */
    if (isNote && note >= 68 && note <= 99) {
        const padIdx = note - 68;

        if (isNoteOn && velocity > 0) {
            /* Color picker mode - select a color */
            if (colorPickerActive) {
                /* Get color from palette (pads are in display order) */
                const row = 3 - Math.floor(padIdx / 8);  // Row 0 is bottom
                const col = padIdx % 8;
                const paletteIdx = row * 8 + col;
                const selectedColor = COLOR_PALETTE[paletteIdx];

                /* Apply color to the set */
                applyColorToSet(colorPickerSetIdx, selectedColor);

                /* Exit color picker */
                colorPickerActive = false;
                const setIdx = colorPickerSetIdx;
                colorPickerSetIdx = -1;

                displayMessage(
                    "COLOR SET",
                    `Set ${setIdx + 1} color updated`,
                    "",
                    ""
                );
                updateLEDs();
                return true;
            }

            /* Convert pad index to set index (bottom-left = set 0) */
            const row = 3 - Math.floor(padIdx / 8);  // Row 0 is bottom
            const col = padIdx % 8;
            const setIdx = row * 8 + col;

            /* Shift held - enter color picker mode */
            if (state.shiftHeld) {
                colorPickerActive = true;
                colorPickerSetIdx = setIdx;
                displayMessage(
                    "SELECT COLOR",
                    `for Set ${setIdx + 1}`,
                    "Press a pad to choose",
                    ""
                );
                updateLEDs();
                return true;
            }

            /* Copy held - copy or paste set */
            if (copyHeld) {
                if (copiedSetData === null) {
                    /* First press: copy this set */
                    const setData = loadSetFromDisk(setIdx);
                    if (setData || state.sets[setIdx]) {
                        copiedSetData = setData || state.sets[setIdx];
                        copiedSetIdx = setIdx;
                        displayMessage(
                            "SET COPIED",
                            `Set ${setIdx + 1} - Hold Copy + press destination`,
                            "",
                            ""
                        );
                        updateLEDs();
                    } else {
                        displayMessage(
                            "CANNOT COPY",
                            `Set ${setIdx + 1} is empty`,
                            "",
                            ""
                        );
                    }
                } else {
                    /* Second press: paste to this set */
                    saveSetToDisk(setIdx, copiedSetData);
                    /* Update in-memory cache */
                    state.sets[setIdx] = copiedSetData;

                    const sourceIdx = copiedSetIdx;
                    /* Clear copy state after paste */
                    copiedSetData = null;
                    copiedSetIdx = -1;

                    /* Update LEDs immediately to show color on destination pad */
                    updateLEDs();

                    updateDisplayContent();  // Set normal display
                    displayTemporaryMessage(
                        "SET PASTED",
                        `Set ${sourceIdx + 1} → Set ${setIdx + 1}`,
                        "",
                        "",
                        1000,
                        updateLEDs
                    );
                }
                return true;
            }

            /* Delete held - enter confirmation mode */
            if (deleteHeld) {
                deleteConfirmSetIdx = setIdx;
                displayMessage(
                    "DELETE SET?",
                    `Set ${setIdx + 1}`,
                    "Jog click: confirm",
                    "Back: cancel"
                );
                updateLEDs();
                return true;
            }

            /* Normal mode - load the set */
            /* Save current set if we have one loaded */
            if (state.currentSet >= 0) {
                flushDirty();
            }

            /* Load the selected set */
            loadSetToTracks(setIdx);

            /* Sync all track data to DSP */
            syncAllTracksToDSP();

            /* Transition to track view */
            onExit();
            enterTrackView();
            trackView.onEnter();
            trackView.updateLEDs();

            displayMessage(
                `SEQOMD - Set ${setIdx + 1}`,
                `Track ${state.currentTrack + 1}`,
                "",
                ""
            );

            return true;
        }
        return true;
    }

    /* Track buttons - inactive in set view, consume but ignore */
    if (isCC && MoveTracks.includes(note)) {
        return true;
    }

    return false;  // Let router handle
}

/**
 * Update all LEDs for set view
 */
export function updateLEDs() {
    updateStepLEDs();
    updateStepUILEDs();
    updatePadLEDs();
    updateKnobLEDs();
    updateTrackButtonLEDs();
    updateTransportLEDs();
    updateCaptureLED();
    updateCopyLED();
    updateDeleteLED();
    updateBackLED();
}

function updateStepUILEDs() {
    /* Clear all step UI mode icons - not used in set view */
    const stepUIButtons = [
        MoveStep1UI, MoveStep2UI, MoveStep3UI, MoveStep4UI,
        MoveStep5UI, MoveStep6UI, MoveStep7UI, MoveStep8UI,
        MoveStep9UI, MoveStep10UI, MoveStep11UI, MoveStep12UI,
        MoveStep13UI, MoveStep14UI, MoveStep15UI, MoveStep16UI
    ];
    for (const button of stepUIButtons) {
        setButtonLED(button, Black);
    }
}

/**
 * Update display content for set view
 */
export function updateDisplayContent() {
    displayMessage(
        "SEQOMD - SELECT SET",
        state.currentSet >= 0 ? `Current: Set ${state.currentSet + 1}` : "",
        "",
        ""
    );
}

/* ============ Color Helpers ============ */

/**
 * Apply a color to a set
 * Creates/updates set data if needed to store the color
 */
function applyColorToSet(setIdx, color) {
    /* Load existing set data or create minimal structure for color */
    let setData = loadSetFromDisk(setIdx);
    if (!setData) {
        setData = state.sets[setIdx];
    }
    if (!setData) {
        /* Create minimal set data just to store the color */
        setData = { color: color };
    } else {
        setData.color = color;
    }

    /* Save to disk and update in-memory cache */
    saveSetToDisk(setIdx, setData);
    state.sets[setIdx] = setData;
}

/**
 * Get the color for a set (custom color or default)
 * Only uses cached data - does NOT read from disk to avoid slow LED updates
 */
function getSetColor(setIdx) {
    /* Check in-memory cache only - no disk reads here */
    if (state.sets[setIdx] && state.sets[setIdx].color !== undefined) {
        return state.sets[setIdx].color;
    }
    /* Default: cycle through track colors */
    return TRACK_COLORS[setIdx % 8];
}

/* ============ LED Updates ============ */

function updateStepLEDs() {
    /* Steps off in set view - focus is on pads */
    for (let i = 0; i < NUM_STEPS; i++) {
        setLED(MoveSteps[i], Black);
    }
}

function updatePadLEDs() {
    /* Color picker mode - show palette */
    if (colorPickerActive) {
        for (let i = 0; i < 32; i++) {
            const padNote = MovePads[i];
            /* Convert pad index to palette index (same layout as sets) */
            const row = 3 - Math.floor(i / 8);  // Row 0 is bottom
            const col = i % 8;
            const paletteIdx = row * 8 + col;
            setLED(padNote, COLOR_PALETTE[paletteIdx]);
        }
        return;
    }

    /* Set view: 32 pads = 32 sets
     * Layout: 4 rows x 8 columns
     * Row 4 (top, indices 0-7): Sets 25-32
     * Row 3 (indices 8-15): Sets 17-24
     * Row 2 (indices 16-23): Sets 9-16
     * Row 1 (bottom, indices 24-31): Sets 1-8
     */
    for (let i = 0; i < 32; i++) {
        const padNote = MovePads[i];
        /* Convert pad index to set index (bottom-left = set 0) */
        const row = 3 - Math.floor(i / 8);  // Row 0 is bottom
        const col = i % 8;
        const setIdx = row * 8 + col;

        const isCurrentSet = state.currentSet === setIdx;
        const hasContent = setHasContent(setIdx);
        const isPendingDelete = deleteConfirmSetIdx === setIdx;

        let color = Black;
        if (isPendingDelete) {
            color = BrightRed;  // Set pending deletion confirmation
        } else if (isCurrentSet) {
            color = Cyan;  // Currently loaded set
        } else if (hasContent) {
            /* Use custom set color if available, otherwise cycle through track colors */
            color = getSetColor(setIdx);
        }
        /* Empty sets stay Black */

        setLED(padNote, color);
    }
}

function updateKnobLEDs() {
    /* Knobs off in set view */
    // Knob LEDs handled by router
}

function updateTrackButtonLEDs() {
    /* Track buttons off in set view - not active */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }
}

function updateTransportLEDs() {
    /* Play button */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);

    /* Loop button - off in set view */
    setButtonLED(MoveLoop, Black);

    /* Record button */
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
}

function updateCaptureLED() {
    /* Capture off in set view */
    setButtonLED(MoveCapture, Black);
}

function updateCopyLED() {
    /* Copy button lit to indicate copy functionality available */
    setButtonLED(MoveCopy, White);
}

function updateDeleteLED() {
    /* Delete button lit when held or confirming deletion */
    const isActive = deleteHeld || deleteConfirmSetIdx >= 0;
    setButtonLED(MoveDelete, isActive ? BrightRed : Black);
}

function updateBackLED() {
    /* Back button lit - press to return to track view */
    setButtonLED(MoveBack, White);
}
