/**
 * styles/enum_square.mjs — SET 4: ten treatments of the enum square.
 *
 * An ENUM is the one knob-grid cell whose value is a WORD. It cannot be drawn
 * as an arc or a bar because "SINE" has no position on a travel, so the
 * shipping widget (`drawEnumSquare`, render_page_movy.mjs) draws a 20x15 solid
 * 1px frame with the value set in it, split onto two 3-character lines by
 * `enumSquareLines`. That construction — a boxed word in a grid of dials — is
 * one of the two most Elektron-reading things on the screen, which is why this
 * set exists.
 *
 * WHY IT COMES LAST, WITH label_cell. Both sets are text-bearing, and set 12
 * (font/) replaces the letterforms outright. Sizing a frame against the
 * shipping face and then swapping the face underneath it is how you ship an
 * option that fitted in the catalog and overflows on the device, so these two
 * waited for the fonts to land. Every note below names the variants the option
 * actually overflows, measured through `styles/font/blit.mjs` against the same
 * text budget the draw function uses, and
 * `tests/host/test_style_catalog.sh` asserts that list exactly rather than
 * asserting a floor it could pass by being loose.
 *
 * ONE FACE FOR ALL TEN, and here that costs nothing: `drawEnumSquare` already
 * sets its lines in font4x5, so unlike the opaque cell there is no
 * substitution and the NOW row is directly comparable with the ten under it.
 *
 * THE REAL CONSTRAINT IS 16 PIXELS OF LINE. `ENUM_TEXT_W` is 16, three
 * characters, and `enumSquareLines` was written around exactly that. Every
 * pixel an option spends on frame is a pixel off the line, and at three
 * characters the difference between 16px and 20px is whether "PAS" survives at
 * all on a wide face. So the axis here is not only decorative — options 2, 3
 * and 5 buy real text width by dropping or absorbing the frame, and options 4,
 * 6, 9 and 10 pay for their construction out of the same 16px.
 *
 * The axis is minimal -> radical:
 *
 *   1-3   the frame thins, then goes. Text on the field, with at most a rule.
 *   4-8   a frame of some other kind — brackets, a heavy rule, a dotted one,
 *         an open-bottomed tab.
 *   9-10  the cell becomes a GROUND: a hatch behind the text, or a solid slab
 *         with the text knocked out of it.
 *
 * CORNER NOTCHES ARE NOT A VARIABLE. Every filled or framed box here is
 * notched, because the user has already said rounded corners stay ("i also do
 * like rounded corners on stuff tho that is pretty elektronny"). An option
 * whose only content was adding or removing them would waste one of ten slots
 * on a question already answered.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { ENUM_W, BOX_H, drawBrackets, drawEnumSquare } from "../render_page_movy.mjs";
import { enumSquareLines } from "../font5x3.mjs";
import { fontWidth4x5, fontPrint4x5, FONT4_HEIGHT } from "../font4x5.mjs";
import {
    CHECKER, DIAG_LIGHT,
    fillDithered, dottedRule, dashedVRule, notchCorners,
} from "./dither.mjs";

/* Two lines of 5 rows with one clear row between them. `drawEnumSquare`'s own
 * arithmetic, restated so an option can size a slab or a plate against it. */
const LINE_PITCH = 6;
const TWO_LINE_H = FONT4_HEIGHT + LINE_PITCH;   /* 11 */

/** Trim until it fits, MEASURED — the face is proportional (I is 1px, W is 5),
 * so "three characters" is not a width. Same loop as `fitLine`. */
function fit(s, maxW) {
    let t = String(s == null ? "" : s);
    while (t.length > 1 && fontWidth4x5(t) > maxW) t = t.slice(0, -1);
    return fontWidth4x5(t) > maxW ? "" : t;
}

/** The two fitted lines an option will actually print, plus their extent. */
function lines(text, maxW) {
    const [r1, r2] = enumSquareLines(text);
    const l1 = fit(r1, maxW);
    const l2 = fit(r2, maxW);
    const w = Math.max(fontWidth4x5(l1), fontWidth4x5(l2));
    return { l1, l2, w, h: l2.length ? TWO_LINE_H : FONT4_HEIGHT };
}

/**
 * Print the value centred in a text box. `box` is the area the option has left
 * for text after its frame; every option differs only in what it passes here
 * and in what it draws around it.
 */
function enumText(ctx, box, text, color) {
    const L = lines(text, box.w);
    const sy = box.y + Math.floor((box.h - L.h) / 2);
    if (L.l1) fontPrint4x5(ctx, box.x + Math.floor((box.w - fontWidth4x5(L.l1)) / 2), sy, L.l1, color);
    if (L.l2) fontPrint4x5(ctx, box.x + Math.floor((box.w - fontWidth4x5(L.l2)) / 2), sy + LINE_PITCH, L.l2, color);
    return L;
}

/** A cleared plate behind each line, so 1px glyphs keep their silhouette over
 * a dither. Two pixels of horizontal margin — the same margin the opaque-cell
 * set arrived at for this face at this size. */
function enumPlate(ctx, box, text, bounds) {
    const L = lines(text, box.w);
    const sy = box.y + Math.floor((box.h - L.h) / 2);
    const clear = (t, y) => {
        if (!t) return;
        const tw = fontWidth4x5(t);
        const tx = box.x + Math.floor((box.w - tw) / 2);
        const x0 = Math.max(bounds.x, tx - 2);
        const x1 = Math.min(bounds.x + bounds.w - 1, tx + tw + 1);
        ctx.fillRect(x0, y - 1, x1 - x0 + 1, FONT4_HEIGHT + 2, 0);
        fontPrint4x5(ctx, tx, y, t, 1);
    };
    clear(L.l1, sy);
    clear(L.l2, sy + LINE_PITCH);
    return L;
}

function frame1(ctx, x, y, w, h) {
    ctx.fillRect(x, y, w, 1, 1);
    ctx.fillRect(x, y + h - 1, w, 1, 1);
    ctx.fillRect(x, y, 1, h, 1);
    ctx.fillRect(x + w - 1, y, 1, h, 1);
}

/* ---------------------------------------------------------------- 1..3 --
 * The frame thins, then goes. */

/**
 * 1. thin-frame — the incumbent with its margins closed up.
 *
 * The same 1px frame, but the text box runs to the inside of the frame instead
 * of holding a 2px margin off it: 18px of line rather than 16. That is not
 * cosmetic at three characters — it is the difference between "PAS" and "PA"
 * on four of the ten faces.
 */
function drawThinFrame(ctx, kx, ky, text) {
    frame1(ctx, kx, ky, ENUM_W, BOX_H);
    notchCorners(ctx, kx, ky, ENUM_W, BOX_H);
    enumText(ctx, { x: kx + 1, y: ky + 1, w: ENUM_W - 2, h: BOX_H - 2 }, text, 1);
}

/**
 * 2. no-frame — the word, and nothing else.
 *
 * The axis end that costs nothing and buys the most: 20px of line, the widest
 * in the set, and the cell stops being a box in a grid of dials. The risk is
 * the one the opaque-cell set is built around from the other direction — with
 * no frame, an enum and a divable opaque cell look the same, and the only
 * thing telling you a knob will turn this one is that it is a shorter word.
 */
function drawNoFrame(ctx, kx, ky, text) {
    enumText(ctx, { x: kx, y: ky, w: ENUM_W, h: BOX_H }, text, 1);
}

/**
 * 3. underline — a baseline instead of a box.
 *
 * One solid rule across the bottom of the cell, inset a pixel each side. It
 * keeps the full 20px of line and still says "field", which is the thing
 * option 2 gives up; and a rule under a word is the one framing idiom that
 * costs no horizontal room at all.
 */
function drawUnderline(ctx, kx, ky, text) {
    /* The text box starts one row DOWN, not at the cell top. Centred in
     * `BOX_H - 3` from `ky` it came out flush with the top edge on a two-line
     * value — a word hanging off the ceiling with a rule floating below it,
     * which read as a rendering fault rather than as a baseline. */
    enumText(ctx, { x: kx, y: ky + 1, w: ENUM_W, h: BOX_H - 4 }, text, 1);
    ctx.fillRect(kx + 1, ky + BOX_H - 2, ENUM_W - 2, 1, 1);
}

/* ---------------------------------------------------------------- 4..8 --
 * Some other frame. */

/**
 * 4. bracket — four corners, no edges.
 *
 * The frame reduced to its corners, which is the grammar the grid already uses
 * for "you can go into this" (`drawBrackets`). Using it here is therefore a
 * real decision rather than a free one: it makes the enum LOOK divable, and
 * since every enum with declared options now IS divable — hold, click, pick
 * from a list — that reading is correct. The counter-argument is that
 * `divable_mark` was deliberately restricted to the ~5 opaque params precisely
 * so the mark would keep meaning something, and this option spends it on ~135
 * enums.
 */
function drawBracketFrame(ctx, kx, ky, text) {
    drawBrackets(ctx, kx, ky, ENUM_W, BOX_H);
    enumText(ctx, { x: kx + 2, y: ky, w: ENUM_W - 4, h: BOX_H }, text, 1);
}

/**
 * 5. inverted — the cell is solid and the word is a hole in it.
 *
 * No frame is needed because the block's own silhouette is the frame, notched.
 * It is the most legible option in the set at a glance and the least usable in
 * quantity: a page can carry four enums, and four black blocks is not a grid
 * of parameters, it is a chequerboard. It also collides with the two things
 * inversion already means here — a touched label strip and a bypassed module.
 */
function drawInverted(ctx, kx, ky, text) {
    ctx.fillRect(kx, ky, ENUM_W, BOX_H, 1);
    notchCorners(ctx, kx, ky, ENUM_W, BOX_H);
    enumText(ctx, { x: kx + 1, y: ky + 1, w: ENUM_W - 2, h: BOX_H - 2 }, text, 0);
}

/**
 * 6. heavy-frame — two pixels of edge, notched.
 *
 * The most direct answer to "the frame is too Elektron": keep the frame and
 * change its weight. It reads as a physical bezel rather than a hairline, and
 * it is the only option here whose difference survives being photographed off
 * the panel at an angle. The cost is arithmetic and it is the worst in the
 * set: two pixels each side leaves 16px of line, the same as the incumbent,
 * with none of the incumbent's 2px breathing space between glyph and edge.
 */
function drawHeavyFrame(ctx, kx, ky, text) {
    frame1(ctx, kx, ky, ENUM_W, BOX_H);
    frame1(ctx, kx + 1, ky + 1, ENUM_W - 2, BOX_H - 2);
    notchCorners(ctx, kx, ky, ENUM_W, BOX_H);
    /* The inner ring is notched too. Without it the bezel has square corners
     * inside a rounded outline, which is the one place a 2px frame stops
     * reading as one object and starts reading as two frames. */
    notchCorners(ctx, kx + 1, ky + 1, ENUM_W - 2, BOX_H - 2);
    enumText(ctx, { x: kx + 2, y: ky + 2, w: ENUM_W - 4, h: BOX_H - 4 }, text, 1);
}

/**
 * 7. dotted-frame — the same frame at 50% density.
 *
 * `dottedRule` along the top and bottom, `dashedVRule` up the sides, corners
 * notched. It keeps the incumbent's geometry exactly and changes only its
 * ink, which makes it the cheapest thing in the set to adopt and the easiest
 * to lose: a 1-on-1-off pattern at 1px on this panel is at the resolution
 * limit, and on hardware it may read as a solid frame that failed to render
 * rather than as a deliberate one. That is the honest risk and it cannot be
 * settled from a 4x PNG.
 */
function drawDottedFrame(ctx, kx, ky, text) {
    dottedRule(ctx, kx, ky, ENUM_W);
    dottedRule(ctx, kx, ky + BOX_H - 1, ENUM_W);
    dashedVRule(ctx, kx, ky, BOX_H);
    dashedVRule(ctx, kx + ENUM_W - 1, ky, BOX_H);
    notchCorners(ctx, kx, ky, ENUM_W, BOX_H);
    enumText(ctx, { x: kx + 1, y: ky + 1, w: ENUM_W - 2, h: BOX_H - 2 }, text, 1);
}

/**
 * 8. tab — the frame with its floor removed.
 *
 * Top and sides only, the two top corners notched, so the cell reads as a tab
 * hanging from the row above rather than as a closed box. It is the one option
 * that says something about the enum's RELATION to its label — the open bottom
 * points at the label band under it, tying the word to the name of the
 * parameter it belongs to, which no other treatment here does.
 *
 * It is also the one most likely to read as a rendering bug. An unclosed box
 * on a 1-bit panel is what a clipped box looks like.
 */
function drawTab(ctx, kx, ky, text) {
    ctx.fillRect(kx, ky, ENUM_W, 1, 1);
    ctx.fillRect(kx, ky, 1, BOX_H, 1);
    ctx.fillRect(kx + ENUM_W - 1, ky, 1, BOX_H, 1);
    ctx.fillRect(kx, ky, 1, 1, 0);
    ctx.fillRect(kx + ENUM_W - 1, ky, 1, 1, 0);
    enumText(ctx, { x: kx + 1, y: ky + 1, w: ENUM_W - 2, h: BOX_H - 2 }, text, 1);
}

/* --------------------------------------------------------------- 9..10 --
 * The fill is the frame. */

/**
 * 9. dither-ground — a 25% hatch, no frame at all.
 *
 * `DIAG_LIGHT` across the whole cell with the text on a cleared plate. The
 * cell gains a SURFACE without gaining an edge, which is the gentlest way to
 * say "this one is different from its neighbours" — and unlike option 5 it
 * says it without spending the screen's strongest signal.
 *
 * The plate is what makes it work and also what limits it: clearing two pixels
 * around each line means the visible hatch is a ring around the word rather
 * than a field behind it, and on a two-line value the ring closes up and there
 * is very little ground left to see. Single-line enums show this option at its
 * best; three-plus-three shows it at its worst.
 */
function drawDitherGround(ctx, kx, ky, text) {
    const b = { x: kx, y: ky, w: ENUM_W, h: BOX_H };
    fillDithered(ctx, kx, ky, ENUM_W, BOX_H, DIAG_LIGHT);
    notchCorners(ctx, kx, ky, ENUM_W, BOX_H);
    enumPlate(ctx, { x: kx + 2, y: ky, w: ENUM_W - 4, h: BOX_H }, text, b);
}

/**
 * 10. slab — a solid plinth sized to the word.
 *
 * The distinction from option 5 is deliberate and it is the whole argument: 5
 * fills the CELL, 10 fills the WORD. A slab that hugs its text gives the row a
 * varying silhouette — a two-letter enum is a small block, an eight-letter one
 * is a wide one — so the page shows you how much value there is before you
 * read any of it, and four enums in a row stop being four identical squares.
 *
 * A CHECKER shoulder fills the rest of the cell at half density, so the slab
 * has something to sit against rather than floating; without it the slab reads
 * as a fragment of a frame that failed to draw. Honest weakness: on a two-line
 * value the slab is 13 of the cell's 15 rows and 18 of its 20 columns, at which
 * point it has collapsed into option 5 with a border. It is a genuinely
 * different option only for short values.
 */
function drawSlab(ctx, kx, ky, text) {
    const L = lines(text, ENUM_W - 4);
    const sw = Math.min(ENUM_W, L.w + 4);
    const sh = Math.min(BOX_H, L.h + 4);
    const sx = kx + Math.floor((ENUM_W - sw) / 2);
    const sy = ky + Math.floor((BOX_H - sh) / 2);
    fillDithered(ctx, kx, ky, ENUM_W, BOX_H, CHECKER);
    notchCorners(ctx, kx, ky, ENUM_W, BOX_H);
    ctx.fillRect(sx, sy, sw, sh, 1);
    notchCorners(ctx, sx, sy, sw, sh);
    enumText(ctx, { x: sx, y: sy, w: sw, h: sh }, text, 0);
}

/* --------------------------------------------------------------- probe --
 *
 * "low pass" rather than "SINE". `enumSquareLines` splits on the space and
 * gives ["LOW", "PAS"] — the TWO-line case, which is the one that stresses
 * every option vertically as well as horizontally, and the one where options 9
 * and 10 are weakest. A single-line probe would flatter them.
 *
 * The probe surface is the 20x15 enum box itself, not the 32px cell: the
 * shipping widget is drawn at ENUM_W and centred in the cell by drawKnobRow,
 * and an option is only allowed the box.
 */
const PROBE_TEXT = "low pass";

/* The line budget each option leaves for text, which is what the cross-set
 * font assertion measures against. Kept beside the option rather than derived,
 * because the draw functions spend it in six different ways. */

export function register() {
    return registerSet({
        id: "enum_square",
        title: "Enum square — the one cell whose value is a word",
        kind: KIND_DRAW,
        replaces: "drawEnumSquare",
        probeSize: { w: ENUM_W, h: BOX_H },
        probe: (ctx, draw) => draw(ctx, 0, 0, PROBE_TEXT),
        fontProbe: PROBE_TEXT,
        baseline: (ctx, kx, ky, text) => drawEnumSquare(ctx, kx, ky, text),
        /* Enum squares go where the knob widgets are, centred in the cell the
         * same way drawKnobRow centres them, so the in-context page shows four
         * across a row at true pitch — which is the only way to judge options 5
         * and 10, whose whole cost is what a ROW of them looks like. */
        context: (ctx, draw, info) => {
            const RM = info.RM;
            const texts = ["low pass", "sine", "-12", "trig"];
            for (const slot of info.slots) {
                const rowY = slot < 4 ? RM.ROW0_Y : RM.ROW1_Y;
                const cellX = (slot % 4) * RM.CELL_W;
                ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
                draw(ctx, cellX + Math.floor((RM.CELL_W - RM.ENUM_W) / 2), rowY, texts[slot % 4]);
            }
        },
        options: [
            {
                position: 1, id: "thin-frame", name: "Thin frame", draw: drawThinFrame, textW: ENUM_W - 2, overflowFonts: ["dot-matrix"],
                note: "The incumbent frame with its 2px inner margin closed up, so the line runs to the inside of the border: 18px instead of ENUM_TEXT_W's 16. At three characters that is not cosmetic — measured on the enumSquareLines split of low pass, wide needs 17px for PAS, so this option keeps the word whole where the incumbents 16px budget cuts it to PA. The cost is that glyphs now sit one pixel off the frame, so a bowl and the border touch at a glance. Survives every variant except dot-matrix, which needs 31px for a single 3-character line and fits in no option here at all.",
            },
            {
                position: 2, id: "no-frame", name: "No frame", draw: drawNoFrame, textW: ENUM_W, overflowFonts: ["dot-matrix"],
                note: "The word on the field, nothing around it. The widest line in the set at 20px, which ties option 3 for the most text any treatment here leaves — and it is still not enough for dot-matrix, whose 3-character line is 31px. Nothing in this set fits that face, which is the honest cross-set finding: dot-matrix and the enum square are mutually exclusive picks. Its real cost is not legibility but grammar: with no frame an enum and an opaque divable cell are the same drawing, and drawOpaqueBox is frameless precisely because the two used to be pixel-identical and you found out which was which by turning one and having nothing happen. Adopting this reopens that.",
            },
            {
                position: 3, id: "underline", name: "Underline", draw: drawUnderline, textW: ENUM_W, overflowFonts: ["dot-matrix"],
                note: "A solid rule along the bottom instead of a box, inset one pixel each side. It keeps option 2s full 20px line — a rule under a word is the only framing idiom that costs nothing horizontally — while still saying field rather than caption. Survives every variant except dot-matrix. The weakness is vertical: the rule takes three of fifteen rows out of the text box, so a two-line value sits high in the cell and the option looks unbalanced next to a single-line neighbour in the same row.",
            },
            {
                position: 4, id: "bracket", name: "Bracket", draw: drawBracketFrame, textW: ENUM_W - 4, overflowFonts: ["dot-matrix"],
                note: "The frame reduced to four corners. This is not a neutral choice: corner brackets are already this grids grammar for you-can-go-into-this, and since every enum with declared options now opens a picker, the reading is CORRECT. The argument against is that divable_mark was deliberately withheld from the ~135 enums so the mark would keep meaning something on the ~5 opaque params, and this spends it. Line budget 16px, the same as the incumbent: overflows wide (17px for PAS) and dot-matrix.",
            },
            {
                position: 5, id: "inverted", name: "Inverted", draw: drawInverted, textW: ENUM_W - 2, overflowFonts: ["dot-matrix"],
                note: "Solid cell, notched, word knocked out. The most legible thing in the set from across a room and the least usable in quantity — a page can carry four enums, and four solid blocks is a chequerboard rather than a grid of parameters. It also spends a signal that is already committed twice over: a touched label strip inverts and a bypassed module inverts, so a screen with all three has three different meanings sharing one appearance. Line budget 18px; overflows dot-matrix only.",
            },
            {
                position: 6, id: "heavy-frame", name: "Heavy frame", draw: drawHeavyFrame, textW: ENUM_W - 4, overflowFonts: ["dot-matrix"],
                note: "Two pixels of edge, notched. The most direct answer to the frame-is-too-Elektron complaint — keep the frame, change its weight — and the only option whose difference survives being seen off the panel at an angle. It is also the worst trade in the set arithmetically: 16px of line, no better than the incumbent, with none of the incumbents 2px of air between glyph and border, so at a two-line value the cell is visibly congested. Overflows wide and dot-matrix.",
            },
            {
                position: 7, id: "dotted-frame", name: "Dotted frame", draw: drawDottedFrame, textW: ENUM_W - 2, overflowFonts: ["dot-matrix"],
                note: "The incumbents geometry exactly, at half the ink: dottedRule top and bottom, dashedVRule up the sides, corners notched. Cheapest option here to adopt and the easiest to lose — a 1-on-1-off pattern at 1px is at this panels resolution limit, and on hardware it may read as a solid frame that failed to render rather than as a deliberate one. That cannot be settled from a 4x PNG and it is the single biggest reason to put this one in front of a human. Line budget 18px; overflows dot-matrix only.",
            },
            {
                position: 8, id: "tab", name: "Tab", draw: drawTab, textW: ENUM_W - 2, overflowFonts: ["dot-matrix"],
                note: "Top and sides, no floor, top corners notched. The only option that says anything about the enums RELATION to anything else: the open bottom points into the label band below, tying the word to the name of the parameter it belongs to. It is also the one most likely to be read as a bug, because an unclosed box on a 1-bit panel is exactly what a clipped box looks like — and this codebase has shipped a clipped box before. Line budget 18px; overflows dot-matrix only.",
            },
            {
                position: 9, id: "dither-ground", name: "Dither ground", draw: drawDitherGround, textW: ENUM_W - 4, overflowFonts: ["dot-matrix"],
                note: "DIAG_LIGHT across the cell with the text on a cleared plate, no frame anywhere. Gains a surface without gaining an edge, which is the gentlest available way to mark the cell as a different kind of thing, and it does it without spending inversion. The plate is what makes it legible and also what caps it: clearing two pixels around each line means on the two-line probe the visible hatch is a thin ring rather than a field, and this option is at its best on single-line values and its worst on three-plus-three. Line budget 16px; overflows wide and dot-matrix.",
            },
            {
                position: 10, id: "slab", name: "Slab", draw: drawSlab, textW: ENUM_W - 4, overflowFonts: ["dot-matrix"],
                note: "A solid plinth sized to the WORD rather than to the cell, on a CHECKER shoulder so it has something to sit against. That is the whole distinction from option 5 and it earns the slot only on short values: the row gains a varying silhouette, so you see how much value there is before reading any of it. On the two-line probe the slab is 13 of 15 rows and 18 of 20 columns and has collapsed into option 5 with a border — an honest weakness, visible in the contact sheet. Line budget 16px; overflows wide and dot-matrix.",
            },
        ],
    });
}
