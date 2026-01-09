/*
 * SEQOMD chain UI shim
 * Exposes sequencer UI callbacks for Signal Chain without touching globals.
 */

import {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
} from './ui_core.mjs';

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
};
