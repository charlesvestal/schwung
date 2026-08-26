#!/usr/bin/env node
/**
 * catalog.mjs — render the SCH-50 option catalog to PNG.
 *
 * Every option is rendered TWICE: an isolated swatch at 4x for comparing
 * construction, and composited into a real module page at 1x for judging it
 * where it will actually live. The in-context render is not optional. An arc
 * that reads cleanly on a blank field can collide with the modulation dot or
 * the label strip once it is in a 32px cell next to seven neighbours, and
 * reviewing widgets in isolation is precisely what has let real defects
 * through before.
 *
 * Output goes to catalog-out/, which is gitignored. Only the contact sheets
 * are committed, as the record of what was in the catalog.
 *
 *   node tools/param-pages/catalog.mjs --list
 *   node tools/param-pages/catalog.mjs --set arc-knob
 *   node tools/param-pages/catalog.mjs --all
 *
 * Node-only. Nothing here ships to the device.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createFramebuffer, drawContext, SCREEN_WIDTH, SCREEN_HEIGHT } from "./harness.mjs";
import * as S from "../../src/shared/param_pages/styles/index.mjs";
import * as RM from "../../src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { fontPrint } from "../../src/shared/param_pages/styles/font/blit.mjs";
import { GLYPHS_FOR_TEST as FONT4X5_GLYPHS } from "../../src/shared/param_pages/font4x5.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const OUT_ROOT = path.join(ROOT, "catalog-out");
const PAGE_FIXTURE = path.join(ROOT, "tools", "param-pages", "catalog-page.json");

function ensureDir(p) { fs.mkdirSync(p, { recursive: true }); }

function writePng(fb, file, scale) {
    ensureDir(path.dirname(file));
    fs.writeFileSync(file, fb.toPng(scale));
}

function surface(w = SCREEN_WIDTH, h = SCREEN_HEIGHT) {
    const fb = createFramebuffer(w, h);
    return { fb, ctx: drawContext(fb) };
}

/**
 * Compose several framebuffers into one taller sheet. Written here rather
 * than in the harness because it is a catalog concern -- the harness models a
 * 128x64 device and a contact sheet is not one.
 */
function stack(frames, gap = 2) {
    const w = Math.max(...frames.map((f) => f.width));
    const h = frames.reduce((a, f) => a + f.height + gap, 0) - gap;
    const out = createFramebuffer(w, h);
    let y = 0;
    for (const f of frames) {
        for (let fy = 0; fy < f.height; fy++)
            for (let fx = 0; fx < f.width; fx++)
                out.setPixel(fx, y + fy, f.pixels[fy * f.width + fx]);
        y += f.height + gap;
    }
    return out;
}

/*
 * The page fixture.
 *
 * catalog-page.json holds the DATA renderPageMovy needs, not the object
 * itself: two of that object's members (`metaIndex`, `viz`) are live objects
 * with methods, so they cannot survive JSON. Both are pure functions of the
 * module contract and the page's keys, so the fixture stores the contract and
 * this rebuilds them -- which was checked against the untrimmed surge and
 * osirus contracts as a zero-pixel diff, so the fixture draws exactly what
 * the fleet fixture draws.
 */
function loadPageCase() {
    const j = JSON.parse(fs.readFileSync(PAGE_FIXTURE, "utf8"));
    const metaIndex = buildMetaIndex({ hierarchy: j.hierarchy, chainParams: j.chain_params });
    const { groups } = resolveViz({ keys: j.page.keys, metaIndex });
    return {
        page: j.page, metaIndex, values: j.values, title: j.title,
        pageIndex: j.pageIndex, pageCount: j.pageCount,
        touched: typeof j.touched === "number" ? j.touched : -1,
        viz: groups, footer: j.footer,
    };
}

/**
 * Which of the eight cells this page actually draws the replaceable widget
 * into. A viz group covers its slots and draws instead of the knob, and an
 * enum or opaque cell gets a different widget entirely -- so those are the
 * cells a knob option must NOT be painted over. Mirrors drawKnobRow's own
 * `covered` computation rather than guessing at it.
 */
function knobSlots(pageCase) {
    const covered = new Array(8).fill(false);
    for (const g of (pageCase.viz || [])) {
        if (!g || typeof g.slotStart !== "number") continue;
        for (let s = g.slotStart; s < g.slotStart + g.slotSpan && s < 8; s++) covered[s] = true;
    }
    const out = [];
    for (let slot = 0; slot < 8; slot++) {
        const key = pageCase.page.keys[slot];
        if (!key || covered[slot]) continue;
        const meta = pageCase.metaIndex.getOrGuess(key);
        if (meta && (meta.kind === "enum" || meta.kind === "opaque")) continue;
        out.push(slot);
    }
    return out;
}

const SWATCH_H = RM.BOX_H + RM.LBL_H + 2;

/*
 * A contact-sheet row: the swatch with its position in a gutter beside it.
 * Without the gutter the sheet is eleven anonymous 32px tiles and the only
 * way to say which one is option 7 is to count -- which defeats the point of
 * committing the sheet as the record of what was in the catalog.
 */
const GUTTER_W = 26;

function sheetRow(sw, label) {
    const out = createFramebuffer(GUTTER_W + sw.width, sw.height);
    out.print(1, Math.max(0, Math.floor((sw.height - 7) / 2)), label, 1);
    for (let y = 0; y < sw.height; y++)
        for (let x = 0; x < sw.width; x++)
            out.setPixel(GUTTER_W + x, y, sw.pixels[y * sw.width + x]);
    return out;
}

/*
 * THE PROBE.
 *
 * Only the arc-knob set has the signature `(ctx, kx, ky, value)`. A fader takes
 * a viz rect and a metaIndex, a footer takes a hint list, an opaque cell takes a
 * value and an override -- so a set that replaces one of those declares its own
 * surface (`probeSize`) and its own call (`probe`), and everything here that
 * used to assume the knob shape asks the set instead.
 *
 * The same pair drives the CLIPPING assertion in tests/host/test_style_catalog.sh.
 * That is deliberate: the surface an option is judged on and the surface it is
 * allowed to draw in must be the same one, or the test proves nothing about what
 * the sheet shows.
 */
function probeSizeOf(set) {
    return set.probeSize || { w: RM.CELL_W, h: SWATCH_H };
}

function paintProbe(set, ctx, draw, v) {
    if (typeof set.probe === "function") { set.probe(ctx, draw, v); return; }
    draw(ctx, Math.floor((RM.CELL_W - RM.KW) / 2), 0, v);
}

/*
 * THE SPECIMEN, for a KIND_FONT set.
 *
 * A font cannot be judged from a swatch of one glyph, and it cannot be judged
 * from a pangram either: what has to be legible here is the specific strings
 * this UI puts on a 128x64 screen. font4x5.mjs records the failure it was
 * written to fix -- the 3-wide font before it "rendered MAIN as 'MAIK', SINE
 * as 'SIKE', SAW as 'SAU'" -- so those three strings are drawn for every
 * option and looked at, along with AMPLITUDE, which is the string the
 * original Elektron measurement was taken from and which has to fit 128px.
 *
 * Each line is drawn at the LEFT MARGIN rather than centred, so the widths of
 * ten options are directly comparable down a contact sheet, and the line that
 * overruns 128px is the one that visibly runs off the right-hand edge instead
 * of being quietly re-centred. Overrun is also counted: `clipped()` is summed
 * by renderSet exactly as it is for a draw option, so a font too wide for the
 * header fails the catalog run rather than looking merely cramped.
 */
const SPEC_PITCH = 7;   /* 5 rows of glyph, 2 clear -- the label band's own spacing */
const SPEC_X = 1;

function specimenStrings(set) {
    return set.specimen || ["AMPLITUDE", "MAIN", "SINE", "SAW", "ATTACK", "KICK", "0123456789"];
}

function renderSpecimen(glyphs, set) {
    const lines = specimenStrings(set);
    const { fb, ctx } = surface(SCREEN_WIDTH, lines.length * SPEC_PITCH);
    for (let i = 0; i < lines.length; i++)
        fontPrint(ctx, SPEC_X, i * SPEC_PITCH, lines[i], 1, glyphs);
    return fb;
}

function renderSwatch(opt, set) {
    if (set.kind === S.KIND_FONT) return renderSpecimen(opt.glyphs, set);
    const size = probeSizeOf(set);
    const { fb, ctx } = surface(size.w, size.h);
    if (set.kind === S.KIND_DRAW) paintProbe(set, ctx, opt.draw, 0.62);
    return fb;
}

/*
 * The page render.
 *
 * The whole page is drawn by the shipping renderer first, then the option is
 * painted into the cells that carry the widget it replaces. The clear before
 * each redraw is confined to the widget box (BOX_H tall, starting at the row
 * origin) so the label strip below it, the bank bar above it and the footer
 * all survive -- and the cells skipped by knobSlots() keep their viz curve or
 * enum square, which is the whole point of judging in context. Divable
 * brackets are the one casualty: they are drawn inside the box, so an option
 * that does not draw its own will lose them on a bracketed cell. The fixture
 * page has none.
 */
function paintInContext(set, ctx, draw, pageCase, slots) {
    /* A set whose widget is not the knob says where it goes -- the footer band
     * is one call at the bottom of the page, a viz cell is a rect rather than a
     * (kx, ky). The default below stays exactly what it was. */
    if (typeof set.context === "function") { set.context(ctx, draw, { RM, slots, pageCase }); return; }
    for (const slot of slots) {
        const row = slot < 4 ? 0 : 1;
        const col = slot % 4;
        const rowY = row === 0 ? RM.ROW0_Y : RM.ROW1_Y;
        const cellX = col * RM.CELL_W;
        const kx = cellX + Math.floor((RM.CELL_W - RM.KW) / 2);
        ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
        /* A spread of values across the row, so one page shows the option at
         * the bottom, the middle and the top of its travel at once. */
        const v = (slot + 1) / 9;
        draw(ctx, kx, rowY, v);
    }
}

function renderInContext(opt, set, pageCase, slots) {
    const { fb, ctx } = surface();
    RM.renderPageMovy(ctx, pageCase);
    if (set.kind !== S.KIND_DRAW) return fb;
    paintInContext(set, ctx, opt.draw, pageCase, slots);
    return fb;
}

function renderSet(set, pageCase) {
    const dir = path.join(OUT_ROOT, set.id);
    ensureDir(dir);
    const sheetFrames = [];
    const slots = knobSlots(pageCase);

    if (set.kind === S.KIND_DRAW) {
        /* The NOW row is the widget this set replaces, drawn through the SAME
         * probe every option gets. A baseline rendered any other way is not
         * comparable with the ten rows under it -- which matters most for the
         * opaque cell, whose frame is drawn by the grid rather than by the
         * widget, and would be missing from a baseline that called the shipping
         * function directly. */
        const bsize = probeSizeOf(set);
        const base = surface(bsize.w, bsize.h);
        paintProbe(set, base.ctx, set.baseline || RM.drawArcKnob, 0.62);
        writePng(base.fb, path.join(dir, "baseline.png"), 4);
        sheetFrames.push(sheetRow(base.fb, "NOW"));

        const basePage = surface();
        RM.renderPageMovy(basePage.ctx, pageCase);
        if (set.baseline) paintInContext(set, basePage.ctx, set.baseline, pageCase, slots);
        writePng(basePage.fb, path.join(dir, "baseline-page.png"), 4);
    }

    if (set.kind === S.KIND_FONT) {
        /* The NOW row is the shipping font drawn through the same specimen,
         * which is the only way the nine traced letterforms are visible next
         * to their replacements. */
        const base = renderSpecimen(FONT4X5_GLYPHS, set);
        writePng(base, path.join(dir, "baseline.png"), 4);
        sheetFrames.push(sheetRow(base, "NOW"));
    }

    let clippedTotal = 0;
    for (const opt of [...set.options].sort((a, b) => a.position - b.position)) {
        const tag = String(opt.position).padStart(2, "0") + "-" + opt.id;

        const sw = renderSwatch(opt, set);
        writePng(sw, path.join(dir, tag + "-swatch.png"), 4);
        sheetFrames.push(sheetRow(sw, String(opt.position).padStart(2, "0")));
        clippedTotal += sw.clipped();

        /* A KIND_FONT option gets no in-context render. renderPageMovy prints
         * its labels through font4x5's own closed-over table, so substituting
         * one would mean plumbing a font through the shipping renderer for
         * the sake of a preview -- and the page would come back byte-identical
         * ten times over, which reads as ten renders that all worked. The
         * specimen is the judged surface here, and it is measured for overrun
         * against the same 128px the header has. */
        if (set.kind === S.KIND_FONT) continue;

        const pg = renderInContext(opt, set, pageCase, slots);
        writePng(pg, path.join(dir, tag + "-page.png"), 4);
        clippedTotal += pg.clipped();
    }

    if (sheetFrames.length) writePng(stack(sheetFrames), path.join(dir, "contact-sheet.png"), 4);
    console.log(set.id + ": " + set.options.length + " option(s) -> " + dir + "/" +
                (clippedTotal ? "   [" + clippedTotal + " px drawn outside the surface]" : ""));
    return clippedTotal;
}

function main() {
    const argv = process.argv.slice(2);
    const problems = S.validateAll();
    if (problems.length) {
        console.error("catalog: registry has structural problems:");
        for (const p of problems) console.error("  " + p);
        process.exit(1);
    }

    if (argv.includes("--list") || argv.length === 0) {
        if (!S.SETS.length) { console.log("catalog: no sets registered yet"); return; }
        for (const s of S.SETS)
            console.log(s.id.padEnd(16) + " " + s.kind.padEnd(7) + " " +
                        String(s.options.length).padStart(2) + "/" + S.OPTIONS_PER_SET +
                        " option(s)  " + s.title);
        return;
    }

    const pageCase = loadPageCase();

    const only = argv.includes("--set") ? argv[argv.indexOf("--set") + 1] : null;
    const sets = only ? [S.setById(only)].filter(Boolean) : S.SETS;
    if (only && !sets.length) { console.error("catalog: no such set: " + only); process.exit(1); }
    if (!sets.length) { console.log("catalog: no sets registered yet"); return; }
    let clipped = 0;
    for (const s of sets) clipped += renderSet(s, pageCase);
    if (clipped) {
        console.error("catalog: " + clipped + " px were drawn outside a surface");
        process.exit(1);
    }
}

main();
