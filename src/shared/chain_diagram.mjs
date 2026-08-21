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

/**
 * 21, not 22, and the parity is the point.
 *
 * Everything centred in a box is ODD: a two-letter shortcode is 11px of ink
 * (34 of the 36 glyphs advance 6, so 5 + 1 + 5), 9 when a letter is narrow
 * like MI, and the settings icon is 11. An even box cannot centre an odd
 * thing — 22 - 11 = 11, so the label sits half a pixel left, every time, on
 * every box. 21 - 11 = 10 centres exactly, and so does 21 - 9.
 */
export const BOX_W = 21;
export const BOX_H = 17;
export const GAP = 2;

/**
 * The strip the boxes live in — right of the four slot indicators, which own
 * x 0..3.
 *
 * 120 at x=6, from 118 at x=8. The budget is tighter than it looks: the scroll
 * rails need a column on EACH side (`lay.x - 2` and `lay.x + DIAGRAM_W + 1`),
 * and the four slot indicators own x 0..3, so the strip cannot start before 6
 * without the left rail landing on them.
 *
 * SETTINGS_GAP then pushes the last box to x+120..x+121, one column past the
 * strip — which is fine, and not by luck. The RIGHT RAIL and the settings box
 * can never both be drawn: the rail means "there is more to the right", and
 * settings is always the last position, so when it is on screen there is
 * nothing beyond it. The two share the column because they are mutually
 * exclusive, and a test pins that so a future position after settings cannot
 * quietly break it.
 *
 * That mattered more than the pixels: three separate tests assert that NOTHING
 * is drawn outside the 128x64 display, and that assertion has caught two real
 * overruns. Letting the box clip would have meant an exception in each of them
 * — three guards weakened to buy a gap the layout could afford outright.
 *
 * x=6 keeps two clear columns beside the indicators, and 6+122 = 128 exactly.
 */
export const DIAGRAM_W = 120;
export const DEFAULT_X = 6;

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

/*
 * The synth landmark: filled bands across the TOP AND BOTTOM of the box.
 *
 * Two rules rather than one, which the 17px box has room for: unselected they
 * sit at rows 1 and 15 with the label at 5..11, so three clear rows either
 * side; selected they are 2px at 1..2 and 14..15, still two clear rows. A
 * pair reads as a band AROUND the box where a single rule reads as a lid.
 *
 * THINNER when the box is unselected, and the asymmetry is the point. On an
 * outlined box the band sits directly under the outline, so at 2px the two
 * merge into one thick top edge and the landmark stops reading as a band at
 * all -- 1px there is separated by the interior and reads cleanly. On a
 * SELECTED box the band is knocked out of a solid fill with nothing above it
 * to confuse it with, and 1px of black in white is a scratch; it needs 2.
 */
export const SYNTH_BAND_H = 2;
export const SYNTH_BAND_H_UNSELECTED = 1;

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

/**
 * The device font puts one blank column AFTER every glyph, including the last,
 * so an advance is one wider than the ink it draws. Centring has to subtract
 * it or everything sits half a glyph left; fitting must NOT, because the
 * trailing column is real space the next glyph would need.
 */
const CHAR_ADVANCE_TAIL = 1;

/**
 * Where a label sits vertically.
 *
 * The device font draws a 7-row cell, so centring the CELL centres every
 * label consistently — a `+` is 5 rows of ink inside that cell and stays put
 * relative to the letters, which is what keeps a row of mixed boxes on one
 * line.
 *
 * This was a hardcoded `y + 5`, one pixel low on everything: caps came out
 * 5 above / 4 below, the `+` 6/5, the empty `--` 8/7. BOX_H is even and the
 * cell is odd so exact is impossible; 4/5 puts the extra pixel BELOW, which
 * is where the eye expects it for capitals with no descenders.
 */
const FONT_CELL_H = 7;
const LABEL_DY = Math.floor((BOX_H - FONT_CELL_H) / 2);

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
    /*
     * ONE continuous path around the rect, not four independent runs.
     *
     * Four runs each restarting the dash at 0 put two dots on the top-left
     * corner, one on each of the other two — and NONE on the bottom-right,
     * because with an even width and an even height neither the horizontal
     * run (w-1 odd) nor the vertical one (h-1 odd) lands on it. A dotted box
     * with a missing corner and a doubled one, which is what it looked like.
     *
     * Walking the perimeter as a single path spaces the dashes evenly THROUGH
     * the corners. It closes cleanly when the perimeter 2*(w-1) + 2*(h-1) is a
     * multiple of `dash`; at 21x16 that is 70, so a 2px dash meets itself.
     *
     * TWO of the four corners land on a dash, not four. For all four, both
     * w-1 and h-1 must be even, so BOX_H would have to be odd — and an odd
     * height decentres the 10px settings icon vertically. Asked which mattered
     * more, Charles said the icon. So the height stays even and the corners
     * stay as they fall; do not "fix" this by making BOX_H odd.
     */
    const px = pixelFn(ctx);
    let n = 0;
    const step = (sx, sy) => { if (n % dash === 0) px(sx, sy, color); n++; };
    for (let i = 0; i < w - 1; i++) step(x + i, y);
    for (let i = 0; i < h - 1; i++) step(x + w - 1, y + i);
    for (let i = w - 1; i > 0; i--) step(x + i, y + h - 1);
    for (let i = h - 1; i > 0; i--) step(x, y + i);
}

/**
 * The settings box shows three faders, not a character.
 *
 * `*` was a stand-in. The box is a door to volume, the LFOs and the preset
 * actions — a mixer strip, not machinery — and at this size a fader icon is
 * the only thing that survives: every feature is 1px, so it stays legible
 * where a gear turns into a dotted ring or a solid blob, and it still reads
 * when the box inverts under selection.
 *
 * 11 wide and therefore half a pixel off centre in a 22px box, deliberately.
 * A symmetric icon with a CENTRAL element — the middle fader — wants an odd
 * width, and odd inside even can never be exact. Widening the icon would move
 * the middle fader off its own centre, which is the more visible error; the
 * alternative was a 21px box, which fits but would move every label and every
 * snapshot to buy half a pixel.
 */
const SETTINGS_ICON = [
    ".#...#...#.",
    ".#..###..#.",
    ".#..#.#..#.",
    ".#..###..#.",
    ".#...#...#.",
    ".#...#...#.",
    "###..#..###",
    "#.#..#..#.#",
    "###..#..###",
    ".#...#...#.",
    ".#...#...#.",
];
const SETTINGS_ICON_W = SETTINGS_ICON[0].length;
const SETTINGS_ICON_H = SETTINGS_ICON.length;

/** Centred in the box, in whichever colour the box is not. */
function drawSettingsIcon(px, x, y, color) {
    const ix = x + Math.floor((BOX_W - SETTINGS_ICON_W) / 2);
    const iy = y + Math.floor((BOX_H - SETTINGS_ICON_H) / 2);
    for (let r = 0; r < SETTINGS_ICON_H; r++) {
        const row = SETTINGS_ICON[r];
        for (let c = 0; c < SETTINGS_ICON_W; c++) {
            if (row[c] === "#") px(ix + c, iy + r, color);
        }
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
 * The settings box is not a chain position, so it is pushed away from the ones
 * that are.
 *
 * Reported as the settings box reading like just another module slot. The
 * extra space is the whole separation — there is no divider glyph, because a
 * gap already means "different kind of thing" everywhere else on this screen.
 *
 * TWO deliberate consequences, both chosen over reflowing the strip:
 *
 * The gap is paid for by the strip, not by the edge: DIAGRAM_W was widened from
 * 118 to 122 using slack that was already there (see above). So it applies
 * always — including while settings is selected — and nothing clips. An
 * earlier version let the box run off the display instead, which would have
 * needed a clipping exception in each of the three tests that assert nothing
 * is drawn outside it.
 */
export const SETTINGS_GAP = 4;

function settingsShift(components, index, first) {
    for (let i = first + 1; i <= index; i++) {
        const c = components[i];
        if (c && c.kind === "settings") return SETTINGS_GAP;
    }
    return 0;
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
        boxX: (index) => x + pad + (index - win.first) * (BOX_W + GAP)
                       + settingsShift(components, index, win.first),
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
        const bw = BOX_W;
        const selected = opts.allSelected || i === selectedIndex;

        if (comp.kind === "add") {
            /* Dotted, ALWAYS — including while selected. A solid outline would
             * read as an empty module position, which is a different thing:
             * one of them holds a seat in the chain and the other offers a new
             * one. Selection fills the interior instead, inside the dots. */
            outline(ctx, x, y, bw, BOX_H, 1, 2);
            if (selected) ctx.fillRect(x + 2, y + 2, bw - 4, BOX_H - 4, 1);
        } else if (selected) {
            ctx.fillRect(x, y, bw, BOX_H, 1);
        } else {
            outline(ctx, x, y, bw, BOX_H, 1, 0);
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
            const bandH = selected ? SYNTH_BAND_H : SYNTH_BAND_H_UNSELECTED;
            const bandColor = selected ? 0 : 1;
            ctx.fillRect(x + 1, y + 1, bw - 2, bandH, bandColor);
            ctx.fillRect(x + 1, y + BOX_H - 1 - bandH, bw - 2, bandH, bandColor);
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
        /* The settings box is an ICON, not a label — see SETTINGS_ICON. Drawn
         * before the marks so a bypass B or an LFO tilde still lands on top. */
        if (comp.kind === "settings") {
            drawSettingsIcon(px, x, y, selected ? 0 : 1);
            drawMarks(ctx, px, x, y, marksOf(comp), bw);
            continue;
        }

        const room = bw - 2;
        const abbrev = fitAbbrev(ctx, String(abbrevOf(comp) || "--"), room);
        /* Selection inverts the label whatever the box is: the `+` box fills
         * its interior rather than the whole rect, but the label sits in that
         * interior, so a white "+" on it would simply vanish. */
        const textColor = selected ? 0 : 1;
        /*
         * Centre the INK in the BOX. Two things used to push every label two
         * pixels left of centre, and they stacked:
         *
         *   - `textWidth` is an ADVANCE, not an ink width: it counts the
         *     trailing inter-character space after the last glyph. Centring a
         *     12-wide advance whose ink is 11 loses a pixel to the right.
         *   - centring inside `room` (bw - 2) and then adding the 1px inset
         *     re-centres within a narrower box, which is the same thing as
         *     shifting left again.
         *
         * `room` is the FIT budget -- it keeps a clear column each side so a
         * long abbrev cannot touch the outline -- and it has no business
         * deciding the position. Measured: OB was 4/6, `+` was 7/9, MI 5/7.
         */
        const ink = Math.max(0, measure(ctx, abbrev) - CHAR_ADVANCE_TAIL);
        ctx.print(x + Math.max(0, Math.floor((bw - ink) / 2)), y + LABEL_DY, abbrev, textColor);

        drawMarks(ctx, px, x, y, marksOf(comp), bw);
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
function drawMarks(ctx, px, x, y, marks, boxW = BOX_W) {
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

    let cx = x + Math.floor(boxW / 2);
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
