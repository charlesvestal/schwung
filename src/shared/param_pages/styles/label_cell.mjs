/**
 * styles/label_cell.mjs — SET 5: ten treatments of the label band.
 *
 * Under every widget on the knob grid is a 32x7 band. At rest it carries the
 * parameter's abbreviated NAME; while its knob is held it carries the VALUE,
 * in a solid white strip with the text knocked out. That inverted strip is one
 * of the two most Elektron-reading things on this screen — the enum square is
 * the other, which is set 4 — and it is the single element a user sees change
 * every time they touch anything.
 *
 * BOTH STATES ARE THE OPTION. An option that only redraws the touched strip has
 * answered half the question, because the resting band is what eight cells look
 * like for 99% of the session and the touched one is what a gesture FEELS like.
 * Every draw below takes the shipping `showValue` / `inverted` pair and every
 * probe renders both, stacked.
 *
 * THERE IS NO GUTTER ABOVE THIS BAND. `ROW0_Y` is 10 and `BOX_H` is 15, so the
 * widget box ends at row 24 and `LBL0_Y` is 25. One row of overflow lands on the
 * bottom of a knob, a fader or a filter curve, and the grid does not repaint the
 * widget when the label changes — so the damage persists until something else
 * forces a redraw. Every option here is therefore bounded to `[lblY, lblY+6]`
 * exactly, and the catalog probe is sized so that a single row of encroachment
 * clips and fails the run rather than being noticed by eye or not.
 *
 * WHY IT COMES LAST, WITH enum_square. Both sets are text-bearing and set 12
 * replaces the letterforms outright, so sizing a strip against the shipping
 * face and then swapping the face is how you ship an option that fitted here and
 * overflows on the device. Every note names the variants the option actually
 * overflows, measured through `styles/font/blit.mjs` against the same budget the
 * draw function uses, and `tests/host/test_style_catalog.sh` asserts that list
 * exactly.
 *
 * The value is the harder string, not the label. Labels are already squeezed to
 * `LABEL_CHARS` worth of Ms by `render_page_movy`'s own abbreviator; a formatted
 * value like "-24.0" is not squeezed by anything, and on `wide` it is 25px
 * against 32 before an option has drawn a single pixel of frame.
 *
 * The axis is minimal -> radical, measured by how far the TOUCHED state moves
 * from a full-width inverted strip:
 *
 *   1        the strip, one row shorter.
 *   2-4      no inversion at all — a rule, a box, brackets around the value.
 *   5-8      the strip stays but changes what it is: half density, a border, a
 *            width that follows the value, a tether pointing at its widget.
 *   9-10     inversion is abandoned as the mechanism. A caret marks the cell,
 *            or the label and value trade places.
 *
 * CORNER NOTCHES ARE NOT A VARIABLE — every filled or framed shape here is
 * notched, because rounded corners are already a decision ("i also do like
 * rounded corners on stuff tho that is pretty elektronny") and an option whose
 * only content was toggling them would waste one of ten slots.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { CELL_W, LBL_H, drawLabelCell, drawBrackets } from "../render_page_movy.mjs";
import { fontWidth4x5, fontPrint4x5, FONT4_HEIGHT } from "../font4x5.mjs";
import { CHECKER, fillDithered, notchCorners } from "./dither.mjs";

/* `cellLeft` is private to render_page_movy; this is the same arithmetic. */
const cellLeftOf = (g, col) => g.x0 + col * g.cellW;

/** Trim until it fits, MEASURED. Nothing in this set is allowed to overrun the
 * cell, so every option runs its text through here — which is why the font
 * assertion is about whether a realistic string SURVIVES intact rather than
 * about whether pixels land outside the band. */
function fit(s, maxW) {
    let t = String(s == null ? "" : s);
    while (t.length > 1 && fontWidth4x5(t) > maxW) t = t.slice(0, -1);
    return fontWidth4x5(t) > maxW ? "" : t;
}

/** render_page_movy's drawWaveMark, copied — it is not exported, and the
 * modulation tilde has to survive every treatment or an option silently drops
 * a signal the grid depends on. */
function waveMark(ctx, x, y, on) {
    ctx.fillRect(x, y, 1, 1, on);
    ctx.fillRect(x + 2, y, 1, 1, on);
    ctx.fillRect(x + 1, y + 1, 1, 1, on);
    ctx.fillRect(x + 3, y + 1, 1, 1, on);
}

/**
 * The text an option prints and where it starts. Every option resolves this
 * identically and then differs only in what it draws around it, which is what
 * keeps the set a comparison of ONE variable.
 */
function run(g, col, label, displayValue, showValue, budget) {
    const cellX = cellLeftOf(g, col);
    const t = fit(showValue ? displayValue : label, Math.min(budget, g.cellW));
    const w = fontWidth4x5(t);
    return { cellX, t, w, tx: cellX + Math.floor((g.cellW - w) / 2) };
}

/** Print, and put the modulation tilde where it will not sit on a glyph. */
function mark(ctx, r, g, lblY, modulated, on) {
    if (!modulated) return;
    waveMark(ctx, Math.max(r.cellX, r.tx - 6), lblY + 1, on);
}

/* ------------------------------------------------------------------- 1 -- */

/**
 * 1. thin-strip — the incumbent, one row shorter.
 *
 * Rows `lblY+1..lblY+6` instead of the full seven, so a clear row opens between
 * the strip and the widget above it. That row is the whole option: it stops the
 * inverted band from butting straight into the bottom of a knob, which is what
 * makes the touched cell currently read as one tall black-and-white object
 * rather than as a label under a control.
 *
 * The clear row has to come off the TOP, not the bottom — taking it off the
 * bottom is what the first draft did, and it leaves the strip still touching
 * the widget while opening a gap toward nothing at all, which is the exact
 * opposite of the stated argument and looks identical in a swatch.
 *
 * The cost is unavoidable and it is visible: a six-row strip cannot enclose a
 * five-row face with air on both sides, so the glyph TOPS sit flush with the
 * strip's top edge and merge into the new clear row above it. This option buys
 * separation from the widget by spending separation from the letterform.
 */
function drawThinStrip(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 2);
    if (inverted) {
        ctx.fillRect(r.cellX, lblY + 1, g.cellW, LBL_H - 1, 1);
        notchCorners(ctx, r.cellX, lblY + 1, g.cellW, LBL_H - 1);
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 0);
    } else {
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    }
    mark(ctx, r, g, lblY, modulated, inverted ? 0 : 1);
}

/* --------------------------------------------------------------- 2..4 --
 * No inversion at all. */

/**
 * 2. underline-value — a rule instead of a reversal.
 *
 * The touched cell prints its value the same way it prints its label and adds
 * one solid rule under it, sized to the text. The page keeps a single ink
 * weight everywhere, so nothing on screen ever flashes — which is either the
 * point or the flaw, because the strip's job is partly to be VISIBLE from the
 * corner of your eye while your attention is on the knob.
 *
 * The rule follows the value's width, so it also encodes how long the value is;
 * that is a small free signal none of options 3-6 give.
 */
function drawUnderlineValue(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 4);
    fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    if (inverted && r.w > 0) ctx.fillRect(r.tx - 1, lblY + LBL_H - 1, r.w + 2, 1, 1);
    mark(ctx, r, g, lblY, modulated, 1);
}

/**
 * 3. boxed-value — the value in a 1px frame.
 *
 * A notched box sized to the value, full band height. It is the loudest of the
 * non-inverting options and the one that reads most like a FIELD — the value
 * looks contained rather than merely printed, which matches what a held knob is
 * doing (editing one thing).
 *
 * It has the collision the enum set is about, in reverse: a boxed word under a
 * widget looks like an enum square that wandered into the label band, and on a
 * page carrying both, the two constructions are a pixel apart in weight.
 */
function drawBoxedValue(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 6);
    if (inverted && r.w > 0) {
        const bx = r.tx - 2, bw = r.w + 4;
        ctx.fillRect(bx, lblY, bw, 1, 1);
        ctx.fillRect(bx, lblY + LBL_H - 1, bw, 1, 1);
        ctx.fillRect(bx, lblY, 1, LBL_H, 1);
        ctx.fillRect(bx + bw - 1, lblY, 1, LBL_H, 1);
        notchCorners(ctx, bx, lblY, bw, LBL_H);
    }
    fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    mark(ctx, r, g, lblY, modulated, 1);
}

/**
 * 4. bracket-value — corners instead of a box.
 *
 * Option 3 with the edges removed, at a 2px arm. Lighter, and it borrows a
 * grammar the grid already has — but it borrows it for a state rather than for a
 * kind, which is the objection: `drawBrackets` currently means "you can go INTO
 * this", and here it would also mean "your finger is on this". A mark that means
 * two unrelated things is a mark that means neither.
 */
function drawBracketValue(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 6);
    if (inverted && r.w > 0) drawBrackets(ctx, r.tx - 2, lblY, r.w + 4, LBL_H, 2);
    fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    mark(ctx, r, g, lblY, modulated, 1);
}

/* --------------------------------------------------------------- 5..8 --
 * Still a strip, but not that strip. */

/**
 * 5. dotted-strip — the strip at half density.
 *
 * A CHECKER ground with the value printed in black, notched, six rows. It is
 * the same gesture as the incumbent at half the ink, so a row of held knobs no
 * longer puts four solid bars across the screen.
 *
 * A SOLID PLATE SITS UNDER THE VALUE, and it is not decoration — it is a
 * correction made after looking at the render. Drawn as the brief described it
 * — black glyphs straight onto the checker — the value was not merely hard to
 * read, it was invisible: this face's strokes are 1px and the lattice's own
 * gaps are 1px, so a letter and the ground are made of the same thing. The
 * shoulders keep the half-density argument (a row of held knobs no longer puts
 * four solid bars across the screen) while the word keeps a real ground.
 *
 * Honest weakness, and it follows from that fix: with a plate under the text
 * and dither only outside it, this option is option 7 with a textured margin.
 * The two are the closest pair in the set and a reviewer may well not separate
 * them.
 */
function drawDottedStrip(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 2);
    if (inverted) {
        /* FULL band height, unlike option 1 — this option's variable is
         * density, so its geometry is the incumbent's exactly and the only
         * thing a reviewer is comparing is the ink. */
        fillDithered(ctx, r.cellX, lblY, g.cellW, LBL_H, CHECKER);
        notchCorners(ctx, r.cellX, lblY, g.cellW, LBL_H);
        if (r.w > 0) {
            const px = Math.max(r.cellX, r.tx - 2);
            const pw = Math.min(r.cellX + g.cellW, r.tx + r.w + 2) - px;
            ctx.fillRect(px, lblY, pw, LBL_H, 1);
        }
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 0);
    } else {
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    }
    mark(ctx, r, g, lblY, modulated, inverted ? 0 : 1);
}

/**
 * 6. double-strip — a solid strip with black shoulders.
 *
 * The band has seven rows and the face has five, so there are exactly two spare
 * rows and this option spends both: a full-width solid rule on the top row and
 * another on the bottom, with the five-row core inset two pixels each side so a
 * black gutter opens between the rules and the core. That is the only inset
 * border this geometry can actually hold — an inset ring drawn inside a 7-row
 * strip would land on the first and last rows of the glyphs.
 *
 * The result is heavier than the incumbent, not lighter, which is worth being
 * explicit about: this option argues that the strip should be MORE deliberate,
 * not less. Its four pixels of inset also make it the tightest text budget here
 * at 24px, and it is the only option that loses a realistic value on humanist
 * and rounded as well as on wide.
 */
function drawDoubleStrip(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 8);
    if (inverted) {
        ctx.fillRect(r.cellX, lblY, g.cellW, 1, 1);
        ctx.fillRect(r.cellX, lblY + LBL_H - 1, g.cellW, 1, 1);
        ctx.fillRect(r.cellX + 2, lblY + 1, g.cellW - 4, FONT4_HEIGHT, 1);
        notchCorners(ctx, r.cellX + 2, lblY + 1, g.cellW - 4, FONT4_HEIGHT);
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 0);
    } else {
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    }
    mark(ctx, r, g, lblY, modulated, inverted ? 0 : 1);
}

/**
 * 7. half-strip — the strip stops where the value does.
 *
 * Same solid inversion, sized to the text plus two pixels each side, centred,
 * notched. A row of held knobs becomes a row of blocks of DIFFERENT widths, so
 * the page shows you how long each value is before you read any of them, and a
 * short value like "ON" stops claiming the same 32px as "-24.0".
 *
 * It is the closest option to the incumbent in kind and the furthest in
 * behaviour, because the shape now moves as the value changes: turning a knob
 * through 9 -> 10 -> 11 makes the block grow and shrink under your finger. That
 * is either useful feedback or visual noise and it is not decidable from a
 * still.
 */
function drawHalfStrip(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    /*
     * 30px of text with 1px shoulders, not 26px with 2px.
     *
     * The original 26px budget applied to the RESTING label as well as the held
     * value, which no per-set contact sheet could show: on a real page LKYTRG,
     * LKYFLL and LSYMMT all lost a character whether or not the knob was being
     * touched. Widened on the strength of that, measured rather than guessed.
     *
     * The pair is forced. The strip is the text plus its shoulders and it must
     * still fit CELL_W, so 30 + 1 + 1 = 32 exactly; 30px with 2px shoulders
     * would be 34 and overflow, and 28px with 2px only rescues two of the three
     * labels. One pixel of shoulder is what buys the third.
     *
     * At full width the strip now spans the whole cell, so two adjacent held
     * knobs abut. The different-widths reading this option exists for survives
     * where it matters -- a short value like ON still draws a short block.
     */
    const r = run(g, col, label, displayValue, showValue, g.cellW - 2);
    if (inverted && r.w > 0) {
        /* FULL band height. The variable here is WIDTH, so the strip keeps the
         * incumbent's seven rows and its clear row above and below the glyphs;
         * shortening it too would put two changes in one option. */
        const sx = r.tx - 1, sw = r.w + 2;
        ctx.fillRect(sx, lblY, sw, LBL_H, 1);
        notchCorners(ctx, sx, lblY, sw, LBL_H);
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 0);
    } else {
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    }
    mark(ctx, r, g, lblY, modulated, inverted && r.w > 0 ? 0 : 1);
}

/**
 * 8. tethered — the strip points at the widget it belongs to.
 *
 * A six-row strip on rows `lblY+1..lblY+6` with a three-pixel nub on `lblY`
 * pointing up at the widget. On a grid where four cells are held at once, this
 * is the only option that states WHICH widget each value belongs to instead of
 * relying on column alignment.
 *
 * THE NUB IS PAID FOR OUT OF THE BAND, NOT OUT OF THE WIDGET — nothing is drawn
 * above `lblY`, because there is no gutter there and the grid does not repaint
 * a widget when its label changes. That is also why it is a nub rather than the
 * 5-3-1 triangle it was drawn as first: a three-row triangle plus five rows of
 * glyph is eight rows in a seven-row band, and squeezing the strip to five rows
 * to make room left the value flush against both its edges and genuinely hard
 * to read in the contact sheet. Three pixels of tether that work beat five that
 * cost the value.
 */
function drawTethered(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 4);
    if (inverted) {
        const cx = r.cellX + Math.floor(g.cellW / 2);
        /* Five pixels, not three. At three the nub is the same size as a glyph
         * stroke and lands over the value's middle character, so it read as
         * part of the digit rather than as a mark on the strip. */
        ctx.fillRect(cx - 2, lblY, 5, 1, 1);
        ctx.fillRect(r.cellX, lblY + 1, g.cellW, LBL_H - 1, 1);
        notchCorners(ctx, r.cellX, lblY + 1, g.cellW, LBL_H - 1);
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 0);
    } else {
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    }
    mark(ctx, r, g, lblY, modulated, inverted ? 0 : 1);
}

/* --------------------------------------------------------------- 9..10 --
 * Inversion abandoned as the mechanism. */

/**
 * 9. caret — a three-pixel mark instead of a reversal.
 *
 * The touched cell prints its value in exactly the same ink as its label and
 * puts a small up-caret at the left of the band, pointing at the widget. The
 * lightest possible answer, and the only one where a screen with four knobs held
 * looks essentially like a screen with none.
 *
 * That is the argument against it as much as for it. The strip is not only
 * decoration — it is what tells you at a glance that the number under your
 * finger is a value and not a name, and three pixels in a corner do not carry
 * that from arm's length. It is included as the honest floor of the axis: if a
 * reviewer prefers it, the strip was doing less work than assumed.
 */
function drawCaret(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const r = run(g, col, label, displayValue, showValue, g.cellW - 6);
    /* Caret first, then text — the run is centred in the whole cell, so on a
     * long value the two could otherwise overlap; the budget above reserves the
     * caret's column pair either side. */
    if (inverted) {
        const cx = r.cellX + 1, cy = lblY + 2;
        ctx.fillRect(cx + 1, cy, 1, 1, 1);
        ctx.fillRect(cx, cy + 1, 1, 1, 1);
        ctx.fillRect(cx + 2, cy + 1, 1, 1, 1);
    }
    fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
    mark(ctx, r, g, lblY, modulated, 1);
}

/**
 * 10. swap — the label and the value trade places.
 *
 * The band never inverts. At rest the label is centred and alone; while held,
 * the value takes the right of the band and the label is pushed to the left edge
 * and squeezed into whatever the value left, so the two swap prominence rather
 * than the cell swapping polarity. It is the only option in the set where the
 * parameter's NAME is still on screen while you are editing it, which is the
 * one real complaint about the incumbent — the touched cell forgets what it is.
 *
 * The degradation is deliberate and it is what makes this survivable across the
 * fonts: the label is fitted to the leftover, so on a wide face it shrinks to
 * two characters and then to none, and the value is never the thing that gets
 * cut. On the shipping face the leftover is about 10px, which is two characters
 * — enough to disambiguate a row, not enough to read.
 */
function drawSwap(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const cellX = cellLeftOf(g, col);
    if (!inverted) {
        const r = run(g, col, label, displayValue, showValue, g.cellW - 2);
        fontPrint4x5(ctx, r.tx, lblY + 1, r.t, 1);
        mark(ctx, r, g, lblY, modulated, 1);
        return;
    }
    const val = fit(displayValue, g.cellW - 2);
    const vw = fontWidth4x5(val);
    const gap = g.cellW - vw - 3;
    const lab = gap > 0 ? fit(label, gap) : "";
    if (lab) fontPrint4x5(ctx, cellX, lblY + 1, lab, 1);
    fontPrint4x5(ctx, cellX + g.cellW - 1 - vw, lblY + 1, val, 1);
    if (modulated) waveMark(ctx, cellX, lblY + 1, 1);
}

/* --------------------------------------------------------------- probe --
 *
 * TWO BANDS, stacked: resting on top, touched below. An option that only
 * redrew one of them would be half an option, and a contact sheet showing only
 * the touched state would rank the flash rather than the design.
 *
 * The surface is exactly 7 + 2 + 7 rows with the resting band at y = 0, so a
 * single row of encroachment above a band CLIPS and fails the catalog run. That
 * is the mechanical form of the no-gutter rule at the top of this file: the
 * damage on the device is silent (the widget above is not repainted when the
 * label changes), so it has to be loud here.
 */
const PROBE_LABEL = "CUTOF";
const PROBE_VALUE = "-24.0";
const PROBE_GEOM = Object.freeze({ x0: 0, cellW: CELL_W });
const TOUCHED_Y = LBL_H + 2;

function probe(ctx, draw) {
    draw(ctx, PROBE_GEOM, 0, 0, PROBE_LABEL, PROBE_VALUE, false, false, false);
    draw(ctx, PROBE_GEOM, 0, TOUCHED_Y, PROBE_LABEL, PROBE_VALUE, true, true, false);
}

export function register() {
    return registerSet({
        id: "label_cell",
        title: "Label band — the name at rest, the value while you hold it",
        kind: KIND_DRAW,
        replaces: "drawLabelCell",
        probeSize: { w: CELL_W, h: LBL_H * 2 + 2 },
        probe,
        fontProbe: [PROBE_LABEL, PROBE_VALUE],
        /* The band's own height, exported for the test's encroachment check —
         * an option is allowed [lblY, lblY + LBL_H - 1] and nothing else. */
        bandH: LBL_H,
        baseline: (ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) =>
            drawLabelCell(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated),
        /*
         * In context, the label band is redrawn for ALL EIGHT cells rather than
         * for the knob slots only: a viz group or an enum square still has a
         * label under it, and the row of four is the thing being judged. The
         * clear is confined to the band, so the widgets above survive — which
         * is also how an option that encroaches becomes visible here, since the
         * widget it eats will not be repainted.
         *
         * Two cells are drawn TOUCHED (one per row) and one is drawn modulated,
         * because a real page never shows eight of either, and an option that
         * only works when every cell agrees is not an option.
         */
        context: (ctx, draw, info) => {
            const RM = info.RM;
            const labels = ["CUTOF", "RESO", "DECAY", "MOD"];
            const values = ["-24.0", "0.62", "100", "SINE"];
            for (let slot = 0; slot < 8; slot++) {
                const lblY = slot < 4 ? RM.LBL0_Y : RM.LBL1_Y;
                const col = slot % 4;
                const cellX = col * RM.CELL_W;
                const touched = slot === 1 || slot === 6;
                ctx.fillRect(cellX, lblY, RM.CELL_W, RM.LBL_H, 0);
                draw(ctx, { x0: 0, cellW: RM.CELL_W }, col, lblY,
                     labels[col], values[col], touched, touched, slot === 2);
            }
        },
        options: [
            {
                position: 1, id: "thin-strip", name: "Thin strip", draw: drawThinStrip, textW: CELL_W - 2, overflowFonts: ["dot-matrix"],
                note: "The incumbent strip at six rows instead of seven, so a clear row opens between it and the widget above. That row IS the option: with no gutter in the layout, a full-height inverted band butts into the bottom of a knob and the touched cell reads as one tall black-and-white object rather than as a label under a control. The row is taken from the bottom, so glyph bottoms sit flush with the strip edge — safe on this face, which has no descenders in caps or digits. Budget 30px: survives every font variant except dot-matrix, which needs 49px for CUTOF.",
            },
            {
                position: 2, id: "underline-value", name: "Underline value", draw: drawUnderlineValue, textW: CELL_W - 4, overflowFonts: ["wide", "dot-matrix"],
                note: "No inversion: the value prints in the same ink as the label with one solid rule under it, sized to the text. The screen keeps a single ink weight everywhere and nothing ever flashes — which is either the point or the flaw, because the strip is partly there to be visible from the corner of your eye while you are looking at the knob. The rule following the value width is a small free signal on how long the value is that options 3-6 do not give. Budget 28px: survives every variant except wide (29px for CUTOF) and dot-matrix.",
            },
            {
                position: 3, id: "boxed-value", name: "Boxed value", draw: drawBoxedValue, textW: CELL_W - 6, overflowFonts: ["wide", "dot-matrix"],
                note: "A notched 1px box sized to the value, full band height, no inversion. The loudest of the non-inverting options and the one that most reads as a FIELD — contained rather than merely printed, which matches what a held knob is doing. Its problem is set 4 in reverse: a boxed word under a widget looks like an enum square that fell into the label band, and on a page carrying both they sit a pixel apart in weight. Budget 26px: overflows wide and dot-matrix.",
            },
            {
                position: 4, id: "bracket-value", name: "Bracket value", draw: drawBracketValue, textW: CELL_W - 6, overflowFonts: ["wide", "dot-matrix"],
                note: "Option 3 with the edges removed, 2px arms. Lighter, and it reuses a grammar the grid already has — but it reuses it for a STATE rather than for a kind, which is the objection: drawBrackets currently means you-can-go-into-this, and here it would also mean your-finger-is-on-this. A mark meaning two unrelated things means neither. Budget 26px: overflows wide and dot-matrix.",
            },
            {
                position: 5, id: "dotted-strip", name: "Dotted strip", draw: drawDottedStrip, textW: CELL_W - 2, overflowFonts: ["dot-matrix"],
                note: "The same gesture at half the ink: a CHECKER ground, notched, six rows, with the value knocked out of a solid plate inside it. The plate is a correction, not decoration — drawn as pure black-on-checker the value was not hard to read, it was invisible, because this faces strokes are 1px and the lattices gaps are 1px, so letter and ground are made of the same thing. The shoulders keep the argument (a row of held knobs stops putting four solid bars across the screen) and the word keeps a real ground. The cost of the fix is that it converges on option 7: this is half-strip with a textured margin, and the two are the closest pair in the set. Budget 30px: overflows dot-matrix only.",
            },
            {
                position: 6, id: "double-strip", name: "Double strip", draw: drawDoubleStrip, textW: CELL_W - 8, overflowFonts: ["humanist", "rounded", "wide", "dot-matrix"],
                note: "Seven rows of band and five of face leaves exactly two spare rows, and this spends both: a full-width rule top and bottom with the five-row core inset two pixels each side, so a black gutter opens between them. That is the only inset border this geometry can hold — a ring drawn inside a 7-row strip lands on the first and last rows of the glyphs. It is HEAVIER than the incumbent, not lighter, and it argues the strip should be more deliberate rather than less. The 4px inset makes it the tightest budget here at 24px, and the only option that loses a realistic value on humanist and rounded as well as on wide and dot-matrix.",
            },
            {
                /* Widening 26 -> 30 also bought a font: `wide` fits now and only
                 * `dot-matrix` still overflows. */
                position: 7, id: "half-strip", name: "Half strip", draw: drawHalfStrip, textW: CELL_W - 2, overflowFonts: ["dot-matrix"],
                note: "The solid inversion, sized to the text plus two pixels each side and centred. A row of held knobs becomes blocks of different widths, so the page shows how long each value is before you read any, and ON stops claiming the same 32px as -24.0. Closest option to the incumbent in kind and the furthest in behaviour: the shape now MOVES as the value changes, so turning through 9-10-11 makes the block grow and shrink under your finger. Useful feedback or visual noise, and a still cannot settle it. Budget 26px: overflows wide and dot-matrix.",
            },
            {
                position: 8, id: "tethered", name: "Tethered", draw: drawTethered, textW: CELL_W - 4, overflowFonts: ["wide", "dot-matrix"],
                note: "A six-row strip with a three-pixel nub on the bands first row pointing up at the widget. With four knobs held at once this is the only option that states WHICH widget each value belongs to instead of relying on column alignment. The nub is paid for out of the band rather than the widget — nothing is drawn above lblY, because there is no gutter there and the grid does not repaint a widget when its label changes. It is a nub rather than the 5-3-1 triangle it was first drawn as: three rows of triangle plus five of glyph is eight rows in a seven-row band, and squeezing the strip to five rows to make room left the value flush against both edges and genuinely hard to read. Three pixels of tether that work beat five that cost the value. Budget 28px: overflows wide and dot-matrix.",
            },
            {
                position: 9, id: "caret", name: "Caret", draw: drawCaret, textW: CELL_W - 6, overflowFonts: ["wide", "dot-matrix"],
                note: "No inversion at all — the value prints in the same ink as the label with a three-pixel up-caret at the left of the band pointing at the widget. The lightest possible answer, and the only one where a screen with four knobs held looks like a screen with none. That is the case against as much as for: the strip is what tells you at a glance that the text under your finger is a value and not a name, and three pixels in a corner do not carry that from arms length. Included as the honest floor of the axis — if a reviewer prefers it, the strip was doing less work than assumed. Budget 26px: overflows wide and dot-matrix.",
            },
            {
                position: 10, id: "swap", name: "Swap", draw: drawSwap, textW: CELL_W - 2, overflowFonts: ["dot-matrix"],
                note: "The band never inverts. At rest the label is centred alone; while held, the value takes the right of the band and the label is pushed left and squeezed into the leftover, so the two trade prominence rather than the cell trading polarity. The only option where the parameters NAME is still on screen while you edit it, which answers the one real complaint about the incumbent — the touched cell forgets what it is. The degradation is deliberate and is what makes it survive the fonts: the label is fitted to the leftover and the value is never cut, so on the shipping face the label gets about 10px (two characters) and on wide it shrinks to nothing. Budget 30px for the value: overflows dot-matrix only.",
            },
        ],
    });
}
