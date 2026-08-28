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

function cellLabels(ctx, g, n, labels) {
    for (let i = 0; i < n; i++) {
        if (!labels[i]) continue;
        RM.drawLabelCell(ctx, g, i, RM.LBL0_Y - RM.ROW0_Y, labels[i], "", false, false, false);
    }
}

/* ------------------------------------------------------------- fleet counts */

/*
 * How many cells in the fleet each widget actually draws.
 *
 * The number is the point of the sheet as much as the picture is: "the enum
 * square" and "the button" are the same amount of prose and 774 against 21
 * cells of reality, and every design argument in this subsystem has turned on
 * that ratio — bracketing 135 enums, peeking on 134 two-way choices, drawing
 * 1392 params as big numbers. A sheet without the counts invites the next
 * person to weigh them equally.
 */
function fleetCounts() {
    const fx = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
    const n = { opaque: 0, button: 0, enum: 0, bignum: 0, arc: 0, brackets: 0, groupMark: 0 };
    const viz = new Map();
    for (const c of fx.modules) {
        let pages;
        try { ({ pages } = planPages({ hierarchy: c.ui_hierarchy, chainParams: c.chain_params })); }
        catch (e) { continue; }
        if (!pages) continue;
        const mi = buildMetaIndex({ hierarchy: c.ui_hierarchy, chainParams: c.chain_params });
        for (const p of pages) {
            if (p.kind !== PAGE_KNOBS) continue;
            let groups = [];
            try { ({ groups } = resolveViz({ keys: p.keys || [], metaIndex: mi })); } catch (e) {}
            const cov = new Array(8).fill(false);
            for (const gr of (groups || [])) {
                viz.set(gr.kind, (viz.get(gr.kind) || 0) + 1);
                for (let s = gr.slotStart; s < gr.slotStart + gr.slotSpan; s++) cov[s] = true;
                let door = false;
                for (let s = gr.slotStart; s < gr.slotStart + gr.slotSpan && !door; s++) {
                    const k = (p.keys || [])[s];
                    if (k) door = opensOnClick(mi.getOrGuess(k));
                }
                if (door) n.groupMark++;
            }
            for (let s = 0; s < 8; s++) {
                const k = (p.keys || [])[s];
                if (!k) continue;
                const m = mi.getOrGuess(k);
                if (alsoOpens(m) && !cov[s]) n.brackets++;
                if (cov[s]) continue;
                if (m.kind === KIND_OPAQUE) n.opaque++;
                else if (m.writeOnly) n.button++;
                else if (m.kind === "enum") n.enum++;
                else if (RM.shouldDrawBigNumber(m)) n.bignum++;
                else n.arc++;
            }
        }
    }
    return { cells: n, viz };
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
    const { cells, viz } = fleetCounts();
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

    return { images, cells, viz };
}

/* ------------------------------------------------------------------ markdown */

function markdown(cells, viz) {
    const img = (n) => `![${n}](${IMG_REL}/${n}.png)`;
    const v = (k) => viz.get(k) || 0;
    return `<!-- GENERATED by tools/param-pages/widget_sheet.mjs — do not edit by hand.
     Every swatch is drawn by the same function the device calls; regenerate
     with \`node tools/param-pages/widget_sheet.mjs\` after changing a widget.
     tests/host/test_widget_sheet.sh fails if this file is stale. -->

# Widget contact sheet

Every widget the knob grid draws, the rule that selects it, and how many cells
in the fleet it actually accounts for.

**The counts are load-bearing.** Almost every design argument in this subsystem
has turned on the ratio rather than on the widget: bracketing 135 enums would
erase what a mark means; drawing 1392 params as big numbers made sweeps
unreadable; 134 two-way *choices* need the option list that 212 booleans do
not. A sheet without the counts invites weighing them equally.

Swatches are 4x. Cell geometry, band height and label placement come from the
renderer's own constants, so what you see is the layout the device has.

---

## How a cell picks its widget

\`drawKnobWidget\` (\`render_page_movy.mjs\`) is a single ordered dispatch. The
order is the specification — each branch owns its cell outright:

| # | test | widget | fleet cells |
|---|---|---|---|
| 1 | \`kind === KIND_OPAQUE\` | opaque box | ${cells.opaque} |
| 2 | \`writeOnly\` (a trigger) | button | ${cells.button} |
| 3 | \`kind === KIND_ENUM\` | enum square | ${cells.enum} |
| 4 | \`shouldDrawBigNumber\` | big number | ${cells.bignum} |
| 5 | *(otherwise)* | arc knob | ${cells.arc} |

A **viz graphic** pre-empts all of it: a resolved group covers its cells and
draws one picture across them, and the per-cell widget is skipped entirely.

---

## Cell widgets

### Arc knob

${img("arc-knob")}

The default, and the majority of the grid. A continuous value as a pointer on
an arc sweeping ${"`ARC_SWEEP_DEG`"} degrees.

- **Selected when** nothing else claims the cell — a \`float\`, or an \`int\`
  whose range is too wide to read as a number.
- **Fleet:** ${cells.arc} cells.

${img("arc-knob-modulated")}

Under modulation the pointer stays on the **base** you dialled in and a **dot**
rides the arc at the live value. The dot is drawn even when the two coincide:
suppressing it there made a modulated knob pixel-identical to an unmodulated
one, so the reading became "there is no LFO" rather than "the LFO is at its
base".

### Big number

${img("big-number")}

An \`int\` whose declared range spans ≤24 (≤48 if bipolar) draws its value in
the 6x7 device font, with a sign only where the range has a negative side.

- **Selected when** \`shouldDrawBigNumber(meta)\`.
- **Fleet:** ${cells.bignum} cells.
- **The span bound is load-bearing.** An earlier version bounded at 128 and
  drew 1392 params big, including \`volume [0..100]\` and \`tune [0..127]\` —
  sweeps, where an arc is the honest picture.
- It is **not** framed. The box is the *enum* affordance; a small int has no
  list behind it, so a frame advertised a door that does not open.

### Enum square

${img("enum-square")}

The one cell whose value is a **word**: two lines of the 5x3 face inside a
notched square.

- **Selected when** \`kind === KIND_ENUM\`.
- **Fleet:** ${cells.enum} cells — by far the largest population after the arc.
- \`short_options\` is consulted **here only**. The full \`options\` still show
  in the held-knob header, which is where a value has room to be read.
- Every enum declaring options is **divable** (click opens a scrolling list),
  and none of them is marked. See *Marks* below.

### Button

${img("button")}

A trigger is an action, not a value, so the cell shows a push button: at rest,
pressed, and mid-burst.

- **Selected when** \`writeOnly\` — i.e. \`access: "write"\`. There is no
  \`trigger\` type; it is an ordinary enum on the write end of the access axis.
- **Fleet:** ${cells.button} cells.
- The cap carries **no text**, deliberately. The module reports a constant idle
  spelling and the fleet proves it is not readable — euclidrum's is an em-dash
  the 5x7 atlas cannot draw at all, which rendered as a blank square: a control
  that looked broken while working perfectly. The cell's own label names the
  action.
- Fired by a jog click **or a knob detent**, either direction, at most once per
  \`TRIGGER_KNOB_COOLDOWN_MS\`.

### Opaque box

${img("opaque-box")}

A cell a knob cannot turn: the value lives behind a door. A notched frame with
its right edge cut away and a **chevron** in the gap, value set left.

- **Selected when** \`kind === KIND_OPAQUE\` — \`filepath\`/\`file\`,
  \`string\`, \`canvas\`, a **non-ranged** \`wav_position\`, and the two picker
  types.
- **Fleet:** ${cells.opaque} cells.
- Three states, and the middle one is a contract: \`""\` reads **NONE** (there
  is no file), \`null\` reads **--** (the read has not answered). Collapsing
  those made an empty slot look identical to one whose name had not arrived.
- **The chevron is not a divable mark — it is the widget.** See *Marks*.

---

## Marks

${img("brackets")}

**Corner brackets** are an annotation on a *working* widget: the knob turns it
**and** it opens something. In practice that is one thing — a **ranged**
\`wav_position\`, a number you can scrub that also has a waveform editor behind
it.

- **Drawn when** \`alsoOpens(meta)\` — \`opaque_type && kind !== KIND_OPAQUE &&
  divable\`.
- **Fleet:** ${cells.brackets} cells, plus ${cells.groupMark} viz groups that
  wear one across their whole span.

**The two marks do not both mean "divable".** Measured over the fleet, 99% of
divable cells wear **no** mark at all, because almost every divable cell is an
enum. Divability is announced by the **footer** — hold the knob and it reads
\`CLK OPEN\`. What the marks split is narrower, with zero overlap:

| mark | knob turns it? | means |
|---|---|---|
| corner brackets | always | the knob works, **and** it opens something |
| chevron box | never | there is no knob here — only a door |

Unifying them does not work in either direction: brackets on an opaque cell put
two frames on one rect and still leave the enums unmarked, and a chevron on
every divable cell puts one on ${cells.enum} enums.

---

## Viz graphics

A graphic replaces the widgets of the cells it covers. Detection runs in a
fixed priority order and each detector gets first refusal on unclaimed keys.
A graphic must be **contiguous and within one row** — it cannot span the label
band between row 0 and row 1.

### Envelope — ${v("envelope")} groups

${img("viz-envelope")}

The ADSR as a graph, from adjacent attack/decay/sustain/release knobs. Roles
are optional: a page with no attack still draws the rest.

### Filter — ${v("filter")} groups

${img("viz-filter")}

The response curve, from cutoff + resonance. Mode/slope join it when they are
adjacent and are dropped when they are not — an optional role must never delete
a corroborated pair.

### LFO — ${v("lfo")} groups

${img("viz-lfo")}

The waveform at its actual depth and phase. Requires the roles to share a stem,
so a chorus LFO's shape beside a delay's rate does not read as one group.

### EQ — ${v("eq")} groups

${img("viz-eq")}

Band gains as a curve. Detected last: its false positives are the hardest to
spot, and a genuine band gain must be bipolar and roughly symmetric.

### Waveform — ${v("waveform")} groups

${img("viz-waveform")}

A single oscillator shape enum drawn as the wave it names. Morphs between
shapes when the value changes.

### Fader — ${v("fader")} groups

${img("viz-fader")}

A level as a column rather than an arc.

### Switch — ${v("switch")} groups

${img("viz-switch")}

A boolean, however its author spelled it: \`enum\` with Off/On options, **or**
\`int\` with min 0 and max 1 — 61 params across 11 modules use the latter and
drew as a number, which is the one widget that tells you nothing.

It draws **both** of its states (the track is one, its inversion the other),
which is why it never raises the option-list peek: a full-screen Off/On says
what the cell already says.

### Sample — ${v("sample")} groups

${img("viz-sample")}

The file's real peaks, with the play cursor cut into the body as its
complement, loop bounds bracketed, and the granular spray as two dotted fences
that wrap.

- The **file does not claim a cell.** It is what the waveform is drawn *from*,
  never *on*: it dives to the file browser while every other member dives to
  the wave editor, so including it drew one continuous picture across a
  boundary where the behaviour changes.
- **No file, no graphic.** An empty sample is suppressed entirely and the cells
  fall back to their own widgets, rather than drawing a bracketed rectangle
  containing nothing.
- There is **no representative shape**. A read that did not answer must never
  become a picture — the synthetic waveform that used to fill in for missing
  peaks drew a sample that was never loaded.
`;
}

/* ---------------------------------------------------------------------- main */

const check = process.argv.includes("--check");
const { images, cells, viz } = build();
const md = markdown(cells, viz);

if (check) {
    let stale = [];
    if (!fs.existsSync(MD) || fs.readFileSync(MD, "utf8") !== md) stale.push("docs/WIDGETS.md");
    for (const [name, png] of images) {
        const p = path.join(IMG_DIR, name + ".png");
        if (!fs.existsSync(p) || Buffer.compare(fs.readFileSync(p), Buffer.from(png)) !== 0)
            stale.push(`${IMG_REL}/${name}.png`);
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
    for (const [name, png] of images) fs.writeFileSync(path.join(IMG_DIR, name + ".png"), png);
    fs.writeFileSync(MD, md);
    console.log("wrote docs/WIDGETS.md and " + images.size + " swatches to " + IMG_REL + "/");
}
