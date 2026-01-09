/*
 * SEQOMD UI entrypoint
 * Loads shared UI core and registers host callbacks.
 */

import {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
} from './ui_core.mjs';

globalThis.init = init;
globalThis.tick = tick;
globalThis.onMidiMessageInternal = onMidiMessageInternal;
globalThis.onMidiMessageExternal = onMidiMessageExternal;
