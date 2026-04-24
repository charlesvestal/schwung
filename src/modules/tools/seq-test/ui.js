/*
 * Seq Test — minimal UI for the MIDI-inject generator test tool.
 *
 * Purpose is to prove the generator path end-to-end with minimum surface:
 * a play button starts/stops a 4-step C-major arp on a user-chosen MIDI
 * channel; the DSP handles timing and injection. This UI is deliberately
 * barebones — no patterns, no knob-for-BPM, no save/load.
 *
 * Capabilities: suspend_keeps_js so the sequence keeps ticking when the
 * user presses Back and returns to Move's UI; full exit happens via
 * Shift+Back (host-intercepted) or the Tools menu.
 */

import { announce, announceMenuItem } from
    '/data/UserData/schwung/shared/screen_reader.mjs';
import { decodeDelta } from
    '/data/UserData/schwung/shared/input_filter.mjs';

/* ---- CC numbers we care about ---- */

const CC_SHIFT = 49;
const CC_BACK  = 51;
const CC_KNOB1 = 71;
const CC_PLAY  = 85;

/* ---- Local state (mirror of DSP) ---- */

let running = 0;
let channel = 0;          /* 0-15, displayed 1-16 */
let step    = 0;
let shiftHeld = 0;
let needsRedraw = true;

/* ---- DSP bridge helpers ---- */

function setDspParam(key, val) {
    if (typeof host_module_set_param === "function") {
        host_module_set_param(key, String(val));
    }
}

function getDspParam(key) {
    if (typeof host_module_get_param === "function") {
        return host_module_get_param(key);
    }
    return null;
}

/* ---- Drawing ---- */

function draw() {
    clear_screen();

    /* Title */
    print(0, 0, "Seq Test", 2);

    /* State lines */
    print(0, 24, running ? "Running" : "Stopped", 1);

    const chStr = "Ch " + String(channel + 1);
    print(80, 24, chStr, 1);

    /* Step indicator as 4 little boxes, the current one filled */
    const y = 40;
    for (let i = 0; i < 4; i++) {
        const x = 20 + i * 22;
        if (i === step && running) {
            fill_rect(x, y, 18, 12, 1);
        } else {
            draw_rect(x, y, 18, 12, 1);
        }
    }

    /* Footer hint */
    print(0, 56, "Play=start/stop  K1=channel", 1);

    needsRedraw = false;
}

/* ---- Lifecycle ---- */

globalThis.init = function() {
    /* Pull initial state from DSP (suspend_keeps_js preserves DSP state too). */
    const r = parseInt(getDspParam("running")) || 0;
    const c = parseInt(getDspParam("channel")) || 0;
    running = r ? 1 : 0;
    channel = c & 0x0F;
    step    = parseInt(getDspParam("step")) || 0;
    shiftHeld = 0;
    needsRedraw = true;

    announceMenuItem("Seq Test", running ? "running" : "stopped");
    draw();
};

globalThis.tick = function() {
    /* Poll step + running from DSP so the display reflects live state. */
    const newStep = parseInt(getDspParam("step")) || 0;
    const newRunning = parseInt(getDspParam("running")) || 0;
    if (newStep !== step || newRunning !== running) {
        step = newStep;
        running = newRunning;
        needsRedraw = true;
    }
    if (needsRedraw) draw();
};

globalThis.onMidiMessageInternal = function(data) {
    if (!data) return;
    const status = data[0] | 0;
    const d1     = data[1] | 0;
    const d2     = data[2] | 0;
    const type   = status & 0xF0;

    /* Ignore capacitive touch (notes 0-9) and aftertouch / sysex / clock. */
    if ((type === 0x90 || type === 0x80) && d1 < 10) return;
    if (type === 0xA0 || type === 0xD0) return;
    if (status === 0xF8 || status === 0xF0 || status === 0xF7) return;

    if (type !== 0xB0) return;   /* only CC handling below */

    /* Track shift locally (host intercepts Back before we see it). */
    if (d1 === CC_SHIFT) { shiftHeld = (d2 > 0) ? 1 : 0; return; }
    if (d1 === CC_BACK)  { return; }   /* host handles suspend/exit */

    /* Play: toggle running. Declared as button_passthrough in module.json
     * so the host still sees it, but we also react here. */
    if (d1 === CC_PLAY && d2 > 0) {
        running = running ? 0 : 1;
        setDspParam("running", String(running));
        announce(running ? "Sequencer started" : "Sequencer stopped");
        needsRedraw = true;
        return;
    }

    /* Knob 1: channel select. */
    if (d1 === CC_KNOB1) {
        const raw = decodeDelta(d2);
        const delta = raw > 0 ? 1 : (raw < 0 ? -1 : 0);
        const next = Math.max(0, Math.min(15, channel + delta));
        if (next !== channel) {
            channel = next;
            setDspParam("channel", String(channel));
            announce("Channel " + (channel + 1));
            needsRedraw = true;
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(_data) {
    /* Tool doesn't consume external MIDI. */
};
