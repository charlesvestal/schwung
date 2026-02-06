/*
 * Shared Sound Generator UI Base
 *
 * Provides a common base for sound generator module UIs with:
 * - Standard display layout using unified parameters
 * - Standard navigation (L/R presets, Up/Down octave, Jog browse)
 * - Hooks for module-specific customization
 *
 * Usage:
 *   import { createSoundGeneratorUI } from '../../shared/sound_generator_ui.mjs';
 *
 *   const ui = createSoundGeneratorUI({
 *       moduleName: 'Dexed',
 *       // Optional hooks for customization
 *       onInit: () => { ... },
 *       onTick: () => { ... },
 *       drawCustom: (y) => { return y; },  // Draw custom content, return next y
 *       onPresetChange: (preset) => { ... },
 *       onOctaveChange: (octave) => { ... },
 *       handleCustomCC: (cc, value) => false,  // Return true if handled
 *   });
 *
 *   // Export required callbacks
 *   globalThis.init = ui.init;
 *   globalThis.tick = ui.tick;
 *   globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
 *   globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
 */

import {
    MoveMainKnob,
    MoveLeft, MoveRight, MoveUp, MoveDown,
    MoveShift,
    MovePads
} from './constants.mjs';

import { isCapacitiveTouchMessage, decodeDelta } from './input_filter.mjs';

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* Default state */
const defaultState = {
    currentPreset: 0,
    presetCount: 1,
    presetName: 'Init',
    bankName: '',
    bankCount: 1,
    bankIndex: 0,
    patchInBank: 1,
    octaveTranspose: 0,
    polyphony: 0,
    shiftHeld: false,
    needsRedraw: true,
    tickCount: 0,
};

const REDRAW_INTERVAL = 6;  /* Redraw every 6 ticks (~10Hz) */

/* CC mappings */
const CC_JOG_WHEEL = MoveMainKnob;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_PLUS = MoveUp;
const CC_MINUS = MoveDown;
const CC_SHIFT = MoveShift;

/* Note range for Move pads */
const PAD_NOTE_MIN = MovePads[0];
const PAD_NOTE_MAX = MovePads[MovePads.length - 1];

/**
 * Create a sound generator UI with standard behavior and customization hooks
 * @param {Object} config - Configuration object
 * @param {string} config.moduleName - Display name for the module
 * @param {Function} [config.onInit] - Called during init
 * @param {Function} [config.onTick] - Called each tick before redraw
 * @param {Function} [config.drawCustom] - Draw custom content, receives y position, returns next y
 * @param {Function} [config.onPresetChange] - Called when preset changes
 * @param {Function} [config.onBankChange] - Called when bank changes
 * @param {Function} [config.onOctaveChange] - Called when octave changes
 * @param {Function} [config.handleCustomCC] - Handle custom CCs, return true if handled
 * @param {Function} [config.handleCustomNote] - Handle custom notes, return true if handled
 * @param {boolean} [config.showPolyphony] - Show polyphony count (default: true)
 * @param {boolean} [config.showOctave] - Show octave transpose (default: true)
 * @param {string} [config.bankParamName] - DSP param name for bank index (default: 'bank_index')
 * @returns {Object} UI callbacks object
 */
export function createSoundGeneratorUI(config) {
    const {
        moduleName = 'Synth',
        onInit = null,
        onTick = null,
        drawCustom = null,
        onPresetChange = null,
        onBankChange = null,
        onOctaveChange = null,
        handleCustomCC = null,
        handleCustomNote = null,
        showPolyphony = true,
        showOctave = true,
        bankParamName = 'bank_index',
    } = config;

    /* State */
    const state = { ...defaultState };

    /* Update state from DSP parameters */
    function syncFromDSP() {
        /* Unified bank/preset parameters */
        const bankName = host_module_get_param('bank_name');
        if (bankName && bankName !== state.bankName) {
            state.bankName = bankName;
            state.needsRedraw = true;
        }

        const patchInBank = host_module_get_param('patch_in_bank');
        if (patchInBank) {
            const p = parseInt(patchInBank) || 1;
            if (p !== state.patchInBank) {
                state.patchInBank = p;
                state.needsRedraw = true;
            }
        }

        const bankCount = host_module_get_param('bank_count');
        if (bankCount) {
            const b = parseInt(bankCount) || 1;
            if (b !== state.bankCount) {
                state.bankCount = b;
                state.needsRedraw = true;
            }
        }

        const bankIdx = host_module_get_param(bankParamName);
        if (bankIdx) {
            const b = parseInt(bankIdx, 10);
            if (isNaN(b)) return;
            if (b !== state.bankIndex) {
                state.bankIndex = b;
                state.needsRedraw = true;
            }
        }

        /* Standard parameters */
        const presetName = host_module_get_param('preset_name');
        if (presetName && presetName !== state.presetName) {
            state.presetName = presetName;
            state.needsRedraw = true;
        }

        const preset = host_module_get_param('preset');
        if (preset) {
            const p = parseInt(preset) || 0;
            if (p !== state.currentPreset) {
                state.currentPreset = p;
                state.needsRedraw = true;
            }
        }

        const presetCount = host_module_get_param('preset_count');
        if (presetCount) {
            const c = parseInt(presetCount) || 1;
            if (c !== state.presetCount) {
                state.presetCount = c;
                state.needsRedraw = true;
            }
        }

        if (showPolyphony) {
            const poly = host_module_get_param('polyphony');
            if (poly) {
                const p = parseInt(poly) || 0;
                if (p !== state.polyphony) {
                    state.polyphony = p;
                    state.needsRedraw = true;
                }
            }
        }

        const octave = host_module_get_param('octave_transpose');
        if (octave) {
            const o = parseInt(octave) || 0;
            if (o !== state.octaveTranspose) {
                state.octaveTranspose = o;
                state.needsRedraw = true;
            }
        }
    }

    /* Send all notes off to DSP */
    function allNotesOff() {
        host_module_set_param('all_notes_off', '1');
    }

    /* Change preset - crosses bank boundaries when scrolling past first/last */
    function setPreset(index) {
        console.log(`setPreset: index=${index}, presetCount=${state.presetCount}, bankCount=${state.bankCount}`);

        /* Check for bank boundary crossing */
        if (state.bankCount > 1) {
            if (index < 0) {
                /* Going before first preset - go to previous bank's last preset */
                console.log('Crossing to previous bank (last preset)');
                setBank(-1, 'last');
                return;  /* setBank handles the preset change */
            } else if (index >= state.presetCount) {
                /* Going past last preset - go to next bank's first preset */
                console.log('Crossing to next bank (first preset)');
                setBank(1, 'first');
                return;  /* setBank handles the preset change */
            }
        } else {
            /* Single bank - wrap around */
            if (index < 0) index = state.presetCount - 1;
            if (index >= state.presetCount) index = 0;
        }

        allNotesOff();
        state.currentPreset = index;
        host_module_set_param('preset', String(state.currentPreset));

        /* Sync name from DSP */
        const name = host_module_get_param('preset_name');
        if (name) {
            state.presetName = name;
        }

        if (onPresetChange) {
            onPresetChange(state.currentPreset);
        }

        state.needsRedraw = true;
        console.log(`${moduleName}: Preset ${state.currentPreset}: ${state.presetName}`);
    }

    /* Change octave */
    function setOctave(delta) {
        allNotesOff();
        state.octaveTranspose += delta;
        if (state.octaveTranspose < -4) state.octaveTranspose = -4;
        if (state.octaveTranspose > 4) state.octaveTranspose = 4;

        host_module_set_param('octave_transpose', String(state.octaveTranspose));

        if (onOctaveChange) {
            onOctaveChange(state.octaveTranspose);
        }

        state.needsRedraw = true;
        console.log(`${moduleName}: Octave: ${state.octaveTranspose}`);
    }

    /* Change bank (Shift+Left/Right, or auto when crossing preset boundaries)
     * @param delta - direction to change bank (-1 or 1)
     * @param targetPreset - optional preset to go to ('first', 'last', or leave undefined for first)
     */
    function setBank(delta, targetPreset) {
        if (state.bankCount <= 1) return;

        allNotesOff();
        let newIndex = state.bankIndex + delta;
        if (newIndex < 0) newIndex = state.bankCount - 1;
        if (newIndex >= state.bankCount) newIndex = 0;

        state.bankIndex = newIndex;
        host_module_set_param(bankParamName, String(state.bankIndex));

        /* Query the new bank's preset count from DSP */
        const newPresetCount = host_module_get_param('preset_count');
        if (newPresetCount) {
            state.presetCount = parseInt(newPresetCount) || 1;
        }

        /* Set preset based on targetPreset parameter */
        if (targetPreset === 'last') {
            /* Go to last preset in the new bank */
            state.currentPreset = Math.max(0, state.presetCount - 1);
            host_module_set_param('preset', String(state.currentPreset));
        } else {
            /* Default to first preset */
            state.currentPreset = 0;
            host_module_set_param('preset', '0');
        }

        /* Sync bank name from DSP */
        const newBankName = host_module_get_param('bank_name');
        if (newBankName) {
            state.bankName = newBankName;
        }

        /* Sync preset name */
        const name = host_module_get_param('preset_name');
        if (name) {
            state.presetName = name;
        }

        if (onBankChange) {
            onBankChange(state.bankIndex);
        }

        state.needsRedraw = true;
        console.log(`${moduleName}: Bank ${state.bankIndex}: ${state.bankName}`);
    }

    /* Draw the UI */
    function drawUI() {
        clear_screen();

        /* Title bar */
        print(2, 2, moduleName, 1);
        fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

        let y = 16;

        /* Bank name (if available and meaningful) */
        if (state.bankName && state.bankName !== moduleName) {
            let bankLabel = state.bankName;
            if (state.bankCount > 1) {
                /* Truncate name to fit with count suffix */
                const maxLen = 16;
                if (bankLabel.length > maxLen) {
                    bankLabel = bankLabel.substring(0, maxLen);
                }
            }
            print(2, y, bankLabel.substring(0, 20), 1);
            y += 10;
        }

        /* Preset number and name */
        const presetNum = state.patchInBank || (state.currentPreset + 1);
        const presetTotal = state.presetCount || 0;
        print(2, y, `#${presetNum}/${presetTotal}`, 1);
        y += 10;

        /* Preset name */
        print(2, y, state.presetName.substring(0, 20), 1);
        y += 12;

        /* Module-specific custom content */
        if (drawCustom) {
            y = drawCustom(y, state);
        }

        /* Bottom status bar */
        if (showOctave || showPolyphony) {
            let statusItems = [];

            if (showOctave) {
                const octStr = state.octaveTranspose >= 0 ? `+${state.octaveTranspose}` : `${state.octaveTranspose}`;
                statusItems.push(`Oct:${octStr}`);
            }

            if (showPolyphony && state.polyphony > 0) {
                statusItems.push(`Poly:${state.polyphony}`);
            }

            if (statusItems.length > 0) {
                print(2, 54, statusItems.join('  '), 1);
            }
        }

        state.needsRedraw = false;
    }

    /* Handle CC messages */
    function handleCC(cc, value) {
        if (cc === CC_SHIFT) {
            state.shiftHeld = value > 0;
            return false;
        }

        /* Module-specific CC handling first */
        if (handleCustomCC && handleCustomCC(cc, value, state)) {
            return true;
        }

        /* Bank switching with Shift+Left/Right */
        if (state.shiftHeld && state.bankCount > 1) {
            if (cc === CC_LEFT && value > 0) {
                setBank(-1);
                return true;
            }
            if (cc === CC_RIGHT && value > 0) {
                setBank(1);
                return true;
            }
        }

        /* Preset navigation with left/right buttons */
        if (cc === CC_LEFT && value > 0) {
            setPreset(state.currentPreset - 1);
            return true;
        }
        if (cc === CC_RIGHT && value > 0) {
            setPreset(state.currentPreset + 1);
            return true;
        }

        /* Octave with up/down */
        if (cc === CC_PLUS && value > 0) {
            setOctave(1);
            return true;
        }
        if (cc === CC_MINUS && value > 0) {
            setOctave(-1);
            return true;
        }

        /* Jog wheel for preset selection */
        if (cc === CC_JOG_WHEEL) {
            const delta = decodeDelta(value);
            if (delta !== 0) {
                setPreset(state.currentPreset + delta);
            }
            return true;
        }

        return false;
    }

    /* === Public API === */

    function init() {
        console.log(`${moduleName} UI initializing...`);

        /* Sync initial state from DSP */
        syncFromDSP();

        /* Module-specific init */
        if (onInit) {
            onInit(state);
        }

        state.needsRedraw = true;
        console.log(`${moduleName} UI ready`);
    }

    function tick() {
        /* Sync state from DSP */
        syncFromDSP();

        /* Module-specific tick */
        if (onTick) {
            onTick(state);
        }

        /* Rate-limited redraw */
        state.tickCount++;
        if (state.needsRedraw || state.tickCount >= REDRAW_INTERVAL) {
            drawUI();
            state.tickCount = 0;
            state.needsRedraw = false;
        }
    }

    function onMidiMessageInternal(data) {
        if (isCapacitiveTouchMessage(data)) return;

        const status = data[0] & 0xF0;
        const isNote = status === 0x90 || status === 0x80;

        if (status === 0xB0) {
            /* CC - handle UI controls */
            if (handleCC(data[1], data[2])) {
                return;
            }
        } else if (isNote) {
            /* Module-specific note handling */
            if (handleCustomNote && handleCustomNote(data[1], data[2], status, state)) {
                return;
            }
            state.needsRedraw = true;
        }
    }

    function onMidiMessageExternal(data) {
        /* External MIDI goes directly to DSP via host */
    }

    /* Return state for external access */
    function getState() {
        return state;
    }

    /* Trigger a redraw */
    function requestRedraw() {
        state.needsRedraw = true;
    }

    return {
        init,
        tick,
        onMidiMessageInternal,
        onMidiMessageExternal,
        getState,
        requestRedraw,
        setPreset,
        setOctave,
        setBank,
    };
}
