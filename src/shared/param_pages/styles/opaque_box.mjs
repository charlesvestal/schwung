/**
 * styles/opaque_box.mjs — SET 6: ten treatments of the opaque cell.
 *
 * An OPAQUE parameter is one a knob cannot turn — a filepath, a canvas, a
 * string. On a knob grid it is a door: you click it and its real editor opens.
 * The shipping widget (`drawOpaqueBox`, render_page_movy.mjs) draws the value's
 * fitted tail and NOTHING ELSE, because the divable corner brackets
 * (`drawDivableMark`, then `drawBrackets`) are its frame.
 *
 * THAT IS THE LOAD-BEARING FACT IN THIS FILE. `tests/host/test_knob_card.sh`
 * pins it on the pixel buffer with a comment saying that dropping
 * `drawDivableMark` looks like an obvious simplification and is not: the box has
 * no frame of its own, so without the brackets an opaque cell is a couple of
 * floating characters. Every option below either draws the brackets itself (1-3) or
 * supplies a replacement frame (4-10), and each note says which. An option that
 * did neither would look fine in a swatch drawn next to its neighbours and be
 * invisible on a real page.
 *
 * The construction it must stay distinct from is the ENUM SQUARE, which is a
 * solid 20x15 frame with text in it. An opaque cell that draws the same box is
 * pixel-identical to a turnable enum, and you find out which is which by
 * turning one and having nothing happen — that is the exact bug the shipping
 * frameless drawing was introduced to fix. So "give it a frame" is not free;
 * the frame has to be a DIFFERENT frame.
 *
 * The axis is minimal -> radical:
 *
 *   1-3   the brackets stay. Only what sits inside them changes.
 *   4-6   the brackets go and a frame of the cell's own arrives — solid,
 *         headed, dashed. Structurally the biggest change, visually moderate.
 *   7-10  the cell becomes a GROUND. The frame is the fill's own silhouette
 *         and the text is knocked out of it.
 *
 * GEOMETRY. `(ctx, kx, ky, value, override)` — `kx` is the left edge of the KW
 * (17) widget box, but the brackets are drawn around the CELL, which is wider.
 * `cellOf` recovers the cell from `kx` using the same centring `drawKnobWidget`
 * applies, so an option can use the full 30px if it wants to. Several do, and
 * that is a real gain: 13px of interior fits three characters and 26px fits six,
 * which is the difference between "KIC" and "KICK_0".
 *
 * ONE FACE FOR ALL TEN. The shipping widget sets its value in the 5x7 device
 * font through `fitDev`; every option below uses the 4x5 face instead, option 1
 * included. That is deliberate and it is the only place this set departs from
 * the incumbent: with two faces in play, "option 4 shows more of the filename"
 * would be a claim about typography as much as about geometry, and the reviewer
 * would end up ranking the font. Held at one face, the extra characters options
 * 4-10 show come purely from using the 30px cell instead of the 17px widget box,
 * which is the thing actually being proposed. The NOW row is the shipping
 * pixels, 5x7 face and all, so the cost of the substitution is visible rather
 * than hidden.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { KW, BOX_H, CELL_W, drawBrackets, drawOpaqueBox } from "../render_page_movy.mjs";
import { fontWidth4x5, fontPrint4x5, FONT4_HEIGHT } from "../font4x5.mjs";
import {
    CHECKER, DIAG_HEAVY, DOTS,
    fillDithered, notchCorners,
} from "./dither.mjs";

/** The cell the widget box is centred in, and the rect the brackets ride. */
function cellOf(kx, ky) {
    const cellX = kx - Math.floor((CELL_W - KW) / 2);
    return { x: cellX + 1, y: ky, w: CELL_W - 2, h: BOX_H };
}

const up = (s) => String(s == null ? "" : s).toUpperCase();

/** The text the cell shows: the override if the grid supplied one, else the
 * value. Mirrors drawOpaqueBox's own precedence. */
function label(value, override) {
    if (override !== null && override !== undefined) return up(override);
    if (value === null || value === undefined) return "--";
    return up(value);
}

/** Truncate to fit, from the tail end of the string — same instinct as the
 * shipping `fitDev`, which keeps the head of a filename. */
function fit(s, maxW) {
    let t = s;
    while (t.length > 1 && fontWidth4x5(t) > maxW) t = t.slice(0, -1);
    return fontWidth4x5(t) > maxW ? "" : t;
}

/** Centre `text` in [x, x+w) and print it. Returns the printed string. */
function centre(ctx, x, y, w, text, color) {
    const t = fit(text, w);
    if (!t) return "";
    fontPrint4x5(ctx, x + Math.floor((w - fontWidth4x5(t)) / 2), y, t, color);
    return t;
}

/** A cleared plate behind knocked-out text, so 1px glyphs keep their silhouette
 * over a dither. Two pixels of horizontal margin, one vertical — the margin the
 * arc-knob set's numeric slab arrived at for the same font at the same size. */
function plate(ctx, x, y, w, text, bounds) {
    const t = fit(text, w);
    if (!t) return "";
    const tw = fontWidth4x5(t);
    const tx = x + Math.floor((w - tw) / 2);
    const px0 = Math.max(bounds.x, tx - 2);
    const px1 = Math.min(bounds.x + bounds.w - 1, tx + tw + 1);
    ctx.fillRect(px0, y - 1, px1 - px0 + 1, FONT4_HEIGHT + 2, 0);
    fontPrint4x5(ctx, tx, y, t, 1);
    return t;
}

const midY = (ky) => ky + Math.floor((BOX_H - FONT4_HEIGHT) / 2);

/* --------------------------------------------------------------- 1..3 --
 * The brackets stay. */

/**
 * 1. brackets — the incumbent, restated as the axis zero.
 *
 * The brackets are drawn here rather than left to the grid because a swatch of
 * the shipping widget alone is a word on a black field with no frame at all —
 * which is exactly the point the file header makes, and showing it framed is
 * the only way this option can be compared like for like with the nine below.
 * The face is 4x5 rather than the shipping 5x7 for the reason the header gives;
 * the NOW row carries the real pixels.
 */
function drawBracketsOnly(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    drawBrackets(ctx, c.x, c.y, c.w, c.h);
    centre(ctx, kx + 2, midY(ky), KW - 4, label(value, override), 1);
}

/**
 * 2. bracket-underline — the value gets a baseline.
 *
 * One solid rule under the text, inset to the widget box. It says "this is a
 * field with something in it" rather than "this is a caption", which is the
 * reading a bare two-character tail currently gets — and it does so without
 * closing a frame, so the cell stays visibly different from an enum square.
 * The lightest thing in the set that changes what the cell MEANS.
 */
function drawBracketUnderline(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    drawBrackets(ctx, c.x, c.y, c.w, c.h);
    const ty = midY(ky) - 1;
    centre(ctx, kx + 2, ty, KW - 4, label(value, override), 1);
    ctx.fillRect(kx + 2, ty + FONT4_HEIGHT + 2, KW - 4, 1, 1);
}

/**
 * 3. bracket-lattice — the value on a barely-there ground.
 *
 * A DOTS(2) lattice fills the widget box behind knocked-out text, so the cell
 * acquires a SURFACE — an area you could point at — without acquiring an edge.
 * Corners notched, because the lattice is a filled box.
 *
 * CELL WIDTH, and DOTS(2) rather than DOTS(3). Both are corrections made after
 * looking at the render, and the second one alone was not enough. A lattice
 * confined to the 17px widget box has the cleared text plate sitting across its
 * whole middle, so the only lit rows that survive are the ones above and below
 * it — the "lattice" rendered as two dotted rules with a word between them, at
 * any pitch. Widening the ground to the 30px cell puts lit pixels either side of
 * the plate on every row, which is what makes it read as a field. The TEXT stays
 * in the widget box and stays the same size as options 1 and 2, so the only
 * thing this option varies is the ground.
 *
 * The weakness is that DOTS is a lattice rather than a hatch, so it is the one
 * fill that shows its own phase: whether a cell's dots land on its corners
 * depends on where the cell is, and the four cells of a row will not agree.
 */
function drawBracketLattice(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    drawBrackets(ctx, c.x, c.y, c.w, c.h);
    fillDithered(ctx, c.x, c.y + 2, c.w, c.h - 4, DOTS(2));
    notchCorners(ctx, c.x, c.y + 2, c.w, c.h - 4);
    plate(ctx, kx + 2, midY(ky), KW - 4, label(value, override), c);
}

/* --------------------------------------------------------------- 4..6 --
 * Its own frame, brackets dropped. */

/**
 * 4. frame-cell — a notched frame at CELL width.
 *
 * NO BRACKETS: this frame replaces them. That is the trade the whole middle of
 * the set is testing, and it buys something concrete — a 1px frame around the
 * 30px cell leaves 26px of interior against the widget box's 13, so the value
 * shows five characters instead of two. "KICK0" is a filename; "KI" is not.
 *
 * The cost is the one the file header names: a framed box with text in it is
 * what an enum square is. The difference here is width — 30px against the enum
 * square's 20 — which is a weaker distinction than the presence or absence of a
 * frame, and it may not survive being seen at true size next to a real enum.
 */
function drawFrameCell(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    ctx.fillRect(c.x, c.y, c.w, 1, 1);
    ctx.fillRect(c.x, c.y + c.h - 1, c.w, 1, 1);
    ctx.fillRect(c.x, c.y, 1, c.h, 1);
    ctx.fillRect(c.x + c.w - 1, c.y, 1, c.h, 1);
    notchCorners(ctx, c.x, c.y, c.w, c.h);
    centre(ctx, c.x + 2, midY(ky), c.w - 4, label(value, override), 1);
}

/**
 * 5. frame-header — a frame with a door mark in its lid.
 *
 * The same cell-width notched frame, plus a solid 4-row band across the top
 * carrying three knocked-out pixels. The ellipsis is the conventional "opens a
 * dialog" mark and it is doing the job the brackets used to: the frame says
 * "field", the lid says "door". Splitting the two makes this the only option
 * in the set where the affordance is stated rather than implied.
 *
 * It also costs four of fifteen rows, so the value sits low in the cell and the
 * whole thing is visibly the heaviest cell on the page.
 */
function drawFrameHeader(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    ctx.fillRect(c.x, c.y, c.w, 1, 1);
    ctx.fillRect(c.x, c.y + c.h - 1, c.w, 1, 1);
    ctx.fillRect(c.x, c.y, 1, c.h, 1);
    ctx.fillRect(c.x + c.w - 1, c.y, 1, c.h, 1);
    ctx.fillRect(c.x + 1, c.y + 1, c.w - 2, 3, 1);
    notchCorners(ctx, c.x, c.y, c.w, c.h);
    const dx = c.x + (c.w >> 1) - 3;
    for (let i = 0; i < 3; i++) ctx.fillRect(dx + i * 3, c.y + 2, 1, 1, 0);
    centre(ctx, c.x + 2, c.y + 7, c.w - 4, label(value, override), 1);
}

/**
 * 6. dashed-frame — a frame that is visibly not solid.
 *
 * NO BRACKETS; the dashed outline is the frame. 2-on-1-off all the way round
 * the cell, which is a real answer to the enum-square collision: a dashed box
 * and a solid box are distinguishable at a glance in a way that two solid boxes
 * of different widths are not, and "dashed = you cannot edit this in place" is
 * a convention people already carry in from every other UI they use.
 *
 * At 1px on a 128x64 panel a dash pattern is close to the limit of what
 * resolves, which is the honest risk: on hardware it may read as a solid frame
 * that failed to render rather than as a dashed one.
 */
function dash(ctx, x, y, w, h, horiz) {
    const n = horiz ? w : h;
    for (let i = 0; i < n; i++) {
        if ((i % 3) === 2) continue;
        ctx.fillRect(horiz ? x + i : x, horiz ? y : y + i, 1, 1, 1);
    }
}
function drawDashedFrame(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    dash(ctx, c.x, c.y, c.w, 1, true);
    dash(ctx, c.x, c.y + c.h - 1, c.w, 1, true);
    dash(ctx, c.x, c.y, 1, c.h, false);
    dash(ctx, c.x + c.w - 1, c.y, 1, c.h, false);
    notchCorners(ctx, c.x, c.y, c.w, c.h);
    centre(ctx, c.x + 2, midY(ky), c.w - 4, label(value, override), 1);
}

/* -------------------------------------------------------------- 7..10 --
 * The fill is the frame. */

/**
 * 7. checker-plate — the cell as a 50% surface.
 *
 * NO BRACKETS AND NO OUTLINE: the frame is the fill's own silhouette, which is
 * what a notched CHECKER rectangle gives you — an edge made of ground rather
 * than of line. Text knocked out on a cleared plate.
 *
 * This is the first option that reads as an object from across the room, and it
 * is also the first that competes with the page: on a grid where every other
 * cell is line work, a 50% block is the loudest thing on screen, and an opaque
 * parameter is rarely the most important one there.
 */
function drawCheckerPlate(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    fillDithered(ctx, c.x, c.y, c.w, c.h, CHECKER);
    notchCorners(ctx, c.x, c.y, c.w, c.h);
    plate(ctx, c.x + 2, midY(ky), c.w - 4, label(value, override), c);
}

/**
 * 8. door-open — a frame with its right edge broken.
 *
 * NO BRACKETS. A solid notched frame whose right side is cut away for five rows
 * with a 3-row chevron sitting in the gap, and the value set left. It is the
 * only option in the set that says which DIRECTION the door goes, and the
 * broken edge means it cannot be confused with an enum square no matter how the
 * widths line up.
 *
 * It is also the most literal thing in the catalog. An arrow leaving a box is a
 * pictogram, and pictograms at 1px either read instantly or read as debris;
 * this one is worth putting in front of a human precisely because that cannot
 * be decided from the code.
 */
function drawDoorOpen(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    const gapY = c.y + ((c.h - 5) >> 1);
    ctx.fillRect(c.x, c.y, c.w, 1, 1);
    ctx.fillRect(c.x, c.y + c.h - 1, c.w, 1, 1);
    ctx.fillRect(c.x, c.y, 1, c.h, 1);
    ctx.fillRect(c.x + c.w - 1, c.y, 1, gapY - c.y, 1);
    ctx.fillRect(c.x + c.w - 1, gapY + 5, 1, c.y + c.h - (gapY + 5), 1);
    notchCorners(ctx, c.x, c.y, c.w, c.h);
    /* The chevron sits IN the gap, not beyond it: a mark outside the cell would
     * land on the neighbouring column. */
    const ax = c.x + c.w - 4;
    for (let i = 0; i < 3; i++) {
        ctx.fillRect(ax + i, gapY + i, 1, 1, 1);
        ctx.fillRect(ax + i, gapY + 4 - i, 1, 1, 1);
    }
    const t = fit(label(value, override), c.w - 9);
    if (t) fontPrint4x5(ctx, c.x + 3, midY(ky), t, 1);
}

/**
 * 9. slab-heavy — a 75% ground on a solid base.
 *
 * NO BRACKETS AND NO OUTLINE: a DIAG_HEAVY fill with notched corners and one
 * solid row along the bottom, which turns the cell into a plinth. The base rule
 * is what stops it reading as a smudge — a dense hatch with no defined edge
 * anywhere is the failure mode of every heavy 1-bit fill, and one solid row
 * gives the shape a floor to sit on.
 *
 * Darker than option 7 and lighter than option 10, which is the whole of its
 * argument: it is the densest an opaque cell can get while the knocked-out
 * value still reads as text on a surface rather than as a hole in a block.
 */
function drawSlabHeavy(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    fillDithered(ctx, c.x, c.y, c.w, c.h - 1, DIAG_HEAVY);
    ctx.fillRect(c.x, c.y + c.h - 1, c.w, 1, 1);
    notchCorners(ctx, c.x, c.y, c.w, c.h);
    plate(ctx, c.x + 2, midY(ky) - 1, c.w - 4, label(value, override), c);
}

/**
 * 10. inverted — the cell is solid and the value is a hole in it.
 *
 * NO BRACKETS AND NO OUTLINE: the frame is the block itself, notched. This is
 * the furthest point on the axis and the argument for it is not aesthetic — an
 * opaque parameter is the only cell on a knob grid whose knob does nothing, and
 * making it the one cell that is unmistakably a different KIND of thing is a
 * defensible use of the screen's strongest signal.
 *
 * The cost is that the strongest signal is a finite resource. Master FX bypass
 * already spends inversion, the footer pill spends it, and a page carrying two
 * opaque params would have two black blocks in it competing with everything
 * else for the same attention. A dotted rule under the text keeps the value
 * from floating in the middle of the block.
 */
function drawInverted(ctx, kx, ky, value, override) {
    const c = cellOf(kx, ky);
    ctx.fillRect(c.x, c.y, c.w, c.h, 1);
    notchCorners(ctx, c.x, c.y, c.w, c.h);
    const ty = midY(ky) - 1;
    const t = fit(label(value, override), c.w - 4);
    if (t) fontPrint4x5(ctx, c.x + Math.floor((c.w - fontWidth4x5(t)) / 2), ty, t, 0);
    /* Knocked out, so it is a dotted rule in reverse — the same separator
     * idiom, spelled in the ground the cell is made of. */
    for (let i = 0; i < c.w - 6; i++)
        if ((i % 2) === 0) ctx.fillRect(c.x + 3 + i, ty + FONT4_HEIGHT + 2, 1, 1, 0);
}

/* --------------------------------------------------------------- probe --
 *
 * A CELL-wide surface, not a KW-wide one. The shipping widget's frame is drawn
 * around the cell by the grid, so a 17px probe would cut the brackets off the
 * three options that keep them and hide the extra room the seven that do not
 * are buying.
 *
 * The value is a real filename rather than a short token: "kick_01.wav" is what
 * this widget actually carries in the fleet, and how badly each option truncates
 * it is one of the things being compared.
 */
const PROBE_KX = Math.floor((CELL_W - KW) / 2);
const PROBE_VALUE = "kick_01.wav";

export function register() {
    return registerSet({
    id: "opaque_box",
    title: "Opaque cell — the door on a knob grid",
    kind: KIND_DRAW,
    replaces: "drawOpaqueBox",
    probeSize: { w: CELL_W, h: BOX_H },
    probe: (ctx, draw) => draw(ctx, PROBE_KX, 0, PROBE_VALUE, null),
    /* The NOW row is drawOpaqueBox WITH the brackets the grid draws round it —
     * without them the baseline swatch is two characters on a black field,
     * which is not what the device shows and would make every option below
     * look like an improvement for the wrong reason. */
    baseline: (ctx, kx, ky, value, override) => {
        const c = cellOf(kx, ky);
        drawBrackets(ctx, c.x, c.y, c.w, c.h);
        drawOpaqueBox(ctx, kx, ky, value, override);
    },
    context: (ctx, draw, info) => {
        const RM = info.RM;
        for (const slot of info.slots) {
            const rowY = slot < 4 ? RM.ROW0_Y : RM.ROW1_Y;
            const cellX = (slot % 4) * RM.CELL_W;
            ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
            draw(ctx, cellX + Math.floor((RM.CELL_W - RM.KW) / 2), rowY, PROBE_VALUE, null);
        }
    },
    options: [
        {
            position: 1, id: "brackets", name: "Brackets", draw: drawBracketsOnly,
            note: "The incumbent construction restated as the axis zero: the value's fitted tail, framed only by the divable corner brackets the grid draws around it. The brackets are drawn here because without them it is a bare word on a black field — drawOpaqueBox has no frame of its own, which test_knob_card.sh pins on the pixel buffer. Set in the 4x5 face like the other nine so the set varies only frame and ground; the NOW row above is the shipping 5x7 pixels. Its weakness is legibility of PURPOSE rather than of pixels: 13px of interior is three characters, so a filename arrives as KIC.",
        },
        {
            position: 2, id: "bracket-underline", name: "Bracket underline", draw: drawBracketUnderline,
            note: "Brackets kept, plus one solid rule under the value. It says field-with-something-in-it rather than caption, which is the reading a bare two-character tail currently gets, and it does so without closing a frame — so the cell stays visibly distinct from a turnable enum square. The lightest change in the set that alters what the cell MEANS rather than how it looks.",
        },
        {
            position: 3, id: "bracket-lattice", name: "Bracket lattice", draw: drawBracketLattice,
            note: "Brackets kept, with a DOTS(2) lattice behind knocked-out text. The cell gains a surface without gaining an edge, which is the gentlest way to make it look like an area you can point at rather than a caption floating in space. Real weakness: DOTS is a lattice rather than a hatch, so it shows its own phase — whether a cell's dots land on its corners depends where the cell is, and the four cells of a row will not agree.",
        },
        {
            position: 4, id: "frame-cell", name: "Frame cell", draw: drawFrameCell,
            note: "DROPS THE BRACKETS and supplies its own notched frame at cell width — this frame IS the replacement, which is the trade the middle of the set is testing. It buys real room: 26px of interior against the widget box's 13, so the value shows five characters instead of two (KICK0, not KI). The cost is that a framed box with text in it is what an enum square already is, and the only thing distinguishing them here is width.",
        },
        {
            position: 5, id: "frame-header", name: "Frame header", draw: drawFrameHeader,
            note: "DROPS THE BRACKETS for its own notched frame plus a solid lid carrying a knocked-out ellipsis. The frame says field and the lid says door, which makes this the one option where the affordance is stated rather than implied — the brackets it replaces were doing both jobs at once. It spends four of fifteen rows on the lid, so the value sits low and the cell is visibly the heaviest on the page.",
        },
        {
            position: 6, id: "dashed-frame", name: "Dashed frame", draw: drawDashedFrame,
            note: "DROPS THE BRACKETS; the dashed outline is the frame. A real answer to the enum-square collision, because a dashed box and a solid box separate at a glance in a way two solid boxes of different widths do not, and dashed-means-not-editable-here is a convention people arrive with. The risk is resolution: at 1px on this panel a 3px dash period is near the limit, and on hardware it may read as a solid frame that failed to render.",
        },
        {
            position: 7, id: "checker-plate", name: "Checker plate", draw: drawCheckerPlate,
            note: "NO BRACKETS AND NO OUTLINE — the frame is the fill's own silhouette, a notched CHECKER rectangle with the value knocked out on a cleared plate. The first option that reads as an object from across the room, and the first that competes with the page for attention: on a grid of line work a 50% block is the loudest thing on screen, and an opaque param is rarely the most important cell there.",
        },
        {
            position: 8, id: "door-open", name: "Door open", draw: drawDoorOpen,
            note: "NO BRACKETS. A solid notched frame with its right edge cut away and a chevron sitting in the gap, value set left. The only option that says which DIRECTION the door goes, and the broken edge means it cannot be confused with an enum square however the widths fall. It is also the most literal thing in the catalog — an arrow leaving a box is a pictogram, and at 1px a pictogram either reads instantly or reads as debris, which is not decidable from the code.",
        },
        {
            position: 9, id: "slab-heavy", name: "Slab heavy", draw: drawSlabHeavy,
            note: "NO BRACKETS AND NO OUTLINE: a DIAG_HEAVY ground with notched corners on one solid base row, which is what stops a dense hatch reading as a smudge. It is the densest an opaque cell can get while the knocked-out value still reads as text on a surface rather than as a hole in a block — that is the whole argument, and it makes this the natural compromise between options 7 and 10.",
        },
        {
            position: 10, id: "inverted", name: "Inverted", draw: drawInverted,
            note: "NO BRACKETS AND NO OUTLINE: the frame is the solid block itself, notched, with the value and a reversed dotted rule knocked out of it. The end of the axis, and the case is structural rather than aesthetic — the opaque cell is the only one on a knob grid whose knob does nothing, so making it the one cell that is unmistakably a different kind of thing uses the screen's strongest signal on its clearest distinction. The cost is that the signal is finite: bypass and the footer pill already spend inversion, and a page with two opaque params would carry two black blocks fighting everything else.",
        },
    ],
    });
}
