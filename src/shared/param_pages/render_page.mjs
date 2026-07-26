/**
 * render_page.mjs — draw one param page onto a 1-bit display.
 *
 * PURE with respect to the device: every pixel goes through the injected draw
 * context, and nothing here reads a param, owns the screen, or handles input.
 * That is what lets the native shadow UI and a tool module (a sequencer drawing
 * the same grid above its own header, capturing parameter locks) share it.
 *
 *   ctx = { fillRect(x,y,w,h,color), print(x,y,text,color), textWidth(text) }
 *
 * Text is the device's own 5x7 font via ctx.print — the library ships no font
 * of its own. Measured against the fleet, 5 characters at ~6 px fits the 32 px
 * cell, which is the same size Movy's bundled 8pt font renders at, so there is
 * nothing to gain from carrying a second font around.
 *
 * Layout (128x64, the whole screen; pass `rect` to draw into a sub-region):
 *
 *   y 0..6    header: "T1 > MODULE" left, page name right
 *   y 8       page indicator — the rule is split into one segment per page,
 *             the current one filled (borrowed from Movy, and a good trick:
 *             it costs no vertical space)
 *   y 10..35  row 0 — four 32 px cells
 *   y 37..62  row 1 — four 32 px cells
 *
 * Cell layouts are switchable because the trade-off is real and only settles by
 * looking at it — see LAYOUT_BAR / LAYOUT_DIAL below.
 */

import { KIND_ENUM, KIND_OPAQUE } from "./param_meta.mjs";
import { formatParamValue } from "../param_format.mjs";

export const SCREEN_WIDTH = 128;
export const SCREEN_HEIGHT = 64;
export const COLS = 4;
export const ROWS = 2;

/**
 * Two cell layouts, and the choice is a real one:
 *
 *   LAYOUT_BAR (default)  label / horizontal bar / value — everything legible
 *                         at once. A 30x4 bar carries far more readable
 *                         resolution than a dial small enough to leave room for
 *                         a value line.
 *   LAYOUT_DIAL           big dial / label, value shown in place of the label
 *                         while the knob is held. Prettier and more instrument-
 *                         like; you trade always-visible numbers for it.
 *
 * A third variant (small dial *and* value) was built and discarded: it is the
 * bar layout with a worse widget in the same space.
 */
export const LAYOUT_BAR = "bar";
export const LAYOUT_DIAL = "dial";

/* Horizontal gutter each side of a cell. 1 px (a 2 px gap between neighbours)
 * is the most that can be spared: a 32 px cell fits exactly five 5x7 characters
 * at 30 px, and a wider gutter costs a whole character of every label. */
const CELL_PAD = 1;

const HEADER_TEXT_Y = 0;
const RULE_Y = 8;
const HEADER_BLOCK = 11;         /* header text + page rule + 1 px breathing */
const FONT_H = 7;

const BAR_H = 4;
/* Full bar cell: label (7) + gap (1) + bar (4) + gap (2) + value (7) = 21.
 * The value must sit closer to its own bar than to the next row's label, or the
 * two rows read as one interleaved block. */
const BAR_FULL_H = FONT_H + 1 + BAR_H + 2 + FONT_H;
/* Dial cell: an 8 px-radius dial (17) + 1 + label (7). */
const DIAL_FULL_H = 17 + 1 + FONT_H;

/**
 * Work out the row geometry for the height actually available. The full-screen
 * case gives 26 px rows; a tool drawing the grid beneath its own header gets
 * less, so the cell degrades — first the value line goes, then the label —
 * rather than overflowing. Callers get whatever fits, never a clipped draw.
 */
function geometry(rect, layout) {
    const gridTop = rect.y + HEADER_BLOCK;
    const gridH = Math.max(0, rect.h - HEADER_BLOCK);
    const rowH = Math.floor(gridH / ROWS);

    if (layout === LAYOUT_DIAL) {
        if (rowH >= DIAL_FULL_H) return { gridTop, rowH, mode: "dial", radius: 8 };
        /* Shrink the dial to whatever is left after the label. */
        const r = Math.floor((rowH - FONT_H - 2) / 2);
        if (r >= 4) return { gridTop, rowH, mode: "dial", radius: r };
        /* Too short for a dial at all — fall back rather than draw a blob. */
    }
    if (rowH >= BAR_FULL_H) return { gridTop, rowH, mode: "bar-value", radius: 0 };
    if (rowH >= FONT_H + 1 + BAR_H) return { gridTop, rowH, mode: "bar-label", radius: 0 };
    return { gridTop, rowH, mode: "bar-only", radius: 0 };
}

/* ------------------------------------------------------------ primitives */

function line(ctx, x0, y0, x1, y1, color) {
    let dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    let dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    let err = dx + dy;
    for (;;) {
        ctx.fillRect(x0, y0, 1, 1, color);
        if (x0 === x1 && y0 === y1) break;
        const e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint circle outline. */
function circle(ctx, cx, cy, r, color) {
    let x = r, y = 0, err = 1 - r;
    while (x >= y) {
        for (const [px, py] of [[x, y], [y, x], [-y, x], [-x, y], [-x, -y], [-y, -x], [y, -x], [x, -y]]) {
            ctx.fillRect(cx + px, cy + py, 1, 1, color);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

function centeredText(ctx, cx, y, text, color) {
    const t = asciiFold(text);
    const w = ctx.textWidth(t);
    ctx.print(Math.round(cx - w / 2), y, t, color);
}

/**
 * Map characters the device font cannot draw onto ASCII it can.
 *
 * The atlas is ASCII-only (5x7, 106 glyphs), so anything outside it renders as
 * *nothing* — silently. Five modules in the fleet ship 15 such strings today:
 * forge's "Copy A→B" and "Swap A↔B", aphex's "MW→MG", signal's "Save → A",
 * sfz's cents unit "¢", and euclidrum's "—" enum option, which is invisible
 * entirely. This is not a grid-specific problem — the list editor drops the same
 * characters — so the fold belongs anywhere module text reaches the screen.
 *
 * Anything unmapped and unprintable becomes "?": a visible wrong character
 * beats an invisible missing one, because only one of them gets reported.
 */
export function asciiFold(text) {
    const s = String(text == null ? "" : text);
    /* Fast path: the overwhelming majority of strings are already clean. */
    if (!/[^\x20-\x7e]/.test(s)) return s;
    let out = "";
    for (const ch of s) {
        if (ch >= " " && ch <= "~") { out += ch; continue; }
        out += ASCII_FOLD[ch] !== undefined ? ASCII_FOLD[ch] : "?";
    }
    return out;
}

const ASCII_FOLD = {
    "→": ">", "←": "<", "↔": "<>", "↑": "^", "↓": "v",
    "—": "-", "–": "-", "−": "-", " ": " ",
    "°": "deg", "¢": "c", "µ": "u", "μ": "u",
    "×": "x", "÷": "/", "±": "+/-",
    "‘": "'", "’": "'", "“": "\"", "”": "\"",
    "…": "...", "≤": "<=", "≥": ">=", "≠": "!=",
    "½": "1/2", "¼": "1/4", "¾": "3/4",
    "²": "2", "³": "3", "∞": "inf", "Ω": "ohm",
};

/* Trim to fit a pixel width, dropping characters rather than scaling. */
function fitText(ctx, text, maxWidth) {
    let s = asciiFold(text);
    if (ctx.textWidth(s) <= maxWidth) return s;
    while (s.length > 1 && ctx.textWidth(s) > maxWidth) s = s.slice(0, -1);
    return s;
}

/* Drop interior vowels from the end backwards ("Resonance" → "Rsnnc"). The
 * first character is never dropped. */
function devowel(word, ctx, maxWidth) {
    const chars = word.split("");
    for (let i = chars.length - 1; i > 0 && ctx.textWidth(chars.join("")) > maxWidth; i--) {
        if (/[aeiou]/i.test(chars[i])) chars.splice(i, 1);
    }
    return chars.join("");
}

/**
 * Shorten a label to fit a cell. A 32 px cell holds about five 5x7 characters,
 * so this is lossy no matter how clever it gets — the full name is shown in
 * place of the value while the knob is held, which is the actual answer to
 * ambiguity. What this does is make the five characters the *best* five.
 *
 * Rules, in order:
 *   - fits already → unchanged
 *   - 3+ words → initials ("Low Freq Osc" → "LFO")
 *   - 2 words → keep the LAST word whole (it is nearly always the
 *     distinguishing noun) and shrink the earlier one: "Filter Env" → "FltEnv"
 *     → "FlEnv", never "Fltr" which loses the part that matters
 *   - one short word (<= 7) → plain truncation, which reads better than
 *     devowelling ("Cutoff" → "Cutof", not "Cutff")
 *   - one long word → devowel, then truncate ("Resonance" → "Rsnnc")
 */
export function shortenLabel(ctx, label, maxWidth) {
    const s = asciiFold(label).trim();
    if (!s) return "";
    if (ctx.textWidth(s) <= maxWidth) return s;

    const words = s.split(/[\s_]+/).filter(Boolean);

    if (words.length >= 3) {
        const initials = words.map((w) => w[0].toUpperCase()).join("");
        if (ctx.textWidth(initials) <= maxWidth) return initials;
        return fitText(ctx, initials, maxWidth);
    }

    if (words.length === 2) {
        const [head, tail] = words;
        /* Shrink the head until the pair fits, keeping the tail intact. */
        for (let n = head.length; n >= 1; n--) {
            const cand = head.slice(0, n) + tail;
            if (ctx.textWidth(cand) <= maxWidth) return cand;
        }
        const devowelledTail = devowel(tail, ctx, maxWidth);
        if (ctx.textWidth(devowelledTail) <= maxWidth) return devowelledTail;
        return fitText(ctx, tail, maxWidth);
    }

    const word = words[0] || s;
    if (word.length <= 7) return fitText(ctx, word, maxWidth);
    return fitText(ctx, devowel(word, ctx, maxWidth), maxWidth);
}

/* --------------------------------------------------------------- widgets */

/** Dial: outline plus a pointer, sweeping 270 degrees from lower-left. */
function dial(ctx, cx, cy, r, frac, color) {
    circle(ctx, cx, cy, r, color);
    const a = (135 + 270 * clamp01(frac)) * Math.PI / 180;
    const px = cx + Math.cos(a) * (r - 1.5);
    const py = cy + Math.sin(a) * (r - 1.5);
    line(ctx, cx, cy, Math.round(px), Math.round(py), color);
}

/**
 * Horizontal bar: a track with a filled portion and an end cap.
 * At 26x3 px it carries far more readable resolution than a radius-5 dial,
 * which is why it is the widget for the compact layout.
 */
function hbar(ctx, x, y, w, h, frac, color) {
    ctx.fillRect(x, y + h - 1, w, 1, color);            /* baseline */
    const fill = Math.round(w * clamp01(frac));
    if (fill > 0) ctx.fillRect(x, y, fill, h, color);
    /* Ticks at both ends so an empty bar still shows its extent. */
    ctx.fillRect(x, y, 1, h, color);
    ctx.fillRect(x + w - 1, y, 1, h, color);
}

/** Vertical bar: outline with a filled portion, for level-ish params. */
function vbar(ctx, x, y, w, h, frac, color) {
    ctx.fillRect(x, y, w, 1, color);
    ctx.fillRect(x, y + h - 1, w, 1, color);
    ctx.fillRect(x, y, 1, h, color);
    ctx.fillRect(x + w - 1, y, 1, h, color);
    const fill = Math.round((h - 2) * clamp01(frac));
    if (fill > 0) ctx.fillRect(x + 1, y + h - 1 - fill, w - 2, fill, color);
}

function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

function fractionOf(meta, raw) {
    if (!meta) return 0;
    const num = Number(raw);
    if (!isFinite(num)) return 0;
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    if (!(max > min)) return 0;
    return clamp01((num - min) / (max - min));
}

/* ----------------------------------------------------------------- cells */

function drawCell(ctx, opts) {
    const { x: cellX, y, w: cellW, h, meta, raw, geo, touched, decoration } = opts;
    /* Draw inside a gutter so neighbouring labels never touch. */
    const x = cellX + CELL_PAD;
    const w = cellW - CELL_PAD * 2;
    const cx = cellX + Math.floor(cellW / 2);
    const color = 1;

    if (!meta) return;

    const locked = decoration && decoration.locked;
    const value = (decoration && decoration.value !== undefined) ? decoration.value : raw;

    const label = shortenLabel(ctx, meta.label || meta.key, w);
    const display = value === null || value === undefined
        ? "--"
        : fitText(ctx, formatParamValue(value, meta), w);

    /* A locked cell (a sequencer's parameter lock on the held step) inverts its
     * label strip, so which of the eight are locked reads at a glance. */
    const labelBg = (yTop) => {
        if (!locked) return;
        ctx.fillRect(x, yTop - 1, w, FONT_H + 1, 1);
    };
    const labelFg = locked ? 0 : 1;

    const dialMode = geo.mode === "dial";
    const showLabel = geo.mode !== "bar-only";
    const showValue = geo.mode === "bar-value" || dialMode;
    const labelY = dialMode ? y + h - FONT_H : y;
    const bodyY = dialMode ? y : y + (showLabel ? FONT_H + 1 : 0);

    if (meta.kind === KIND_OPAQUE) {
        /* Not turnable — the cell is a door to a fullscreen editor. Show the
         * tail of the value (a filename, usually) and mark it as openable. */
        if (showLabel) { labelBg(labelY); centeredText(ctx, cx, labelY, label, labelFg); }
        const shown = String(value == null ? "" : value).split("/").pop() || "--";
        centeredText(ctx, cx, bodyY, fitText(ctx, shown, w), color);
        if (bodyY + FONT_H * 2 + 1 <= y + h) {
            /* ASCII only: the device font atlas has no "…" and would draw nothing. */
            centeredText(ctx, cx, bodyY + FONT_H + 1, "...", color);
        }
        if (touched) ctx.fillRect(x, y + h - 1, w, 1, color);
        return;
    }

    if (meta.kind === KIND_ENUM) {
        /* A dial cannot say "up_down". Enums show the option text boxed, so it
         * reads as a discrete setting rather than a number. */
        if (showLabel) { labelBg(labelY); centeredText(ctx, cx, labelY, label, labelFg); }
        const bw = Math.min(w, ctx.textWidth(display) + 4);
        const bx = cx - Math.floor(bw / 2);
        const boxH = FONT_H + 4;
        const by = Math.min(bodyY, y + h - boxH);
        ctx.fillRect(bx, by, bw, 1, color);
        ctx.fillRect(bx, by + boxH - 1, bw, 1, color);
        ctx.fillRect(bx, by, 1, boxH, color);
        ctx.fillRect(bx + bw - 1, by, 1, boxH, color);
        centeredText(ctx, cx, by + 2, fitText(ctx, display, bw - 4), color);
        if (touched) ctx.fillRect(x, y + h - 1, w, 1, color);
        return;
    }

    /* Number. */
    const frac = fractionOf(meta, value);
    if (dialMode) {
        const r = geo.radius;
        if (meta.render === "vbar") vbar(ctx, cx - 5, y + 1, 10, h - FONT_H - 3, frac, color);
        else dial(ctx, cx, y + 1 + r, r, frac, color);
        labelBg(labelY);
        centeredText(ctx, cx, labelY, touched ? display : label, touched ? 1 : labelFg);
        return;
    }

    if (showLabel) { labelBg(labelY); centeredText(ctx, cx, labelY, label, labelFg); }
    if (meta.render === "vbar") vbar(ctx, cx - 4, bodyY, 8, Math.max(BAR_H, y + h - bodyY - (showValue ? FONT_H : 0)), frac, color);
    else hbar(ctx, x, bodyY, w, BAR_H, frac, color);
    if (showValue) {
        const valueY = bodyY + BAR_H + 2;
        centeredText(ctx, cx, valueY, display, color);
        /* Underline sits directly beneath this cell own value, not at the row
         * boundary — at the boundary it reads as an overline on the row below. */
        if (touched) ctx.fillRect(x, valueY + FONT_H, w, 1, color);
    } else if (touched) {
        ctx.fillRect(x, bodyY + BAR_H + 1, w, 1, color);
    }
}

/* ------------------------------------------------------------------ page */

/**
 * @param {object} ctx   draw context { fillRect, print, textWidth }
 * @param {object} o
 * @param {object} o.page       a PAGE_KNOBS page from page_plan.planPages
 * @param {object} o.metaIndex  from param_meta.buildMetaIndex (needs getOrGuess)
 * @param {object} o.values     key -> raw value (string or number), may be partial
 * @param {string} o.title      left side of the header, e.g. "T1 > OB-XD"
 * @param {number} o.pageIndex  0-based, for the segmented rule
 * @param {number} o.pageCount
 * @param {number} [o.touched]  physical knob 0-7 currently held, or -1
 * @param {Array}  [o.decorations] per-slot { value, locked } overrides — how a
 *                 sequencer shows the held step's parameter locks
 * @param {string} [o.layout]   LAYOUT_BAR (default) or LAYOUT_DIAL
 * @param {object} [o.rect]     sub-region to draw into; defaults to the screen
 */
export function renderPage(ctx, o) {
    const rect = o.rect || { x: 0, y: 0, w: SCREEN_WIDTH, h: SCREEN_HEIGHT };
    const layout = o.layout || LAYOUT_BAR;
    const touched = typeof o.touched === "number" ? o.touched : -1;
    const page = o.page;

    drawHeader(ctx, rect, o.title || "", page ? page.name : "", o.pageIndex | 0, Math.max(1, o.pageCount | 0));

    if (!page || !page.keys) return;

    /* While a knob is held, the header is replaced by that param's FULL name and
     * value. A 30 px cell cannot show "Resonance" — truncating it in place just
     * yields "Reson", no better than the abbreviation already there — but the
     * full screen width can, and the module name is the one thing you do not
     * need at the moment you are turning something. */
    if (touched >= 0 && page.keys[touched] && o.metaIndex) {
        const m = o.metaIndex.getOrGuess(page.keys[touched]);
        const dec = o.decorations ? o.decorations[touched] : null;
        const v = dec && dec.value !== undefined ? dec.value : (o.values ? o.values[page.keys[touched]] : null);
        drawTouchStrip(ctx, rect, m, v, dec && dec.locked);
    }

    const geo = geometry(rect, layout);
    if (geo.rowH < 6) return;   /* nothing meaningful fits */
    const cellW = Math.floor(rect.w / COLS);
    for (let slot = 0; slot < COLS * ROWS; slot++) {
        const key = page.keys[slot];
        if (!key) continue;
        const row = Math.floor(slot / COLS);
        const col = slot % COLS;
        drawCell(ctx, {
            x: rect.x + col * cellW,
            y: geo.gridTop + row * geo.rowH,
            w: cellW,
            h: geo.rowH,
            geo,
            meta: o.metaIndex ? o.metaIndex.getOrGuess(key) : null,
            raw: o.values ? o.values[key] : null,
            touched: touched === slot,
            decoration: o.decorations ? o.decorations[slot] : null,
        });
    }
}

/**
 * The held-knob readout: an inverted strip over the header carrying the param's
 * full name on the left and its value on the right. A locked param is marked so
 * you can tell "this step's value" from "the module's value" at a glance.
 */
function drawTouchStrip(ctx, rect, meta, value, locked) {
    if (!meta) return;
    const x = rect.x, y = rect.y, w = rect.w;
    ctx.fillRect(x, y, w, FONT_H + 1, 1);
    const val = value === null || value === undefined ? "--" : formatParamValue(value, meta);
    const suffix = locked ? " *" : "";
    const right = asciiFold(val + suffix);
    const rw = ctx.textWidth(right);
    ctx.print(x + w - rw - 1, y, right, 0);
    ctx.print(x + 1, y, fitText(ctx, meta.label || meta.key, w - rw - 4), 0);
}

/**
 * Header plus the segmented page rule. Exported so a non-grid page kind
 * (preset browser, items list) can reuse the same chrome and the page position
 * stays visible everywhere.
 */
export function drawHeader(ctx, rect, title, pageName, pageIndex, pageCount) {
    const x = rect.x, y = rect.y, w = rect.w;
    ctx.print(x + 1, y + HEADER_TEXT_Y, fitText(ctx, title, Math.floor(w * 0.62)), 1);
    if (pageName) {
        const t = fitText(ctx, pageName, Math.floor(w * 0.42));
        ctx.print(x + w - ctx.textWidth(t) - 1, y + HEADER_TEXT_Y, t, 1);
    }

    /* One segment per page, current filled. Above ~24 pages the segments stop
     * being individually readable, so fall back to a plain rule with a
     * proportional marker. */
    const ry = y + RULE_Y;
    if (pageCount > 1 && pageCount <= 24) {
        const seg = w / pageCount;
        for (let i = 0; i < pageCount; i++) {
            const sx = Math.round(x + i * seg);
            const sw = Math.max(1, Math.round(x + (i + 1) * seg) - sx - 1);
            ctx.fillRect(sx, ry, sw, i === pageIndex ? 3 : 1, 1);
        }
    } else {
        ctx.fillRect(x, ry, w, 1, 1);
        if (pageCount > 1) {
            const mx = Math.round(x + (w - 8) * (pageIndex / (pageCount - 1)));
            ctx.fillRect(mx, ry, 8, 3, 1);
        }
    }
}
