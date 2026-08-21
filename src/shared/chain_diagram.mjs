/**
 * chain_diagram.mjs — the row of boxes at the top of the chain editor.
 *
 * Pure: takes a draw context (`fillRect` / `print` / `textWidth`, the shape
 * every other renderer here takes) and a list of positions from
 * chain_model.mjs. No device globals, no param reads, no state — so the whole
 * diagram can be rendered into tools/param-pages/harness.mjs and inspected
 * pixel by pixel, which is the only way to catch the failures it actually has:
 * a box past the right edge, a synth that looks like an FX, a `+` that looks
 * like an empty module position.
 *
 * The scroll is HYBRID, per the design: while the whole chain fits, nothing
 * scrolls and the synth sits where the user drew it; past that the window
 * follows the SELECTION, because a jog-driven editor must never be pointing at
 * something off-screen. Both halves live in chain_model.mjs's scrollWindow.
 */

import { scrollWindow } from "./chain_model.mjs";

export const BOX_W = 22;
export const BOX_H = 16;
export const GAP = 2;

/** The strip the boxes live in — right of the four slot indicators, which own
 *  x 0..3. Unchanged from the fixed five-box diagram, so the screen the user
 *  knows keeps its proportions as the chain grows past it. */
export const DIAGRAM_W = 118;
export const DEFAULT_X = 8;

/**
 * 14, not the old 20: the movy header band is 7 rows against the old header's
 * ~18. Not higher, though — the LFO tilde and the bypass "B" draw at
 * `y + MARK_DY`, so 11 would have put them at row 5, inside the header band.
 * 14 lands them at 8-11, one row clear of it. The whole column:
 *   0-6 header | 8-11 marks | 14-29 boxes | 33-39 label | 44-50 info
 *   55 rule | 56-63 footer
 */
export const DEFAULT_Y = 14;
export const MARK_DY = -6;

/* The synth landmark: a filled band across the top of the box. 2px reads as
 * deliberate at this size where 1px reads as a drawing artefact, and the label
 * sits at y+5 so there is no chance of overlap. */
export const SYNTH_BAND_H = 2;

/**
 * How many boxes fit.
 *
 * `floor(DIAGRAM_W / (BOX_W + GAP))` is the obvious formula and is off by one:
 * the LAST box has no gap after it, so the strip holds one more than dividing
 * by the pitch suggests. At 118px that is the difference between 4 boxes and
 * the 5 the screen has always shown.
 */
export const CAPACITY = Math.floor((DIAGRAM_W + GAP) / (BOX_W + GAP));

/* The device gives the renderers fillRect/print/textWidth and nothing else, so
 * single pixels go through a 1x1 rect unless the caller hands us a real
 * setPixel (shadow_ui.js does; the harness's drawContext does not). */
const pixelFn = (ctx) =>
    (typeof ctx.setPixel === "function"
        ? ctx.setPixel
        : (x, y, c) => ctx.fillRect(x, y, 1, 1, c));

const measure = (ctx, text) =>
    (typeof ctx.textWidth === "function" ? ctx.textWidth(text) : String(text).length * 5);

/**
 * The widest prefix of `text` that fits in `room` pixels.
 *
 * Truncates rather than scaling or scrolling: a box is 22px and holds two or
 * three characters, so there is no room for an ellipsis and nothing to animate.
 * The full name is on the label line under the diagram either way.
 *
 * Always returns at least one character. A label clipped to "9" is wrong but
 * legible and inside its box; an empty box says the position is empty, which is
 * a different and worse lie.
 */
export function fitAbbrev(ctx, text, room) {
    let s = String(text === null || text === undefined ? "" : text);
    if (!s) return "";
    while (s.length > 1 && measure(ctx, s) > room) s = s.slice(0, -1);
    return s;
}

/** A 1px outline. `dash` > 0 draws every `dash`-th pixel only. */
function outline(ctx, x, y, w, h, color, dash) {
    if (!dash) {
        ctx.fillRect(x, y, w, 1, color);
        ctx.fillRect(x, y + h - 1, w, 1, color);
        ctx.fillRect(x, y, 1, h, color);
        ctx.fillRect(x + w - 1, y, 1, h, color);
        return;
    }
    const px = pixelFn(ctx);
    for (let i = 0; i < w; i++) {
        if (i % dash) continue;
        px(x + i, y, color);
        px(x + i, y + h - 1, color);
    }
    for (let i = 0; i < h; i++) {
        if (i % dash) continue;
        px(x, y + i, color);
        px(x + w - 1, y + i, color);
    }
}

/**
 * The default two-character label inside a box.
 *
 * The real editor overrides this with the module's declared `abbrev` (see
 * getModuleAbbrev), which it can only do because it has read module.json. The
 * fallback is the same one that has: the first two characters, capitalised.
 */
export function defaultAbbrev(comp) {
    if (!comp) return "--";
    if (comp.kind === "add") return "+";
    if (comp.kind === "settings") return "*";
    if (comp.kind === "patch") return "P";
    const mod = comp.module && comp.module.module;
    return mod ? String(mod).substring(0, 2).toUpperCase() : "--";
}

/**
 * Which components are on screen and where each one lands.
 *
 * Split out from the drawing so the caller — and the test — can ask about the
 * geometry without a framebuffer. `selectedIndex` of -1 means the chain itself
 * is selected (the editor's patch selection), which scrolls to the start.
 */
export function layoutChainDiagram(components, selectedIndex, opts = {}) {
    const x = opts.x == null ? DEFAULT_X : opts.x;
    const y = opts.y == null ? DEFAULT_Y : opts.y;
    const total = components.length;
    const win = scrollWindow(total, selectedIndex < 0 ? 0 : selectedIndex, CAPACITY);
    const count = Math.min(win.count, total - win.first);
    /*
     * A chain SHORTER than the window is centred, not left-aligned.
     *
     * The strip is sized for CAPACITY boxes, so anything shorter used to hug
     * the left edge with the whole remainder as dead space on the right —
     * reported as "weirdly smooshed to the left". A slot chain never hit it:
     * its shortest possible form is patch + `+` + synth + `+` + settings,
     * which is exactly CAPACITY, so it always fills the strip. Master FX is a
     * single section and starts at two boxes, so it is nearly always short.
     *
     * This is deliberately NOT a scroll offset — it only applies when nothing
     * is scrolled (a short chain has no off-window positions), so it cannot
     * fight `scrollWindow`, and `scrolledLeft`/`scrolledRight` stay false.
     */
    const pad = count < CAPACITY
        ? Math.floor(((CAPACITY - count) * (BOX_W + GAP)) / 2)
        : 0;
    return {
        first: win.first,
        count,
        x: x + pad, y,
        boxW: BOX_W, boxH: BOX_H,
        capacity: CAPACITY,
        scrolledLeft: win.first > 0,
        scrolledRight: win.first + count < total,
        /** Screen x of component `index` (an index into `components`, not into
         *  the window — off-window indices are simply not drawn). */
        boxX: (index) => x + pad + (index - win.first) * (BOX_W + GAP),
    };
}

/**
 * Draw the diagram.
 *
 * @param ctx        fillRect / print / textWidth (+ optional setPixel)
 * @param components chain_model.mjs positions, in signal order
 * @param selectedIndex index into `components`, or -1 for "the whole chain"
 * @param opts       { x, y, allSelected, abbrev(comp), marks(comp) }
 *                   `marks` returns { bypassed, lfo1, lfo2 } for a position.
 * @returns the layout, so a caller can put its own decoration on a box
 */
export function drawChainDiagram(ctx, components, selectedIndex, opts = {}) {
    const lay = layoutChainDiagram(components, selectedIndex, opts);
    const abbrevOf = opts.abbrev || defaultAbbrev;
    const marksOf = opts.marks || (() => null);
    const px = pixelFn(ctx);

    for (let i = lay.first; i < lay.first + lay.count; i++) {
        const comp = components[i];
        const x = lay.boxX(i);
        const y = lay.y;
        const selected = opts.allSelected || i === selectedIndex;

        if (comp.kind === "add") {
            /* Dotted, ALWAYS — including while selected. A solid outline would
             * read as an empty module position, which is a different thing:
             * one of them holds a seat in the chain and the other offers a new
             * one. Selection fills the interior instead, inside the dots. */
            outline(ctx, x, y, BOX_W, BOX_H, 1, 2);
            if (selected) ctx.fillRect(x + 2, y + 2, BOX_W - 4, BOX_H - 4, 1);
        } else if (selected) {
            ctx.fillRect(x, y, BOX_W, BOX_H, 1);
        } else {
            outline(ctx, x, y, BOX_W, BOX_H, 1, 0);
        }

        /*
         * The synth wears a filled band across its top, drawn in whichever
         * colour the box is NOT. It is the landmark the scroll leans on — once
         * the chain is longer than the screen it is the only orientation left —
         * so it has to stay distinguishable while selected too.
         *
         * A BAND, not a ring. The ring this replaces was inset on all four
         * sides, which cost width the label needed: a three-character abbrev
         * ("9W9") is ~17px against the 16px a ring left, so the label collided
         * with its own landmark. A horizontal band costs no width at all, so
         * the synth gets the same room as every other box and the collision
         * cannot come back by arithmetic.
         */
        if (comp.kind === "synth") {
            ctx.fillRect(x + 1, y + 1, BOX_W - 2, SYNTH_BAND_H, selected ? 0 : 1);
        }

        /*
         * Fit the label to the room INSIDE whatever was just drawn.
         *
         * Module abbrevs come from module.json (getModuleAbbrev), so their
         * length is a third party's choice, not ours. Centring without fitting
         * gave a negative x for anything wider than the box — a four-character
         * abbrev started outside its own box and ran over the neighbour. And
         * the synth's inner ring is not accounted for by the box width at all.
         */
        /* Every box now has the same label room: the synth mark is horizontal,
         * so it takes none. */
        const room = BOX_W - 2;
        const abbrev = fitAbbrev(ctx, String(abbrevOf(comp) || "--"), room);
        /* Selection inverts the label whatever the box is: the `+` box fills
         * its interior rather than the whole rect, but the label sits in that
         * interior, so a white "+" on it would simply vanish. */
        const textColor = selected ? 0 : 1;
        const inset = Math.floor((BOX_W - room) / 2);
        ctx.print(x + inset + Math.max(0, Math.floor((room - measure(ctx, abbrev)) / 2)),
                  y + 5, abbrev, textColor);

        drawMarks(ctx, px, x, y, marksOf(comp));
    }

    /* A dotted rail in the margin either side, so "there is more chain that
     * way" is visible without stealing a box's worth of width for a chevron —
     * there isn't one to spare: five boxes fill the strip exactly. */
    if (lay.scrolledLeft) dottedRail(px, lay.x - 2, lay.y, BOX_H);
    if (lay.scrolledRight) dottedRail(px, lay.x + DIAGRAM_W + 1, lay.y, BOX_H);

    return lay;
}

function dottedRail(px, x, y, h) {
    for (let i = 0; i < h; i += 2) px(x, y + i, 1);
}

/**
 * The bypass "B" and the LFO `~1` / `~2` / `~1+2` tell-tales above a box.
 *
 * 4px-high hand-plotted glyphs rather than the font: they sit in a 4-row band
 * between the header and the boxes, and the smallest real face is 5.
 */
function drawMarks(ctx, px, x, y, marks) {
    if (!marks) return;
    const my = y + MARK_DY;

    if (marks.bypassed) {
        /* Left-aligned so it does not collide with the centred LFO mark when a
         * module is both bypassed and modulated. "B": ##. / #.# / ##. / ### */
        const bx = x + 1;
        px(bx, my, 1); px(bx + 1, my, 1);
        px(bx, my + 1, 1); px(bx + 2, my + 1, 1);
        px(bx, my + 2, 1); px(bx + 1, my + 2, 1);
        px(bx, my + 3, 1); px(bx + 1, my + 3, 1); px(bx + 2, my + 3, 1);
    }

    const has1 = !!marks.lfo1, has2 = !!marks.lfo2;
    if (!has1 && !has2) return;

    let cx = x + Math.floor(BOX_W / 2);
    cx -= (has1 && has2) ? 7 : 3;
    /* tilde: .... / .#.# / #.#. / .... */
    px(cx + 1, my + 1, 1); px(cx + 3, my + 1, 1);
    px(cx, my + 2, 1); px(cx + 2, my + 2, 1);

    let dx = cx + 5;
    const one = () => {
        px(dx + 1, my, 1);
        px(dx, my + 1, 1); px(dx + 1, my + 1, 1);
        px(dx + 1, my + 2, 1);
        px(dx + 1, my + 3, 1);
        dx += 3;
    };
    const two = () => {
        px(dx, my, 1); px(dx + 1, my, 1);
        px(dx + 2, my + 1, 1);
        px(dx + 1, my + 2, 1);
        px(dx, my + 3, 1); px(dx + 1, my + 3, 1); px(dx + 2, my + 3, 1);
        dx += 3;
    };
    if (has1) one();
    if (has1 && has2) {
        px(dx, my + 2, 1);
        px(dx + 1, my + 1, 1); px(dx + 1, my + 2, 1); px(dx + 1, my + 3, 1);
        px(dx + 2, my + 2, 1);
        dx += 4;
    }
    if (has2) two();
}
