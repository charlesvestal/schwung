/*
 * Feedback gate — prevents audible feedback when activating mic-input paths
 * with built-in speakers active.
 *
 * Risk fires only when speakers are on AND no line-in cable is plugged
 * (so the input source defaults to the internal mic, which picks up the
 * speakers).
 *
 * Three callers:
 *   - Quantized Sampler source toggle (when source flips to "Move Input")
 *   - Chain slot module pick (when picked module's metadata indicates it
 *     consumes line-in audio)
 *   - Tool module launch (same heuristic on the tool's metadata)
 *
 * The modal renders via drawConfirmOverlay from menu_layout. The caller is
 * responsible for invoking feedbackGateDraw each tick while the gate is
 * active; this module exposes the state and input handling.
 */

import { drawConfirmOverlay } from '/data/UserData/schwung/shared/menu_layout.mjs';

let gateActive = false;
let gateLabel = '';
let gateResolve = null;

/**
 * Heuristic: true if the module pulls audio in from the line-in / internal mic.
 * sound_generator + audio_in: line-in is the only upstream source.
 * tool + audio_in: tool taps line-in (e.g. AutoSample).
 * audio_fx / midi_fx with audio_in: audio comes from upstream chain slot, NOT line-in.
 */
export function consumesLineInput(meta) {
    if (!meta) return false;
    const c = meta.capabilities ?? {};
    if (!c.audio_in) return false;
    const t = meta.component_type ?? c.component_type ?? '';
    if (t === 'audio_fx' || t === 'midi_fx') return false;
    return true;
}

/**
 * Returns true if conditions for feedback are present (warn) or false (silent pass).
 */
function feedbackRiskPresent() {
    if (typeof host_speaker_active !== 'function') return false;
    if (typeof host_line_in_connected !== 'function') return false;
    if (!host_speaker_active()) return false;
    if (host_line_in_connected()) return false;
    return true;
}

/**
 * Begin a confirm flow. Returns a Promise that resolves to true (Yes) or false (No).
 * If conditions are safe, resolves to true immediately without showing the modal.
 *
 * Caller must:
 *   1. Pump feedbackGateDraw() in the tick path while feedbackGateActive() is true.
 *   2. Forward jog-click and back-button input via feedbackGateInput(cc, val).
 */
export function confirmLineInput(label) {
    if (!feedbackRiskPresent()) return Promise.resolve(true);
    if (gateActive) {
        /* Already showing — refuse to stack. */
        return Promise.resolve(false);
    }
    gateActive = true;
    gateLabel = label || 'Line input';
    return new Promise((resolve) => {
        gateResolve = resolve;
    });
}

/**
 * Combined helper: check meta + condition in one call.
 */
export function maybeConfirmForModule(meta) {
    if (!consumesLineInput(meta)) return Promise.resolve(true);
    return confirmLineInput((meta && meta.name) || (meta && meta.id) || 'Module');
}

/**
 * True when the modal is currently showing.
 */
export function feedbackGateActive() {
    return gateActive;
}

/**
 * Render the modal. Call this in the tick path while feedbackGateActive() is true.
 */
export function feedbackGateDraw() {
    if (!gateActive) return;
    drawConfirmOverlay('Speaker Feedback Risk', [
        'Speakers are active!',
        'Monitoring mic input',
        'creates feedback.',
        'Plug in headphones',
        'or use line-in.',
    ]);
}

/**
 * Forward MIDI input to the modal. Returns true if the input was consumed.
 * Yes = jog click (CC 3, val=127); No = back (CC 51, val=127).
 * Releases (val=0) and other CCs are swallowed so the user can't accidentally
 * edit underlying state while the modal is showing.
 */
export function feedbackGateInput(cc, val) {
    if (!gateActive) return false;
    if (val === 0) return true; /* swallow releases while active */
    if (cc === 3) {       /* CC_JOG_CLICK */
        resolveGate(true);
        return true;
    }
    if (cc === 51) {      /* CC_BACK */
        resolveGate(false);
        return true;
    }
    /* While active, swallow all other CC input to prevent stray edits. */
    return true;
}

function resolveGate(answer) {
    const r = gateResolve;
    gateActive = false;
    gateLabel = '';
    gateResolve = null;
    if (r) r(answer);
}
