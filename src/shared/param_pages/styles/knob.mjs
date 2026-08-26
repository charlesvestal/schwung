/**
 * styles/knob.mjs — SET 1: ten alternatives to the arc knob.
 *
 * The shipping widget (`drawArcKnob`, render_page_movy.mjs) is the single most
 * Elektron-shaped thing on the screen, and it says so in its own comments: its
 * 230-degree start and 260-degree sweep were "measured off Elektron's screen
 * (128x64, recovered from a 4x capture)". If any one widget has to move for
 * this UI to stop reading as a clone, it is this one.
 *
 * ORDERING IS BY FAMILY, NOT BY DISTANCE. The first cut of this set was
 * ordered minimal -> radical, and ordering by distance is what produced it:
 * position 10 has to be the furthest thing from the baseline, so the tail of
 * the set was chosen for novelty and novelty is not quality. That set was
 * reported back as "all pretty shitty", which was right — a stipple that reads
 * as punctuation, a sunburst you cannot read a value off, digits over a
 * checker ground. None of that is fixable by tuning; it was the wrong brief.
 *
 * So this set is ten designs that could ship, grouped by construction:
 *
 *   1-5   ARC FAMILY. An open or closed track with the value carried by a
 *         pointer, a dot, a fill or a notch.
 *   6-7   CENTRE-RADIATING. No track — the value is the angle of something
 *         drawn out of the hub.
 *   8-9   LINEAR. A framed bar, vertical then horizontal.
 *   10    GEOMETRIC. A closed ring whose value is a hole in it.
 *
 * Every option here obeys the same four rules, and they are rules rather than
 * preferences because each one is a way the previous set failed:
 *
 *   - SOLID INK ONLY. No dither anywhere inside the widget. At 17x15 a 50%
 *     ground laps at a 1px stroke and the whole cell turns to mush; dither is
 *     for the large grounds and fills elsewhere in this catalog.
 *   - v=0 AND v=1 MUST BE UNMISTAKABLE, and unmistakably different from each
 *     other. Anything that draws nothing at one end fails, so every option
 *     carries a permanent frame, track or end mark.
 *   - MONOTONE TRAVEL, verified by eye at v = 0, .25, .5, .75, 1 rather than
 *     assumed from the maths.
 *   - NOTHING THAT READS AS A GLYPH. If a cell can be mistaken for a comma, a
 *     bracket or a rendering fault, it is not a control.
 *
 * GEOMETRY. Every option gets a box of KW (17) by BOX_H (15) at (kx, ky), and
 * must not put a pixel outside it. One row of overflow lands on the label
 * band, which the grid does not repaint — it would persist on hardware and
 * appear nowhere in a preview of a single widget.
 *
 * Two centres are in use, and the difference is forced rather than stylistic.
 * The OPEN-ARC options inherit the shipping centre (kx+8, ky+8) at r=8, which
 * fits a 15-row box only because the track is open at the bottom: a closed
 * circle at that radius would need 17 rows. The CLOSED-RING options therefore
 * sit at (kx+8, ky+7) with r=7, which spans exactly ky..ky+14. Raising the
 * radius by one, or dropping the centre by one, clips — silently, on hardware
 * only, which is what `tests/host/test_style_catalog.sh` asserts against.
 *
 * MODULATION, and this is the part the catalog CANNOT show you. `drawModDot`
 * rides a 5px plus centred at radius KNOB_R-2 = 6, so it occupies r=5..7.
 * `renderInContext` clears the widget box and draws only the option, so no
 * page in catalog-out/ has a modulation dot on it. Verdicts from the geometry:
 *
 *   1 arc-pointer   clear. Pointer stops at 0.74r, inside the dot's band, but
 *                   they coincide only when the live value equals the base —
 *                   which is true of the shipping widget too.
 *   2 arc-dot       COLLIDES by construction: this option's own dot rides at
 *                   r=5, straight through the modulation band. Adopting it
 *                   means giving the two marks different shapes (a filled dot
 *                   for the value, the existing plus for modulation) or
 *                   different radii.
 *   3 arc-fill      clear. The fill is r=7..8, the dot spans r=5..7 — they
 *                   share one radius, so the mark reads as a bump on the
 *                   inside of the fill. Legible but not clean; MOD_DOT_R 6->5
 *                   separates them.
 *   4 arc-thick     COLLIDES. The 2px track is r=7..8 and the dot's outer arm
 *                   is at r=7. Same fix as 3.
 *   5 ring-fill     COLLIDES on the filled span (r=5..7) and is clear on the
 *                   rest, so the mark would blink as the value passed it.
 *   6 needle-only   NO HOST, and not for want of room — its pivot is at
 *                   (kx+8, ky+12) and its sweep is 124 degrees, so a dot on
 *                   the shipping centre and radius would sit in the middle of
 *                   the needle's travel and mean nothing. A VU meter's
 *                   modulation mark is a second, shorter needle.
 *   7 wedge         COLLIDES. The pie is solid out to r=6.
 *   8, 9            NO HOST. A linear bar needs a linear modulation mark — a
 *                   tick outside the frame, or a second thumb.
 *   10 notch-ring   clear. The ring is r=7 exactly and the dot's outer arm is
 *                   r=7, so they touch; one pixel of radius on either fixes
 *                   it, and unlike 4 there is no 2px band to shrink.
 *
 * That is a real cost and it is not a reason to drop any of these: the dot is
 * ~20 lines and moves with whatever widget wins. It is a reason not to read
 * this catalog as ten drop-in replacements.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { KW, BOX_H, KNOB_R } from "../render_page_movy.mjs";
import { notchCorners } from "./dither.mjs";
import { ring, ringBand } from "./ring.mjs";

/* The shipping numbers, repeated rather than imported because they are not
 * exported — and because an option is allowed to disagree with them. */
const ARC_START = 230, ARC_SWEEP = 260;   /* the track */
const PTR_START = 225, PTR_SWEEP = 270;   /* the pointer's travel, 5 degrees proud at each end */

/* The closed-ring geometry. See the header: r=7 about (kx+8, ky+7) is the
 * largest full circle that fits fifteen rows. */
const RING_R = 7;
const RING_CY = 7;

const clamp01 = (v) => (v > 1 ? 1 : v < 0 ? 0 : (typeof v === "number" && v === v ? v : 0));
const rad = (deg) => deg * Math.PI / 180;

/* ---------------------------------------------------------- primitives --
 *
 * `line` is optional on a draw context (render_page_movy takes a different
 * path when the caller omits them), so every use here goes through a wrapper
 * with the same JS fallback the device binding is ported from. A catalog that
 * only ever exercised the native path would be a preview that can disagree
 * with the OLED, which is the specific way a knob-ring bug got through here
 * before.
 *
 * `drawArc` is no longer used AT ALL — see styles/ring.mjs. Its rasteriser is
 * a distance-rounded union of a row scan and a column scan, and near the top
 * and bottom of a circle the column scan puts five consecutive pixels on one
 * row: a five-pixel flat cap, so a radius-6 circle reads as a rounded
 * rectangle. Every option in the arc family inherited that, and it is the
 * whole of why the set was reported back as looking sloppy. The rings are
 * tabulated instead, which is what `drawSwitch` in viz_draw.mjs already does
 * for exactly this reason. Nothing about the designs changed. */

function seg(ctx, x0, y0, x1, y1, color = 1) {
    x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
    if (typeof ctx.line === "function") { ctx.line(x0, y0, x1, y1, color); return; }
    let dx = Math.abs(x1 - x0), dy = Math.abs(y1 - y0);
    const sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1;
    let err = dx - dy;
    for (;;) {
        ctx.fillRect(x0, y0, 1, 1, color);
        if (x0 === x1 && y0 === y1) break;
        const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

/**
 * A solid pie from the hub, plotted by SAMPLING the angle.
 *
 * Not tabulated, and it does not need to be: the pie's only curved edge is its
 * outer rim, which is covered by the tabulated track drawn one pixel outside
 * it, so no cap of the pie is ever the silhouette of the widget. Sampling
 * finely enough that consecutive samples never skip a pixel is what gives its
 * two RADIAL edges — the part a wedge is actually read by — clean straight
 * ends.
 */
function pie(ctx, cx, cy, r, start, sweep) {
    if (r <= 0) return;
    const steps = Math.max(2, Math.ceil(Math.max(sweep, 1) * Math.PI / 180 * r * 2));
    for (let i = 0; i <= steps; i++) {
        const a = rad(start + sweep * i / steps);
        const s = Math.sin(a), c = Math.cos(a);
        for (let rr = 0; rr <= r; rr++)
            ctx.fillRect(Math.round(cx + rr * s), Math.round(cy - rr * c), 1, 1, 1);
    }
}

/** Point on the dial at `t` (0..1) along the pointer's travel, radius `rr`. */
function polar(kx, ky, t, rr, startDeg = PTR_START, sweepDeg = PTR_SWEEP, cyOff = KNOB_R) {
    const a = rad(startDeg + t * sweepDeg);
    return { x: kx + KNOB_R + rr * Math.sin(a), y: ky + cyOff - rr * Math.cos(a) };
}

/**
 * A solid 3x3 bead.
 *
 * ODD-SIZED, not a 2x2, and that is not a taste call: an even-sized mark has
 * its centroid on a pixel BOUNDARY, so whichever way it rounds it lands half a
 * pixel off the angle it is showing, and at the cardinal angles floating point
 * decides the tie — at exactly 50% sin() returns -2.4e-16 rather than 0 and the
 * block rounds a whole pixel to the left of the knob centre.
 *
 * SOLID, not the five-pixel plus `drawModDot` uses. The plus is right for a
 * mark that has to stay legible while overlapping a ring; this one has two
 * pixels of clear track on either side, so it can afford to be a bead, and at
 * 4x the plus read as sparse — four separated pixels rather than one mark.
 */
function dot(ctx, x, y) {
    ctx.fillRect(Math.round(x) - 1, Math.round(y) - 1, 3, 3, 1);
}

/** A short radial stub between two radii — the scale mark idiom here. */
function stub(ctx, cx, cy, rIn, rOut, deg) {
    const s = Math.sin(rad(deg)), c = Math.cos(rad(deg));
    for (let r = rIn; r <= rOut; r++)
        ctx.fillRect(Math.round(cx + r * s), Math.round(cy - r * c), 1, 1, 1);
}

/* --------------------------------------------------------------- 1..5 --
 * The arc family. */

/**
 * 1. arc-pointer — the classic, refined.
 *
 * Open track at r=8, needle from the hub out to 0.74r. The one change from the
 * shipping widget is that outer stop: at the shipping 0.85r (6.8px against a
 * ring at 8) the needle's tip lands adjacent to the track and merges with it,
 * so the marker reads as a lump growing off the rim rather than as a pointer.
 * 0.74r leaves a clear pixel of track at every angle.
 *
 * This is the control against which the other nine are judged. If none of them
 * beats it, the honest outcome of the exercise is to keep the dial and change
 * something else.
 */
function drawArcPointer(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    ring(ctx, kx + KNOB_R, ky + KNOB_R, KNOB_R, ARC_START, ARC_SWEEP, 1);
    const tip = polar(kx, ky, v, KNOB_R * 0.74);
    seg(ctx, kx + KNOB_R, ky + KNOB_R, tip.x, tip.y, 1);
}

/**
 * 2. arc-dot — the value is a bead riding inside the track.
 *
 * Same track, no needle: a 5px dot at r=5, which is far enough in that a clear
 * ring of unlit pixels separates it from the track at every angle, and far
 * enough out that it is unambiguously travelling rather than sitting on the
 * hub. The hub pip is not decoration — without it the widget at any value is
 * one arc and one blob, and the blob has no centre to be measured against.
 *
 * Reads as a lighter, calmer dial than the pointer: at a page of eight the
 * difference is that the eye tracks eight beads instead of eight angles.
 */
function drawArcDot(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ring(ctx, cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    const p = polar(kx, ky, v, 5);
    ctx.fillRect(cx, cy, 1, 1, 1);
    dot(ctx, p.x, p.y);
}

/**
 * 3. arc-fill — the travelled portion of the track, and nothing else.
 *
 * A 2px band grown from the start of travel to the value, with a 4px radial
 * stop at each end of the sweep. The stops are what make this legible at the
 * bottom of the range: a fill alone draws nothing at v=0, and a cell that
 * draws nothing is indistinguishable from an unpopulated one. With them, v=0
 * is a visibly EMPTY gauge — which is a value, not an absence.
 *
 * The most direct reading of the four arc options: there is no marker to
 * locate, only a quantity of ink, so the page can be scanned rather than read.
 */
function drawArcFill(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    /*
     * The UNTRAVELLED range stays drawn, as the 1px track. Two rounds were
     * spent trying to honour "the travelled portion and nothing else", and
     * both failed at the bottom of the range for the same reason: with nothing
     * behind the fill, v=0 is whatever end marks you invented to stop the cell
     * being empty, and a pair of 4px radial stubs on a black field reads as
     * debris rather than as an empty gauge. Keeping the track costs one thin
     * arc and buys a v=0 that is unmistakably a control at rest.
     */
    ring(ctx, cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    /* The fill grows INWARD from the same outer radius, so the track is never
     * erased or moved — the silhouette is constant and only its weight
     * changes. Three pixels rather than two: against a 1px track, 2px reads as
     * a slightly darker stretch of the same line, and 3:1 is the ratio at
     * which the boundary between filled and unfilled becomes the thing you
     * see first. */
    ringBand(ctx, cx, cy, KNOB_R - 2, KNOB_R, ARC_START, v * ARC_SWEEP);
}

/**
 * 4. arc-thick — a heavy track with a short stub pointer.
 *
 * The answer to the real complaint about eight 1px dials in a row: they turn
 * to grey mush at true size before anyone gets as far as calling them
 * derivative. The track is 2px (r=7..8) and the pointer is a 2px stub floating
 * between 0.30r and 0.62r, so the widget's weight is in the frame and the
 * marker is a deliberate, blunt mark inside it rather than a hair.
 *
 * The stub stops well short of the track (5.0 against 7) so that at no angle
 * do the two shapes touch — a fat pointer welded to a fat ring is a keyhole,
 * not a control.
 */
function drawArcThick(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ringBand(ctx, cx, cy, KNOB_R - 1, KNOB_R, ARC_START, ARC_SWEEP);
    const a = rad(PTR_START + v * PTR_SWEEP);
    const inner = { x: cx + KNOB_R * 0.30 * Math.sin(a), y: cy - KNOB_R * 0.30 * Math.cos(a) };
    const outer = { x: cx + KNOB_R * 0.62 * Math.sin(a), y: cy - KNOB_R * 0.62 * Math.cos(a) };
    /*
     * Thicken along ONE AXIS, never along the true perpendicular.
     *
     * The perpendicular is the obvious choice and it is wrong on a raster: at
     * 45 degrees it is (1,-1), so the copy is displaced along the ANTI-
     * diagonal — which is the one direction a 45-degree Bresenham line is
     * invariant under. The two strokes come out as parallel hairlines with a
     * lit gap between them: a hollow wedge, not a 2px pointer.
     *
     * A pure (1,0) or (0,1) displacement always yields a solid double stroke,
     * because a Bresenham line and the same line shifted one pixel along an
     * axis are 8-connected everywhere. Pick the axis the stroke is NOT running
     * along, or the copy lands on top of the original and thickens nothing.
     */
    const steep = Math.abs(Math.cos(a)) >= Math.abs(Math.sin(a));
    const ox = steep ? 1 : 0, oy = steep ? 0 : -1;
    seg(ctx, inner.x, inner.y, outer.x, outer.y, 1);
    seg(ctx, inner.x + ox, inner.y + oy, outer.x + ox, outer.y + oy, 1);
}

/**
 * 5. ring-fill — a closed collar filled clockwise from twelve o'clock.
 *
 * The only option in the set that uses the whole circle, which is what makes
 * it distinct from 3 at a glance rather than on inspection: 3 is an open gauge
 * with two feet, this is a complete O whose rim thickens. The thin ring is
 * always there, so the unused range stays visible and v=0 is an outline rather
 * than a blank.
 *
 * Radius drops to 7 about a centre one row higher, because a closed circle at
 * the shipping r=8 needs seventeen rows and the box has fifteen.
 */
function drawRingFill(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + RING_CY;
    ring(ctx, cx, cy, RING_R, 0, 360, 1);
    ctx.fillRect(cx, cy, 1, 1, 1);
    ringBand(ctx, cx, cy, RING_R - 2, RING_R, 0, v * 360);
}

/* --------------------------------------------------------------- 6..7 --
 * Centre-radiating: no track at all. */

/**
 * 6. needle-only — a VU needle over a base rule.
 *
 * "No arc at all — just a clean needle from centre, plus a base mark" went
 * through three rounds as a needle at the centre of an INVISIBLE circle, and
 * all three failed in context for one reason: a stroke needs something to be
 * at an angle TO. Three marks where the ring would have been read as dirt;
 * five read as noise; adding a hub helped and did not fix it. On the page
 * every cell was a diagonal lying next to some specks, and not one of them
 * said "control".
 *
 * The base rule fixes it, because a horizontal line is a reference the eye
 * accepts without being told. Pivot on the rule, sweep 124 degrees above it,
 * and the construction is a VU meter — which is the one gauge that has never
 * drawn its own arc and has never needed to.
 *
 * It is also the most distinct silhouette in the set by a distance: nine of
 * these are round or rectangular, and this is a triangle.
 */
const VU_HALF = 62;    /* degrees either side of vertical */
const VU_LEN = 9;      /* 9*sin(62) = 7.9, which is the widest a 17px cell allows */
function drawNeedleOnly(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + BOX_H - 3;
    /*
     * The rule sits one row PROUD of the bottom of the box, and stops a pixel
     * short at each end. On the last row and at full width it is directly
     * against the label band, where it stops being this widget's base and
     * becomes an underline on the parameter name — visible on the page render
     * and nowhere else. One clear row and two clear columns reattach it.
     */
    ctx.fillRect(kx + 1, ky + BOX_H - 2, KW - 2, 1, 1);
    const a = rad(-VU_HALF + v * 2 * VU_HALF);
    seg(ctx, cx, cy, cx + VU_LEN * Math.sin(a), cy - VU_LEN * Math.cos(a), 1);
    /* A 3x3 pivot. Without it the needle meets the rule at a bare point and
     * the two read as one bent line rather than as a pointer on a scale. */
    ctx.fillRect(cx - 1, cy - 1, 3, 3, 1);
}

/**
 * 7. wedge — the value as a solid pie swept out of the hub.
 *
 * The heaviest option, and the one that reads from furthest away: value is
 * carried by AREA rather than by the position of a mark, so it survives being
 * glanced at, seen out of focus, or photographed badly. The open track at r=8
 * stays, one clear pixel outside the pie, so the wedge always has a scale to
 * be read against and the ends of travel are marked.
 *
 * At v=0 the pie degenerates to a single radial line at the start of travel,
 * which is correct rather than a special case — zero area is zero value — and
 * the track behind it is what stops that reading as an empty cell.
 */
function drawWedge(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ring(ctx, cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    pie(ctx, cx, cy, KNOB_R - 2, PTR_START, v * PTR_SWEEP);
}

/* --------------------------------------------------------------- 8..9 --
 * Linear. */

/**
 * 8. bar-vertical — a fader in a notched frame, filling upward.
 *
 * The most universally legible construction there is, and the one that needs
 * no explanation to anybody who has seen a mixer. Corner notches, because a
 * framed box in this UI is a notched box — the idiom recurs across the catalog
 * rather than being one option's trick.
 *
 * The frame is 9px wide in a 17px cell, so the widget is a tall column with
 * air either side. That is deliberate: it keeps the cell's silhouette
 * different from the ENUM SQUARE, which is a wide framed rectangle, so a
 * continuous cell and a discrete one still read apart at a glance.
 */
function drawBarVertical(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const bx = kx + 4, by = ky, bw = 9, bh = BOX_H;
    ctx.fillRect(bx, by, bw, 1, 1);
    ctx.fillRect(bx, by + bh - 1, bw, 1, 1);
    ctx.fillRect(bx, by, 1, bh, 1);
    ctx.fillRect(bx + bw - 1, by, 1, bh, 1);
    notchCorners(ctx, bx, by, bw, bh);
    /* One clear pixel between the frame and the fill on all four sides, so a
     * full bar is a bar inside a box rather than a solid block. */
    const tx = bx + 2, ty = by + 2, tw = bw - 4, th = bh - 4;
    const fh = Math.round(v * th);
    if (fh > 0) ctx.fillRect(tx, ty + th - fh, tw, fh, 1);
}

/**
 * 9. bar-horizontal — the same construction laid down, filling rightward.
 *
 * Worth having as a separate option rather than as a rotation of 8: the two
 * are read differently. A vertical bar is a LEVEL and its neighbours form a
 * skyline, so a page of eight can be compared in one look. A horizontal bar is
 * a PROGRESS and each one is read on its own, which suits a page where the
 * parameters are unrelated to each other.
 *
 * It also spans the full cell width, so it is the highest-contrast option here
 * at a distance — and the one most likely to be confused with the enum square.
 * That trade is the point of putting both in front of a human.
 */
function drawBarHorizontal(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const bx = kx, by = ky + 3, bw = KW, bh = 9;
    ctx.fillRect(bx, by, bw, 1, 1);
    ctx.fillRect(bx, by + bh - 1, bw, 1, 1);
    ctx.fillRect(bx, by, 1, bh, 1);
    ctx.fillRect(bx + bw - 1, by, 1, bh, 1);
    notchCorners(ctx, bx, by, bw, bh);
    const tx = bx + 2, ty = by + 2, tw = bw - 4, th = bh - 4;
    const fw = Math.round(v * tw);
    if (fw > 0) ctx.fillRect(tx, ty, fw, th, 1);
}

/* ----------------------------------------------------------------- 10 --
 * Geometric. */

/**
 * 10. notch-ring — a closed ring with one hole in it, and the hole is the value.
 *
 * The inverse of every other option: nothing is drawn AT the value, the value
 * is where the ring stops. That is a real advantage on a 1-bit display, where
 * a hole in a continuous stroke is about the most reliably visible thing there
 * is — it needs no clearance from anything, because it IS clearance.
 *
 * The notch travels the pointer's 270 degrees rather than a full circle, so
 * v=0 puts it at the lower left and v=1 at the lower right and the two are
 * plainly different. A 44-degree notch is ~5px of arc at r=7: unmistakably a
 * gap, where the 26 degrees this was first drawn at is 3px and reads as a
 * rasterisation wobble.
 */
function drawNotchRing(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + RING_CY;
    /*
     * A 2px ring with a 56-degree notch, not a 1px ring with 44.
     *
     * A hole is only readable against the stroke it interrupts. On a 1px ring
     * a 44-degree bite is ~5px of missing hairline, which at true size is a
     * ring that looks very slightly imperfect — you have to hunt for the value
     * rather than see it. Doubling the stroke and widening the notch makes the
     * ring heavy enough that the gap is the brightest event in the cell.
     */
    const NOTCH = 56;
    const at = PTR_START + v * PTR_SWEEP;
    ringBand(ctx, cx, cy, RING_R - 1, RING_R, at + NOTCH / 2, 360 - NOTCH);
    /* The hub pip anchors the hole. Without it the widget is a broken circle,
     * and a broken circle with no centre reads as a drawing error rather than
     * as a reading off a scale. */
    ctx.fillRect(cx, cy, 1, 1, 1);
}

/**
 * Called from the bottom of index.mjs rather than running on import — see the
 * cycle note there. Registering at module scope would read the KIND_* consts
 * while they are still in the temporal dead zone.
 */
export function register() {
    return registerSet({
    id: "knob",
    title: "Arc knob — the continuous-value widget",
    kind: KIND_DRAW,
    replaces: "drawArcKnob",
    options: [
        {
            position: 1, id: "arc-pointer", name: "Arc pointer", draw: drawArcPointer,
            note: "The shipping construction with the one defect taken out: the needle stops at 0.74r instead of 0.85r, so its tip no longer merges with the track and the marker reads as a pointer rather than as a lump on the rim. This is the control the other nine are judged against — if none of them beats it, the honest outcome is to keep the dial and differentiate somewhere else.",
        },
        {
            position: 2, id: "arc-dot", name: "Arc dot", draw: drawArcDot,
            note: "The same track with the needle replaced by a 5px bead riding at r=5, plus a hub pip to measure it against. A clear ring of unlit pixels separates bead from track at every angle. Lighter than the pointer and read differently — a page of eight is eight positions rather than eight angles — but it collides with the modulation dot, which is the same shape at nearly the same radius.",
        },
        {
            position: 3, id: "arc-fill", name: "Arc fill", draw: drawArcFill,
            note: "Only the travelled part of the track is drawn, as a 2px band, between two 4px radial end stops. There is no marker to locate, only a quantity of ink, so a page can be scanned instead of read. The stops are load-bearing: without them v=0 draws nothing and an empty gauge is indistinguishable from an unpopulated cell.",
        },
        {
            position: 4, id: "arc-thick", name: "Arc thick", draw: drawArcThick,
            note: "A 2px track with a 2px stub pointer floating at 0.30r..0.62r. The answer to the real complaint about eight 1px dials in a row, which is that they turn to grey mush at true size well before anyone calls them derivative. The stub stops far short of the track on purpose — a fat pointer welded to a fat ring reads as a keyhole.",
        },
        {
            position: 5, id: "ring-fill", name: "Ring fill", draw: drawRingFill,
            note: "A closed collar at r=7, filled clockwise from twelve o'clock by thickening the rim inward. The only option that uses the whole circle, which is what separates it from the arc fill at a glance rather than on inspection: that one is an open gauge with two feet, this is a complete O. Maps directly onto the LED collars around the physical encoders this fleet emulates.",
        },
        {
            position: 6, id: "needle-only", name: "Needle only", draw: drawNeedleOnly,
            note: "A VU needle pivoting on a base rule, sweeping 124 degrees above it. Three earlier cuts drew a needle at the centre of an invisible circle with marks where the track would have been, and all three read on the page as a diagonal lying next to some specks — a stroke needs something to be at an angle TO, and a horizontal rule is a reference the eye accepts without being told. The most distinct silhouette in the set: nine of these are round or rectangular and this one is a triangle.",
        },
        {
            position: 7, id: "wedge", name: "Wedge", draw: drawWedge,
            note: "Value as AREA: a solid pie swept out of the hub inside the open track. The heaviest option and the one that survives being glanced at, seen out of focus or photographed badly, because there is no mark whose position has to be resolved. At v=0 the pie degenerates to a single radial line, which is correct rather than a special case — the track behind it is what keeps that from reading as an empty cell.",
        },
        {
            position: 8, id: "bar-vertical", name: "Bar vertical", draw: drawBarVertical,
            note: "A mixer fader in a corner-notched frame, filling upward, 9px wide in a 17px cell. Universally legible and needs no explanation. The narrow frame is deliberate — it keeps the cell's silhouette distinct from the enum square, which is a WIDE framed rectangle, so continuous and discrete cells still read apart at a glance.",
        },
        {
            position: 9, id: "bar-horizontal", name: "Bar horizontal", draw: drawBarHorizontal,
            note: "The same frame laid down and filling rightward across the full cell. Not a rotation of option 8 but a different reading: a vertical bar is a level and eight of them form a comparable skyline, a horizontal bar is a progress and each is read on its own. Highest contrast at a distance, and the most likely of the ten to be confused with the enum square.",
        },
        {
            position: 10, id: "notch-ring", name: "Notch ring", draw: drawNotchRing,
            note: "The inverse of every other option: nothing is drawn at the value, the value is where the ring stops. A hole in a continuous stroke is about the most reliably visible thing on a 1-bit display, because it needs no clearance from anything — it IS clearance. The notch travels the pointer's 270 degrees rather than a full circle, so the two extremes sit at the lower left and lower right and are plainly different.",
        },
    ],
    });
}
