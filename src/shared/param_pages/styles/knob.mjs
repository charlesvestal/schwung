/**
 * styles/knob.mjs — SET 1: ten alternatives to the arc knob.
 *
 * The shipping widget (`drawArcKnob`, render_page_movy.mjs) is the single most
 * Elektron-shaped thing on the screen, and it says so in its own comments: its
 * 230-degree start and 260-degree sweep were "measured off Elektron's screen
 * (128x64, recovered from a 4x capture)". If any one widget has to move for
 * this UI to stop reading as a clone, it is this one.
 *
 * The axis is minimal -> radical:
 *
 *   1-3   still a dial. Same centre, same sweep, different WEIGHT — the
 *         changes a reviewer would call a cosmetic tweak, kept because the
 *         cheapest honest outcome of this exercise is "thin the arc and go".
 *   4-6   still rotary, but the track is QUANTISED. Ticks, stamps and
 *         segments all read as travel around a circle without reproducing
 *         Elektron's continuous-ring-plus-pointer construction.
 *   7-10  not a dial at all. This is where the reference product lands:
 *         vimana2-rust has NO rotary widget on its 1-bit display — its
 *         `render_indicator` (crates/vimana-app/src/hud.rs) draws a
 *         continuous parameter as a dotted rail with a 3x3 thumb, which is
 *         option 8 almost verbatim.
 *
 * GEOMETRY. Every option gets a box of KW (17) by BOX_H (15) at (kx, ky), and
 * must not put a pixel outside it. One row of overflow lands on the label
 * band, which the grid does not repaint — it would persist on hardware and
 * appear nowhere in a preview of a single widget. The dial options inherit the
 * shipping centre (kx+8, ky+8) with r=8; that fits vertically only because the
 * sweep is open at the bottom, so a full circle at this radius WOULD clip.
 *
 * MODULATION, and this is the part the catalog CANNOT show you. `drawModDot`
 * rides a 5px plus centred at radius KNOB_R-2 = 6, so it occupies r=5..7 —
 * chosen, per its own comment, to stay exactly one pixel clear of a ring at
 * r=8. `renderInContext` clears the widget box and draws only the option, so
 * no page in catalog-out/ has a modulation dot on it. Every verdict below is
 * from the geometry, not from a render:
 *
 *   1 thin-arc      clear. Pointer stops at 0.72r; dot and pointer coincide
 *                   only when the modulated value equals the base, which is
 *                   true of the shipping widget too.
 *   2 open-gap      clear, and arguably the best of the ten: the gap is the
 *                   base and the dot is the live value, on one track, with
 *                   nothing else competing for the ring.
 *   3 inset-rim     COLLIDES. The track drops to r=7 and the dot's outer arm
 *                   is at r=7, so it merges with the rim at every angle.
 *                   Adopting this means MOD_DOT_R 6 -> 5.
 *   4 tick-ladder   COLLIDES. Lit ticks span r=5..8; the dot lands inside
 *                   them and disappears. Needs the ticks shortened or the dot
 *                   moved inside r=4.
 *   5 stipple-arc   COLLIDES. Stamps sit at r=6.5, straight through the dot's
 *                   band.
 *   6 segment-ring  COLLIDES on lit segments (r=6..8) and is clear on unlit
 *                   ones, which is the worst of both — the mark would blink
 *                   in and out as the value crosses a segment boundary.
 *   7-10            NO HOST. There is no track for a dot to ride, so all four
 *                   need a linear modulation mark designed for them (a second
 *                   thumb, a tick under the bar) before they could ship.
 *
 * That is a real cost and it is not a reason to drop 3-6: the dot is ~20
 * lines and moves with whatever widget wins. It is a reason not to read this
 * catalog as ten drop-in replacements.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { KW, BOX_H, KNOB_R } from "../render_page_movy.mjs";
import { fontWidth4x5, fontPrint4x5 } from "../font4x5.mjs";
import {
    SOLID, CHECKER, DIAG_LIGHT, DIAG_THIRD,
    fillDithered, fillTerrain, dottedRule, notchCorners,
} from "./dither.mjs";

/* The shipping numbers, repeated rather than imported because they are not
 * exported — and because an option is allowed to disagree with them. */
const ARC_START = 230, ARC_SWEEP = 260;   /* the track */
const PTR_START = 225, PTR_SWEEP = 270;   /* the pointer's travel, 5 degrees proud at each end */

const clamp01 = (v) => (v > 1 ? 1 : v < 0 ? 0 : (typeof v === "number" && v === v ? v : 0));
const rad = (deg) => deg * Math.PI / 180;

/* ---------------------------------------------------------- primitives --
 *
 * `line` / `drawArc` are optional on a draw context (render_page_movy takes a
 * different path when the caller omits them), so every use here goes through
 * a wrapper with the same JS fallback the device binding is ported from. A
 * catalog that only ever exercised the native path would be a preview that
 * can disagree with the OLED, which is the specific way a knob-ring bug got
 * through here before. */

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

function arc(ctx, cx, cy, r, start, sweep, color = 1) {
    if (r < 0 || sweep <= 0) return;
    if (typeof ctx.drawArc === "function") { ctx.drawArc(cx, cy, r, start, sweep, color); return; }
    const inSweep = (dx, dy) => {
        if (sweep >= 360) return true;
        let a = Math.atan2(dx, -dy) * 180 / Math.PI;
        if (a < 0) a += 360;
        let d = a - ((start % 360) + 360) % 360;
        if (d < 0) d += 360;
        return d <= sweep;
    };
    const plot = (dx, dy) => { if (inSweep(dx, dy)) ctx.fillRect(cx + dx, cy + dy, 1, 1, color); };
    for (let dy = -r; dy <= r; dy++) {
        const dx = Math.round(Math.sqrt(r * r - dy * dy));
        plot(dx, dy); if (dx !== 0) plot(-dx, dy);
    }
    for (let dx = -r; dx <= r; dx++) {
        const dy = Math.round(Math.sqrt(r * r - dx * dx));
        plot(dx, dy); if (dy !== 0) plot(dx, -dy);
    }
}

/**
 * A band of arc between two radii, plotted by SAMPLING the angle rather than
 * by stacking `drawArc` calls.
 *
 * `drawArc` scans rows and columns and keeps the pixel whose distance rounds
 * to r, which is right for a whole ring and wrong for a 20-degree piece of
 * one: each radius picks up a different number of pixels at the ends, so
 * concentric short arcs come out ragged and no two segments of the same
 * nominal size look alike. Sampling the angle finely enough to hit every
 * pixel gives segments that are identical to each other and symmetric about
 * the top, which is the whole point of a segmented display.
 */
function arcBand(ctx, cx, cy, rIn, rOut, start, sweep) {
    if (sweep <= 0 || rOut < rIn) return;
    /* Two samples per pixel of outer arc length — enough that consecutive
     * samples never skip a pixel. */
    const steps = Math.max(2, Math.ceil(sweep * Math.PI / 180 * rOut * 2));
    for (let i = 0; i <= steps; i++) {
        const a = rad(start + sweep * i / steps);
        const s = Math.sin(a), c = Math.cos(a);
        for (let r = rIn; r <= rOut; r++)
            ctx.fillRect(Math.round(cx + r * s), Math.round(cy - r * c), 1, 1, 1);
    }
}

/** Point on the dial at `t` (0..1) along the pointer's travel, radius `rr`. */
function polar(kx, ky, t, rr, startDeg = PTR_START, sweepDeg = PTR_SWEEP) {
    const a = rad(startDeg + t * sweepDeg);
    return { x: kx + KNOB_R + rr * Math.sin(a), y: ky + KNOB_R - rr * Math.cos(a) };
}

/* --------------------------------------------------------------- 1..3 --
 * Still a dial. */

/** 1. thin-arc — the shipping construction with the pointer taken off the hub. */
function drawThinArc(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    arc(ctx, kx + KNOB_R, ky + KNOB_R, KNOB_R, ARC_START, ARC_SWEEP, 1);
    /*
     * A FLOATING stroke. The shipping pointer starts at the exact centre,
     * which is what makes it read as a clock hand; lifting the inner end off
     * the hub leaves the same information in half the ink.
     *
     * The OUTER end came in from the shipping 0.85r as well, and that was not
     * a taste call: at 0.85 (6.8px against a ring at 8) the stroke's tip lands
     * adjacent to the track and merges with it, so the marker reads as a lump
     * growing off the rim rather than as a pointer — the same failure the
     * modulation dot's own comment describes and solves by sitting wholly
     * inside. 0.68r leaves a clear pixel of track at every angle.
     */
    const a = polar(kx, ky, v, KNOB_R * 0.26);
    const b = polar(kx, ky, v, KNOB_R * 0.72);
    seg(ctx, a.x, a.y, b.x, b.y, 1);
}

/**
 * 2. open-gap — the value is a HOLE in the track. No pointer at all.
 *
 * The one construction that removes the pointer without removing the ring:
 * the arc is drawn in two runs with a 26-degree bite taken out at the value.
 * At the extremes the bite merges with the track's own bottom opening, so the
 * gap grows on one side instead of hitting a wall — which is a truer picture
 * of "you are at the end of travel" than a pointer parked on the last pixel.
 */
function drawOpenGap(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    /*
     * 38 degrees, not the 26 this was first drawn at. 26 is 3px of missing
     * track at r=8, which reads as a rasterisation wobble rather than as a
     * deliberate hole — and at the extremes, where the bite merges with the
     * track opening, it moved the arc end by two pixels and v=0 and v=1 were
     * near enough indistinguishable. At 38 the hole is unmistakably a hole,
     * and the extremes differ by a visible 3px at each end.
     */
    const GAP = 38;
    const at = ARC_START + v * ARC_SWEEP;
    arc(ctx, cx, cy, KNOB_R, ARC_START, (at - GAP / 2) - ARC_START, 1);
    const tail = ARC_START + ARC_SWEEP - (at + GAP / 2);
    arc(ctx, cx, cy, KNOB_R, at + GAP / 2, tail, 1);
    /* A hub pip. Without it a gap at either extreme leaves a plain arc and no
     * mark anywhere — the widget would read as decoration rather than a
     * control, and at v=0 there would be nothing distinguishing it from an
     * unlit cell. */
    ctx.fillRect(cx, cy, 1, 1, 1);
}

/**
 * 3. inset-rim — heavier, tighter, and welded to the rim.
 *
 * The opposite bet to option 1: instead of thinning the dial, commit to it.
 * The track drops to r=7 so it sits a pixel inside the cell rather than
 * touching its edges, and the pointer is 2px and runs hub to rim. This is the
 * only option that gets DARKER than the baseline, which matters because a
 * bank of eight thin dials is the thing that currently reads as grey mush at
 * true size.
 */
function drawInsetRim(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R, r = KNOB_R - 1;
    arc(ctx, cx, cy, r, ARC_START, ARC_SWEEP, 1);
    const a = rad(PTR_START + v * PTR_SWEEP);
    const tip = { x: cx + r * Math.sin(a), y: cy - r * Math.cos(a) };
    /*
     * Thicken along ONE AXIS, never along the true perpendicular.
     *
     * The perpendicular is the obvious choice and it is wrong on a raster: at
     * 45 degrees it is (1,-1), so the copy is displaced along the ANTI-
     * diagonal — which is the one direction a 45-degree Bresenham line is
     * invariant under, give or take. The two strokes came out as parallel
     * hairlines with a lit gap between them, a hollow wedge rather than a 2px
     * pointer.
     *
     * A pure (1,0) or (0,1) displacement always yields a solid double
     * stroke, because a Bresenham line and the same line shifted one pixel
     * along an axis are 8-connected everywhere. Pick the axis the stroke is
     * NOT running along (offset x for a steep pointer, y for a shallow one),
     * or the copy lands on top of the original and thickens nothing. For the
     * y case prefer -1: at the ends of travel the tip is already on row
     * ky+13 of a 15-row box.
     */
    const steep = Math.abs(Math.cos(a)) >= Math.abs(Math.sin(a));
    const ox = steep ? 1 : 0, oy = steep ? 0 : -1;
    seg(ctx, cx, cy, tip.x, tip.y, 1);
    seg(ctx, cx + ox, cy + oy, tip.x + ox, tip.y + oy, 1);
}

/* --------------------------------------------------------------- 4..6 --
 * Rotary, but quantised. */

/**
 * 4. tick-ladder — twelve radial ticks, lit up to the value.
 *
 * There is no continuous ring anywhere in this one, which is what takes it
 * out of Elektron's vocabulary: their dial is a track with a marker on it,
 * this is a bar graph bent round a circle. It also reads at a glance from
 * further away than an arc does, because the value is carried by AREA rather
 * than by the angle of one 6px stroke.
 */
const TICKS = 12;
function drawTickLadder(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    /* At least one tick lit at v=0: an all-unlit ladder and an unpopulated
     * cell look the same, and "minimum" is a value, not an absence. */
    const lit = 1 + Math.round(v * (TICKS - 1));
    for (let i = 0; i < TICKS; i++) {
        const t = i / (TICKS - 1);
        const outer = polar(kx, ky, t, KNOB_R);
        if (i < lit) {
            const inner = polar(kx, ky, t, KNOB_R - 3);
            seg(ctx, inner.x, inner.y, outer.x, outer.y, 1);
        } else {
            /* The unlit remainder is still drawn, as single pixels on the
             * outer radius. A ladder that erased its own unused travel would
             * not show how much room is left. */
            ctx.fillRect(Math.round(outer.x), Math.round(outer.y), 1, 1, 1);
        }
    }
}

/**
 * 5. stipple-arc — a fixed number of stamps, spread by the value.
 *
 * The one option where the value is encoded by SPACING rather than by extent
 * or angle: seven 2x2 stamps always appear, distributed from the start of
 * travel to the current value, so low values are a tight cluster at the
 * bottom-left and high values are an even necklace round the whole track. It
 * is the most distinctive construction in the set and the least conventional
 * — worth putting in front of a human precisely because no hardware synth
 * draws a knob this way.
 */
/*
 * FIVE stamps, not the seven this was drawn with first. Seven is too many for
 * the geometry: at mid travel the spacing falls to ~3px of arc, and a 2x2
 * stamp is 2px wide, so consecutive stamps near the top of the sweep touched
 * and fused into a `####` bar. That destroys the only thing the option
 * encodes — you cannot read spacing off a run of stamps that have merged into
 * one shape. At five, the closest pair at any value the option is meant to be
 * read at stays separated.
 */
const STAMPS = 5;
const STAMP_R = 6.5;   /* the 2x2 hangs down and right of this, so it cannot be KNOB_R */
function drawStippleArc(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    for (let i = 0; i < STAMPS; i++) {
        const t = v * (i / (STAMPS - 1));
        const p = polar(kx, ky, t, STAMP_R, ARC_START, ARC_SWEEP);
        ctx.fillRect(Math.round(p.x), Math.round(p.y), 2, 2, 1);
    }
}

/**
 * 6. segment-ring — eight arc segments, thickened up to the value.
 *
 * A LED collar, which is what the hardware most of this fleet emulates
 * actually has around its encoders. The track stays complete at 1px so the
 * unused range is visible, and a lit segment gains a second band at r-1 —
 * value as WEIGHT rather than as presence, which keeps the ring's silhouette
 * stable while it fills instead of growing a shape out of nothing.
 */
const SEGS = 8;
function drawSegmentRing(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    const lit = Math.round(v * SEGS);
    const pitch = ARC_SWEEP / SEGS;
    /*
     * The first cut of this was a 7-degree gap and ONE extra band at r-1, and
     * in context all four dials on the page were indistinguishable — a plain
     * ring, at every value. Both halves were too timid to survive
     * rasterisation: 7 degrees at r=8 is under a pixel of arc, so the
     * segments never separated, and r + (r-1) is a 2px band that reads as a
     * slightly fat ring rather than as a filled cell.
     *
     * 12 degrees opens a real gap, and a lit segment is 3px deep (r-2..r),
     * which at ~4px of arc makes each one a distinct block. The unlit track
     * stays 1px, so lit against unlit is now 3:1 rather than 2:1.
     */
    const GAP = 12;
    for (let i = 0; i < SEGS; i++) {
        /* The gap is CENTRED on the segment boundary, not trailing it. With
         * the gap trailing, the boundary at the sweep midpoint (which is
         * exactly twelve o'clock for an even segment count) put a segment
         * edge on one side of the top and a gap on the other, and the ring
         * read as lopsided at every value. Splitting the gap across the
         * boundary makes the collar mirror-symmetric about the top. */
        const s = ARC_START + i * pitch + GAP / 2;
        const w = pitch - GAP;
        if (i < lit) arcBand(ctx, cx, cy, KNOB_R - 2, KNOB_R, s, w);
        else arcBand(ctx, cx, cy, KNOB_R, KNOB_R, s, w);
    }
}

/* -------------------------------------------------------------- 7..10 --
 * Not a dial. */

/**
 * 7. bar-in-box — a horizontal fill in a notched frame.
 *
 * The conservative way out of the rotary metaphor: a mixer-style level, which
 * everyone already reads, inside the corner-notched box that is this UI's own
 * signature shape. The unfilled remainder is a DIAG_LIGHT ground rather than
 * empty, so the track is visible at v=0 and the widget never looks broken.
 *
 * Cheapest of the four to adopt, and the least differentiated of them: a
 * bordered horizontal bar is generic rather than distinctive.
 */
function drawBarInBox(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const bx = kx, by = ky + 2, bw = KW, bh = 11;
    ctx.fillRect(bx, by, bw, 1, 1);
    ctx.fillRect(bx, by + bh - 1, bw, 1, 1);
    ctx.fillRect(bx, by, 1, bh, 1);
    ctx.fillRect(bx + bw - 1, by, 1, bh, 1);
    notchCorners(ctx, bx, by, bw, bh);
    const tx = bx + 2, ty = by + 2, tw = bw - 4, th = bh - 4;
    fillDithered(ctx, tx, ty, tw, th, DIAG_LIGHT);
    const fw = Math.round(v * tw);
    if (fw > 0) fillDithered(ctx, tx, ty, fw, th, SOLID);
}

/**
 * 8. dotted-track — vimana's own indicator, near enough verbatim.
 *
 * A 50%-dotted rail with a solid 3x3 thumb and a stop mark at each end. This
 * is `render_indicator`'s Continuous arm from crates/vimana-app/src/hud.rs,
 * fitted to a 17px cell: the reference product renders EVERY continuous knob
 * parameter this way and has no dial widget at all, so adopting it is the
 * option with a shipped precedent rather than a hypothesis behind it.
 *
 * The thumb is not notched. A notched 3x3 is a five-pixel plus, which is
 * already the modulation dot's shape — and at three pixels across there is no
 * corner left to soften, only a different glyph.
 */
function drawDottedTrack(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const y = ky + 7;
    /* The rail stops one pixel short of each end mark for the same reason the
     * thumb does: a rail dot sitting against a stop bridges it to the thumb on
     * the centre row, and at the extremes that is the one row you are reading. */
    dottedRule(ctx, kx + 2, y, KW - 4, 0);
    /* End stops. Without them the rail's ends are ambiguous at a 50% dither —
     * whether the last dot is travel or track is exactly the question a value
     * near an extreme is asking. */
    ctx.fillRect(kx, y - 2, 1, 5, 1);
    ctx.fillRect(kx + KW - 1, y - 2, 1, 5, 1);
    /*
     * Travel is inset one pixel INSIDE the stops, so a clear column always
     * separates them. Running the thumb the full width of the rail welded it
     * to the stop at both extremes — a 4px block where a 1px stop and a 3px
     * thumb should be — and those are the two values where knowing whether
     * you are AT the end matters most.
     */
    const tx = kx + 2 + Math.round(v * (KW - 4 - 3));
    ctx.fillRect(tx, y - 1, 3, 3, 1);
}

/**
 * 9. terrain-cell — the value as MASS, filled bottom-up.
 *
 * DIAG_THIRD hatch under a solid crest, which is the vimana filter page's
 * idiom and the reason `fillTerrain` strokes a crest at all: a light pattern
 * alone leaves the boundary ambiguous, and the boundary IS the value. Eight
 * of these side by side make a skyline, so a whole page can be read as one
 * shape rather than as eight widgets that each need a separate look.
 *
 * The floor is a dotted rule, so v=0 is an empty cell with a visible base
 * rather than an empty cell.
 */
function drawTerrainCell(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const rx = kx + 1, ry = ky + 1, rw = KW - 2, rh = 12;
    /*
     * The floor sits one row PROUD of the bottom of the box, not on it. On
     * the last row it is directly against the label band, and at low values —
     * where the floor is the only thing drawn — it read as an underline on
     * the parameter name rather than as the base of a cell. One clear row is
     * all it takes to reattach it to the widget.
     */
    dottedRule(ctx, rx, ky + BOX_H - 2, rw, 0);
    fillTerrain(ctx, rx, ry, rw, rh, new Array(rw).fill(v), DIAG_THIRD, true);
    /* The mass is a filled box at full travel, so it gets the notch too —
     * otherwise the one value where the cell is most prominent is also the
     * one where it is squarest. */
    notchCorners(ctx, rx, ry, rw, rh);
}

/**
 * 10. numeric-slab — no graphic at all.
 *
 * The value as a number on a CHECKER tile, with the glyphs knocked out on a
 * cleared plate so they stay crisp over the dither. The furthest point on the
 * axis, and the argument for it is not aesthetic: every one of the other nine
 * options answers "roughly where in its range" and none of them answers "what
 * exactly", which is the question the touched-knob header currently exists to
 * answer. A grid of slabs makes the header redundant.
 *
 * The cost is equally plain — a page of numbers cannot be scanned in one look
 * the way a page of shapes can, and the tile carries no modulation position
 * at all. It is in the set to mark the end of the axis honestly.
 */
function drawNumericSlab(ctx, kx, ky, normVal) {
    const v = clamp01(normVal);
    const bx = kx, by = ky + 2, bw = KW, bh = 11;
    fillDithered(ctx, bx, by, bw, bh, CHECKER);
    notchCorners(ctx, bx, by, bw, bh);
    /* font4x5, not the label face: "100" is 13px here against 17 in the 5x7,
     * and 17 is the whole cell. The three-digit case is not an edge case — it
     * is every parameter at the top of its range. */
    const text = String(Math.round(v * 100));
    const tw = fontWidth4x5(text);
    const tx = bx + Math.floor((bw - tw) / 2);
    const ty = by + 3;
    /* A TWO-pixel horizontal margin round the plate, clamped to the slab. One
     * pixel left the checker lapping at the glyphs — a 4x5 digit has 1px
     * strokes, so a 50% ground one pixel away is close enough to break the
     * silhouette of a 3 or an 8. Vertically one row is enough: nothing in
     * this font ascends or descends into it. */
    const px0 = Math.max(bx + 1, tx - 2);
    const px1 = Math.min(bx + bw - 2, tx + tw + 1);
    ctx.fillRect(px0, ty - 1, px1 - px0 + 1, 7, 0);
    fontPrint4x5(ctx, tx, ty, text, 1);
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
            position: 1, id: "thin-arc", name: "Thin arc", draw: drawThinArc,
            note: "The shipping dial with the pointer lifted off the hub and pulled back from the rim (0.26r..0.72r), so it reads as an indicator floating on a track rather than as a clock hand. Half the ink, same information, and the only option a reviewer could adopt without anyone noticing the UI changed — which is why it is position 1 and not a straw man. Its weakness is the same as its virtue: at true size the marker is a 4px dash and a page of eight is a page of faint specks.",
        },
        {
            position: 2, id: "open-gap", name: "Open gap", draw: drawOpenGap,
            note: "The value is a 26-degree bite out of the track, with no pointer at all. Removes the pointer-on-a-ring construction that is the most literally Elektron thing on the screen, while keeping the circle a knob is expected to be; at the extremes the bite merges with the track opening, so ends of travel show as a widening gap rather than a marker jammed against a stop.",
        },
        {
            position: 3, id: "inset-rim", name: "Inset rim", draw: drawInsetRim,
            note: "Commits to the dial instead of thinning it: track at r=7 so it clears the cell edges, 2px pointer welded hub to rim. The only option heavier than the baseline, which is the answer to the real complaint about eight 1px dials in a row — they turn to grey at true size before they look derivative.",
        },
        {
            position: 4, id: "tick-ladder", name: "Tick ladder", draw: drawTickLadder,
            note: "Twelve radial ticks lit up to the value: a bar graph bent round a circle, so value is carried by area rather than by the angle of one short stroke. Still rotary, but there is no continuous ring anywhere in it, and it survives being read from across a room in a way an arc does not.",
        },
        {
            position: 5, id: "stipple-arc", name: "Stipple arc", draw: drawStippleArc,
            note: "Five 2x2 stamps, always five, spread from the start of travel to the value — so the encoding is SPACING, not extent. Low values cluster, high values open into an even necklace. The most distinctive construction here and the least legible: on a real page the four cells read as four unrelated punctuation marks rather than as four instances of one control, and at low values it is a hook in a corner that looks more like a rendering fault than a knob.",
        },
        {
            position: 6, id: "segment-ring", name: "Segment ring", draw: drawSegmentRing,
            note: "An eight-segment LED collar: the 1px track stays complete so the unused range is visible, and a lit segment thickens inward to 3px. Value as weight rather than as presence keeps the ring's silhouette stable while it fills, and it maps directly onto the physical encoder collars this fleet emulates. Reads at a glance from further off than any other option here that is still round.",
        },
        {
            position: 7, id: "bar-in-box", name: "Bar in box", draw: drawBarInBox,
            note: "A mixer-style horizontal fill inside the corner-notched box, over a DIAG_LIGHT ground so the empty track is still visible at zero. The conservative exit from the rotary metaphor: universally legible, trivially cheap, and the least differentiated of the four non-dials. It also has a cost the swatch hides — in context it is a framed rectangle sitting next to the ENUM SQUARE, which is also a framed rectangle, so the page loses the shape distinction that currently tells a continuous cell from a discrete one at a glance.",
        },
        {
            position: 8, id: "dotted-track", name: "Dotted track", draw: drawDottedTrack,
            note: "vimana's render_indicator fitted to a 17px cell: 50%-dotted rail, solid 3x3 thumb, a stop mark at each end. The only option in the set with a shipped precedent rather than a hypothesis behind it — the reference product renders every continuous parameter this way and carries no dial widget at all.",
        },
        {
            position: 9, id: "terrain-cell", name: "Terrain cell", draw: drawTerrainCell,
            note: "Value as mass: DIAG_THIRD hatch under a solid crest, filled bottom-up from a dotted floor. Eight of these read as one skyline rather than as eight separate widgets, which is the only option here that changes what a PAGE looks like and not just what a cell looks like.",
        },
        {
            position: 10, id: "numeric-slab", name: "Numeric slab", draw: drawNumericSlab,
            note: "No graphic at all — the value 0..100 knocked out of a CHECKER tile. Every other option answers roughly where in range; this is the only one that answers exactly what, which is the job the touched-knob header currently does. The cost is that a page of numbers cannot be scanned in one look, and the tile has nowhere to put a modulation position.",
        },
    ],
    });
}
