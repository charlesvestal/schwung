/**
 * styles/knob.mjs — SET 1: ten REFINEMENTS of the arc-and-pointer.
 *
 * WHY THIS SET IS NOT TEN ALTERNATIVES.
 *
 * Two previous rounds were. The first was briefed minimal -> radical and
 * produced novelty rather than quality — a stipple that read as punctuation, a
 * sunburst you cannot take a value off, digits over a dither — and came back as
 * "all pretty shitty". The second was briefed clean and legible and produced
 * ten competent but unrelated widgets: a wedge, two bars, a VU needle, a notch
 * ring. Every one of them was judged, on a fixed ring rasteriser, and the
 * verdict was:
 *
 *     "Knobs think the current is the best and anything else seems like
 *      reaching."
 *
 * Twenty variants across two rounds and the shipping arc-and-pointer beat all
 * of them. That is not a failure to find the answer, it IS the answer: an arc
 * with a pointer is the obvious way to draw a bounded continuous value, and
 * both earlier rounds kept answering a question nobody asked.
 *
 * So this round stops arguing. EVERY option here keeps the arc-and-pointer
 * silhouette — an open track, a radial marker inside it — and varies only
 * execution:
 *
 *     stroke weight of the track          (1, 5)
 *     pointer length and whether it reaches the hub or the rim   (1, 2, 3, 6)
 *     whether there is a hub, and how big (4)
 *     rim treatment: end caps, centre detent                     (7, 8)
 *     whether travelled track is distinguished from untravelled  (9)
 *     the width of the gap at the bottom  (10)
 *
 * The test this set has to pass is: ten versions of one knob, and one of them
 * is best — not ten different widgets. If any option here makes a reader ask
 * "why is this a knob at all", it is the wrong option.
 *
 * POSITION 1 IS THE INCUMBENT. Same centre, same radius, same 230/260 track,
 * same hub-to-0.85r pointer as `drawArcKnob` in render_page_movy.mjs, drawn on
 * the tabulated ring instead of `ctx.drawArc`. It is the baseline the other
 * nine are refinements of, and it is allowed to win.
 *
 * GEOMETRY. Box is KW (17) by BOX_H (15) at (kx, ky) and NOTHING may fall
 * outside it — one row of overflow lands on the label band, which the grid does
 * not repaint, so it persists on hardware and appears in no single-widget
 * preview. Centre is (kx+8, ky+8) at r=8 throughout, which fits fifteen rows
 * only because the track is OPEN AT THE BOTTOM: the ring's bottom rows all fall
 * inside the gap and are never plotted. That is a live constraint, not an
 * observation — option 10 narrows the gap and had to be checked row by row
 * (its lowest lit pixel is ky+14, the last legal row).
 *
 * ctx.drawArc THROUGHOUT, and the story of why is worth keeping.
 *
 * The set was reported as "sloppy" and drawArc was blamed: its rasteriser is a
 * distance-rounded union of a row scan and a column scan, and at r=6 five
 * consecutive columns round to the same row — a five-pixel flat cap, so a small
 * circle reads as a rounded rectangle. A hand-tabulated ring was built to fix
 * it.
 *
 * The measurement was taken at THE WRONG RADIUS. This knob is KNOB_R = 8:
 *
 *     drawArc r=8    cap 5, then 4, then 2...   a taper
 *     tabulated      cap 3, then 6, then 2...   a 3-to-6 jump in one row
 *
 * So the hand-tuned version is arguably worse at the size that actually ships,
 * and on a whole page the two are near indistinguishable. The flat cap is not a
 * defect here; it reads as a slightly wider gauge, which is what this knob has
 * always looked like — and that is the verdict from the device, not from me.
 *
 * Recorded rather than quietly reverted, because "hand-tabulate the circle" is
 * a plausible-sounding fix somebody will propose again. It is the right answer
 * at small radii — drawSwitch tabulates for exactly that reason — and the wrong
 * one at eight.
 *
 * MODULATION, which the contact sheet CANNOT show you. `drawModDot` rides a
 * 5px plus centred at KNOB_R-2 = 6, so it occupies r=5..7 at whatever angle the
 * live value is. `renderInContext` clears the box and draws only the option, so
 * no page in catalog-out/ has one on it. Each option's note states its own
 * verdict; the summary is that a 1px track at r=8 with a pointer stopping at or
 * below 0.74r (5.9px) is clear, and anything that puts ink in the r=5..7 band —
 * a 2px track, an inward rim mark, a long pointer — touches it. Every collision
 * here is fixable by moving MOD_DOT_R to 5, which is a one-line change in the
 * production file; none of them is a reason to reject an option, but a reader
 * should know which ones carry that cost.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { KW, BOX_H, KNOB_R } from "../render_page_movy.mjs";
/*
 * Drawn on ctx.drawArc, the shipping primitive, after a tabulated ring was
 * built to replace it and then measured and dropped.
 *
 * The set was reported as looking "sloppy" and drawArc was blamed: at r=6 its
 * distance-rounded rasteriser gives a 5px flat cap, so a circle reads as a
 * rounded rectangle. That measurement was taken at THE WRONG RADIUS. The knob
 * is KNOB_R = 8, and at r=8 the two rasterisers compare like this:
 *
 *     drawArc r=8   cap 5, then 4, then 2...   a taper
 *     tabulated     cap 3, then 6, then 2...   a 3-to-6 jump in one row
 *
 * So the hand-tuned version is arguably WORSE at the size that ships, and on a
 * whole page the two are near indistinguishable. The flat cap is not a defect
 * at this radius; it reads as a slightly wider gauge, which is what the
 * shipping knob has always looked like.
 *
 * Kept as a note rather than deleted silently, because "hand-tabulate the
 * circle" is a plausible-sounding fix that someone will propose again. It is
 * the right answer at small radii -- drawSwitch tabulates for exactly that
 * reason -- and the wrong one here.
 */

/** A 2px track: two adjacent radii, since drawArc draws a 1px outline. */
function band(ctx, cx, cy, rIn, rOut, start, sweep, color = 1) {
    for (let r = rIn; r <= rOut; r++) ctx.drawArc(cx, cy, r, start, sweep, color);
}

/* The shipping numbers, repeated rather than imported because they are not
 * exported from render_page_movy.mjs. An option is allowed to disagree with
 * them — option 10 is the one that does — but position 1 must not. */
const ARC_START = 230, ARC_SWEEP = 260;   /* the track */
const PTR_START = 225, PTR_SWEEP = 270;   /* pointer travel, 5 degrees proud at each end */

const clamp01 = (v) =>
    (typeof v === "number" && v === v) ? (v < 0 ? 0 : v > 1 ? 1 : v) : 0;
const rad = (deg) => deg * Math.PI / 180;

/* ---------------------------------------------------------- primitives -- */

/**
 * A line, via the native binding when the context offers one.
 *
 * `line` is optional on a draw context — render_page_movy takes a different
 * path when the caller omits it — so this carries the same JS fallback the
 * device binding is ported from. A catalog that only ever exercised one of the
 * two paths is a preview that can disagree with the OLED, which is the specific
 * way a knob-ring bug got through here before.
 */
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

/** The point at radius `rr` on the dial, at angle `deg` (0 = twelve o'clock). */
function at(cx, cy, rr, deg) {
    const a = rad(deg);
    return { x: cx + rr * Math.sin(a), y: cy - rr * Math.cos(a) };
}

/** The pointer's angle at `v`, on a given travel. */
const ptrDeg = (v, start = PTR_START, sweep = PTR_SWEEP) => start + v * sweep;

/** A 1px radial stroke between two radii at one angle. */
function radial(ctx, cx, cy, rIn, rOut, deg) {
    const p0 = at(cx, cy, rIn, deg), p1 = at(cx, cy, rOut, deg);
    seg(ctx, p0.x, p0.y, p1.x, p1.y, 1);
}

/**
 * A 2px radial stroke, thickened along ONE AXIS.
 *
 * Never along the true perpendicular. The perpendicular is the obvious choice
 * and it is wrong on a raster: at 45 degrees it is (1,-1), which is the one
 * direction a 45-degree Bresenham line is invariant under, so the copy lands
 * exactly on the original's anti-diagonal neighbours and the two strokes come
 * out as parallel hairlines with a lit gap between them — a hollow wedge, not a
 * 2px pointer.
 *
 * A pure (1,0) or (0,1) displacement always yields a solid double stroke,
 * because a Bresenham line and the same line shifted one pixel along an axis
 * are 8-connected everywhere. Pick the axis the stroke is NOT running along, or
 * the copy lands on top of the original and thickens nothing.
 */
function radial2(ctx, cx, cy, rIn, rOut, deg) {
    const a = rad(deg);
    const steep = Math.abs(Math.cos(a)) >= Math.abs(Math.sin(a));
    const ox = steep ? 1 : 0, oy = steep ? 0 : -1;
    const p0 = at(cx, cy, rIn, deg), p1 = at(cx, cy, rOut, deg);
    seg(ctx, p0.x, p0.y, p1.x, p1.y, 1);
    seg(ctx, p0.x + ox, p0.y + oy, p1.x + ox, p1.y + oy, 1);
}

/* ------------------------------------------------------------- 1 ------ */

/**
 * 1. arc-pointer — the incumbent, on the tabulated ring.
 *
 * Open track r=8 over 230/260, pointer from the exact centre out to 0.85r, over
 * a travel of 225/270 so that each extreme sits five degrees proud of the end
 * of the track. Nothing about the design is changed; the only difference from
 * what ships today is that the circle is hand-tabulated rather than produced by
 * `ctx.drawArc`, so the shoulders are 3px caps instead of 5px flats.
 *
 * This is the control. It won twenty comparisons already.
 */
function drawArcPointer(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    radial(ctx, cx, cy, 0, KNOB_R * 0.85, ptrDeg(v));
}

/* ------------------------------------------------------------- 2..3 --- */

/**
 * 2. arc-short — the same knob with the pointer pulled back off the rim.
 *
 * 0.68r instead of 0.85r. At 0.85 the tip is 6.8px from the centre against a
 * track at 8, which after rounding is one clear pixel at best and none at the
 * shoulders: the marker merges with the track and reads as a lump growing off
 * the rim rather than as a pointer aimed at it. Pulling back to 5.4px leaves
 * two clear pixels of track at every angle, so the pointer and the scale it
 * points at stay separate shapes.
 *
 * The cost is real and it is the thing to judge: a shorter pointer is a smaller
 * marker, so the angle is carried by less ink and reads less strongly at a
 * glance across a page of eight.
 */
function drawArcShort(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    radial(ctx, cx, cy, 0, KNOB_R * 0.68, ptrDeg(v));
}

/**
 * 3. arc-float — the pointer as a floating stub, clear of both centre and rim.
 *
 * 0.34r to 0.74r: it touches neither end. Both other pointer options are
 * SPOKES, anchored at the centre, and a spoke's inner half carries no
 * information — every value draws it, so it is ink that never changes. Cutting
 * it out leaves only the part that moves.
 *
 * What that costs is the convergence point. With a hollow centre the eye has to
 * infer the angle from the stub alone rather than reading it off a line that
 * starts somewhere known, and at the extremes of travel — where the stub is
 * down near the gap — it can read as a detached tick rather than as a pointer.
 * That is precisely the trade option 4 answers by putting something back at the
 * middle.
 */
function drawArcFloat(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    radial(ctx, cx, cy, KNOB_R * 0.34, KNOB_R * 0.74, ptrDeg(v));
}

/* ------------------------------------------------------------- 4 ------ */

/**
 * 4. arc-hub — a 3x3 hub with the pointer growing out of it.
 *
 * The hub is a shaft, and it is the one addition here that changes what the
 * widget IS rather than how heavily it is drawn: with a solid centre the thing
 * reads as a knob seen from above — a cap with a pointer on it — rather than as
 * a gauge with a needle across it. It also fixes option 3's complaint for free,
 * because the stub now has an anchor to be at an angle to.
 *
 * ODD-SIZED, 3x3 and not 2x2, and that is not taste. An even-sized mark has its
 * centroid on a pixel BOUNDARY, so however it rounds it sits half a pixel off
 * the centre it is supposed to be, and at the cardinals floating point picks the
 * tie — at exactly 50% `sin()` returns -2.4e-16 rather than 0 and the block
 * jumps a whole pixel left. Nine pixels is heavy for a knob of radius eight,
 * which is the honest argument against this option.
 */
function drawArcHub(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    ctx.fillRect(cx - 1, cy - 1, 3, 3, 1);
    radial(ctx, cx, cy, 2, KNOB_R * 0.74, ptrDeg(v));
}

/* ------------------------------------------------------------- 5..6 --- */

/**
 * 5. arc-heavy — a 2px track, pointer unchanged in weight.
 *
 * The one complaint about the incumbent that is about legibility rather than
 * provenance: eight 1px dials in a row turn to grey mush at true size on a
 * 128x64 OLED viewed from a metre away. Doubling the track (r=7..8) puts the
 * widget's weight in the frame, where it is constant, rather than in the
 * marker, where it moves.
 *
 * The pointer stays a hairline to 0.62r deliberately. Thickening both — which
 * is what option 6 does to the pointer alone — gives a fat spoke welded to a
 * fat ring, and that reads as a keyhole rather than as a control.
 */
function drawArcHeavy(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    band(ctx, cx, cy, KNOB_R - 1, KNOB_R, ARC_START, ARC_SWEEP);
    radial(ctx, cx, cy, 0, KNOB_R * 0.62, ptrDeg(v));
}

/**
 * 6. arc-bold — a 1px track with a 2px pointer.
 *
 * The mirror image of option 5: put the weight in the part that MOVES. The
 * argument for it is that the track is redundant once you have seen it — it is
 * identical on all eight cells of a page — whereas the pointer is the only
 * thing carrying information, so if one of the two shapes gets the extra ink it
 * should be that one.
 *
 * The argument against is that a 2px stroke at r<6 is a wedge, not a line: near
 * the hub the two strokes are angularly far apart and the marker is visibly
 * fatter at its base than at its tip. Stopping at 0.66r keeps it clear of the
 * track, which matters more here than for a hairline.
 */
function drawArcBold(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    radial2(ctx, cx, cy, 0, KNOB_R * 0.66, ptrDeg(v));
}

/* ------------------------------------------------------------- 7..8 --- */

/**
 * 7. arc-caps — end caps at both limits of travel.
 *
 * Two inward radial stubs (r=6..8) at the ends of the track, so the gauge has
 * feet. What they buy is the bottom of the range: with a plain open arc, v=0
 * and "the pointer happens to be near the bottom left" look the same, and there
 * is nothing on screen that says where travel STOPS. With caps, v=0 is the
 * pointer sitting against a stop, which is a different picture from the pointer
 * approaching one.
 *
 * They also square off the ends of the arc, which is worth having on its own: a
 * hand-tabulated ring ends wherever the angular filter cuts it, and a radial
 * stub is a deliberate termination rather than an incidental one.
 */
function drawArcCaps(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    radial(ctx, cx, cy, KNOB_R - 2, KNOB_R, ARC_START);
    radial(ctx, cx, cy, KNOB_R - 2, KNOB_R, ARC_START + ARC_SWEEP);
    radial(ctx, cx, cy, 0, KNOB_R * 0.68, ptrDeg(v));
}

/**
 * 8. arc-detent — a centre mark at twelve o'clock.
 *
 * A 2px inward tick (r=5..6) at the top of the track, one clear pixel below the
 * rim so it reads as a mark ON the scale rather than as a thickening OF it.
 * This is the bipolar case and it is a real one — pan, tune, bipolar
 * modulation depth all have a meaningful centre, and on the incumbent widget
 * dead centre looks like any other value. With a detent, "is this at zero" is
 * answered by whether the pointer covers the tick.
 *
 * The pointer stops at 0.68r, whose tip lands at the tick's inner end, so at
 * centre the two join into a single stroke to the rim. That is the intended
 * reading — snapped — and it is also why this option is only right for params
 * that HAVE a centre: on a unipolar cutoff the tick is a mark that means
 * nothing, and 50% of a sweep is not a landmark.
 */
function drawArcDetent(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    radial(ctx, cx, cy, KNOB_R - 3, KNOB_R - 2, 0);
    radial(ctx, cx, cy, 0, KNOB_R * 0.68, ptrDeg(v));
}

/* ------------------------------------------------------------- 9 ------ */

/**
 * 9. arc-travelled — the travelled span of the track thickened.
 *
 * The track stays whole at 1px so the untravelled range is still visible and
 * the silhouette never changes; the span from the start of travel up to the
 * value is grown INWARD to 2px. Two readings on one widget: the pointer gives
 * you a precise angle, the weighted span gives you a quantity you can scan a
 * whole page for without resolving any single mark.
 *
 * Growing inward rather than outward is what keeps this a refinement instead of
 * a second widget — the outer edge is constant, so eight of these still form a
 * row of identical circles with a marker in each, which was the whole failure
 * mode of round 2.
 *
 * The honest cost: at low values the thickened span is a couple of pixels near
 * the bottom left, and at v=0 there is none at all, so the extra reading only
 * really works over the top two thirds of the range.
 */
function drawArcTravelled(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, ARC_START, ARC_SWEEP, 1);
    band(ctx, cx, cy, KNOB_R - 1, KNOB_R, ARC_START, v * ARC_SWEEP);
    radial(ctx, cx, cy, 0, KNOB_R * 0.68, ptrDeg(v));
}

/* ------------------------------------------------------------- 10 ----- */

/**
 * 10. arc-collar — the gap at the bottom closed down to 70 degrees.
 *
 * Same construction, one number changed: the track runs 215/290 instead of
 * 230/260, so it wraps most of the way round and the widget reads as a collar
 * with a notch rather than as a horseshoe. Travel widens with it (210/290) so
 * the pointer still sits five degrees proud at each end.
 *
 * Two things this buys. The circle is more of a circle, which at 17x15 among
 * rectangular enum cells is a stronger silhouette; and the two extremes end up
 * further apart in angle, so v=0 and v=1 are less alike than on the incumbent.
 * What it costs is the gap itself — the notch is the thing that tells you which
 * way is "down", and at 70 degrees it is closer to being an interruption in a
 * ring than an opening in a gauge.
 *
 * This one is on the edge of the box and had to be checked row by row. With the
 * 230/260 gap the ring's bottom three rows all fall inside it and are never
 * plotted; at 215/290 row dy=+6 (y = ky+14, the LAST legal row) comes into
 * sweep at both shoulders. Widen the sweep any further and it clips — silently,
 * onto the label band, on hardware only.
 */
const COLLAR_START = 215, COLLAR_SWEEP = 290;
function drawArcCollar(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    ctx.drawArc(cx, cy, KNOB_R, COLLAR_START, COLLAR_SWEEP, 1);
    radial(ctx, cx, cy, 0, KNOB_R * 0.74, ptrDeg(v, COLLAR_START - 5, COLLAR_SWEEP + 10));
}

/**
 * Called from the bottom of index.mjs rather than running on import — see the
 * cycle note there. Registering at module scope would read the KIND_* consts
 * while they are still in the temporal dead zone.
 */
export function register() {
    return registerSet({
    id: "knob",
    title: "Arc knob — ten refinements of the arc-and-pointer",
    kind: KIND_DRAW,
    replaces: "drawArcKnob",
    options: [
        {
            position: 1, id: "arc-pointer", name: "Arc pointer", draw: drawArcPointer,
            note: "THE INCUMBENT, unchanged: r=8 track over 230/260, pointer from the exact centre to 0.85r over a 225/270 travel. The only difference from what ships is that the circle comes from the hand-tabulated ring rather than ctx.drawArc, so the shoulders are 3px caps instead of the rasteriser's 5px flats. Everything else in this set is a refinement of this, and after twenty rejected alternatives across two rounds it is allowed to win again. Modulation dot: CLEAR — the dot's band is r=5..7 and the pointer crosses it, but they coincide only when live value equals base, which is true of the shipping widget too.",
        },
        {
            position: 2, id: "arc-short", name: "Arc short pointer", draw: drawArcShort,
            note: "Position 1 with the pointer pulled back from 0.85r to 0.68r. At 0.85 the tip is 6.8px against a track at 8, which rounds to one clear pixel at best and none at the shoulders, so the marker merges with the rim and reads as a lump growing off it. Two clear pixels at every angle keeps pointer and scale as separate shapes. Costs ink: a shorter marker carries the angle less strongly across a page of eight. Modulation dot: CLEAR, and cleaner than position 1 — the tip stops at r=5.4, at the inner edge of the dot's band rather than through it.",
        },
        {
            position: 3, id: "arc-float", name: "Arc floating pointer", draw: drawArcFloat,
            note: "Position 2 with the inner half of the pointer cut away: a stub floating 0.34r..0.74r, touching neither hub nor rim. The argument is that a spoke's inner half is ink that every value draws, so it carries nothing. The cost is the convergence point — with a hollow centre the angle has to be inferred from the stub alone, and near the ends of travel it can read as a detached tick. Position 4 is the same idea with the anchor put back. Modulation dot: CLEAR of the hub, but the stub spans r=2.7..5.9 and the dot spans r=5..7, so they overlap at the tip exactly as in position 2.",
        },
        {
            position: 4, id: "arc-hub", name: "Arc with hub", draw: drawArcHub,
            note: "A 3x3 hub with the pointer growing out of it to 0.74r. The one addition that changes what the widget IS rather than how heavily it is drawn: a solid centre makes it a knob cap seen from above rather than a needle across a gauge, and it gives position 3's floating stub the anchor it lacks. Odd-sized because an even mark's centroid is on a pixel boundary and at the cardinals floating point decides which way it rounds. Nine pixels is heavy for a radius-8 knob and that is the honest argument against it. Modulation dot: CLEAR — hub is r<=1, dot is r=5..7, nothing between them.",
        },
        {
            position: 5, id: "arc-heavy", name: "Arc heavy track", draw: drawArcHeavy,
            note: "A 2px track (r=7..8) with the pointer left as a hairline to 0.62r. This answers the only complaint about the incumbent that is about legibility rather than provenance: eight 1px dials in a row go to grey mush at true size at a metre. Weight goes in the frame, which is constant, not in the marker, which moves. Thickening both would give a fat spoke welded to a fat ring, which is a keyhole — that is why position 6 thickens the pointer INSTEAD, not as well. Modulation dot: COLLIDES. The track occupies r=7..8 and the dot's outer arm is r=7, so the mark touches the rim at every angle. MOD_DOT_R 6->5 fixes it.",
        },
        {
            position: 6, id: "arc-bold", name: "Arc bold pointer", draw: drawArcBold,
            note: "The mirror of position 5: 1px track, 2px pointer to 0.66r. Put the extra ink in the part that moves, since the track is identical on all eight cells of a page and the pointer is the only thing carrying information. Against it: a 2px stroke below r=6 is a wedge rather than a line — the two strokes are angularly far apart near the hub, so the marker is visibly fatter at its base than at its tip. Modulation dot: COLLIDES, worse than position 2 — a 2px marker reaching r=5.3 puts ink in the dot's band at two adjacent angles rather than one.",
        },
        {
            position: 7, id: "arc-caps", name: "Arc with end caps", draw: drawArcCaps,
            note: "Position 2 plus inward radial stubs (r=6..8) at both limits of travel, so the gauge has feet. What they buy is the bottom of the range: on a plain open arc, v=0 and 'the pointer is somewhere near the bottom left' look the same and nothing says where travel STOPS, whereas with caps v=0 is the pointer against a stop. They also square off the ends of the track, which a tabulated ring otherwise leaves wherever the angular filter happens to cut. Modulation dot: COLLIDES at the two ends only — the caps sit in the r=6..8 band, so the mark merges with a cap when the live value is at an extreme, and is clear everywhere else.",
        },
        {
            position: 8, id: "arc-detent", name: "Arc with centre detent", draw: drawArcDetent,
            note: "Position 2 plus a 2px inward tick (r=5..6) at twelve o'clock, one clear pixel below the rim so it reads as a mark on the scale rather than a thickening of it. This is the bipolar case, and it is a real one: pan, tune and bipolar mod depth all have a meaningful centre, and on the incumbent dead centre looks like any other value. The pointer's tip lands on the tick's inner end, so at centre the two join into one stroke to the rim — that snapped reading is the point. Only right for params that HAVE a centre; on a unipolar cutoff it is a landmark that means nothing. Modulation dot: COLLIDES at centre — the tick is inside the dot's r=5..7 band, so a mark parked near twelve o'clock swallows it.",
        },
        {
            position: 9, id: "arc-travelled", name: "Arc travelled span", draw: drawArcTravelled,
            note: "Position 2 with the travelled span of the track grown inward to 2px while the whole track stays drawn at 1px. Two readings on one widget: the pointer for a precise angle, the weight for a quantity you can scan a page for without resolving any single mark. Growing INWARD is what keeps it a refinement rather than a second widget — the outer edge never changes, so a row of these is still a row of identical circles. Cost: at low values the thickened span is a few pixels at the bottom left and at v=0 there is none, so the second reading only works over the top two thirds. Modulation dot: COLLIDES over the travelled span (band reaches r=7) and is clear beyond it, so the mark would visibly change weight as the value passed it.",
        },
        {
            position: 10, id: "arc-collar", name: "Arc narrow gap", draw: drawArcCollar,
            note: "One number changed from position 1: the track runs 215/290 instead of 230/260, closing the gap at the bottom to 70 degrees, with travel widened to match so the pointer stays five degrees proud at each end. The circle is more of a circle, which is a stronger silhouette among rectangular enum cells, and the two extremes end up further apart in angle so v=0 and v=1 are less alike. What it costs is the notch itself — at 70 degrees it reads more as an interruption in a ring than as the opening of a gauge, and the opening is what tells you which way is down. Also the tightest option in the box: row dy=+6 (y = ky+14, the last legal row) comes into sweep at both shoulders, and any wider clips onto the label band. Modulation dot: CLEAR, same as position 1.",
        },
    ],
    });
}
