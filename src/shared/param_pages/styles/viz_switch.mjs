/**
 * styles/viz_switch.mjs — SET 11: ten treatments of the two-state cell.
 *
 * POSITION 1 IS NOT A NEUTRAL BASELINE. The shipping widget (`drawSwitch`,
 * viz_draw.mjs) is a 26px circle rasterised into six lookup tables — SW_X,
 * SW_W, SW_IN_X, SW_IN_W, SW_KN_X, SW_KN_W — ported PIXEL FOR PIXEL from
 * schwung-movy's `renderer/knob.ts drawSwitch`. It is not a construction that
 * was designed here and it is not one that can be adjusted: a circle rasterised
 * at one size cannot be stretched to another and stay round, which is why
 * `VIZ_MIN_W` exists at all and why the renderer stands the graphics down
 * rather than let one overhang a narrow cell. Of everything in this catalog it
 * is the most directly derived widget in the fleet, and it is the thing this
 * set exists to move away from — so option 1 is the incumbent under
 * examination, not the reference the others are measured against.
 *
 * That framing matters for the preference data. Ranking option 1 first would
 * mean "keep the port"; every other outcome is a design of our own, and even a
 * narrow win for something else is worth more than the margin suggests.
 *
 * The axis is minimal -> radical:
 *
 *   1-3   still a sliding knob in a track. Same metaphor, progressively less
 *         of Movy's rasterisation in it — option 2 rebuilds the same shape from
 *         rectangles so it scales, option 3 flips which half carries the ink.
 *   4-6   not a slider. A box, a pair of lamps, a lever: two-state controls
 *         that do not pretend anything moves along a track.
 *   7-8   the state is spelled or measured rather than depicted.
 *   9-10  the switch adopts another widget's language entirely — vimana's
 *         thumb rail, then the value-as-mass terrain the knob and fader sets
 *         use, so a boolean becomes the extreme case of a continuous cell.
 *
 * LEGIBILITY IS THE WHOLE JOB HERE. A continuous widget that is slightly hard
 * to read still tells you roughly where you are; a switch that is slightly hard
 * to read tells you nothing at all, because there is no rough. Every option
 * below is drawn so that ON and OFF differ in AREA, not only in position — the
 * probe renders both states side by side for exactly this reason.
 *
 * GEOMETRY. `rect` is the viz cell, `{x, y, w, h}` with `h` = VIZ_ROWS (13),
 * and everything lives in topY = rect.y + 1 .. topY + 12.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { VIZ_ROWS, drawSwitch } from "../viz_draw.mjs";
import { enumIndexOf } from "../param_meta.mjs";
import { fontWidth4x5, fontPrint4x5, FONT4_HEIGHT } from "../font4x5.mjs";
import {
    DIAG_LIGHT, DIAG_THIRD,
    fillDithered, fillTerrain, dottedRule, notchCorners,
} from "./dither.mjs";

function band(rect) {
    const topY = rect.y + 1;
    return { topY, botY: topY + VIZ_ROWS - 1, midY: topY + ((VIZ_ROWS - 1) >> 1) };
}

/**
 * Is this cell ON?
 *
 * Goes through `enumIndexOf` when the module declares options, for the reason
 * drawSwitch's own comment gives: a bare `Number(raw)` reads NaN for the
 * "Off"/"On" spelling and pins the widget to the OFF seat forever. Falls back
 * to the number so the catalog can drive an option without a metaIndex.
 */
function isOn(key, values, metaIndex) {
    const raw = values ? values[key] : undefined;
    const meta = metaIndex && typeof metaIndex.getOrGuess === "function"
        ? metaIndex.getOrGuess(key) : null;
    if (meta && Array.isArray(meta.options)) return enumIndexOf(meta, raw) === 1;
    const n = Number(raw);
    return isFinite(n) && n >= 0.5;
}

const cxOf = (rect) => Math.round(rect.x + rect.w / 2);

/**
 * The pill track and its slug, shared by options 2 and 3.
 *
 * THE SLUG IS INSET TWO PIXELS FROM THE FRAME, not one. Seated at one pixel it
 * is 8-connected to the track wall on the row it sits on, and the two merge:
 * OFF stopped reading as "a block parked at one end of a track" and started
 * reading as "the left half of this box is thick", which is the same picture at
 * both ends of travel and therefore no switch at all. Two pixels leaves a clear
 * column at the outer end and three at the inner one, and the slug is visibly a
 * separate object at both seats.
 */
const PILL_H = 11, SLUG_W = 8, SLUG_H = 7, SLUG_INSET = 3;
function pillGeom(rect) {
    const { topY } = band(rect);
    const w = Math.min(24, rect.w - 4);
    return { x: cxOf(rect) - (w >> 1), y: topY + 1, w, h: PILL_H };
}
function slugX(g, on) {
    return on ? g.x + g.w - SLUG_INSET - SLUG_W : g.x + SLUG_INSET;
}

/** A 1px notched frame. Returns nothing; the interior is the caller's. */
function frame(ctx, x, y, w, h) {
    ctx.fillRect(x, y, w, 1, 1);
    ctx.fillRect(x, y + h - 1, w, 1, 1);
    ctx.fillRect(x, y, 1, h, 1);
    ctx.fillRect(x + w - 1, y, 1, h, 1);
    notchCorners(ctx, x, y, w, h);
}

/* --------------------------------------------------------------- 1..3 --
 * Still a knob in a track. */

/* Movy's tables, copied verbatim from viz_draw.mjs so option 1 is the shipping
 * pixels and not a redrawing of them. */
const SW_X = [4, 2, 1, 1, 0, 0, 0, 1, 1, 2, 4];
const SW_W = [18, 22, 24, 24, 26, 26, 26, 24, 24, 22, 18];
const SW_IN_X = [3, 1, 1, 0, 0, 0, 1, 1, 3];
const SW_IN_W = [18, 22, 22, 24, 24, 24, 22, 22, 18];
const SW_KN_X = [3, 1, 1, 0, 0, 0, 1, 1, 3];
const SW_KN_W = [3, 7, 7, 9, 9, 9, 7, 7, 3];

/** 1. movy-sprite — the port, unchanged. See the file header. */
function drawMovySprite(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const kx = Math.round(rect.x + rect.w / 2 - 8), ky = rect.y;
    const x = kx - 5, y = ky + 2;
    for (let i = 0; i < 11; i++) ctx.fillRect(x + SW_X[i], y + i, SW_W[i], 1, 1);
    if (!on) for (let i = 0; i < 9; i++) ctx.fillRect(x + 1 + SW_IN_X[i], y + 1 + i, SW_IN_W[i], 1, 0);
    const seat = on ? x + 16 : x + 1;
    const v = on ? 0 : 1;
    for (let i = 0; i < 9; i++) ctx.fillRect(seat + SW_KN_X[i], y + 1 + i, SW_KN_W[i], 1, v);
}

/**
 * 2. pill — the same shape, built from rectangles.
 *
 * A notched 24x11 track with a notched 9x9 slug seated left or right. It reads
 * as the same control as option 1 and shares none of its code: no lookup
 * tables, no fixed 26px, so it scales with `rect.w` and would survive a
 * narrower cell — which is the constraint `VIZ_MIN_W` was invented to work
 * around.
 *
 * The honest limitation is that the notch is all the rounding a 1px raster
 * offers, so a pill built this way has square-ish ends. That is the same
 * corner treatment every other box in this UI gets, which makes it consistent
 * and makes it less obviously a "switch".
 */
function drawPill(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const g = pillGeom(rect);
    frame(ctx, g.x, g.y, g.w, g.h);
    const sx = slugX(g, on), sy = g.y + 2;
    ctx.fillRect(sx, sy, SLUG_W, SLUG_H, 1);
    notchCorners(ctx, sx, sy, SLUG_W, SLUG_H);
}

/**
 * 3. pill-inverted — the TRACK carries the state, not the slug.
 *
 * ON fills the whole track and knocks the slug out of it; OFF leaves the track
 * empty with a solid slug. So the two states differ by most of the widget's
 * area rather than by the position of a 9px block, and the cell is legible at a
 * distance where option 2 is a grey lozenge with a bump.
 *
 * This is the strongest version of the slider metaphor available at this size,
 * and its cost is that ON is a dark cell — on a page of eight, several switches
 * ON is a row of black blocks.
 */
function drawPillInverted(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const g = pillGeom(rect);
    const sx = slugX(g, on), sy = g.y + 2;
    if (on) {
        ctx.fillRect(g.x, g.y, g.w, g.h, 1);
        notchCorners(ctx, g.x, g.y, g.w, g.h);
        ctx.fillRect(sx, sy, SLUG_W, SLUG_H, 0);
        /* The knockout gets the notch too, in reverse — four SET pixels at its
         * corners. Without it the hole is the only square-cornered shape on a
         * page where every filled box is softened. */
        ctx.fillRect(sx, sy, 1, 1, 1);
        ctx.fillRect(sx + SLUG_W - 1, sy, 1, 1, 1);
        ctx.fillRect(sx, sy + SLUG_H - 1, 1, 1, 1);
        ctx.fillRect(sx + SLUG_W - 1, sy + SLUG_H - 1, 1, 1, 1);
    } else {
        frame(ctx, g.x, g.y, g.w, g.h);
        ctx.fillRect(sx, sy, SLUG_W, SLUG_H, 1);
        notchCorners(ctx, sx, sy, SLUG_W, SLUG_H);
    }
}

/* --------------------------------------------------------------- 4..6 --
 * Not a slider. */

/**
 * 4. checkbox — one box, filled or not.
 *
 * An 11x11 notched frame; ON fills the interior solid, OFF leaves a DIAG_LIGHT
 * ground so the empty box is still visibly a box rather than an absence. There
 * is no travel, no seat and no second position — which is the point: a boolean
 * has two states and nothing slides between them, and every pixel spent
 * depicting a slide is a pixel not spent on contrast.
 *
 * The smallest widget in the set by a wide margin, which leaves room in the
 * cell that nothing currently uses.
 */
function drawCheckbox(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const { topY } = band(rect);
    const s = 11, x = cxOf(rect) - (s >> 1), y = topY + 1;
    frame(ctx, x, y, s, s);
    if (on) ctx.fillRect(x + 2, y + 2, s - 4, s - 4, 1);
    else fillDithered(ctx, x + 2, y + 2, s - 4, s - 4, DIAG_LIGHT);
}

/**
 * 5. two-lamp — a two-position selector.
 *
 * Two 11x11 notched cells side by side, the active one solid and the other at
 * DIAG_LIGHT. It says the thing a checkbox cannot: that this is a CHOICE
 * between two named positions rather than a flag being set. That matters on
 * this fleet, where `detectSwitch` accepts Off/On, No/Yes and Disabled/Enabled
 * — pairs where neither side is obviously the null one.
 *
 * It is also the natural bridge to a three-or-more enum, so a page could use
 * one visual language for every small discrete parameter instead of two.
 */
function drawTwoLamp(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const { topY } = band(rect);
    const s = 11, gap = 2;
    const x = cxOf(rect) - s - (gap >> 1), y = topY + 1;
    const cells = [x, x + s + gap];
    for (let i = 0; i < 2; i++) {
        frame(ctx, cells[i], y, s, s);
        if ((i === 1) === on) ctx.fillRect(cells[i] + 2, y + 2, s - 4, s - 4, 1);
        else fillDithered(ctx, cells[i] + 2, y + 2, s - 4, s - 4, DIAG_LIGHT);
    }
}

/**
 * 6. lever — a vertical two-position throw.
 *
 * A 9x13 notched frame with a solid 5-row slug at the top or the bottom and a
 * dotted rule across the middle marking the throw. Vertical, which is the whole
 * argument: nothing else on a knob grid is tall and narrow, so a switch drawn
 * this way is identifiable by SILHOUETTE before it is read — the state is the
 * second question, not the first.
 *
 * Up-is-on is a hardware convention and it is not universal; on a page mixing
 * these with faders, a lever at the bottom and a fader at the bottom mean
 * different things and look similar.
 */
function drawLever(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const { topY, botY, midY } = band(rect);
    const w = 9, x = cxOf(rect) - (w >> 1), h = botY - topY + 1;
    frame(ctx, x, topY, w, h);
    dottedRule(ctx, x + 1, midY, w - 2, 0);
    /* Two pixels of inset on every side, for the reason `pillGeom` records: a
     * slug seated against the wall fuses with it and the throw stops reading as
     * a throw. Here that also keeps it a clear row off the dotted mid rule,
     * which is the mark the throw is measured against. */
    const sy = on ? topY + 2 : botY - 5;
    ctx.fillRect(x + 2, sy, w - 4, 4, 1);
    notchCorners(ctx, x + 2, sy, w - 4, 4);
}

/* --------------------------------------------------------------- 7..8 --
 * Spelled, or measured. */

/**
 * 7. text-state — the state written out.
 *
 * "ON" on a solid notched plate with the glyphs knocked out; "OFF" as plain
 * text in a notched frame. No graphic at all, and the case for it is the same
 * as the arc-knob set's numeric slab: every other option answers "which
 * position is it in" and requires you to know which position means what, while
 * this one answers the actual question. For Off/No/Disabled pairs, where the
 * two sides are not symmetric, that is worth more than it sounds.
 *
 * The cost is that a page of eight text cells cannot be scanned as a shape the
 * way a page of eight graphics can, and ON and OFF differ in width, so a row of
 * them does not line up.
 */
function drawTextState(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const { topY } = band(rect);
    const t = on ? "ON" : "OFF";
    const tw = fontWidth4x5(t);
    const w = tw + 8, h = 11;
    const x = cxOf(rect) - (w >> 1), y = topY + 1;
    if (on) {
        ctx.fillRect(x, y, w, h, 1);
        notchCorners(ctx, x, y, w, h);
    } else {
        frame(ctx, x, y, w, h);
    }
    fontPrint4x5(ctx, x + 4, y + Math.floor((h - FONT4_HEIGHT) / 2), t, on ? 0 : 1);
}

/**
 * 8. bar-half — the boolean as a two-position fill.
 *
 * A full-width notched bar with the left half solid for OFF and the right half
 * solid for ON. It puts a switch in the same visual family as a fader — a
 * framed track that is partly filled — so a page mixing continuous and boolean
 * parameters reads as one system instead of as two vocabularies sharing a grid.
 *
 * The objection is the mirror of the argument: making a boolean look like a
 * half-full continuous control invites the reading that it could be
 * three-quarters full, which is the one thing it can never be.
 */
function drawBarHalf(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const { topY } = band(rect);
    const w = rect.w - 6, h = 9;
    const x = rect.x + 3, y = topY + 2;
    frame(ctx, x, y, w, h);
    const half = (w - 2) >> 1;
    const fx = on ? x + 1 + (w - 2 - half) : x + 1;
    ctx.fillRect(fx, y + 1, half, h - 2, 1);
}

/* -------------------------------------------------------------- 9..10 --
 * Another widget's language. */

/**
 * 9. dot-rail — vimana's indicator, quantised to two stops.
 *
 * A 50%-dotted rail with end stops and a solid 3x3 thumb parked at one end or
 * the other. Identical construction to the fader set's dotted-thumb, which is
 * the point: pick both and every parameter on the page — continuous, boolean —
 * is drawn with one rail and one thumb, and the only difference is how many
 * places the thumb can be.
 *
 * At 3x3 the thumb is small, and the two states differ ONLY in its position.
 * That is the weakness the file header warns about, and it is why this sits at
 * 9 rather than lower: it is the most coherent option in the set and among the
 * least legible.
 */
function drawDotRail(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const { midY } = band(rect);
    const w = Math.min(24, rect.w - 4);
    const x = cxOf(rect) - (w >> 1);
    dottedRule(ctx, x + 2, midY, w - 4, 0);
    ctx.fillRect(x, midY - 2, 1, 5, 1);
    ctx.fillRect(x + w - 1, midY - 2, 1, 5, 1);
    const tx = on ? x + w - 4 : x + 1;
    ctx.fillRect(tx, midY - 1, 3, 3, 1);
}

/**
 * 10. terrain-flip — the boolean as the extreme of a continuous cell.
 *
 * `fillTerrain` across the cell: ON is a full-height DIAG_THIRD mass under a
 * solid crest at the top, OFF is the same construction collapsed onto the
 * floor. The states differ by the entire area of the cell, which makes it the
 * most legible option here by a distance — readable across a room, readable out
 * of the corner of your eye, and impossible to misread as the other one.
 *
 * It is the end of the axis because it abandons the idea that a switch is a
 * different KIND of control. On a page where the knob and fader sets are also
 * terrain, a boolean becomes a parameter that happens to have two values, which
 * is a coherent position and a genuinely arguable one — a control that can only
 * be full or empty is arguably owed a shape that says so.
 */
function drawTerrainFlip(ctx, rect, key, values, metaIndex) {
    const on = isOn(key, values, metaIndex);
    const { topY, botY } = band(rect);
    const w = Math.min(26, rect.w - 4);
    const x = cxOf(rect) - (w >> 1), h = botY - topY;
    /*
     * The floor row is drawn SOLID and separately, not left to fillTerrain. At
     * height 0 fillTerrain puts the crest one row past the bottom edge and
     * emits nothing, so OFF would be an empty cell — indistinguishable from a
     * parameter that failed to load, which is the one thing a switch may never
     * look like.
     */
    ctx.fillRect(x, botY, w, 1, 1);
    fillTerrain(ctx, x, topY, w, h, new Array(w).fill(on ? 1 : 0), DIAG_THIRD, true);
    notchCorners(ctx, x, topY, w, h + 1);
}

/* --------------------------------------------------------------- probe --
 *
 * BOTH STATES, side by side, in one swatch. A switch drawn at one state cannot
 * be judged at all — the only question worth asking about it is whether ON and
 * OFF are unmistakable — so the probe renders OFF on the left and ON on the
 * right in two adjacent 32px cells and ignores the value the catalog passes.
 */
const PROBE_META = {
    getOrGuess: (key) => ({ key, type: "enum", kind: "enum", options: ["Off", "On"] }),
};

export function register() {
    return registerSet({
    id: "viz_switch",
    title: "Switch — the two-state cell",
    kind: KIND_DRAW,
    replaces: "drawSwitch",
    probeSize: { w: 64, h: 15 },
    probe: (ctx, draw) => {
        draw(ctx, { x: 0, y: 0, w: 32, h: VIZ_ROWS }, "sw", { sw: "Off" }, PROBE_META);
        draw(ctx, { x: 32, y: 0, w: 32, h: VIZ_ROWS }, "sw", { sw: "On" }, PROBE_META);
    },
    baseline: drawSwitch,
    context: (ctx, draw, info) => {
        const RM = info.RM;
        for (const slot of info.slots) {
            const rowY = slot < 4 ? RM.ROW0_Y : RM.ROW1_Y;
            const cellX = (slot % 4) * RM.CELL_W;
            ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
            /* Alternating, so one page shows four of each and the two states
             * can be compared where they will actually sit — next to each
             * other, at true size, under their own labels. */
            draw(ctx, { x: cellX, y: rowY, w: RM.CELL_W, h: VIZ_ROWS },
                 "sw", { sw: (slot % 2) ? "On" : "Off" }, PROBE_META);
        }
    },
    options: [
        {
            position: 1, id: "movy-sprite", name: "Movy sprite", draw: drawMovySprite,
            note: "The shipping widget, and NOT a neutral baseline: it is a 26px circle held in six lookup tables, ported pixel for pixel from schwung-movy's renderer/knob.ts. Nothing about it was designed here and nothing about it can be adjusted — a circle rasterised at one size cannot be stretched and stay round, which is why VIZ_MIN_W exists and why the renderer stands graphics down rather than let one overhang. It is the most directly derived widget in the fleet and it is the thing this set exists to move away from. Ranking it first means keeping the port.",
        },
        {
            position: 2, id: "pill", name: "Pill", draw: drawPill,
            note: "The same control rebuilt from rectangles: a notched 24x11 track with a notched 9x9 slug. Shares no code and no tables with option 1, so it scales with the cell and would survive a width VIZ_MIN_W currently forbids. Honest limitation: a notch is all the rounding a 1px raster offers, so the ends are square-ish — consistent with every other box in this UI, and correspondingly less obviously a switch.",
        },
        {
            position: 3, id: "pill-inverted", name: "Pill inverted", draw: drawPillInverted,
            note: "The track carries the state instead of the slug: ON fills the whole track and knocks the slug out of it, OFF is an empty track with a solid slug. The two states now differ by most of the widget's area rather than by the position of one block, which makes it legible at a distance where option 2 is a lozenge with a bump. The cost is that ON is a dark cell, so several switches on becomes a row of black blocks.",
        },
        {
            position: 4, id: "checkbox", name: "Checkbox", draw: drawCheckbox,
            note: "An 11x11 notched box, solid inside for ON and DIAG_LIGHT for OFF so the empty state is still visibly a box. No travel, no seat, nothing sliding — which is the argument: a boolean has two states and nothing moves between them, so every pixel spent depicting a slide is a pixel not spent on contrast. Smallest widget in the set, which leaves cell room nothing currently uses.",
        },
        {
            position: 5, id: "two-lamp", name: "Two lamp", draw: drawTwoLamp,
            note: "Two 11x11 notched cells, the active one solid and the other at DIAG_LIGHT. Says what a checkbox cannot: that this is a choice between two positions rather than a flag being set — which matters here, because detectSwitch accepts No/Yes and Disabled/Enabled as well as Off/On, and in none of those is one side obviously the null one. Also the natural bridge to a three-option enum, so one language could cover every small discrete param.",
        },
        {
            position: 6, id: "lever", name: "Lever", draw: drawLever,
            note: "A 9x13 notched frame with a solid slug thrown to the top or the bottom, over a dotted mid rule. Vertical is the whole argument: nothing else on a knob grid is tall and narrow, so this is identifiable by silhouette before it is read and the state becomes the second question rather than the first. Up-is-on is a hardware convention rather than a universal one, and on a page mixing these with faders a low lever and a low fader mean different things while looking alike.",
        },
        {
            position: 7, id: "text-state", name: "Text state", draw: drawTextState,
            note: "ON knocked out of a solid notched plate, OFF as plain text in a notched frame. No graphic at all: every other option answers which position it is in and needs you to know what that position means, while this answers the question directly — worth most for the asymmetric pairs where Off and Disabled are not interchangeable. Costs the ability to scan a page as a shape, and ON and OFF are different widths, so a row of them does not line up.",
        },
        {
            position: 8, id: "bar-half", name: "Bar half", draw: drawBarHalf,
            note: "A full-width notched bar, left half solid for OFF and right half for ON, which puts the switch in the same visual family as a fader — a framed track, partly filled. A page mixing continuous and boolean params then reads as one system instead of two vocabularies sharing a grid. The objection is the mirror of the argument: a half-full control invites the reading that it could be three-quarters full, which is the one thing it can never be.",
        },
        {
            position: 9, id: "dot-rail", name: "Dot rail", draw: drawDotRail,
            note: "vimana's render_indicator quantised to two stops: dotted rail, end stops, a solid 3x3 thumb parked at one end. Identical construction to the fader set's dotted-thumb, so picking both gives a page where every parameter is one rail and one thumb and the only difference is how many places the thumb can sit. The most coherent option in the set and among the least legible — the two states differ only in the position of a 3px block, which is exactly the failure a switch cannot afford.",
        },
        {
            position: 10, id: "terrain-flip", name: "Terrain flip", draw: drawTerrainFlip,
            note: "fillTerrain at full height for ON and collapsed onto the floor for OFF, so the states differ by the entire area of the cell. The most legible option here by a distance: readable across a room, readable peripherally, impossible to misread as the other one. It is the end of the axis because it gives up the idea that a switch is a different kind of control — on a page where the knobs and faders are also terrain, a boolean becomes a parameter that happens to have two values, which is a coherent and genuinely arguable position.",
        },
    ],
    });
}
