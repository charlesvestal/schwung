/*
 * MIDI Delay Test - Overtake Module
 * Sends periodic MIDI note-on/off on cable 2 to diagnose output latency.
 * Tracks timing drift between expected and actual send times.
 */

let tickCount = 0;
const TEST = 15;
let state = 1;
let cable = 2;
let notesSent = 0;
let midiInCount = 0;

/* Timing diagnostics */
let lastSendTime = 0;
let maxInterval = 0;
let minInterval = 99999;
let expectedInterval = 0;
let firstSendTime = 0;

function init() {
    console.log("midi-delay-test: init");
    lastSendTime = Date.now();
    firstSendTime = lastSendTime;
}

function tick() {
    tickCount++;

    if (tickCount % TEST === 0) {
        if (state === 1) {
            let now = Date.now();
            let interval = now - lastSendTime;

            move_midi_external_send([cable << 4 | (0x90 / 16), 0x90 | 0, 38, 127]);
            state = 2;
            notesSent++;
            lastSendTime = now;

            if (interval < minInterval && notesSent > 1) minInterval = interval;
            if (interval > maxInterval) maxInterval = interval;

            /* Log every 10 notes */
            if (notesSent > 0 && notesSent % 10 === 0) {
                let elapsed = now - firstSendTime;
                let expectedElapsed = (notesSent - 1) * (expectedInterval || interval);
                let drift = elapsed - expectedElapsed;
                console.log("midi-delay-test: notes=" + notesSent +
                    " interval=" + interval + "ms" +
                    " min=" + minInterval + " max=" + maxInterval +
                    " midiIn=" + midiInCount +
                    " drift=" + drift + "ms" +
                    " elapsed=" + elapsed + "ms");
                midiInCount = 0;
            }

            if (notesSent === 2) {
                expectedInterval = interval;
            }
        } else if (state === 2) {
            move_midi_external_send([cable << 4 | (0x80 / 16), 0x80 | 0, 38, 0]);
            state = 3;
        } else if (state === 3) {
            state = 1;
        }
    }

    /* Display every 50 ticks using shadow UI display API */
    if (tickCount % 50 === 0) {
        clear_screen();
        print(0, 0, "MIDI Delay Test", 1);
        print(0, 12, "Notes:" + notesSent + " In:" + midiInCount, 1);
        print(0, 24, "Min:" + minInterval + " Max:" + maxInterval, 1);
        print(0, 36, "Drum pads to reproduce!", 1);
        host_flush_display();
    }
}

function onMidiInternal(data) {
    midiInCount++;
}

function onMidiExternal(data) {
    /* Ignore external MIDI */
}

globalThis.init = init;
globalThis.tick = tick;
globalThis.onMidiMessageInternal = onMidiInternal;
globalThis.onMidiMessageExternal = onMidiExternal;
