/**
 * styles/fader.mjs — SET 2: ten alternatives to the viz fader.
 *
 * The shipping widget (`drawFader`, viz_draw.mjs) is a direct port of
 * schwung-movy's `renderer/knob.ts drawFader`: two 50%-dotted rails four pixels
 * either side of centre, a 3px column filled from the bottom, and a 7px head
 * across the top of the fill. It is the second-most Elektron-shaped thing on a
 * page after the arc knob, and unlike the knob it has no rotary metaphor to
 * hide behind — a rail-and-column level meter is what every hardware mixer
 * screen in this class draws.
 *
 * The axis is minimal -> radical:
 *
 *   1-3   still rails and a column. Only the WEIGHT changes: thin the fill,
 *         drop the rails, widen the head. The cheapest honest outcomes.
 *   4-6   still a vertical column, but the TRACK acquires structure — a
 *         ghosted remainder, notches, an outline with a hatched interior.
 *         Value stops being "how much solid ink" and becomes "how far up a
 *         thing that is always visible".
 *   7-9   the column is quantised or abandoned. Discrete blocks, vimana's
 *         dotted rail with a thumb, and value-as-mass.
 *   10    not vertical at all.
 *
 * GEOMETRY. `rect` is the viz cell: `{x, y, w, h}` with `h` = VIZ_ROWS (13).
 * Like viz_draw's own `band()`, every option here works in topY = rect.y + 1
 * .. botY = topY + 12, which is why the widget fits a BOX_H (15) cell with a
 * clear row above and below. Nothing may leave that band: a row of overflow
 * lands on the label strip, which the grid does not repaint.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { VIZ_ROWS, drawFader } from "../viz_draw.mjs";
import { fractionOf } from "../render_page.mjs";
import {
    SOLID, CHECKER, DIAG_LIGHT, DIAG_HEAVY, DIAG_THIRD,
    fillDithered, fillTerrain, dottedRule, dashedVRule, notchCorners,
} from "./dither.mjs";

const clamp01 = (v) => (typeof v === "number" && v === v ? (v > 1 ? 1 : v < 0 ? 0 : v) : 0);

/** viz_draw's `band()`, repeated rather than imported — it is not exported, and
 * an option is allowed to disagree with it. */
function band(rect) {
    const topY = rect.y + 1;
    return { topY, botY: topY + VIZ_ROWS - 1, midY: topY + ((VIZ_ROWS - 1) >> 1) };
}

/** The 0..1 fraction this cell is showing. Falls back to the raw number so the
 * catalog can drive an option without building a metaIndex. */
function fv(key, values, metaIndex) {
    const raw = values ? values[key] : undefined;
    if (metaIndex && typeof metaIndex.getOrGuess === "function")
        return clamp01(fractionOf(metaIndex.getOrGuess(key), raw));
    return clamp01(Number(raw));
}

/** Centre column, rounded once so every option shares one lattice. */
const cxOf = (rect) => Math.round(rect.x + rect.w / 2);

/* --------------------------------------------------------------- 1..3 --
 * Still rails and a column. */

/**
 * 1. thin-rails — the shipping construction at half the ink.
 *
 * A 1px fill column instead of 3px, rails and head unchanged. The reason this
 * is position 1 and not a straw man: eight faders at 3px is the single
 * heaviest thing a movy page can contain, and thinning the column is the
 * change nobody would have to be argued into.
 */
function drawThinRails(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect), h = botY - topY;
    for (let y = topY; y <= botY; y += 2) {
        ctx.fillRect(cx - 4, y, 1, 1, 1);
        ctx.fillRect(cx + 4, y, 1, 1, 1);
    }
    const y = Math.round(botY - v * h);
    if (y < botY) ctx.fillRect(cx, y, 1, botY - y + 1, 1);
    ctx.fillRect(cx - 3, y, 7, 1, 1);
}

/**
 * 2. no-rails — the column and its head, nothing else.
 *
 * The rails are the part of the shipping widget that reads as a fader rather
 * than as a bar, so removing them is the smallest change that alters the
 * widget's IDENTITY instead of its weight. It costs the empty range: at v=0
 * the cell is a single 7px dash sitting on the floor with nothing saying how
 * far it could travel.
 */
function drawNoRails(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect), h = botY - topY;
    const y = Math.round(botY - v * h);
    if (y < botY) ctx.fillRect(cx - 1, y, 3, botY - y + 1, 1);
    ctx.fillRect(cx - 3, y, 7, 1, 1);
}

/**
 * 3. capped — the head widened rail to rail.
 *
 * The shipping head is 7px against rails 9px apart, so it stops one pixel
 * short on each side and the value line never quite meets its own track. At
 * 9px the cap spans the rails, which turns the head from a marker floating on
 * a column into a RULE across the track — the same reading as a level line on
 * a meter. Heavier than the baseline, and the only option here that makes the
 * value easier to compare across neighbouring cells, because the caps line up
 * on a common pair of rails.
 */
function drawCapped(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect), h = botY - topY;
    for (let y = topY; y <= botY; y += 2) {
        ctx.fillRect(cx - 4, y, 1, 1, 1);
        ctx.fillRect(cx + 4, y, 1, 1, 1);
    }
    const y = Math.round(botY - v * h);
    if (y < botY) ctx.fillRect(cx - 1, y, 3, botY - y + 1, 1);
    ctx.fillRect(cx - 4, y, 9, 1, 1);
}

/* --------------------------------------------------------------- 4..6 --
 * The track acquires structure. */

/**
 * 4. ghost-track — the unfilled remainder at CHECKER.
 *
 * The travel a fader has LEFT is information the shipping widget throws away:
 * between the rails there is nothing until the fill arrives. A 50% ground
 * makes the whole track a visible object at every value, so the cell reads as
 * a control rather than as a mark. The cost is that the cell is never empty,
 * so a page of eight is uniformly grey and the fills have less to stand out
 * against.
 */
function drawGhostTrack(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect), h = botY - topY;
    for (let y = topY; y <= botY; y += 2) {
        ctx.fillRect(cx - 4, y, 1, 1, 1);
        ctx.fillRect(cx + 4, y, 1, 1, 1);
    }
    const y = Math.round(botY - v * h);
    if (y > topY) fillDithered(ctx, cx - 3, topY, 7, y - topY, CHECKER);
    if (y < botY) ctx.fillRect(cx - 1, y, 3, botY - y + 1, 1);
    ctx.fillRect(cx - 3, y, 7, 1, 1);
}

/**
 * 5. notched — the column cut into rungs.
 *
 * One row cleared out of every four, phased from the FLOOR so the notches sit
 * at fixed heights and do not slide as the value changes. That is what makes
 * it a scale rather than a texture: two neighbouring cells can be compared by
 * counting rungs, which a solid column does not allow at 13 rows.
 */
function drawNotched(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect), h = botY - topY;
    for (let y = topY; y <= botY; y += 2) {
        ctx.fillRect(cx - 4, y, 1, 1, 1);
        ctx.fillRect(cx + 4, y, 1, 1, 1);
    }
    const y = Math.round(botY - v * h);
    for (let r = y + 1; r <= botY; r++) {
        /* Phased on distance from the floor, never from the head. */
        if (((botY - r) % 4) === 3) continue;
        ctx.fillRect(cx - 1, r, 3, 1, 1);
    }
    ctx.fillRect(cx - 3, y, 7, 1, 1);
}

/**
 * 6. outline-fill — a framed column with a hatched interior.
 *
 * The fill becomes a BOX: 7px wide, 1px frame, DIAG_HEAVY inside, corners
 * notched. It borrows the page's own box vocabulary instead of the mixer's, so
 * a fader stops being a different species from an enum square — which is
 * either the point or the objection, depending on whether you want the page to
 * read as one system or want continuous and discrete cells to be
 * distinguishable at a glance.
 *
 * At very low values there is no interior left, so it degrades to a 7x2 bar
 * rather than to a frame with a hole in it.
 */
function drawOutlineFill(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect), h = botY - topY;
    dashedVRule(ctx, cx - 4, topY, h + 1, 1, 1);
    dashedVRule(ctx, cx + 4, topY, h + 1, 1, 1);
    const y = Math.round(botY - v * h);
    const bh = botY - y + 1, bx = cx - 3, bw = 7;
    ctx.fillRect(bx, y, bw, 1, 1);
    ctx.fillRect(bx, botY, bw, 1, 1);
    ctx.fillRect(bx, y, 1, bh, 1);
    ctx.fillRect(bx + bw - 1, y, 1, bh, 1);
    if (bh >= 3) {
        fillDithered(ctx, bx + 1, y + 1, bw - 2, bh - 2, DIAG_HEAVY);
        notchCorners(ctx, bx, y, bw, bh);
    }
}

/* --------------------------------------------------------------- 7..9 --
 * Quantised, or no column at all. */

/**
 * 7. stepped — six discrete blocks.
 *
 * Value as a COUNT. Six 2-row blocks stacked from the floor, the lit ones
 * solid and the unlit ones a dotted lid, so the full travel is always drawn
 * and the reading is "four of six" rather than "about two thirds". That is
 * genuinely better for the parameters a fader gets used for on this fleet
 * (mix, send, drive) and genuinely worse for anything being nudged a detent at
 * a time, because five of the six steps show no change at all.
 */
const STEPS = 6;
function drawStepped(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect);
    /* At least one block lit at v=0: an all-unlit ladder and an unpopulated
     * cell look the same, and minimum is a value, not an absence. */
    const lit = 1 + Math.round(v * (STEPS - 1));
    for (let i = 0; i < STEPS; i++) {
        /*
         * ONE row per rung on a two-row pitch, not two solid rows butted
         * together. Drawn as 2px blocks the lit rungs merged into a single
         * solid column and the option lost the only thing it encodes — you
         * cannot count four-of-six off a shape that has become one block. The
         * clear row between rungs is the entire construction.
         */
        const y0 = botY - i * 2;
        if (y0 < topY) break;
        if (i < lit) ctx.fillRect(cx - 3, y0, 7, 1, 1);
        else dottedRule(ctx, cx - 3, y0, 7, 0);
    }
}

/**
 * 8. dotted-thumb — vimana's indicator, stood on end.
 *
 * A 50%-dashed rail with a solid 3x3 thumb and a stop at each end, no fill at
 * all. This is `render_indicator` from crates/vimana-app/src/hud.rs: the
 * reference product draws every continuous parameter this way, so it is the
 * one option in the set with a shipped precedent rather than a hypothesis
 * behind it. It also throws away the thing a fader is best at — you cannot
 * compare eight of these at a glance the way you can compare eight fills,
 * because a thumb has no area.
 */
function drawDottedThumb(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect);
    /* The rail stops one row short of each stop so a rail dot can never bridge
     * a stop to the thumb — at the extremes that is the one row being read. */
    dashedVRule(ctx, cx, topY + 2, botY - topY - 3, 1, 1);
    ctx.fillRect(cx - 2, topY, 5, 1, 1);
    ctx.fillRect(cx - 2, botY, 5, 1, 1);
    const lo = topY + 1, hi = botY - 3;
    const ty = hi - Math.round(v * (hi - lo));
    ctx.fillRect(cx - 1, ty, 3, 3, 1);
}

/**
 * 9. terrain — value as mass.
 *
 * DIAG_THIRD hatch under a solid crest, filled bottom-up across a 9px column,
 * which is the vimana filter page's idiom and the reason `fillTerrain` strokes
 * a crest at all: a light pattern alone leaves the boundary ambiguous, and the
 * boundary IS the value. Eight of these side by side read as one skyline, so
 * this is the option that changes what a PAGE looks like rather than what a
 * cell looks like. At v=0 the crest lands on the floor, so an empty cell still
 * shows a base.
 */
function drawTerrain(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const cx = cxOf(rect), w = 9, h = botY - topY;
    /*
     * The floor is a dotted rule on its OWN row, below the terrain, not the
     * terrain's bottom row. `fillTerrain` at v=0 puts the crest one row past
     * the bottom edge and draws nothing at all, so without a floor the minimum
     * of the range and an unpopulated cell are the same picture — which is the
     * one reading a level widget must never allow.
     */
    dottedRule(ctx, cx - 4, botY, w, 0);
    fillTerrain(ctx, cx - 4, topY, w, h, new Array(w).fill(v), DIAG_THIRD, true);
    /* Full travel is a filled box, so it gets the notch too — otherwise the one
     * value where the cell is most prominent is also the one where it is
     * squarest. */
    notchCorners(ctx, cx - 4, topY, w, h);
}


/* ------------------------------------------------------------------ 10 --
 * Not vertical. */

/**
 * 10. horizontal — the fader laid on its side.
 *
 * A framed, notched bar across the full cell, filled left to right, with the
 * unused remainder at DIAG_LIGHT so the track is visible at zero. The
 * argument is spatial rather than aesthetic: a 32px cell is twice as wide as
 * it is tall, so a horizontal fill has twice the resolution of a vertical one
 * — 26 distinguishable positions against 13. The argument against is that it
 * abandons the one thing a column of faders is for, which is being read as a
 * profile across the page, and it collides visually with the enum square, which
 * is also a framed horizontal box.
 */
function drawHorizontal(ctx, rect, key, values, metaIndex) {
    const v = fv(key, values, metaIndex);
    const { midY } = band(rect);
    const bx = rect.x + 3, bw = rect.w - 6, by = midY - 2, bh = 5;
    ctx.fillRect(bx, by, bw, 1, 1);
    ctx.fillRect(bx, by + bh - 1, bw, 1, 1);
    ctx.fillRect(bx, by, 1, bh, 1);
    ctx.fillRect(bx + bw - 1, by, 1, bh, 1);
    notchCorners(ctx, bx, by, bw, bh);
    const tx = bx + 1, ty = by + 1, tw = bw - 2, th = bh - 2;
    fillDithered(ctx, tx, ty, tw, th, DIAG_LIGHT);
    const fw = Math.round(v * tw);
    if (fw > 0) fillDithered(ctx, tx, ty, fw, th, SOLID);
}

/* --------------------------------------------------------------- probe --
 *
 * The catalog needs a value and a metaIndex to drive a viz signature. A stub
 * index that reports a plain 0..1 float is enough: `fractionOf` then returns
 * the value unchanged, so the swatch shows the option at exactly the fraction
 * the catalog asked for.
 */
const PROBE_META = { getOrGuess: (key) => ({ key, type: "float", min: 0, max: 1, kind: "number" }) };
const PROBE_RECT = { x: 0, y: 0, w: 32, h: VIZ_ROWS };

export function register() {
    return registerSet({
    id: "fader",
    title: "Fader — the viz level column",
    kind: KIND_DRAW,
    replaces: "drawFader",
    /* A full BOX_H cell, not VIZ_ROWS: the band sits one row in from the top of
     * the cell and reaches its last row, so a 13-tall surface would report the
     * shipping widget itself as clipping. */
    probeSize: { w: 32, h: 15 },
    probe: (ctx, draw, v) => draw(ctx, PROBE_RECT, "level", { level: v }, PROBE_META),
    /* The NOW row and the NOW page are the shipping widget itself, drawn
     * through the same probe as every option so the comparison is like for
     * like. */
    baseline: drawFader,
    context: (ctx, draw, info) => {
        const RM = info.RM;
        for (const slot of info.slots) {
            const rowY = slot < 4 ? RM.ROW0_Y : RM.ROW1_Y;
            const cellX = (slot % 4) * RM.CELL_W;
            ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
            const v = (slot + 1) / 9;
            draw(ctx, { x: cellX, y: rowY, w: RM.CELL_W, h: VIZ_ROWS }, "level", { level: v }, PROBE_META);
        }
    },
    options: [
        {
            position: 1, id: "thin-rails", name: "Thin rails", draw: drawThinRails,
            note: "The shipping Movy fader with the fill column taken from 3px to 1px — rails, head and geometry untouched. Half the ink for the same information, and the one change here nobody would have to be argued into. Its weakness is that it is barely a change: at true size a page of these still reads as the same widget, so adopting it answers the weight complaint and not the derivation one.",
        },
        {
            position: 2, id: "no-rails", name: "No rails", draw: drawNoRails,
            note: "Column and head only. The rails are the part that makes this read as a fader rather than as a bar, so dropping them is the smallest change that alters the widget's identity instead of its weight. The cost is real and shows at the bottom of travel: with no track, v=0 is a 7px dash on the floor and nothing says how far it could go.",
        },
        {
            position: 3, id: "capped", name: "Capped", draw: drawCapped,
            note: "The head widened from 7px to 9px so it spans the rails instead of stopping a pixel short of each. That turns a marker floating on a column into a rule across a track, and because every cell shares the same rail spacing the caps line up — the only option in the set that makes values easier to compare BETWEEN cells. Heavier than the baseline, which is the opposite bet to option 1.",
        },
        {
            position: 4, id: "ghost-track", name: "Ghost track", draw: drawGhostTrack,
            note: "The unfilled remainder drawn at CHECKER, so the whole track is a visible object at every value and the cell reads as a control rather than as a mark. Recovers the information the shipping widget throws away — how much travel is left. The cost is that no cell is ever empty, so a page of eight is uniformly grey and the fills have less to stand out against.",
        },
        {
            position: 5, id: "notched", name: "Notched", draw: drawNotched,
            note: "One row cleared out of every four, phased from the floor so the notches sit at fixed heights and do not slide with the value. That makes it a scale rather than a texture — two cells can be compared by counting rungs, which 13 rows of solid column does not allow. Reads as slightly damaged at a glance until you see the second one.",
        },
        {
            position: 6, id: "outline-fill", name: "Outline fill", draw: drawOutlineFill,
            note: "The fill becomes a notched box with a DIAG_HEAVY interior, over dashed rails. It borrows the page's own box vocabulary instead of the mixer's, which is either the point or the objection: the page reads as one system, but a continuous cell and an enum square stop being distinguishable by silhouette. Degrades to a plain bar at the bottom of travel, where there is no interior left to hatch.",
        },
        {
            position: 7, id: "stepped", name: "Stepped", draw: drawStepped,
            note: "Six 2-row blocks from the floor, lit ones solid and unlit ones a dotted lid, so the reading is four-of-six rather than about-two-thirds. Better than the baseline for what faders are actually used for here (mix, send, drive) and worse for anything nudged a detent at a time, because five turns out of six move nothing on screen.",
        },
        {
            position: 8, id: "dotted-thumb", name: "Dotted thumb", draw: drawDottedThumb,
            note: "vimana's render_indicator stood on end: a dashed rail, a solid 3x3 thumb, a stop at each end, no fill. The only option here with a shipped precedent rather than a hypothesis — the reference product draws every continuous parameter this way. It also throws away what a fader is best at: a thumb has no area, so eight of them cannot be compared in one look the way eight fills can.",
        },
        {
            position: 9, id: "terrain", name: "Terrain", draw: drawTerrain,
            note: "Value as mass — DIAG_THIRD hatch under a solid crest, filled bottom-up across a 9px column. Eight of these read as one skyline, so this is the only option in the set that changes what a PAGE looks like rather than what a cell looks like. Pairs exactly with the arc-knob set's terrain-cell, so choosing both gives a page with one fill language and no rails anywhere.",
        },
        {
            position: 10, id: "horizontal", name: "Horizontal", draw: drawHorizontal,
            note: "The fader laid on its side in a notched frame, unused range at DIAG_LIGHT. The case for it is resolution: a 32px cell is twice as wide as it is tall, so a horizontal fill has 26 distinguishable positions against 13. The case against is that it gives up being read as a profile across the page, and it lands on the same silhouette as the enum square, which is also a framed horizontal box.",
        },
    ],
    });
}
