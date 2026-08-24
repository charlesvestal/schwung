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

import { KIND_ENUM, KIND_OPAQUE, enumIndexOf } from "./param_meta.mjs";
import { formatParamValue } from "../param_format.mjs";
import { drawVizGroup, VIZ_ROWS, VIZ_MIN_W } from "./viz_draw.mjs";

/* One definition, in the leaf; re-exported so this module's consumers are
 * untouched. See ../list_geometry.mjs for why it is a leaf. */
import { SCREEN_WIDTH, SCREEN_HEIGHT } from "../list_geometry.mjs";
export { SCREEN_WIDTH, SCREEN_HEIGHT };
export const COLS = 4;
export const ROWS = 2;

/**
 * Two cell layouts.
 *
 *   LAYOUT_DIAL (default)  big dial / label. The value is not absent — holding
 *                          a knob puts its full name and value in the strip
 *                          over the header — so this keeps informational parity
 *                          while being quicker to parse: you are usually reading
 *                          relative position, and a pointer angle reads faster
 *                          than a fill length. Eight dials are also eight
 *                          distinct shapes rather than eight similar bars.
 *   LAYOUT_BAR             label / horizontal bar / value. Every value visible
 *                          simultaneously, which dials cannot do — worth it on
 *                          a levels or mixer page, or anywhere precise offsets
 *                          (tuning in cents, transpose) are compared at a
 *                          glance.
 *
 * `revealValues` closes most of that gap without a settings trip: while a
 * modifier is held, the dial layout swaps every label for its value. Eight
 * glances instead of eight touches.
 *
 * A third variant (small dial *and* value) was built and discarded: it is the
 * bar layout with a worse widget in the same space.
 */
export const LAYOUT_DIAL = "dial";
export const LAYOUT_BAR = "bar";

/* Horizontal gutter each side of a cell. 1 px (a 2 px gap between neighbours)
 * is the most that can be spared: a 32 px cell fits exactly five 5x7 characters
 * at 30 px, and a wider gutter costs a whole character of every label. */
const CELL_PAD = 1;

const HEADER_TEXT_Y = 0;
const RULE_Y = 8;
const HEADER_BLOCK = 11;         /* header text + page rule + 1 px breathing */
/*
 * The PICKER header is its own measurement, deliberately smaller than
 * HEADER_BLOCK — which belongs to the dial/bar grid and cannot move without
 * relaying that whole layout.
 *
 * The picker sat 3 rows lower than the page it draws over: an 8-row filled band
 * (HEADER_BLOCK - 3) plus 3 rows of breathing, against the Movy page header of a
 * 7-row band and a 1-row bank bar. Two headers for the same screen, one visibly
 * taller than the other, and the extra rows cost list entries.
 *
 * At 7 + a dark row + a gutter the two agree, and the list gets rows back:
 *   full screen (h 64):        floor((64 - 9) / 9) = 6   was 5
 *   with the hint footer (55): floor((55 - 9) / 9) = 5   was 4
 * so the footer no longer costs an entry at all.
 */
/*
 * 8, not 7: the header text is the 7-row device font, so a 7-row band leaves it
 * no clear row and the glyphs run into the edge of the fill — the same smudge
 * LBL_H exists to avoid. 8 gives one clear row under the text, and costs
 * nothing: both row counts below are unchanged at 8.
 */
const PICKER_HEADER_H = 8;       /* 7-row text + 1 clear row */
const PICKER_LIST_TOP = 9;       /* band + 1 gutter */
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

export function line(ctx, x0, y0, x1, y1, color) {
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
export function circle(ctx, cx, cy, r, color) {
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

export function centeredText(ctx, cx, y, text, color) {
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
export function fitText(ctx, text, maxWidth) {
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
 * `joiner` separates the abbreviated head words from the tail: "" in a 32 px
 * cell, where a space costs a sixth of the budget, and " " in the header, where
 * there is room and "Osc 1 - 2" reads far better than "Osc1 - 2".
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
export function shortenLabel(ctx, label, maxWidth, joiner = "") {
    const s = asciiFold(label).trim();
    if (!s) return "";
    if (ctx.textWidth(s) <= maxWidth) return s;

    const words = s.split(/[\s_]+/).filter(Boolean);

    if (words.length > 1) {
        const head = words.slice(0, -1);
        const tail = words[words.length - 1];
        /* Numbers survive whole: "Osc 1 Octave" must not collapse to "O1O" —
         * an index is the whole point of the name. Everything else in the head
         * reduces towards its initial. */
        const abbrev = (w, n) => (/^\d+$/.test(w) ? w : w.slice(0, Math.max(1, n)));
        /* Shrink the head progressively, keeping the last word whole for as
         * long as possible: "Osc 1 Octave" → "Osc1Octave" → "O1Octave" → and
         * only then does the tail start giving ground. */
        for (let n = Math.max(...head.map((w) => w.length)); n >= 1; n--) {
            const cand = head.map((w) => abbrev(w, n)).join(joiner) + joiner + tail;
            if (ctx.textWidth(cand) <= maxWidth) return cand;
        }
        const stem = head.map((w) => abbrev(w, 1)).join(joiner) + joiner;
        const room = maxWidth - ctx.textWidth(stem);
        const shortTail = tail.length <= 7 ? tail : devowel(tail, ctx, room);
        return fitText(ctx, stem + shortTail, maxWidth);
    }

    const word = words[0] || s;
    if (word.length <= 7) return fitText(ctx, word, maxWidth);
    return fitText(ctx, devowel(word, ctx, maxWidth), maxWidth);
}

/**
 * Longest common prefix / suffix across a set of strings, used to find what
 * actually differs between sibling keys.
 */
function commonPrefixLen(strs) {
    if (strs.length < 2) return 0;
    let n = 0;
    while (n < strs[0].length && strs.every((s) => s[n] === strs[0][n])) n++;
    return n;
}
function commonSuffixLen(strs, skip) {
    if (strs.length < 2) return 0;
    let n = 0;
    const room = Math.min(...strs.map((s) => s.length - skip));
    while (n < room && strs.every((s) => s[s.length - 1 - n] === strs[0][strs[0].length - 1 - n])) n++;
    return n;
}

/**
 * Labels for one page's cells, disambiguated against each other.
 *
 * Shortening each label in isolation is not enough: 47 of the fleet's 572 grid
 * pages end up with two cells reading the same, and euclidrum's Main page
 * produces EIGHT cells all saying "Enabl" — the module declares every lane's
 * switch as `name: "Enabled"` and only the key (`lane1_enabled` …) says which
 * is which. Eight identical labels is not an abbreviation, it is a broken
 * screen.
 *
 * So: shorten, find collisions, and for each colliding group derive a
 * discriminator from what differs in the KEYS — strip the common prefix and
 * suffix and keep the remainder ("1".."8") — then re-shorten the label to leave
 * room and append it: "Enab1" … "Enab8". If the keys differ in nothing usable,
 * fall back to the knob number, which at least maps to the thing under your
 * finger.
 */
export function pageCellLabels(ctx, keys, metaIndex, width) {
    const metas = keys.map((k) => (k && metaIndex ? metaIndex.getOrGuess(k) : null));
    const labels = metas.map((m, i) => (m ? shortenLabel(ctx, m.label || keys[i], width) : ""));

    const groups = new Map();
    labels.forEach((l, i) => {
        if (!l) return;
        if (!groups.has(l)) groups.set(l, []);
        groups.get(l).push(i);
    });

    for (const [, idxs] of groups) {
        if (idxs.length < 2) continue;
        const groupKeys = idxs.map((i) => String(keys[i]));
        const pre = commonPrefixLen(groupKeys);
        const suf = commonSuffixLen(groupKeys, pre);
        let discs = groupKeys.map((k) => k.slice(pre, k.length - suf).replace(/^[_\-]+|[_\-]+$/g, ""));
        /* If the key remainders do not separate the group either — aphex has
         * "EG1 Trig" and "EG1+2 Trig", where one key is a prefix of the other —
         * number the whole group by knob instead. Mixing derived and numbered
         * discriminators is how you get two cells both ending in "2". */
        if (discs.some((d) => !d) || new Set(discs).size !== discs.length) {
            discs = idxs.map((cell) => String(cell + 1));
        }
        idxs.forEach((cell, n) => {
            const disc = fitText(ctx, discs[n], Math.floor(width / 2));
            const room = width - ctx.textWidth(disc);
            const stem = shortenLabel(ctx, metas[cell].label || keys[cell], room);
            labels[cell] = fitText(ctx, stem + disc, width);
        });
    }
    return labels;
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
export function hbar(ctx, x, y, w, h, frac, color) {
    ctx.fillRect(x, y + h - 1, w, 1, color);            /* baseline */
    const fill = Math.round(w * clamp01(frac));
    if (fill > 0) ctx.fillRect(x, y, fill, h, color);
    /* Ticks at both ends so an empty bar still shows its extent. */
    ctx.fillRect(x, y, 1, h, color);
    ctx.fillRect(x + w - 1, y, 1, h, color);
}

export function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/*
 * An enum may report its value as the option NAME rather than as an index, so
 * resolve it the way every other reader in this layer does before treating it
 * as a number. A bare Number("On") is NaN, which lands as fraction 0 — the
 * widget then sits at the bottom of its travel no matter what the module says.
 *
 * That is exactly the bug #228 fixed in drawSwitch, and this is the same flaw
 * two functions away in the file it was editing: drawFader and viz_draw's frac()
 * helper both come through here with no enum handling of their own.
 *
 * LATENT, not live: no module in the fleet capture currently binds a viz role
 * or a fader to an enum key. It is worth closing anyway because detectSwitch
 * DERIVES a switch from any two-option boolean enum rather than requiring a
 * declaration, so a module can create one of these bindings without opting in —
 * which is how the drawSwitch case went unnoticed.
 */
export function fractionOf(meta, raw) {
    if (!meta) return 0;
    let value = raw;
    if (meta.kind === KIND_ENUM) {
        const idx = enumIndexOf(meta, raw);
        if (idx >= 0) value = idx;
    }
    const num = Number(value);
    if (!isFinite(num)) return 0;
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    if (!(max > min)) return 0;
    return clamp01((num - min) / (max - min));
}

/* ----------------------------------------------------------------- cells */

function drawCell(ctx, opts) {
    const { x: cellX, y, w: cellW, h, meta, raw, geo, touched, decoration, revealValues, modulated } = opts;
    /* Draw inside a gutter so neighbouring labels never touch. */
    const x = cellX + CELL_PAD;
    const w = cellW - CELL_PAD * 2;
    const cx = cellX + Math.floor(cellW / 2);
    const color = 1;

    if (!meta) return;

    /* A modulated param is one an LFO or mod source is moving under you, so the
     * value you see is not the value you set. The list editor appends "~" to
     * the name; a cell cannot spare a character, so it gets a tick in the top
     * right corner instead. */
    if (modulated) ctx.fillRect(cellX + cellW - 3, y, 2, 2, 1);

    const locked = decoration && decoration.locked;
    const value = (decoration && decoration.value !== undefined) ? decoration.value : raw;

    const label = opts.label !== undefined ? opts.label : shortenLabel(ctx, meta.label || meta.key, w);
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
        dial(ctx, cx, y + 1 + r, r, frac, color);
        labelBg(labelY);
        /* The touched cell keeps its NAME: the header strip is already saying
         * name and value, and swapping the label out would leave the one cell
         * you are looking at unlabelled. It gets an underline instead. */
        centeredText(ctx, cx, labelY, revealValues ? display : label, labelFg);
        if (touched) ctx.fillRect(x, y + h - 1, w, 1, color);
        return;
    }

    if (showLabel) { labelBg(labelY); centeredText(ctx, cx, labelY, label, labelFg); }
    hbar(ctx, x, bodyY, w, BAR_H, frac, color);
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

/** An unused knob position: a short centred tick, deliberately quiet. */
function drawEmptyCell(ctx, cellX, y, cellW, h) {
    const cx = cellX + Math.floor(cellW / 2);
    const w = 6;
    ctx.fillRect(cx - Math.floor(w / 2), y + Math.floor(h / 2), w, 1, 1);
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
 * @param {Array} [o.pageGroups] one section id per page, so the page rule can
 *                 show section structure rather than an undifferentiated row
 * @param {Function} [o.modulated] (key) => boolean; marks params an LFO or
 *                 modulation source is driving, as the list editor's "~" does
 * @param {string} [o.layout]   LAYOUT_DIAL (default) or LAYOUT_BAR
 * @param {boolean} [o.revealValues] dial layout only: show every value in place
 *                 of its label, for as long as a modifier is held
 * @param {object} [o.rect]     sub-region to draw into; defaults to the screen
 * @param {Array} [o.viz]       resolved graphic groups (viz.mjs resolveViz),
 *                 caller-computed and passed in — this function only draws
 *                 what it is given. Omit for the plain knob grid; a group's
 *                 member slots are replaced by one picture spanning them.
 */
export function renderPage(ctx, o) {
    const rect = o.rect || { x: 0, y: 0, w: SCREEN_WIDTH, h: SCREEN_HEIGHT };
    const layout = o.layout || LAYOUT_DIAL;
    const touched = typeof o.touched === "number" ? o.touched : -1;
    const page = o.page;

    drawHeader(ctx, rect, o.title || "", page ? page.name : "", o.pageIndex | 0, Math.max(1, o.pageCount | 0), o);

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
    /* Labels are resolved for the whole page at once so cells can be
     * disambiguated against each other — see pageCellLabels. */
    const cellLabels = pageCellLabels(ctx, page.keys, o.metaIndex, cellW - CELL_PAD * 2);

    /* Graphics: `o.viz` is a caller-resolved group list (see viz.mjs
     * resolveViz), never computed here — resolution and drawing stay
     * separate, exactly like decorations/modulated are caller state. A slot
     * inside a group's span is drawn once, from the group's first (anchor)
     * slot; the rest of the span is skipped rather than drawn twice.
     *
     * A graphic is the one cell that does NOT degrade with the rect. Its body
     * is a fixed VIZ_ROWS band measured from `rect.y + 1` (the band's parity
     * is what lets a bipolar wave have a real zero row — see viz_draw.mjs),
     * and the switch is a fixed VIZ_MIN_W sprite. Neither shrinks; both spill.
     * Measured over the fleet: 99 of 276 graphic-bearing pages drew up to 100
     * pixels below a 32px-high rect, 200 of them below a 24px one, and a
     * 96px-wide rect put the switch up to 12 pixels outside on 79 pages.
     * That breaks the promise the geometry above makes, and it breaks it for
     * the tool case specifically, since the shadow UI always has the screen.
     *
     * Rather than scale the graphics — which would move every full-screen
     * pixel this library has snapshots of, to serve a caller that would get
     * an illegible 6px envelope anyway — a cell too small for a graphic
     * simply does not get one. Those slots fall through to ordinary cells,
     * which DO degrade. A tool asking for less than a graphic needs gets
     * eight honest dials instead of one clipped picture.
     *
     * Page-wide rather than per-group on purpose: which graphic happens to
     * have a fixed size is an implementation detail of the vocabulary, and a
     * row that drew an envelope but replaced the switch beside it with a dial
     * would read as a bug rather than as a decision.
     */
    const vizFits = geo.rowH >= VIZ_ROWS + 1 && cellW >= VIZ_MIN_W;
    const vizAnchor = new Array(COLS * ROWS).fill(null);
    const vizCovered = new Array(COLS * ROWS).fill(false);
    for (const g of (vizFits ? (o.viz || []) : [])) {
        if (!g || typeof g.slotStart !== "number" || !(g.slotSpan > 0)) continue;
        vizAnchor[g.slotStart] = g;
        for (let s = g.slotStart; s < g.slotStart + g.slotSpan; s++) vizCovered[s] = true;
    }

    for (let slot = 0; slot < COLS * ROWS; slot++) {
        const row = Math.floor(slot / COLS);
        const col = slot % COLS;
        const key = page.keys[slot];

        if (vizAnchor[slot]) {
            const g = vizAnchor[slot];
            drawVizGroup(ctx, {
                x: rect.x + col * cellW,
                y: geo.gridTop + row * geo.rowH,
                w: cellW * Math.min(g.slotSpan, COLS - col),
                h: geo.rowH,
            }, g, o.values, o.metaIndex);
            continue;
        }
        if (vizCovered[slot]) continue;   /* drawn by this group's anchor cell */

        if (!key) {
            /* A page with three controls leaves five cells empty, and empty
             * cells read as a failed render rather than as "this section has
             * three things". A faint tick under each unused knob says the
             * screen is fine and that knob simply does nothing here. */
            drawEmptyCell(ctx, rect.x + col * cellW, geo.gridTop + row * geo.rowH, cellW, geo.rowH);
            continue;
        }
        drawCell(ctx, {
            x: rect.x + col * cellW,
            y: geo.gridTop + row * geo.rowH,
            w: cellW,
            h: geo.rowH,
            geo,
            meta: o.metaIndex ? o.metaIndex.getOrGuess(key) : null,
            label: cellLabels[slot],
            modulated: o.modulated ? !!o.modulated(key) : false,
            raw: o.values ? o.values[key] : null,
            touched: touched === slot,
            revealValues: !!o.revealValues,
            decoration: o.decorations ? o.decorations[slot] : null,
        });
    }
}

/**
 * Fit a page name into the header without losing which page it is.
 *
 * Plain truncation turns "Oscillator 1 - 2" into "Oscillator 1 -", dropping the
 * one part that says where you are among that level's pages. So the trailing
 * " - N" is held back, the base is shortened to fit what is left, and the
 * suffix goes back on: "Osc1 - 2".
 */
export function fitPageName(ctx, name, maxWidth) {
    const s = asciiFold(name);
    if (ctx.textWidth(s) <= maxWidth) return s;

    const m = s.match(/^(.*?)( - \d+)$/);
    const base = m ? m[1] : s;
    const suffix = m ? m[2] : "";
    const room = maxWidth - ctx.textWidth(suffix);

    /* A nested name keeps its prefix — "Tone1/" is what distinguishes it from
     * three identical siblings — so shorten only the part after the slash. */
    const slash = base.lastIndexOf("/");
    if (slash > 0) {
        const head = base.slice(0, slash + 1);
        const leaf = shortenLabel(ctx, base.slice(slash + 1), room - ctx.textWidth(head), " ");
        return fitText(ctx, head + leaf + suffix, maxWidth);
    }
    return fitText(ctx, shortenLabel(ctx, base, room, " ") + suffix, maxWidth);
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
 * A first-run hint panel, drawn over the grid until the user does anything.
 *
 * The grid has no room for a permanent footer — it uses all 64 px — and its
 * gestures are not guessable: jog pages, shift+jog jumps a section, click with
 * nothing held opens the section list, holding a knob names it, holding shift
 * reveals every value. A preview nobody can operate produces no useful
 * feedback, so it says so once and then never again.
 *
 * The LINES are supplied by the caller, not baked in: the gestures belong to
 * whoever owns the input mapping, and a tool driving this grid has its own.
 */
export function renderHint(ctx, { rect, lines, title }) {
    const r = rect || { x: 0, y: 0, w: SCREEN_WIDTH, h: SCREEN_HEIGHT };
    /* Five lines plus a title is 53 px of the 64 available; the panel is only
     * up until the first input, so it can afford to be complete. */
    const rows = (lines || []).slice(0, 5);
    const h = (rows.length + 1) * (FONT_H + 1) + 5;
    const y = r.y + Math.floor((r.h - h) / 2);
    const x = r.x + 4;
    const w = r.w - 8;

    /* Punch a hole in the grid so the text is readable over whatever is behind. */
    ctx.fillRect(x, y, w, h, 0);
    ctx.fillRect(x, y, w, 1, 1);
    ctx.fillRect(x, y + h - 1, w, 1, 1);
    ctx.fillRect(x, y, 1, h, 1);
    ctx.fillRect(x + w - 1, y, 1, h, 1);

    let ty = y + 3;
    if (title) {
        ctx.fillRect(x + 1, ty - 1, w - 2, FONT_H + 1, 1);
        centeredText(ctx, x + Math.floor(w / 2), ty, title, 0);
        ty += FONT_H + 2;
    }
    for (const line of rows) {
        ctx.print(x + 3, ty, fitText(ctx, line, w - 6), 1);
        ty += FONT_H + 1;
    }
}

/**
 * The section picker: a scrollable list of the module's sections, drawn over
 * the grid.
 *
 * Rendered here rather than by the host because it is navigation over the page
 * set, which is this library's job — and because a tool with no shadow_ui
 * screens needs it too. Sections rather than pages: minijv has 76 pages and
 * under 25 sections, and a 76-entry list is the same chore in a different
 * shape.
 */
export function renderPicker(ctx, { rect, entries, index, title, header = true }) {
    const r = rect || { x: 0, y: 0, w: SCREEN_WIDTH, h: SCREEN_HEIGHT };
    /* header:false is the MENU PAGE case — the page chrome (drawHeader + the
     * bank bar) has already drawn a header, and drawing a second one under it
     * is two headers for one screen. Body-only also buys a row back. */
    const top = header ? PICKER_LIST_TOP : 0;
    const rows = Math.max(1, Math.floor((r.h - top) / (FONT_H + 2)));
    const total = (entries || []).length;
    /* index < 0 = nothing selected. An INERT menu page previews its entries
     * without claiming one is current, so no row inverts and the window starts
     * at the top. */
    const none = (index | 0) < 0;
    const cur = none ? 0 : Math.max(0, Math.min(total - 1, index | 0));

    /* Keep the selection centred so the list does not jump when it can scroll. */
    let first = cur - Math.floor(rows / 2);
    if (first > total - rows) first = total - rows;
    if (first < 0) first = 0;

    if (header) {
        ctx.fillRect(r.x, r.y, r.w, PICKER_HEADER_H, 1);
        ctx.print(r.x + 1, r.y + HEADER_TEXT_Y, fitText(ctx, title || "Sections", r.w - 40), 0);
        const pos = `${cur + 1}/${total}`;
        ctx.print(r.x + r.w - ctx.textWidth(pos) - 1, r.y + HEADER_TEXT_Y, pos, 0);
    }

    for (let i = 0; i < rows && first + i < total; i++) {
        const e = entries[first + i];
        const y = r.y + top + i * (FONT_H + 2);
        const selected = !none && (first + i) === cur;
        if (selected) ctx.fillRect(r.x, y - 1, r.w, FONT_H + 2, 1);
        /* A section spanning several pages says so, so "Filter" and a 6-page
         * "Oscillator" do not look like the same size of thing. */
        /* A menu entry may carry a `value` instead of a page count — same
         * column, same right alignment, so a settings menu reads like the list
         * it replaces. */
        const count = e.value !== undefined && e.value !== null && e.value !== ""
            ? String(e.value)
            : (e.pages > 1 ? `${e.pages}` : "");
        const cw = count ? ctx.textWidth(count) + 4 : 0;
        ctx.print(r.x + 2, y, fitText(ctx, e.name, r.w - 6 - cw), selected ? 0 : 1);
        if (count) ctx.print(r.x + r.w - ctx.textWidth(count) - 2, y, count, selected ? 0 : 1);
    }
    return { first, rows };
}

/**
 * Header plus the segmented page rule. Exported so a non-grid page kind
 * (preset browser, items list) can reuse the same chrome and the page position
 * stays visible everywhere.
 */
export function drawHeader(ctx, rect, title, pageName, pageIndex, pageCount, o) {
    const x = rect.x, y = rect.y, w = rect.w;

    /* The page name gets first claim on the width. It is what changes as you
     * navigate and therefore what you are reading; the module name is constant
     * and you already know it. Splitting the header evenly truncated both —
     * minijv rendered "MINI-JV  EditT/Ton" with the two colliding. */
    let right = "";
    if (pageName) {
        right = fitPageName(ctx, pageName, Math.floor(w * 0.68));
        ctx.print(x + w - ctx.textWidth(right) - 1, y + HEADER_TEXT_Y, right, 1);
    }
    const leftRoom = w - (right ? ctx.textWidth(right) + 4 : 1);
    if (leftRoom > 8) ctx.print(x + 1, y + HEADER_TEXT_Y, fitText(ctx, title, leftRoom), 1);

    /* One segment per page, the current one taller — and the SEPARATORS carry
     * the section structure that Shift+jog steps through: pages of one section
     * sit flush against each other, and a 1 px gap marks where the next section
     * begins. A wide block is therefore a multi-page section, so the bar reads
     * as a map of the module rather than an undifferentiated row of ticks.
     * (Approach taken from Movy, which solved the same problem first.)
     *
     * A gap is a separator, never a spacer: 1 px or nothing. Leftover pixels go
     * into the SEGMENTS, spread so no two differ by more than one — otherwise
     * the last page ends up several times wider than the rest, which reads as a
     * bug. Not paying for a separator between pages of the same section is also
     * what makes 76 pages fit at all: minijv needs 76 + 15 rather than 76 + 75.
     */
    const ry = y + RULE_Y;
    if (pageCount <= 1) { ctx.fillRect(x, ry, w, 1, 1); return; }

    const groups = (o && o.pageGroups && o.pageGroups.length === pageCount) ? o.pageGroups : null;
    let boundaries = 0;   /* section changes, i.e. how many separators we owe */
    for (let i = 1; i < pageCount; i++) {
        if (!groups || groups[i] !== groups[i - 1]) boundaries++;
    }

    /* Separators are the first thing to go. Section grouping buys back a lot of
     * width — minijv pays 15 boundaries instead of 75 — but at 76 pages across
     * 60 levels even that does not fit, and a visible PAGE matters more than a
     * visible separator. Only when a page cannot have one pixel does the ruler
     * become a position marker. */
    let available = w - boundaries;
    if (Math.floor(available / pageCount) < 1) {
        boundaries = 0;
        available = w;
    }
    const base = Math.floor(available / pageCount);
    if (base < 1) {
        ctx.fillRect(x, ry, w, 1, 1);
        const mx = Math.round(x + (w - 8) * (pageIndex / (pageCount - 1)));
        ctx.fillRect(mx, ry, 8, 3, 1);
        return;
    }
    const separated = boundaries > 0;
    let extra = available - base * pageCount;

    /* Coalesce: pages that sit flush at the same height are one rectangle, not
     * one each. Same pixels, fewer calls. (Measured, a binding crossing is
     * ~490ns, not the 90-100us this library long assumed — see
     * src/shared/draw_bench.mjs — so this is tidiness, not a rescue.) */
    let cx = x;
    let runStart = -1, runW = 0, runH = 0;
    const flush = () => {
        if (runStart >= 0 && runW > 0) ctx.fillRect(runStart, ry, runW, runH, 1);
        runStart = -1; runW = 0;
    };
    for (let i = 0; i < pageCount; i++) {
        const gap = separated && i > 0 && (!groups || groups[i] !== groups[i - 1]);
        if (gap) { flush(); cx += 1; }
        const segW = base + (extra > 0 ? 1 : 0);
        if (extra > 0) extra--;
        const h = i === pageIndex ? 3 : 1;
        if (runStart >= 0 && h === runH) {
            runW += segW;
        } else {
            flush();
            runStart = cx; runW = segW; runH = h;
        }
        cx += segW;
    }
    flush();
}
