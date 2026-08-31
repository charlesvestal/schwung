#!/usr/bin/env node
/**
 * widget_sheet.mjs — generate the widget reference: the picture of every
 * widget the knob grid ships, beside the rule that selects it.
 *
 * GENERATED, not written. A hand-drawn sheet is stale the first time a widget
 * moves a pixel, and this codebase has already learned that reviewing widgets
 * from their code rather than their render lets real defects through. Every
 * swatch here comes out of the SAME functions the device calls, so the sheet
 * cannot describe a widget the grid does not draw.
 *
 * Deliberately NOT the SCH-50 catalog (tools/param-pages/catalog.mjs). That
 * renders ten ALTERNATIVES per widget for choosing between, nine of which were
 * rejected, and its output is gitignored. This renders the one that WON.
 *
 * TWO AUDIENCES, ONE SOURCE. The full reference goes into docs/MODULES.md
 * between markers, where a module author is already reading and where the
 * density belongs; a 14-image subset goes into the user manual in the sibling
 * repo. There is no third document: a standalone WIDGETS.md ended up being a
 * second user-facing page in the same voice as the manual's, which is one
 * document too many, and it kept the pictures away from the rules.
 *
 *   node tools/param-pages/widget_sheet.mjs          write the section + images
 *   node tools/param-pages/widget_sheet.mjs --check  fail if it would change
 *   node tools/param-pages/widget_sheet.mjs --manual sync the manual's section too
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
import zlib from "node:zlib";

/*
 * COMPARE THE PICTURE, NOT THE FILE.
 *
 * harness.mjs writes its PNGs with zlib.deflateSync, and zlib's output is not
 * byte-stable across versions — the same framebuffer compresses differently on
 * a different node. A byte comparison therefore reports "stale" for images that
 * are pixel-identical, and it does it ONLY somewhere other than the machine
 * that generated them: green locally, red in CI, with a message telling you to
 * regenerate files that are already correct. Regenerating on the CI runner
 * would just move the failure to the next machine.
 *
 * So the diff is taken on the INFLATED IDAT — the raw scanlines, which are what
 * the drawing code actually produced. Dimension changes still show up, because
 * a different width or height changes the raw length.
 *
 * GIFs are compared byte-for-byte: gif.mjs is our own LZW encoder with no
 * library underneath it, so its output is deterministic by construction.
 */
function pngPixels(buf) {
    if (buf.length < 8 || buf[0] !== 0x89 || buf[1] !== 0x50) return null;
    const idat = [];
    let off = 8;
    while (off + 8 <= buf.length) {
        const len = buf.readUInt32BE(off);
        const type = buf.toString("latin1", off + 4, off + 8);
        if (type === "IDAT") idat.push(buf.subarray(off + 8, off + 8 + len));
        off += 12 + len;                     /* len + type + data + crc */
        if (type === "IEND") break;
    }
    if (!idat.length) return null;
    try { return zlib.inflateSync(Buffer.concat(idat)); } catch (e) { return null; }
}

/** Same drawing? Falls back to a byte compare when either side is not a PNG we
 *  can decode — a corrupt or truncated file must read as CHANGED, never as
 *  equal, or the check would pass by failing to look. */
function sameImage(a, b) {
    if (Buffer.compare(a, b) === 0) return true;
    const pa = pngPixels(a), pb = pngPixels(b);
    if (!pa || !pb) return false;
    return Buffer.compare(pa, pb) === 0;
}

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const MD = path.join(ROOT, "docs", "MODULES.md");
const IMG_DIR = path.join(ROOT, "docs", "images", "widgets");
const IMG_REL = "images/widgets";
const MD_BEGIN = "<!-- BEGIN generated widgets";
const MD_END = "<!-- END generated widgets -->";

/* A swatch is keyed by NAME; the motion clips carry their own ".gif" in the
 * key so one map holds both kinds. Declared up here because the markdown, the
 * manual writer and the image writer all want it. */
function fileFor(name) { return name.endsWith(".gif") ? name : name + ".png"; }
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

    /*
     * READOUTS, each beside its editable twin.
     *
     * PAIRED on purpose: the frame's whole job is to be different from the
     * cell next to it, and a strip of three framed cells says nothing about
     * whether it is. Three widgets because those are the three a readout is in
     * the fleet — an enum square (keydetect, gesture-test), an arc knob and a
     * big number (4K EQ peaks and its clip flag).
     */
    add("readout", strip(6, (ctx, g) => {
        /* A FULL-WIDTH value, deliberately: it is keydetect's real case (musical
         * keys) and it is the width an outer frame could not mark. A narrow box
         * would show the treatment at its easiest. */
        const eR = META({ key: "detected_key", name: "Key", type: "enum", access: "read",
                          options: ["G Maj", "A Min", "C Maj"] });
        const eW = META({ key: "mode", name: "Mode", type: "enum",
                          options: ["G Maj", "A Min", "C Maj"] });
        const kR = META({ key: "in_peak_l", name: "In L", type: "float", access: "read",
                          min: 0, max: 1, step: 0.01 });
        const kW = META({ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 });
        const nR = META({ key: "clip", name: "Clip", type: "int", min: 0, max: 9, access: "read" });
        const nW = META({ key: "voices", name: "Voices", type: "int", min: 0, max: 9 });
        const cells = [[eR, "0"], [eW, "0"], [kR, "0.66"], [kW, "0.66"], [nR, "3"], [nW, "3"]];
        cells.forEach(([m, v], i) => {
            RM.drawKnobWidget(ctx, g, i, 0, m, v, undefined, undefined, null, null);
            /* drawKnobRow's own call, not a re-statement of when a mark is
             * added: the enum square dots its own stroke inside drawKnobWidget
             * and must NOT also get an outer frame, and the sheet is the last
             * place that rule should exist twice. */
            if (m.readOnly) RM.drawReadoutMark(ctx, g, i, 0, m);
        });
        cellLabels(ctx, g, 6, ["READ", "EDIT", "READ", "EDIT", "READ", "EDIT"]);
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

    /*
     * NO motion-switch CLIP. The switch had a 160ms fill and it is gone —
     * removed for reading as distracting on hardware, since a switch is the
     * control you flip most often and least deliberately. It toggles between
     * two settled frames now, and a two-frame gif of a cut is not a motion
     * study, it is a flicker. The two states are shown as stills under
     * *Values*, which is where they belong.
     */

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
    return `${MD_BEGIN}. Written by tools/param-pages/widget_sheet.mjs
     from the same code the device draws with. Do not hand-edit between these
     markers; regenerate instead. -->

#### Which widget a cell draws

Authors do not pick a widget. Declare \`type\`, a range and \`options\`; the
widget follows. \`drawKnobWidget\` (\`render_page_movy.mjs\`) is one ordered
dispatch, and the order is the specification — each branch owns its cell
outright:

| # | test | widget | |
|---|---|---|---|
| 1 | \`kind === KIND_OPAQUE\` | opaque box | ${img("opaque-box")} |
| 2 | \`writeOnly\` (a trigger) | button | ${img("button")} |
| 3 | \`kind === KIND_ENUM\` | enum square | ${img("enum-square")} |
| 4 | \`shouldDrawBigNumber\` | big number | ${img("big-number")} |
| 5 | *(otherwise)* | arc knob | ${img("arc-knob")} |

A **viz graphic** pre-empts all of it: a resolved group covers its cells and
draws one picture across them, and the per-cell widget is skipped.

Notes worth having before you declare something:

- **The opaque box shows three states** — a value, \`NONE\` for \`""\`, and
  \`--\` for a read that has not answered. Do not collapse the last two; an
  empty slot and a slow one are different facts.
- **A trigger is \`access: "write"\` on an ordinary enum**, not a type. The cap
  carries no text: the module reports a constant idle spelling and the fleet
  proves it is not readable (euclidrum's is an em-dash the 5x7 atlas cannot
  draw, which rendered as a blank square). The cell's label names the action.
  Fired by a jog click *or* a knob detent, either direction — the footer says
  \`CLK FIRE\` and \`KNB FIRE\`, one verb because it is one action. The knob
  path LATCHES: a whole spin is one fire, and the latch clears on RELEASE, or
  after \`TRIGGER_KNOB_GESTURE_GAP_MS\` of stillness if the cap sensor never
  registered. A rate limit was tried first and still fired eight times across
  a two-second spin.
- **\`short_options\` is for the enum square only.** The held-knob header keeps
  the full spelling, which is where a value has room to be read.
- **The big-number span bound is load-bearing.** An earlier version bounded at
  128 and drew 1392 params big, including \`volume [0..100]\` — a sweep, where
  an arc is the honest picture.
- **A modulated knob keeps the pointer on the base and adds a dot** at the live
  value. The dot is drawn even when they coincide: suppressing it there made a
  modulated knob pixel-identical to an unmodulated one.

  ${img("arc-knob-modulated")}

#### Viz graphics

Detection runs in a fixed priority order and each detector gets first refusal
on unclaimed keys. A graphic must be **contiguous and within one row** — it
cannot span the label band between row 0 and row 1.

| graphic | from | |
|---|---|---|
| envelope | adjacent attack/decay/sustain/release | ${img("viz-envelope")} |
| filter | cutoff + resonance (mode/slope optional) | ${img("viz-filter")} |
| lfo | shape + rate + depth, sharing a stem | ${img("viz-lfo")} |
| eq | bipolar, roughly symmetric band gains | ${img("viz-eq")} |
| waveform | one oscillator-shape enum | ${img("viz-waveform")} |
| fader | a level | ${img("viz-fader")} |
| switch | \`enum\` Off/On **or** \`int\` 0..1 | ${img("viz-switch")} |
| sample | a file plus positions within it | ${img("viz-sample")} |

- **An optional role is dropped when it does not fit.** \`detectFilter\` used to
  require every role it found to be contiguous, so a Mode knob parked at the far
  end of the page deleted the corroborated cutoff/resonance pair.
- **The switch takes \`int\` 0..1 as well as an Off/On enum** — 61 params across
  11 modules spell it that way and drew as a number, which is the one widget
  that tells you nothing. It draws both states, which is why it never raises the
  option-list peek.
- **The sample's file does not claim a cell.** It is \`roles.value\` — the
  waveform is drawn *from* it, never *on* it — because it dives to the file
  browser while every other member dives to the wave editor. With no file the
  graphic is not drawn at all and the cells fall back to their own widgets.
- **There is no representative shape.** A read that did not answer must never
  become a picture; the synthetic waveform that used to fill in for missing
  peaks drew a sample that was never loaded.

#### The marks

${img("brackets")}

Corner brackets mean **the knob works, and it also opens something** —
\`alsoOpens(meta)\`, which in practice is a ranged \`wav_position\`. A viz group
wears one across its whole span when any covered cell \`opensOnClick\`.

See *Divability, and the two cell marks* below for why the brackets and the
chevron are not two spellings of one idea.

${img("readout")}

A **dotted frame** means \`access: "read"\` — telemetry you can look at and not
change. Each pair above is the same declaration with and without it.

The input layer has always honoured \`access\`: turning a readout shows the
reading and writes nothing, a click opens no picker, and \`isDivable\` /
\`isTurnable\` exclude it. The *drawing* did not, so a readout was
pixel-identical to a control — reported from the device as a knob that "does
not seem to do anything", which was a correct reading of the picture.

- **The rule is *a readout is dotted*; where the stroke lives is the widget's
  business.** A dial and a big number have none, so a frame is added around the
  cell; the enum square has one, so it dots that. One dotted rectangle per cell,
  never two. Either way the value does not move.
- **The square dots its own stroke because an outer frame did not work there.**
  Measured against an identical editable twin, an outer frame differed by 17
  pixels at full width against 27 at the narrow one — the wider the value, the
  more of the mark the square absorbed — and side by side the two cells were
  indistinguishable. It failed exactly where the feature is for: \`keydetect\`'s
  values are musical keys, always full width. Dotting the stroke inverts the
  gradient, to 39 against 26.
- **Not an inverted slab** — inversion already means *a finger is on this knob*
  in the label band and *this is the selection* in a list. **Not corner
  brackets** — those mean the opposite claim, that the knob works *and* opens
  something.
- **An opaque cell is not marked at all.** Its own notched frame is on the same
  rect, so an outer frame's dots would show only in the chevron's cut, and
  dotting that frame would blunt the one widget that says which direction its
  door goes.
- **A readout inside a viz graphic is not framed.** No fleet module has one;
  if one appears, mark the span once, the way the door bracket does.

#### Chrome

| | |
|---|---|
| ${img("chrome-header")} | **Header** — where you are, and which page. The right side is a measured share against a \`HEADER_MIN_LEFT\` floor, not a fixed column. |
| ${img("chrome-header-held")} | Holding a knob inverts the band and shows that parameter's full name and value. One clear row above and below is load-bearing *only* when inverted. |
| ${img("chrome-bank-bar")} | **Bank bar** — one tick per page. It owns row 7, which is why a menu page cannot start its list at y=9 the way the enum picker does. |
| ${img("chrome-footer")} | **Footer** — hint pairs, key inverted into a pill. Fit-aware: three pairs need every word ≤4 chars, and a longer one drops a pair rather than overflowing. Hints come from the caller, never the renderer. |
| ${img("chrome-label-cell")} | **Label cell** — name at rest, value while held, \`~\` while modulated. Budgeted in *characters*, not pixels. |
| ${img("chrome-list")} | **List** — one dotted column with a solid thumb, in \`drawMenuList\`, so every list in the tree has it. 2px thumb floor; the track covers the rows, not the rect; the selection highlight stops short of the gutter or it draws a phantom second thumb. |

#### Motion

Time is passed **in**, never read — there is no \`Date.now()\` in the renderer,
which is what lets a page be filmed deterministically.

**The store must be passed from the controller.** Every widget guards on
\`anim && typeof nowMs === "number"\`, so an undefined store draws the settled
frame forever — silently, and identically to a correct render of a value that
is not moving. \`createAnimState\` was written, exported, unit-tested and never
*called*; every animation below shipped inert for months.

*Slowed 5x — sampled at real time, so the curve is the device's.*

| | |
|---|---|
| ${gif("motion-waveform")} | **Waveform**, 100ms — one shape bends into the next. The enum peek is instant and covers this while it plays. |
| ${gif("motion-enum")} | **Enum square**, 120ms — the frame travels, the glyphs swap outright. Text is served short while the box is narrow and completes as it arrives. |
| ${gif("motion-button")} | **Trigger**, 300ms — press then rings. Bursts append rather than replace, so a double-tap throws two. |

${MD_END}`;
}


/*
 * Replace the generated block inside a hand-written document.
 *
 * MODULES.md is authored prose with one generated section in it, so this
 * cannot overwrite the file the way a standalone page could. Throwing on a
 * missing marker rather than appending: silently adding a second copy at the
 * end of a 2000-line document is the kind of thing nobody notices for a while.
 */
function spliced(doc, block) {
    const i = doc.indexOf(MD_BEGIN), j = doc.indexOf(MD_END);
    if (i < 0 || j < 0) throw new Error("the widget markers are missing from docs/MODULES.md");
    return doc.slice(0, i) + block + doc.slice(j + MD_END.length);
}

/* ------------------------------------------------------------------ manual */

/*
 * The compact version, written into the user manual in the SIBLING REPO.
 *
 * The manual already tells the reader that each cell is "drawn as the control
 * it actually is — a dial, a fader, an envelope, a switch". That sentence is
 * describing pictures, so it should be showing them.
 *
 * EVERY widget, plus the chrome that carries the gestures. This started as a
 * fourteen-image subset on the reasoning that manual.html is prose and
 * twenty-five pictures would change what the page is. That was the wrong
 * trade: a reader looking for "what does this cell mean" wants the one they
 * are looking at, and a partial set sends them to MODULES.md -- a document
 * written for module authors -- to find it. The page is a reference now, and
 * it should be complete.
 *
 * Between MARKERS, and generated, because the alternative is a hand-copied
 * block in another repo that silently stops matching the device — which is the
 * failure the whole generator exists to avoid, made worse by being one repo
 * further away.
 *
 * OPT-IN (--manual) and skipped when the sibling repo is absent: the host test
 * runs on machines that only have schwung checked out, and a missing
 * ../schwung-catalog-site must not be a failure.
 */
/*
 * Found by walking UP, not by "../schwung-catalog-site".
 *
 * That relative path is right for a normal checkout and wrong inside a git
 * worktree, where ROOT is .claude/worktrees/<name> and the parent is the
 * worktree directory. The failure is silent and misleading: the tool reports
 * the sibling repo as "not checked out" while it sits three levels up.
 */
function findManualRoot() {
    let dir = ROOT;
    for (let i = 0; i < 6; i++) {
        const c = path.join(path.dirname(dir), "schwung-catalog-site");
        if (fs.existsSync(path.join(c, "manual.html"))) return c;
        const up = path.dirname(dir);
        if (up === dir) break;
        dir = up;
    }
    return path.join(path.dirname(ROOT), "schwung-catalog-site");   /* for the message */
}
const MANUAL_ROOT = findManualRoot();
const MANUAL_HTML = path.join(MANUAL_ROOT, "manual.html");
const MANUAL_IMG = path.join(MANUAL_ROOT, "images", "widgets");
const BEGIN = "<!-- BEGIN generated widgets";
const END = "<!-- END generated widgets -->";

const MANUAL_PICKS = {
    values: [
        ["arc-knob", "<strong>Knob</strong> — a continuous value, swept from minimum to maximum."],
        ["big-number", "<strong>Number</strong> — a small whole number reads better as itself."],
        ["enum-square", "<strong>Choice</strong> — a value that is a word. Click to open the list."],
        ["viz-switch", "<strong>Switch</strong> — on/off, with both states drawn."],
        ["button", "<strong>Action</strong> — does something rather than holding a value."],
        ["opaque-box", "<strong>Editor</strong> — contents live on another screen. <em>NONE</em> means empty."],
    ],
    pictures: [
        ["viz-envelope", "<strong>Envelope</strong> — attack, decay, sustain and release as the shape they make."],
        ["viz-filter", "<strong>Filter</strong> — the response curve, from cutoff and resonance."],
        ["viz-lfo", "<strong>LFO</strong> — the actual waveform, at its rate and depth."],
        ["viz-sample", "<strong>Sample</strong> — the file's real waveform, play position and loop points."],
    ],
    more: [
        ["viz-fader", "<strong>Fader</strong> — a level, drawn as the travel it has."],
        ["viz-eq", "<strong>EQ band</strong> — gain, frequency and Q as one curve."],
        ["viz-waveform", "<strong>Waveform</strong> — an oscillator's shape, drawn as itself."],
        ["arc-knob-modulated", "<strong>Modulated</strong> — an LFO on a knob shows the range it sweeps."],
    ],
    gestures: [
        ["chrome-header-held", "<strong>Hold a knob</strong> and the header names it and shows its value. Let go and nothing has changed — looking is free."],
        ["brackets", "<strong>Corner brackets mean a door.</strong> Hold that knob and jog-click to open what is behind it; a plain cell has nothing to open."],
        ["readout", "<strong>A dotted frame means you can only look.</strong> It is a reading the module is reporting — turning the knob shows it, and changes nothing."],
        ["chrome-bank-bar", "<strong>The bar counts the pages.</strong> The filled block is where you are; turn the jog to move along it."],
        ["chrome-list", "<strong>Click a choice</strong> and it opens as a list — the jog scrolls, the click picks."],
    ],
    chrome: [
        ["chrome-header", "<strong>Header</strong> — the slot and module you are in, and the page name."],
        ["chrome-footer", "<strong>Footer</strong> — what the jog, shift and click do on this page."],
        ["chrome-label-cell", "<strong>Cell</strong> — the control, with its name beneath it."],
    ],
    motion: [
        ["motion-waveform.gif", "One shape bends into the next."],
        ["motion-enum.gif", "The square grows to fit the new word."],
        ["motion-button.gif", "Presses, and throws a ring."],
    ],
};


/*
 * The pixel width of an encoded image, read back from its own bytes.
 *
 * Needed because the swatches are NOT all the same size — a four-cell knob row
 * is 512px and a one-cell switch is 128px — and `width: 100%` in a grid blew
 * the switch up to the width of the knob row, so a single cell rendered four
 * times the size of a cell. Sizing each figure from its natural width keeps
 * the scale honest across the whole section: one cell looks like one cell.
 *
 * Read rather than tracked, so nothing has to be threaded through the image
 * map: PNG carries width at byte 16 of IHDR, GIF at byte 6 of the screen
 * descriptor.
 */
function naturalWidth(buf) {
    const b = Buffer.from(buf);
    if (b[0] === 0x89 && b[1] === 0x50) return b.readUInt32BE(16);   /* PNG */
    if (b[0] === 0x47 && b[1] === 0x49) return b.readUInt16LE(6);    /* GIF */
    throw new Error("unrecognised image");
}

function manualFragment(images) {
    /* Real ALT text, derived from the caption with its markup stripped.
     * alt="" says "decorative", and these are the opposite -- the page is
     * telling you what a control looks like, so the picture IS the content.
     * Derived rather than written twice so the two cannot disagree. */
    const alt = (cap) => cap.replace(/<[^>]+>/g, "").replace(/\s+/g, " ").trim()
                            .replace(/"/g, "&quot;");
    const fig = ([name, cap]) => {
        /* Half of natural: the swatches are drawn at 4x device pixels, so
         * this shows them at 2x — large enough to read a 5x3 glyph, small
         * enough that a four-cell row still fits a phone column. */
        const w = Math.round(naturalWidth(
            images.get(name.endsWith(".gif") ? name : name)) / 2);
        return (
        `                        <figure class="wg-fig">\n` +
        `                            <img src="images/widgets/${fileFor(name)}" alt="${alt(cap)}" width="${w}">\n` +
        `                            <figcaption>${cap}</figcaption>\n` +
        `                        </figure>`);
    };
    const grid = (items) =>
        `                    <div class="wg-grid">\n${items.map(fig).join("\n")}\n                    </div>`;

    /* Scoped styles rather than an edit to style.css: this block is generated
     * and the stylesheet is not, so keeping them in one file keeps the
     * regeneration honest. `image-rendering: pixelated` is not optional — these
     * are 1-bit device frames scaled 4x, and smoothing turns a crisp pixel
     * widget into a grey smear. */
    return `${BEGIN}. Written by schwung's tools/param-pages/widget_sheet.mjs
                         --manual, from the same code the device draws with.
                         Do not hand-edit between these markers. -->
                    <style>
                    .wg-grid { display: grid; gap: 1rem; margin: 1.25rem 0;
                               grid-template-columns: repeat(auto-fit, minmax(160px, max-content));
                               align-items: start; }
                    .wg-fig { margin: 0; }
                    .wg-fig img { max-width: 100%; height: auto; display: block;
                                  image-rendering: pixelated;
                                  border: 1px solid var(--border); border-radius: 3px;
                                  background: #000; }
                    .wg-fig figcaption { margin-top: .4rem; font-size: .85rem;
                                         color: var(--text-muted); line-height: 1.35; }
                    </style>

                    <p>You never choose these — a module declares what a parameter
                    <em>is</em>, and the right control follows.</p>
${grid(MANUAL_PICKS.values)}

                    <p>Where several knobs describe one thing, Schwung draws the thing
                    instead of the knobs, across the cells they occupy.</p>
${grid(MANUAL_PICKS.pictures)}
${grid(MANUAL_PICKS.more)}

                    <p>The gestures are the same on every page of every module.</p>
${grid(MANUAL_PICKS.gestures)}

                    <p>And the furniture around them.</p>
${grid(MANUAL_PICKS.chrome)}

                    <p>Four of them move when their value changes, so a change is visible
                    rather than just present. <em>(Slowed 5&times; here — on the device
                    these take a fraction of a second.)</em></p>
${grid(MANUAL_PICKS.motion)}

                    <p class="note">Module authors: the full reference — every widget,
                    the page chrome, and the rule that selects each one — is in
                    <a href="https://github.com/charlesvestal/schwung/blob/main/docs/MODULES.md#which-widget-a-cell-draws"
                       target="_blank" rel="noopener">MODULES.md</a>.</p>
                    ${END}`;
}

function syncManual(images, check) {
    if (!fs.existsSync(MANUAL_HTML)) {
        console.log("skip: " + MANUAL_HTML + " not found (sibling repo not checked out)");
        return true;
    }
    const html = fs.readFileSync(MANUAL_HTML, "utf8");
    const i = html.indexOf(BEGIN), j = html.indexOf(END);
    if (i < 0 || j < 0) {
        console.error("FAIL: the widget markers are missing from manual.html");
        return false;
    }
    const next = html.slice(0, i) + manualFragment(images) + html.slice(j + END.length);

    /* EVERY group, flattened -- not a second hand-kept list. This named three
     * of the groups explicitly, so adding a fourth wrote figures into the
     * manual whose images were never copied: eleven broken <img> in the
     * sibling repo, and nothing here to say so. */
    const names = Object.values(MANUAL_PICKS).flat().map(([n]) => fileFor(n));
    let stale = [];
    if (next !== html) stale.push("manual.html");
    for (const n of names) {
        const dst = path.join(MANUAL_IMG, n);
        const src = images.get(n.endsWith(".gif") ? n : n.replace(/\.png$/, ""));
        if (!src) { console.error("FAIL: manual wants " + n + ", which the sheet does not produce"); return false; }
        if (!fs.existsSync(dst) || !sameImage(fs.readFileSync(dst), Buffer.from(src)))
            stale.push("images/widgets/" + n);
    }

    if (check) {
        if (stale.length) {
            console.error("FAIL: the manual's widget section is stale — regenerate with");
            console.error("      node tools/param-pages/widget_sheet.mjs --manual");
            for (const t of stale) console.error("      " + t);
            return false;
        }
        console.log("PASS: the manual's widget section is current");
        return true;
    }

    fs.mkdirSync(MANUAL_IMG, { recursive: true });
    for (const n of names) {
        const src = images.get(n.endsWith(".gif") ? n : n.replace(/\.png$/, ""));
        fs.writeFileSync(path.join(MANUAL_IMG, n), src);
    }
    fs.writeFileSync(MANUAL_HTML, next);
    console.log("wrote the manual's widget section and " + names.length + " images");
    return true;
}

/* ---------------------------------------------------------------------- main */

const check = process.argv.includes("--check");
const doManual = process.argv.includes("--manual");
const images = build();
const md = markdown();

if (check) {
    let stale = [];
    const wanted = new Set([...images.keys()].map(fileFor));
    const cur = fs.existsSync(MD) ? fs.readFileSync(MD, "utf8") : "";
    if (spliced(cur, md) !== cur) stale.push("docs/MODULES.md");
    for (const [name, png] of images) {
        const p = path.join(IMG_DIR, fileFor(name));
        if (!fs.existsSync(p) || !sameImage(fs.readFileSync(p), Buffer.from(png)))
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
    console.log("PASS: docs/MODULES.md's widget section is current (" + images.size + " swatches)");
    if (doManual && !syncManual(images, true)) process.exit(1);
} else {
    fs.mkdirSync(IMG_DIR, { recursive: true });
    const wanted = new Set([...images.keys()].map(fileFor));
    for (const f of fs.readdirSync(IMG_DIR)) {
        if (!wanted.has(f)) fs.unlinkSync(path.join(IMG_DIR, f));   /* renamed away */
    }
    for (const [name, png] of images) fs.writeFileSync(path.join(IMG_DIR, fileFor(name)), png);
    fs.writeFileSync(MD, spliced(fs.readFileSync(MD, "utf8"), md));
    console.log("wrote docs/MODULES.md's widget section and " + images.size + " swatches to docs/" + IMG_REL + "/");
    if (doManual && !syncManual(images, false)) process.exit(1);
}
