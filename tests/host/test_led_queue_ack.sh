#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The overtake LED queue must ACKNOWLEDGE what it accepts.
#
# activateLedQueue() in shadow_ui.js replaces move_midi_internal_send with a
# wrapper that coalesces LED packets by note/CC and flushes a few per tick.
# input_filter.mjs's setLED reads the return value to decide whether to cache
# the colour it just wrote:
#
#     const sent = move_midi_internal_send(...);
#     ledCache[note] = sent ? color : -1;
#
# That is right against the REAL binding, where false means the MIDI-out buffer
# was full and the colour must not be recorded as shown. It is wrong against a
# wrapper that queues the packet and returns undefined: the cache then records
# -1 for every write, never suppresses a repaint, and every overtake module
# repaints its whole surface every frame -- ~58 writes into a 16-per-tick flush
# budget, with notes flushed before CCs so button LEDs never flush at all. The
# surface reads as "LEDs don't work". Shipped in #319; hit davebox, tb3po,
# control, mark, mono and every other overtake module painting through shared
# setLED, but NOT the skip_led_clear ones (chorddex, song-mode) which never
# activate this queue.
#
# This wires the REAL wrapper (extracted from shadow_ui.js) to the REAL setLED
# and counts writes across identical frames. Behaviour, not a grep.

node --input-type=module - <<'NODE'
import fs from "fs";
import path from "path";

const JS = "src/shadow/shadow_ui.js";
const src = fs.readFileSync(JS, "utf8");

const start = src.indexOf("function activateLedQueue() {");
if (start < 0) { console.error("FAIL: activateLedQueue not found in " + JS); process.exit(1); }
const end = src.indexOf("\n}\n", start);
if (end < 0) { console.error("FAIL: could not delimit activateLedQueue"); process.exit(1); }
const body = src.slice(src.indexOf("{", start) + 1, end);

/*
 * activateLedQueue's FIRST statement is
 *     originalMidiInternalSend = globalThis.move_midi_internal_send;
 * so the binding it wraps must be installed on globalThis before we run it.
 * Handing one in as a local is silently discarded — and that reads as the very
 * failure this test is looking for, so getting it wrong fakes a FAIL.
 */
const makeQueue = (realBinding) => {
    globalThis.move_midi_internal_send = realBinding;
    new Function("globalThis",
        `let ledQueueNotes = {}, ledQueueCCs = {}, ledQueueActive = false,
             originalMidiInternalSend = null;
         ${body}`)(globalThis);
    if (typeof globalThis.move_midi_internal_send !== "function") {
        console.error("FAIL: activateLedQueue did not install move_midi_internal_send");
        process.exit(1);
    }
    return globalThis.move_midi_internal_send;
};

const fails = [];

/* ---- 1. queued LED packets must be acknowledged ------------------------ */

const queueWrapper = makeQueue(() => true);
let queued = 0;
globalThis.move_midi_internal_send = (arr) => { queued++; return queueWrapper(arr); };

const { setLED, setButtonLED } = await import(
    "file://" + path.resolve("src/shared/input_filter.mjs")
);

/* One overtake module's steady-state repaint: the same surface, unchanged. */
function paintFrame() {
    for (let n = 68; n < 100; n++) setLED(n, 0x7F);            /* 32 pads  */
    for (let n = 16; n < 32; n++) setLED(n, 0x03);             /* 16 steps */
    for (const cc of [40, 41, 42, 43, 49, 50, 51, 55, 56, 60]) setButtonLED(cc, 0x7F);
}

const FRAMES = 10;
const PER_FRAME = 58;
for (let f = 0; f < FRAMES; f++) paintFrame();

if (queued !== PER_FRAME) {
    fails.push(
        `LED cache is dead under the overtake queue: ${queued} writes across ` +
        `${FRAMES} identical frames, expected ${PER_FRAME} (first frame only). ` +
        `activateLedQueue's wrapper must return a truthy value for a packet it ` +
        `has accepted onto the queue — setLED reads it to decide whether to cache.`
    );
}

/* ---- 2. a non-LED write still reports the real binding's result -------- */

const passthrough = makeQueue(() => false);
if (passthrough([0x04, 0xF0, 0x7E, 0x00]) !== false) {
    fails.push("non-LED passthrough must return the real binding's result, not true");
}

/* ---- 3. and the LED path must not invent success when it cannot queue -- */

const inactive = makeQueue(() => false);
globalThis.move_midi_internal_send = inactive;
/* An LED write IS queued, so it reports true even though the underlying
 * binding would have refused — that is the point of the queue. Assert the
 * wrapper is genuinely queueing rather than forwarding. */
let forwarded = 0;
const counting = makeQueue(() => { forwarded++; return false; });
if (counting([0x09, 0x90, 70, 0x7F]) !== true) {
    fails.push("an accepted LED packet must report true");
}
if (forwarded !== 0) {
    fails.push("an LED packet must be queued, not forwarded to the binding");
}

if (fails.length) {
    for (const f of fails) console.error("FAIL: " + f);
    process.exit(1);
}
console.log("PASS: overtake LED queue acknowledges queued packets (" +
            queued + " writes over " + FRAMES + " identical frames)");
NODE
