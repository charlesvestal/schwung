/**
 * render_page_movy.mjs — draw one param page in schwung-movy's own layout.
 *
 * A direct port of schwung-movy's knob-grid rendering
 * (`src/renderer/{header,knob,label}.ts`, © 2026 megadake, MIT —
 * https://github.com/DimaDake/schwung-movy), not a new design: the header,
 * the bank bar, the arc knob, the enum square, and the per-column label cell
 * are Movy's actual pixel geometry, adapted to read from this library's own
 * page/metaIndex/values/viz data instead of Movy's ParamVM. The five graphic
 * bodies (envelope/filter/lfo/eq/sample) are the SAME functions render_page.mjs
 * uses, from viz_draw.mjs — also ported from Movy — called here with the
 * exact 16px-tall row boxes Movy itself draws them into.
 *
 * This is a SEPARATE renderer from render_page.mjs's own dial/bar grid, not a
 * third `layout` value on it: the two do not share cell geometry (Movy's
 * fixed ROW0_Y/LBL0_Y/ROW1_Y/LBL1_Y rows vs. the dial/bar grid's dynamic row
 * height) and mixing them into one function would make neither read as
 * either original. Selected via `page_controller.mjs`'s `LAYOUT_MOVY`.
 *
 * Same library rules as render_page.mjs: no param I/O, no screen ownership
 * (draws through the injected ctx only), no input handling. One exception to
 * "no font of its own": the enum square's two-line abbreviation needs Movy's
 * condensed 5x3 font (font5x3.mjs, ported) to fit — the device's regular 5x7
 * font cannot show two lines in a 16px box, which is the entire reason that
 * font exists. Everywhere else (header, labels) still goes through the
 * device's own print(), same as the dial/bar grid.
 */

import { KIND_ENUM, KIND_OPAQUE } from "./param_meta.mjs";
import { formatParamValue } from "../param_format.mjs";
import { asciiFold, fitText, shortenLabel, line } from "./render_page.mjs";
import { drawVizGroup } from "./viz_draw.mjs";
import { fontWidth5x3, fontPrint5x3, enumSquareLines } from "./font5x3.mjs";

/** Selects this renderer from page_controller.mjs's `setLayout`. */
export const LAYOUT_MOVY = "movy";

/* schwung-movy renderer/layout.ts, verbatim. */
export const W = 128;
export const HEADER_H = 7;
export const BAR_Y = 8;
export const ROW0_Y = 11;
export const LBL0_Y = 27;
export const ROW1_Y = 35;
export const LBL1_Y = 51;
export const CELL_W = 32;
export const LBL_H = 7;
export const KW = 16;
export const TOAST_Y = 58;

/* ------------------------------------------------------------- primitives */

/* Native draw_line (src/host/js_display.c) costs one QuickJS->C crossing
 * regardless of length, unlike this library's own JS Bresenham — see
 * viz_draw.mjs's note on the same tradeoff. Prefer it when the caller
 * provides one. */
function drawLine(ctx, x0, y0, x1, y1) {
    if (typeof ctx.line === "function") ctx.line(Math.round(x0), Math.round(y0), Math.round(x1), Math.round(y1), 1);
    else line(ctx, Math.round(x0), Math.round(y0), Math.round(x1), Math.round(y1), 1);
}

function centeredText(ctx, cx, y, text, color) {
    const t = asciiFold(text);
    const w = ctx.textWidth(t);
    ctx.print(Math.round(cx - w / 2), y, t, color);
}

/**
 * schwung-movy renderer/header.ts drawHeader, ported — with one fix on top:
 * Movy's own HEADER_H (7) exactly matches ITS font's glyph height. Schwung's
 * device font is a 5x7 bitmap printed one row lower (`print(x, 1, ...)`, to
 * clear the very top edge of the display), so a 7-row-tall inverted
 * background starting at row 0 only covers rows 0-6 while the glyphs drawn
 * at y=1 occupy rows 1-7 — the bottom row of every character fell outside
 * the highlight and read as a black bar under the text. The working
 * precedent for this exact strip (render_page.mjs's drawTouchStrip) already
 * solves it: fill one row taller than the font (`FONT_H + 1`). Only the
 * background height changes here; the text position is untouched, so the
 * plain (non-inverted) header keeps Movy's original row.
 */
export function drawHeader(ctx, left, right, inverted = false) {
    if (inverted) ctx.fillRect(0, 0, W, HEADER_H + 1, 1);
    const color = inverted ? 0 : 1;
    const l = fitText(ctx, left, right ? Math.floor(W * 0.55) : W - 4);
    ctx.print(2, 1, l, color);
    if (right) {
        const r = fitText(ctx, right, Math.floor(W * 0.6));
        ctx.print(W - ctx.textWidth(r) - 2, 1, r, color);
    }
}

/**
 * schwung-movy renderer/header.ts drawBankBar, ported. One segment per page,
 * the current one double height; `groups` (one id per page) draws a 1px gap
 * where the bank changes, same idea as render_page.mjs's own page rule.
 */
export function drawBankBar(ctx, pageIndex, pageCount, groups) {
    if (pageCount <= 1) return;

    if (pageCount > W) {
        ctx.fillRect(0, BAR_Y, W, 1, 1);
        const x = Math.min(W - 1, Math.floor(pageIndex * W / pageCount));
        ctx.fillRect(x, BAR_Y, 1, 2, 1);
        return;
    }

    const gap = new Array(pageCount).fill(0);
    const useGroups = !!groups && groups.length === pageCount;
    const bounds = [];
    for (let b = 1; b < pageCount; b++) {
        if (!useGroups || groups[b] !== groups[b - 1]) bounds.push(b);
    }
    const keep = Math.min(bounds.length, Math.max(0, W - pageCount));
    for (let i = 0; i < keep; i++) gap[bounds[Math.floor(i * bounds.length / keep)]] = 1;

    const area = W - keep;
    const edge = (b) => Math.floor(b * area / pageCount);

    let x = 0;
    for (let b = 0; b < pageCount; b++) {
        x += gap[b];
        const segW = edge(b + 1) - edge(b);
        const h = b === pageIndex ? 2 : 1;
        if (segW > 0) ctx.fillRect(x, BAR_Y, segW, h, 1);
        x += segW;
    }
}

/* ------------------------------------------------------------------ knobs */

/**
 * schwung-movy renderer/knob.ts drawCircleBorder + drawArcKnob, ported. The
 * outline itself prefers the native `fillCircle(cx,cy,r,color)` binding
 * (src/host/js_display.c: js_display_fill_circle) when the caller provides
 * one — a filled disk, then a slightly smaller filled disk in the
 * background colour punched through the middle, is a ring in TWO native
 * calls instead of the ~40 fillRect calls a per-pixel midpoint-circle walk
 * costs for r=7, and 8 of these draw on every knob page. Without a native
 * fillCircle, falls back to the JS midpoint walk (pixel-identical outline;
 * the two-disk trick only reads as identical because fillCircle draws a
 * true disk, which this fallback cannot cheaply reproduce, so it keeps its
 * own — thinner but visually equivalent at this size — outline instead). */
function drawArcKnob(ctx, kx, ky, normVal) {
    const cx = kx + 7, cy = ky + 7, r = 7;
    if (typeof ctx.fillCircle === "function") {
        ctx.fillCircle(cx, cy, r, 1);
        ctx.fillCircle(cx, cy, r - 1, 0);
    } else {
        let x = r, y = 0, err = 0;
        while (x >= y) {
            for (const [px, py] of [[x, y], [y, x], [-y, x], [-x, y], [-x, -y], [-y, -x], [y, -x], [x, -y]]) {
                ctx.fillRect(cx + px, cy + py, 1, 1, 1);
            }
            y++;
            if (err <= 0) err += 2 * y + 1;
            if (err > 0) { x--; err -= 2 * x + 1; }
        }
    }
    const angleDeg = 210 + normVal * 300;
    const rad = angleDeg * Math.PI / 180;
    const ex = Math.round(cx + r * Math.sin(rad));
    const ey = Math.round(cy - r * Math.cos(rad));
    drawLine(ctx, cx, cy, ex, ey);
}

/**
 * schwung-movy renderer/knob.ts drawEnumSquare, adapted: Movy uses a custom
 * condensed 5x3 font to fit two short lines in the 16px box; this device only
 * has the one 5x7 font, so the option text is fit on a single centred line
 * instead. The frame and box geometry are Movy's.
 */
function drawEnumSquare(ctx, kx, ky, text) {
    ctx.fillRect(kx, ky, KW, 1, 1);
    ctx.fillRect(kx, ky + KW - 1, KW, 1, 1);
    ctx.fillRect(kx, ky, 1, KW, 1);
    ctx.fillRect(kx + KW - 1, ky, 1, KW, 1);

    const [line1, line2] = enumSquareLines(text);
    const inner = KW - 2;
    const totalH = line2.length > 0 ? 11 : 5;
    const startY = ky + 1 + Math.floor((inner - totalH) / 2);
    const l1w = fontWidth5x3(line1);
    fontPrint5x3(ctx, kx + 1 + Math.floor((inner - l1w) / 2), startY, line1, 1);
    if (line2.length > 0) {
        const l2w = fontWidth5x3(line2);
        fontPrint5x3(ctx, kx + 1 + Math.floor((inner - l2w) / 2), startY + 6, line2, 1);
    }
}

/** A knob that cannot be turned (filepath/canvas/wav_position/string): same
 * framed box as an enum square, showing the value's tail. Not part of Movy's
 * vocabulary (its modules do not carry this param type in the fleet this was
 * built against) but needed so an opaque param on a Movy-style page is a door
 * to its existing editor rather than a blank cell. */
function drawOpaqueBox(ctx, kx, ky, value) {
    ctx.fillRect(kx, ky, KW, 1, 1);
    ctx.fillRect(kx, ky + KW - 1, KW, 1, 1);
    ctx.fillRect(kx, ky, 1, KW, 1);
    ctx.fillRect(kx + KW - 1, ky, 1, KW, 1);
    const shown = String(value == null ? "" : value).split("/").pop() || "--";
    centeredText(ctx, kx + KW / 2, ky + Math.floor((KW - 7) / 2), fitText(ctx, shown, KW - 2), 1);
}

function drawKnobWidget(ctx, col, rowY, meta, raw) {
    const kx = col * CELL_W + Math.floor((CELL_W - KW) / 2), ky = rowY;
    if (meta.kind === KIND_OPAQUE) { drawOpaqueBox(ctx, kx, ky, raw); return; }
    if (meta.kind === KIND_ENUM) {
        const idx = Math.round(Number(raw));
        const text = (Array.isArray(meta.options) && meta.options[idx] !== undefined) ? String(meta.options[idx]) : String(raw ?? "");
        drawEnumSquare(ctx, kx, ky, text);
        return;
    }
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    const num = Number(raw);
    const normVal = (max > min && isFinite(num)) ? Math.max(0, Math.min(1, (num - min) / (max - min))) : 0;
    drawArcKnob(ctx, kx, ky, normVal);
}

/* schwung-movy renderer/label.ts drawWaveMark (the modulation tilde), ported. */
function drawWaveMark(ctx, x, y, on) {
    ctx.fillRect(x, y, 1, 1, on);
    ctx.fillRect(x + 2, y, 1, 1, on);
    ctx.fillRect(x + 1, y + 1, 1, 1, on);
    ctx.fillRect(x + 3, y + 1, 1, 1, on);
}

/**
 * schwung-movy renderer/label.ts drawLabelCell, ported. Shows the short name
 * normally, and the value while touched (inverted strip) — Movy's automation
 * dot has no equivalent in this library's data model (that is a
 * per-lane-automation concept the grid does not have) so only the touched
 * inversion and the modulation mark are ported.
 */
function drawLabelCell(ctx, col, lblY, label, displayValue, touched, modulated) {
    const cellX = col * CELL_W;
    const text = touched ? displayValue : label;
    const tw = ctx.textWidth(text);
    const tx = cellX + Math.floor(CELL_W / 2) - Math.floor(tw / 2);
    if (touched) {
        ctx.fillRect(cellX, lblY, CELL_W, LBL_H + 1, 1);
        ctx.print(tx, lblY + 1, text, 0);
    } else {
        ctx.print(tx, lblY + 1, text, 1);
    }
    if (modulated) {
        const wx = Math.max(cellX, tx - 6);
        drawWaveMark(ctx, wx, lblY, touched ? 0 : 1);
    }
}

/* --------------------------------------------------------------- one row */

function drawKnobRow(ctx, o, row, rowY, lblY) {
    const { page, metaIndex, values, touched, modulated, viz } = o;
    const slotBase = row * 4;

    const covered = new Array(4).fill(false);
    for (const g of (viz || [])) {
        if (!g || typeof g.slotStart !== "number") continue;
        if (Math.floor(g.slotStart / 4) !== row) continue;
        const localStart = g.slotStart - slotBase;
        for (let s = localStart; s < localStart + g.slotSpan && s < 4; s++) covered[s] = true;
        drawVizGroup(ctx, {
            x: localStart * CELL_W, y: rowY, w: g.slotSpan * CELL_W, h: LBL0_Y - ROW0_Y,
        }, g, values, metaIndex);
    }

    for (let col = 0; col < 4; col++) {
        const slot = slotBase + col;
        const key = page.keys[slot];
        if (!key) continue;
        const meta = metaIndex.getOrGuess(key);
        const raw = values ? values[key] : null;
        const isTouched = touched === slot;

        if (!covered[col]) drawKnobWidget(ctx, col, rowY, meta, raw);

        const label = shortenLabel(ctx, meta.label || meta.key, CELL_W - 2);
        const display = (raw === null || raw === undefined) ? "--" : fitText(ctx, formatParamValue(raw, meta), CELL_W - 2);
        drawLabelCell(ctx, col, lblY, label, display, isTouched, modulated ? !!modulated(key) : false);
    }
}

/* ------------------------------------------------------------------ page */

/**
 * @param {object} ctx  draw context { fillRect, print, textWidth }
 * @param {object} o
 * @param {object} o.page        a PAGE_KNOBS page from page_plan.planPages
 * @param {object} o.metaIndex   from param_meta.buildMetaIndex
 * @param {object} o.values      key -> raw value
 * @param {string} o.title       left side of the header
 * @param {number} o.pageIndex
 * @param {number} o.pageCount
 * @param {number} [o.touched]   physical knob 0-7 currently held, or -1
 * @param {Array}  [o.pageGroups] one bank id per page, for the bank bar
 * @param {Function} [o.modulated] (key) => boolean
 * @param {Array}  [o.viz]       resolved graphic groups (viz.mjs resolveViz)
 */
export function renderPageMovy(ctx, o) {
    const page = o.page;
    const touched = typeof o.touched === "number" ? o.touched : -1;

    if (touched >= 0 && page && page.keys && page.keys[touched] && o.metaIndex) {
        const m = o.metaIndex.getOrGuess(page.keys[touched]);
        const v = o.values ? o.values[page.keys[touched]] : null;
        const val = (v === null || v === undefined) ? "--" : formatParamValue(v, m);
        drawHeader(ctx, m.label || m.key, val, true);
    } else {
        drawHeader(ctx, o.title || "", page ? page.name : null, false);
    }
    drawBankBar(ctx, o.pageIndex | 0, Math.max(1, o.pageCount | 0), o.pageGroups);

    if (!page || !page.keys) return;
    const hasParams = page.keys.some(Boolean);
    if (!hasParams) {
        ctx.print(2, ROW0_Y + 4, "No params", 1);
        return;
    }

    drawKnobRow(ctx, o, 0, ROW0_Y, LBL0_Y);
    drawKnobRow(ctx, o, 1, ROW1_Y, LBL1_Y);
}
