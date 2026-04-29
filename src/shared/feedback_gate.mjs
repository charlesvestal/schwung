/*
 * Feedback gate — prevents audible feedback when activating mic-input paths
 * with built-in speakers active.
 *
 * Risk fires only when speakers are on AND no line-in cable is plugged
 * (so the input source defaults to the internal mic, which picks up the
 * speakers).
 *
 * Two callers:
 *   - Chain slot module pick (when picked module's metadata indicates it
 *     consumes line-in audio)
 *   - Tool module launch (same heuristic on the tool's metadata)
 *
 * The modal renders via drawConfirmOverlay from menu_layout. The caller is
 * responsible for invoking feedbackGateDraw each tick while the gate is
 * active; this module exposes the state and input handling.
 *
 * NOTE: callback-based, NOT Promise-based. The schwung host's QuickJS
 * runtime does not pump pending jobs (no JS_ExecutePendingJob calls), so
 * Promise.then callbacks never fire. Callers pass a continuation function.
 */

import { drawConfirmOverlay } from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';

let gateActive = false;
let gateContinuation = null;

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
 * Begin a confirm flow. Calls `cb(true)` immediately if no risk. Otherwise
 * shows the modal and calls `cb(true)` on Yes / `cb(false)` on No once the
 * user resolves it. If a gate is already active, calls `cb(false)`.
 *
 * Caller must:
 *   1. Pump feedbackGateDraw() in the tick path while feedbackGateActive() is true.
 *   2. Forward CC input via feedbackGateInput(cc, val).
 */
export function confirmLineInput(label, cb) {
    if (typeof cb !== 'function') return;
    if (!feedbackRiskPresent()) { cb(true); return; }
    if (gateActive) { cb(false); return; }
    gateActive = true;
    gateContinuation = cb;
    announce('Speaker feedback risk. Speakers and mic active. Plug in headphones. Jog click for yes, back for no.');
}

/**
 * Combined helper: check meta + condition in one call.
 */
export function maybeConfirmForModule(meta, cb) {
    if (typeof cb !== 'function') return;
    if (!consumesLineInput(meta)) { cb(true); return; }
    confirmLineInput((meta && meta.name) || (meta && meta.id) || 'Module', cb);
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
        'Speakers + Mic',
        'Active!',
        'Plug in headphones.',
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
    const cb = gateContinuation;
    gateActive = false;
    gateContinuation = null;
    announce(answer ? 'Confirmed.' : 'Cancelled.');
    if (cb) cb(answer);
}
