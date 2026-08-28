#!/usr/bin/env node
/**
 * widget_sheet.mjs — generate docs/WIDGETS.md, the contact sheet of every
 * widget the knob grid actually ships.
 *
 * GENERATED, not written. A hand-drawn sheet is stale the first time a widget
 * moves a pixel, and this codebase has already learned that reviewing widgets
 * from their code rather than their render lets real defects through. Every
 * swatch here comes out of the SAME functions the device calls, so the sheet
 * cannot describe a widget the grid does not draw.
 *
 * Deliberately NOT the SCH-50 catalog (tools/param-pages/catalog.mjs). That
 * renders ten ALTERNATIVES per widget for choosing between, nine of which were
 * rejected, and its output is gitignored. This renders the one that WON, with
 * the rule that selects it and how many cells in the fleet hit that rule.
 *
 *   node tools/param-pages/widget_sheet.mjs          write docs/WIDGETS.md + images
 *   node tools/param-pages/widget_sheet.mjs --check  fail if it would change
 *
 * --check is what a test uses: it regenerates into memory and diffs, so a
 * widget change that nobody documented shows up as a failure rather than as a
 * quietly wrong page.
 *
 * Node-only. Nothing here ships to the device.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createFramebuffer, drawContext } from "./harness.mjs";
import * as RM from "../../src/shared/param_pages/render_page_movy.mjs";
import { drawVizGroup } from "../../src/shared/param_pages/viz_draw.mjs";
import { buildMetaIndex, KIND_OPAQUE, alsoOpens, opensOnClick }
    from "../../src/shared/param_pages/param_meta.mjs";
import { resolveViz, VIZ_SAMPLE } from "../../src/shared/param_pages/viz.mjs";
import { planPages, PAGE_KNOBS } from "../../src/shared/param_pages/page_plan.mjs";
import { setWavPeaksIO, wavPeaksTick } from "../../src/shared/param_pages/wav_peaks.mjs";
import { createAnimState } from "../../src/shared/param_pages/anim_state.mjs";
import { drawMenuList } from "../../src/shared/menu_layout.mjs";
import { encodeGif } from "./gif.mjs";
import os from "node:os";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const MD = path.join(ROOT, "docs", "WIDGETS.md");
const IMG_DIR = path.join(ROOT, "docs", "images", "widgets");
const IMG_REL = "images/widgets";
const FIXTURE = path.join(ROOT, "tests", "fixtures", "module-contracts.json");

/*
 * A REAL WAV, so the sample swatch shows a real envelope.
 *
 * wav_peaks reads through an injectable IO shaped like the QuickJS std/os
 * pair, so node can drive the same decoder the device does. Without this the
 * sample graphic renders as a bare baseline -- honest, and a useless picture
 * of the one widget whose whole point is the file's own shape.
 */
setWavPeaksIO({
    open: (p) => {
        let fd;
        try { fd = fs.openSync(p, "r"); } catch (e) { return null; }
        let cursor = 0;
        return {
            read: (buf, pos, len) => {
                const n = fs.readSync(fd, new Uint8Array(buf, pos, len), 0, len, cursor);
                cursor += n; return n;
            },
            seek: (off, whence) => { if (whence === 0) cursor = off; return 0; },
            close: () => fs.closeSync(fd),
        };
    },
    stat: (p) => {
        try { const st = fs.statSync(p); return { size: st.size, mtime: Math.floor(st.mtimeMs) }; }
        catch (e) { return null; }
    },
});

/* A drum-ish one-shot: sharp attack, exponential decay, then a softer second
 * hit. Deterministic, so the committed PNG is reproducible on any machine. */
function writeSampleWav() {
    const rate = 8000, frames = 8000;
    const data = Buffer.alloc(frames * 2);
    for (let i = 0; i < frames; i++) {
        const t = i / rate;
        const hit = (t0, f, d) => (t < t0 ? 0
            : Math.sin(2 * Math.PI * f * (t - t0)) * Math.exp(-(t - t0) / d));
        const v = hit(0.02, 140, 0.10) * 0.95 + hit(0.55, 190, 0.16) * 0.45;
        data.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(v * 32767))), i * 2);
    }
    const hdr = Buffer.alloc(44);
    hdr.write("RIFF", 0); hdr.writeUInt32LE(36 + data.length, 4); hdr.write("WAVE", 8);
    hdr.write("fmt ", 12); hdr.writeUInt32LE(16, 16); hdr.writeUInt16LE(1, 20);
    hdr.writeUInt16LE(1, 22); hdr.writeUInt32LE(rate, 24);
    hdr.writeUInt32LE(rate * 2, 28); hdr.writeUInt16LE(2, 32); hdr.writeUInt16LE(16, 34);
    hdr.write("data", 36); hdr.writeUInt32LE(data.length, 40);
    const out = path.join(os.tmpdir(), "schwung-widget-sheet.wav");
    fs.writeFileSync(out, Buffer.concat([hdr, data]));
    /* Peaks stream a couple of blocks per tick; pump until it says done. */
    for (let i = 0; i < 200; i++) if (!wavPeaksTick(out)) break;
    return out;
}

const CELL_W = 32;
const ROW_H = RM.LBL0_Y - RM.ROW0_Y + RM.LBL_H;   /* widget band + its label */

/* ------------------------------------------------------------------ swatch */

/*
 * One strip of cells, drawn with the REAL cell geometry.
 *
 * The band height is taken from the renderer's own constants rather than
 * written here: a widget is budgeted against BOX_H and its label sits at
 * LBL0_Y, and a swatch that invented its own spacing would be a picture of a
 * layout the device does not have.
 */
function strip(nCells, draw) {
    const w = nCells * CELL_W;
    const fb = createFramebuffer(w, ROW_H);
    const ctx = drawContext(fb);
    const g = { x0: 0, cellW: CELL_W };
    draw(ctx, g, fb);
    return fb;
}


/*
 * A horizontal band of a rendered screen, as its own framebuffer.
 *
 * The chrome pieces draw in ABSOLUTE screen coordinates — the footer at
 * FOOTER_Y, the rule at RULE_Y — so they cannot be rendered into a
 * band-sized buffer the way a cell widget can. Render the whole screen, then
 * take the rows that matter.
 */
function cropRows(fb, y0, y1) {
    const h = y1 - y0;
    const out = createFramebuffer(128, h);
    const octx = drawContext(out);
    for (let y = y0; y < y1; y++)
        for (let x = 0; x < fb.width; x++)
            if (fb.pixels[y * fb.width + x]) octx.fillRect(x, y - y0, 1, 1, 1);
    return out;
}

function cellLabels(ctx, g, n, labels) {
    for (let i = 0; i < n; i++) {
        if (!labels[i]) continue;
        RM.drawLabelCell(ctx, g, i, RM.LBL0_Y - RM.ROW0_Y, labels[i], "", false, false, false);
    }
}


/* ---------------------------------------------------------------- motion */

/*
 * An animated GIF of one widget, one frame per step.
 *
 * ONE anim store across the whole clip is the trick. The store is what
 * remembers the previous value; a fresh one per frame stamps every sighting as
 * already-past and renders the settled frame N times — which is exactly how
 * every animation in this subsystem shipped inert for months. If a clip below
 * ever looks still, suspect this before suspecting the widget.
 *
 * `hold` extra copies of the last frame, so a 160ms animation does not loop so
 * tightly that it reads as a flicker.
 */
function clip(nFrames, dtMs, w, h, drawFrame,
              { slowdown = 5, holdMs = 900, scale = 4 } = {}) {
    const anim = createAnimState();
    const frames = [];
    for (let i = 0; i < nFrames; i++) {
        const fb = createFramebuffer(w, h);
        /* SAMPLED at real time -- what is slowed is playback, not the
         * animation. The frames are the ones the device draws at 0, dt, 2dt;
         * stretching dt instead would sample a different set and show a
         * different curve, which for an eased transition is a different
         * picture rather than the same one slower. */
        drawFrame(drawContext(fb), i * dtMs, anim, fb);
        frames.push(fb);
    }
    /*
     * PLAYED at 1/slowdown speed, and held at the end.
     *
     * At real time these are unreadable: the longest is 160ms, so the clip is
     * over before the eye lands on it and the loop restarts immediately --
     * "those gifs play too fast". A GIF cannot be scrubbed, so the only way to
     * make a 160ms transition legible in a document is to stretch it, and the
     * page says it is stretched rather than implying the device is this slow.
     *
     * The hold is what separates one play from the next. Without it a short
     * clip reads as a stutter rather than as a gesture that happens and then
     * settles.
     */
    const delay = dtMs * slowdown;
    const last = frames[frames.length - 1];
    for (let held = 0; held < holdMs; held += delay) frames.push(last);
    return encodeGif(frames, { delayMs: delay, scale });
}

/* --------------------------------------------------------------- the sheet */

const META = (o) => buildMetaIndex({ chainParams: [o] }).getOrGuess(o.key);

function vizStrip(cp, keys, values, span) {
    const mi = buildMetaIndex({ chainParams: cp });
    const { groups } = resolveViz({ keys, metaIndex: mi });
    const g0 = groups[0];
    return strip(span, (ctx, g) => {
        if (g0) {
            drawVizGroup(ctx, { x: 0, y: RM.ROW0_Y - RM.ROW0_Y, w: span * CELL_W, h: RM.BOX_H },
                         g0, values, mi);
        }
    });
}

function build() {
    const images = new Map();
    const add = (name, fb) => images.set(name, fb.toPng(4));

    /* --- cell widgets, in drawKnobWidget dispatch order -------------------- */

    add("arc-knob", strip(4, (ctx, g) => {
        const m = META({ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 });
        const vals = ["0", "0.33", "0.75", "1"];
        for (let i = 0; i < 4; i++)
            RM.drawKnobWidget(ctx, g, i, 0, m, vals[i], undefined, undefined, null, null);
        cellLabels(ctx, g, 4, ["MIN", "LOW", "HIGH", "MAX"]);
    }));

    add("arc-knob-modulated", strip(2, (ctx, g) => {
        const m = META({ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 });
        RM.drawKnobWidget(ctx, g, 0, 0, m, "0.5", undefined, undefined, null, null);
        RM.drawKnobWidget(ctx, g, 1, 0, m, "0.5", "0.85", "0.85", null, null);
        cellLabels(ctx, g, 2, ["BASE", "MOD"]);
    }));

    add("big-number", strip(3, (ctx, g) => {
        const m = META({ key: "voices", name: "Voices", type: "int", min: 1, max: 8 });
        const b = META({ key: "octave", name: "Octave", type: "int", min: -4, max: 4 });
        RM.drawKnobWidget(ctx, g, 0, 0, m, "1", undefined, undefined, null, null);
        RM.drawKnobWidget(ctx, g, 1, 0, m, "8", undefined, undefined, null, null);
        RM.drawKnobWidget(ctx, g, 2, 0, b, "-2", undefined, undefined, null, null);
        cellLabels(ctx, g, 3, ["VOICES", "VOICES", "OCT"]);
    }));

    add("enum-square", strip(3, (ctx, g) => {
        const m = META({ key: "mode", name: "Mode", type: "enum",
                         options: ["Low Pass", "Band Pass", "Notch"] });
        for (let i = 0; i < 3; i++)
            RM.drawKnobWidget(ctx, g, i, 0, m, String(i), undefined, undefined, null, null);
        cellLabels(ctx, g, 3, ["MODE", "MODE", "MODE"]);
    }));

    add("button", strip(3, (ctx, g) => {
        const m = META({ key: "clear", name: "Clear", type: "enum",
                         options: ["—", "Rnd!"], access: "write" });
        RM.drawKnobWidget(ctx, g, 0, 0, m, "—", undefined, undefined, null,
                          { pressed: false, filled: false, bursts: [] });
        RM.drawKnobWidget(ctx, g, 1, 0, m, "—", undefined, undefined, null,
                          { pressed: true, filled: true, bursts: [0.1] });
        RM.drawKnobWidget(ctx, g, 2, 0, m, "—", undefined, undefined, null,
                          { pressed: false, filled: true, bursts: [0.7] });
        cellLabels(ctx, g, 3, ["CLEAR", "CLEAR", "CLEAR"]);
    }));

    add("opaque-box", strip(3, (ctx, g) => {
        const m = META({ key: "sample_path", name: "Sample", type: "filepath" });
        RM.drawKnobWidget(ctx, g, 0, 0, m, "/x/kick_01.wav", undefined, undefined, null, null);
        RM.drawKnobWidget(ctx, g, 1, 0, m, "", undefined, undefined, null, null);
        RM.drawKnobWidget(ctx, g, 2, 0, m, null, undefined, undefined, null, null);
        cellLabels(ctx, g, 3, ["LOADED", "EMPTY", "UNREAD"]);
    }));

    /* --- the mark ---------------------------------------------------------- */

    add("brackets", strip(2, (ctx, g) => {
        const pos = META({ key: "position", name: "Pos", type: "float",
                           ui_type: "wav_position", min: 0, max: 1, step: 0.01 });
        const plain = META({ key: "size", name: "Size", type: "float", min: 0, max: 1, step: 0.01 });
        RM.drawKnobWidget(ctx, g, 0, 0, pos, "0.4", undefined, undefined, null, null);
        RM.drawBrackets(ctx, 1, 0, CELL_W - 2, RM.BOX_H);
        RM.drawKnobWidget(ctx, g, 1, 0, plain, "0.4", undefined, undefined, null, null);
        cellLabels(ctx, g, 2, ["OPENS", "PLAIN"]);
    }));

    /* --- viz graphics ------------------------------------------------------ */

    const F = (k, extra = {}) =>
        ({ key: k, name: k, type: "float", min: 0, max: 1, step: 0.01, ...extra });

    add("viz-envelope", vizStrip(
        [F("attack"), F("decay"), F("sustain"), F("release")],
        ["attack", "decay", "sustain", "release"],
        { attack: "0.2", decay: "0.4", sustain: "0.6", release: "0.5" }, 4));

    add("viz-filter", vizStrip(
        [F("cutoff"), F("resonance")], ["cutoff", "resonance"],
        { cutoff: "0.55", resonance: "0.7" }, 2));

    add("viz-lfo", vizStrip(
        [{ key: "lfo_shape", name: "Shape", type: "enum",
           options: ["Sine", "Triangle", "Saw", "Square"] },
         F("lfo_rate"), F("lfo_depth")],
        ["lfo_shape", "lfo_rate", "lfo_depth"],
        { lfo_shape: "0", lfo_rate: "0.4", lfo_depth: "0.8" }, 3));

    add("viz-eq", vizStrip(
        [F("eq_low", { min: -12, max: 12 }), F("eq_mid", { min: -12, max: 12 }),
         F("eq_high", { min: -12, max: 12 })],
        ["eq_low", "eq_mid", "eq_high"],
        { eq_low: "4", eq_mid: "-3", eq_high: "6" }, 3));

    add("viz-waveform", vizStrip(
        [{ key: "osc_wave", name: "Wave", type: "enum",
           options: ["Sine", "Triangle", "Saw", "Square"] }],
        ["osc_wave"], { osc_wave: "2" }, 1));

    add("viz-fader", vizStrip(
        [F("level", { name: "Level" })], ["level"], { level: "0.65" }, 1));

    add("viz-switch", vizStrip(
        [{ key: "sync", name: "Sync", type: "enum", options: ["Off", "On"] }],
        ["sync"], { sync: "1" }, 1));

    add("viz-sample", vizStrip(
        [{ key: "position", name: "Pos", type: "float", ui_type: "wav_position",
           min: 0, max: 1, step: 0.01, filepath_param: "sample_path" },
         F("spray"),
         { key: "sample_path", name: "File", type: "filepath" }],
        ["position", "spray", "sample_path"],
        { position: "0.45", spray: "0.12", sample_path: writeSampleWav() }, 2));


    /* --- chrome ------------------------------------------------------------ */

    add("chrome-header", (() => {
        const fb = createFramebuffer(128, RM.ROW0_Y);
        const ctx = drawContext(fb);
        RM.drawHeader(ctx, "S1 > OBXD", "FILTER", false);
        return fb;
    })());

    add("chrome-header-held", (() => {
        const fb = createFramebuffer(128, RM.ROW0_Y);
        const ctx = drawContext(fb);
        RM.drawHeader(ctx, "Cutoff", "4.20 kHz", true);
        return fb;
    })());

    add("chrome-bank-bar", (() => {
        const fb = createFramebuffer(128, 10);
        const ctx = drawContext(fb);
        RM.drawBankBar(ctx, 2, 7);
        return fb;
    })());

    /*
     * PAIRS, not a flat list. drawFooter takes [[key, action], ...] and
     * inverts the KEY into a pill; handed four loose strings it drew
     * "J o P a C L O p" — every other character pilled, which is what a
     * mis-shaped argument looks like rather than an error.
     *
     * Drawn into a full 128x64 because the footer works in absolute screen
     * coordinates, then cropped to the band so the swatch is the footer and
     * not 50 rows of black above it.
     */
    add("chrome-footer", (() => {
        const full = createFramebuffer(128, 64);
        RM.drawFooter(drawContext(full), [["JOG", "SEL"], ["CLK", "LOAD"], ["BACK", "EXIT"]]);
        return cropRows(full, RM.RULE_Y - 1, 64);
    })());

    add("chrome-label-cell", strip(3, (ctx, g) => {
        RM.drawLabelCell(ctx, g, 0, RM.LBL0_Y - RM.ROW0_Y, "CUTOFF", "0.42", false, false, false);
        RM.drawLabelCell(ctx, g, 1, RM.LBL0_Y - RM.ROW0_Y, "CUTOFF", "0.42", true, true, false);
        RM.drawLabelCell(ctx, g, 2, RM.LBL0_Y - RM.ROW0_Y, "CUTOFF", "0.42", false, false, true);
    }));

    add("chrome-list", (() => {
        const fb = createFramebuffer(128, 64);
        const ctx = drawContext(fb);
        const items = Array.from({ length: 24 }, (_, i) => ({ n: "Preset " + (i + 1) }));
        drawMenuList({
            ctx, items, selectedIndex: 9,
            listArea: { topY: 10, bottomY: 54 },
            getLabel: (it) => it.n,
            getValue: () => "",
            announce: false,
        });
        return fb;
    })());

    /* --- motion ------------------------------------------------------------ */

    const addGif = (name, buf) => images.set(name + ".gif", buf);

    const SW = [{ key: "sync", name: "Sync", type: "enum", options: ["Off", "On"] }];
    const swMi = buildMetaIndex({ chainParams: SW });
    const swGroup = resolveViz({ keys: ["sync"], metaIndex: swMi }).groups[0];
    addGif("motion-switch", clip(9, 20, CELL_W, RM.BOX_H, (ctx, t, anim) => {
        /* Frame 0 primes the store with the OLD value; the flip is at t=0 too,
         * so the clip reads as one gesture from the first frame. */
        drawVizGroup(ctx, { x: 0, y: 0, w: CELL_W, h: RM.BOX_H },
                     swGroup, { sync: t === 0 ? "0" : "1" }, swMi, anim, t);
    }));

    const WV = [{ key: "osc_wave", name: "Wave", type: "enum",
                  options: ["Sine", "Triangle", "Saw", "Square"] }];
    const wvMi = buildMetaIndex({ chainParams: WV });
    const wvGroup = resolveViz({ keys: ["osc_wave"], metaIndex: wvMi }).groups[0];
    addGif("motion-waveform", clip(9, 15, CELL_W, RM.BOX_H, (ctx, t, anim) => {
        drawVizGroup(ctx, { x: 0, y: 0, w: CELL_W, h: RM.BOX_H },
                     wvGroup, { osc_wave: t === 0 ? "0" : "3" }, wvMi, anim, t);
    }));

    /*
     * "On" (15px) against "1/16" (23px).
     *
     * The first cut used LP -> Band Pass, which is 15px -> 16px: ONE pixel of
     * travel, so the clip showed a box that did not move under a caption
     * saying it resized. Reported as exactly that. The full spread across
     * fleet spellings is 15..23px and the widest is a rate division, because
     * the width follows the longest WRAPPED line rather than the string.
     */
    const enumMeta = META({ key: "sync_div", name: "Sync", type: "enum",
                            options: ["On", "1/16"] });
    addGif("motion-enum", clip(9, 18, CELL_W, RM.BOX_H, (ctx, t, anim) => {
        RM.drawKnobWidget(ctx, { x0: 0, cellW: CELL_W }, 0, 0, enumMeta,
                          t === 0 ? "0" : "1", undefined, undefined, null, null,
                          anim, t, "sync_div");
    }));

    const trigMeta = META({ key: "clear", name: "Clear", type: "enum",
                            options: ["\u2014", "Rnd!"], access: "write" });
    addGif("motion-button", clip(10, 35, CELL_W, RM.BOX_H, (ctx, t) => {
        /* buttonPhase is the renderer's own, not restated: a second copy of
         * "how long is a press" would document an animation the device does
         * not play. */
        RM.drawKnobWidget(ctx, { x0: 0, cellW: CELL_W }, 0, 0, trigMeta, "\u2014",
                          undefined, undefined, null, RM.buttonPhase([0], t, false));
    }));

    return images;
}

/* ------------------------------------------------------------------ markdown */

function markdown() {
    const img = (n) => `![${n}](${IMG_REL}/${n}.png)`;
    const gif = (n) => `![${n}](${IMG_REL}/${n}.gif)`;
    return `<!-- GENERATED by tools/param-pages/widget_sheet.mjs — do not edit by hand.
     Regenerate with \`node tools/param-pages/widget_sheet.mjs\` after changing a
     widget; tests/host/test_widget_sheet.sh fails while this file is stale.
     Keep this page SHORT and user-facing — the rationale lives in
     docs/MODULES.md and in the code. -->

# Widgets

What each control on a knob page looks like, and what it means. Every picture
here is drawn by the code the device runs.

A cell shows the control it *is* — a dial for a sweep, a word for a choice, a
button for an action — and Schwung picks that from what the module declares.
Authors do not choose a widget; they declare a \`type\`, a range and options,
and the widget follows. (Developers: the selection rules are in
[MODULES.md](MODULES.md).)

---

## Values

### Knob

${img("arc-knob")}

A continuous value. The pointer sweeps an arc from minimum to maximum. This is
most of the grid.

${img("arc-knob-modulated")}

When an LFO or other source is driving the parameter, the pointer stays on the
value **you** set and a **dot** rides the arc at the value it is being pushed
to. The dot is there even when the two agree, so a knob under modulation never
looks like one that is not.

### Number

${img("big-number")}

A small whole number — voices, an octave, a division — reads better as the
number itself than as a position on an arc. Ranges wider than about two dozen
go back to a knob, where the sweep is the useful part.

### Choice

${img("enum-square")}

A value that is a word rather than an amount. Hold the knob and click to open
the full list; the knob alone steps through it one at a time. The square sizes
itself to the word.

### Switch

${img("viz-switch")}

An on/off. Both states are drawn — the filled track is one, its inverse the
other — so you can see what the other position is without moving it.

### Action

${img("button")}

Some parameters *do* something rather than hold a value: Clear, Randomise,
Recover Loop. They draw as a push button and the label underneath names the
action. Click the jog to fire, or just turn the knob — a whole flick counts as
one press, so you cannot fire it repeatedly by accident.

### File, sample, and other editors

${img("opaque-box")}

A cell whose contents live on another screen: a sample file, a drawing canvas,
a target picker. It shows what is loaded and a **›** meaning "there is more
in here". Click to open it. **NONE** means nothing is loaded yet.

---

## Pictures

Where several knobs describe one thing, Schwung draws the thing instead of the
knobs, across the cells they occupy.

| | |
|---|---|
| ${img("viz-envelope")} | **Envelope** — attack, decay, sustain and release as the shape they make. |
| ${img("viz-filter")} | **Filter** — the response curve, from cutoff and resonance. |
| ${img("viz-lfo")} | **LFO** — the actual waveform, at its actual rate and depth. |
| ${img("viz-eq")} | **EQ** — band gains as a curve. |
| ${img("viz-waveform")} | **Waveform** — an oscillator shape drawn as the wave it names. |
| ${img("viz-fader")} | **Fader** — a level as a column. |

### Sample

${img("viz-sample")}

The file's real waveform, with the play position cut into it, loop points
bracketed, and a granular spray shown as the dotted fences it wanders between.
Clicking anywhere in the picture opens the full-screen wave editor.

With **no sample loaded there is no waveform** — the cells go back to being
ordinary controls, and the file cell reads NONE.

### Corner brackets

${img("brackets")}

Brackets mean **this knob works, and it also opens something** — a sample
position you can scrub *and* edit full-screen. A cell with no brackets and no
**›** is just a control.

Plenty of other cells open things too — every list of options does. The footer
tells you: hold a knob and it reads \`CLK OPEN\` when there is something behind
it.

---

## The frame

| | |
|---|---|
| ${img("chrome-header")} | **Header** — where you are, and which page. |
| ${img("chrome-header-held")} | Holding a knob replaces it with that parameter's full name and value. |
| ${img("chrome-bank-bar")} | **Pages** — one tick each, the current one filled. Turn the jog to move. |
| ${img("chrome-footer")} | **Hints** — what the jog, the click and Back do right here. |
| ${img("chrome-label-cell")} | **Labels** — the name at rest, the value while you hold the knob, and a \`~\` when something is modulating it. |
| ${img("chrome-list")} | **Lists** — the bar on the right shows how far through you are and how much is left. |

---

## Motion

Four widgets move when their value changes. The movement is the point: it tells
you something changed, and which way.

*Slowed 5x — on the device these take between a tenth and a third of a second.*

| | |
|---|---|
| ${gif("motion-switch")} | **Switch** — the slug snaps to its new position and the fill sweeps after it. |
| ${gif("motion-waveform")} | **Waveform** — one shape bends into the next. |
| ${gif("motion-enum")} | **Choice** — the square grows or shrinks to fit the new word. |
| ${gif("motion-button")} | **Action** — the button presses and throws a ring. Tap twice quickly and you get two rings, not one restarted. |
`;
}

/* A swatch is keyed by NAME; the motion clips carry their own ".gif"
 * extension in the key so one map holds both kinds. */
const fileFor = (name) => (name.endsWith(".gif") ? name : name + ".png");

/* ---------------------------------------------------------------------- main */

const check = process.argv.includes("--check");
const images = build();
const md = markdown();

if (check) {
    let stale = [];
    const wanted = new Set([...images.keys()].map(fileFor));
    if (!fs.existsSync(MD) || fs.readFileSync(MD, "utf8") !== md) stale.push("docs/WIDGETS.md");
    for (const [name, png] of images) {
        const p = path.join(IMG_DIR, fileFor(name));
        if (!fs.existsSync(p) || Buffer.compare(fs.readFileSync(p), Buffer.from(png)) !== 0)
            stale.push(`${IMG_REL}/${fileFor(name)}`);
    }
    /* An image the generator no longer produces. Renaming a swatch leaves the
     * old file behind, and a leftover is invisible: nothing links it, so the
     * page looks right while the repo carries a picture of a widget that may
     * no longer exist. */
    for (const f of (fs.existsSync(IMG_DIR) ? fs.readdirSync(IMG_DIR) : [])) {
        if (!wanted.has(f)) stale.push(`${IMG_REL}/${f} (orphaned)`);
    }
    if (stale.length) {
        console.error("FAIL: the widget sheet is stale — regenerate with");
        console.error("      node tools/param-pages/widget_sheet.mjs");
        for (const s of stale) console.error("      " + s);
        process.exit(1);
    }
    console.log("PASS: docs/WIDGETS.md is current (" + images.size + " swatches)");
} else {
    fs.mkdirSync(IMG_DIR, { recursive: true });
    const wanted = new Set([...images.keys()].map(fileFor));
    for (const f of fs.readdirSync(IMG_DIR)) {
        if (!wanted.has(f)) fs.unlinkSync(path.join(IMG_DIR, f));   /* renamed away */
    }
    for (const [name, png] of images) fs.writeFileSync(path.join(IMG_DIR, fileFor(name)), png);
    fs.writeFileSync(MD, md);
    console.log("wrote docs/WIDGETS.md and " + images.size + " swatches to " + IMG_REL + "/");
}
