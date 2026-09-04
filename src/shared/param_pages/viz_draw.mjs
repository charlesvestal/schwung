/**
 * viz_draw.mjs — draw one resolved viz group (see viz.mjs) into a rect.
 *
 * PURE with respect to the device, exactly like render_page.mjs: everything
 * goes through the injected draw context, nothing here reads a param or owns
 * the screen. A group replaces the individual cells its roles occupy with one
 * picture spanning the same slot range — never more.
 *
 * This is a direct port of schwung-movy's renderer geometry
 * (`src/renderer/{envelope,filter-curve,lfo-wave,eq-curve,wav-form,knob}.ts`,
 * © 2026 megadake, MIT — https://github.com/DimaDake/schwung-movy), not an
 * independent design. Movy draws real per-pixel curves (Bresenham lines, one
 * fillRect column at a time) and its absolute pixel constants are fixed to its
 * 128px-wide, 16px-tall knob-box layout (`ROW0_Y=11`/`ROW1_Y=35`, `KW=16`,
 * `CELL_W=32`) — see render_page_movy.mjs, which is the actual port of that
 * layout and calls into the functions here with `rect` set to exactly one of
 * those 16px-tall knob boxes (`rect.y+1..rect.y+14` is Movy's row content
 * area). These functions draw the GRAPHIC BODY ONLY, no label — Movy draws a
 * column's label separately (`renderer/label.ts` drawLabelCell), and
 * render_page_movy.mjs does the same.
 *
 * render_page.mjs's own dial/bar grid also calls these (a different, wider
 * `rect` per group's cell span) so a graphic can appear there too; the top 16
 * rows of whatever rect it is given are used and the rest of the cell is left
 * to that caller.
 */

import {
    clamp01, fractionOf, line,
    CHECKER, fillDithered, dashedVRule, notchCorners,
} from "./render_page.mjs";
import {
    VIZ_ENVELOPE, VIZ_FILTER, VIZ_LFO, VIZ_WAVEFORM, VIZ_FADER, VIZ_SWITCH, VIZ_EQ, VIZ_SAMPLE,
} from "./viz.mjs";
import { enumIndexOf } from "./param_meta.mjs";
import { wavPeaks, resamplePeaks } from "./wav_peaks.mjs";
import { observeLanded, easeOut, lerp } from "./anim_state.mjs";
import { isCustomKind, drawCustom } from "./widget_registry.mjs";

/* ------------------------------------------------------------- animation --
 *
 * EVERY ANIMATION HERE IS OPTIONAL, and that is a hard contract rather than a
 * convenience. `anim` (a store from anim_state.mjs) and `nowMs` arrive as
 * TRAILING parameters on drawVizGroup and on the two widgets that move; with
 * neither supplied every widget draws exactly the pixels it drew before, which
 * is what lets the pinned baselines, the host tests and the device stay
 * untouched until a caller opts in. A missing `anim` is the normal case, not an
 * error — do not "fix" it by defaulting to a fresh store, which would make the
 * renderer stateful and every first frame animate.
 */


/* -------------------------------------------------------------- primitives */

/* schwung-movy renderer/primitives.ts: dot / dottedV / dottedH. */
function dot(ctx, x, y) { ctx.fillRect(x, y, 2, 2, 1); }
function dottedV(ctx, x, y0, y1) {
    const lo = Math.min(y0, y1), hi = Math.max(y0, y1);
    for (let y = lo; y <= hi; y += 2) ctx.fillRect(x, y, 1, 1, 1);
}

/*
 * A BOUNDARY INSIDE A DITHERED MASS IS A GAP, NOT A LINE.
 *
 * The envelope's section markers were `dottedV`, drawn over the CHECKER mass —
 * and a dotted line over a 50% checker is never dotted. dottedV steps by 2, so
 * every pixel it draws shares one parity of y; CHECKER lights (x+y)%2===0. So
 * the whole marker either coincides with the mass and VANISHES, or falls
 * entirely in its gaps and the column comes out SOLID WHITE. Which of the two
 * you get is decided by the parity of (x + susY) — it changes as the value
 * moves the boundary one pixel, so the same marker flickers between invisible
 * and a hard white rule as a knob is turned. Reported from the device as a
 * white line appearing where sections collide.
 *
 * Clearing instead of drawing is parity-independent by construction: black over
 * anything is black. It also matches what the rest of the page already does
 * with a mark on filled ground — the switch's slug is a knockout, the corner
 * notches are knockouts.
 */
function knockoutV(ctx, x, y0, y1) {
    const lo = Math.min(y0, y1), hi = Math.max(y0, y1);
    if (hi < lo) return;
    ctx.fillRect(x, lo, 1, hi - lo + 1, 0);
}
function dottedH(ctx, x0, x1, y) {
    const lo = Math.min(x0, x1), hi = Math.max(x0, x1);
    for (let x = lo; x <= hi; x += 2) ctx.fillRect(x, y, 1, 1, 1);
}

/**
 * MEASURED ON DEVICE (src/shared/draw_bench.mjs, run 2026-08-19):
 *
 *     text_width (a crossing with no pixel work)   489ns
 *     fill_rect 1x1                                487ns
 *     fill_rect 32x8 (256 pixels)                 1.47us  -> 5.8ns/pixel
 *     draw_line 40px                               764ns
 *     print "MMMM"                                1.28us
 *     draw_arc r=7                                5.75us
 *     a whole renderPageMovy page                 1.62ms  -> 7% of a 44Hz frame
 *
 * A QuickJS->C crossing costs about 490ns, or ~250ns once the benchmark's own
 * closure call (235ns) is subtracted. It is roughly twice a JS function call
 * and cheaper than three interpreted loop iterations.
 *
 * This file used to claim 90-100us per binding, and every "spend fewer draw
 * calls" decision in this library descends from that figure. It is wrong by
 * about 200x. A worst-case 475-call page costs 0.11ms of crossing overhead,
 * not 45ms. Do not reintroduce a draw-call budget without re-running the
 * benchmark first.
 *
 * The corollary matters more than the correction: a JS typed-array write is
 * 243ns, while C fills a pixel in 5.8ns, so moving rasterisation into JS —
 * building the framebuffer there and blitting it in one call — would be ~42x
 * SLOWER per pixel. Native primitives are the right design; the boundary was
 * never the problem.
 *
 * `ctx.line` is still preferred over a JS Bresenham where a caller offers
 * one, because the whole walk happens in C for one call rather than one call
 * per pixel run. That reasoning survives; only the magnitude changed.
 */

function jsLine(ctx, x0, y0, x1, y1, color) {
    x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
    const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    let err = dx + dy, x = x0, y = y0;
    /* Coalesce consecutive same-axis steps (a shallow or flat stretch, or a
     * purely vertical one) into one wide/tall fillRect instead of one per
     * pixel; only a genuinely diagonal run costs a call per pixel here — the
     * native ctx.line path above has none of that limit, this is only the
     * fallback for a caller that doesn't provide one. */
    let runX = x0, runY = y0, runLen = 1, runAxis = 0;   /* 0 none, 1 horiz, 2 vert */
    const flush = () => {
        if (runAxis === 1) ctx.fillRect(sx > 0 ? runX : runX - runLen + 1, runY, runLen, 1, color);
        else if (runAxis === 2) ctx.fillRect(runX, sy > 0 ? runY : runY - runLen + 1, 1, runLen, color);
        else ctx.fillRect(runX, runY, 1, 1, color);
    };
    for (;;) {
        const atEnd = x === x1 && y === y1;
        const e2 = 2 * err;
        let nx = x, ny = y;
        if (!atEnd) {
            if (e2 >= dy) { err += dy; nx = x + sx; }
            if (e2 <= dx) { err += dx; ny = y + sy; }
        }
        const movedX = nx !== x, movedY = ny !== y;
        if (!atEnd && movedX && !movedY && (runAxis === 1 || runAxis === 0)) {
            runAxis = 1; runLen++;
        } else if (!atEnd && movedY && !movedX && (runAxis === 2 || runAxis === 0)) {
            runAxis = 2; runLen++;
        } else {
            flush();
            runX = nx; runY = ny; runLen = 1; runAxis = 0;
        }
        x = nx; y = ny;
        if (atEnd) { flush(); break; }
    }
}

/** One connecting segment: the native binding when the caller provides one
 * (one C-side call regardless of length), else a coalesced JS Bresenham. */
function drawLine(ctx, x0, y0, x1, y1, color = 1) {
    if (typeof ctx.line === "function") ctx.line(Math.round(x0), Math.round(y0), Math.round(x1), Math.round(y1), color);
    else jsLine(ctx, x0, y0, x1, y1, color);
}

/** Connect consecutive points with drawLine — one call per segment. */
function drawPolyline(ctx, points, color = 1) {
    for (let i = 0; i < points.length - 1; i++) {
        drawLine(ctx, points[i][0], points[i][1], points[i + 1][0], points[i + 1][1], color);
    }
}

/**
 * Refine where the curve crosses into the skip region. `inX` is a sample that
 * draws, `outX` an adjacent one that skips; returns the x closest to `inX`
 * that ALREADY skips, so the polyline can be terminated exactly on the
 * boundary. Pure math — no draw calls, so the cost is a handful of `yAt`
 * evaluations per crossing and nothing on the wire.
 */
function skipBoundaryX(yAt, skipY, inX, outX) {
    let lo = inX, hi = outX;
    for (let k = 0; k < 6; k++) {
        const mid = (lo + hi) / 2;
        if (skipY(yAt(Math.round(mid)))) hi = mid; else lo = mid;
    }
    return hi;
}

/**
 * Sample a column-defined curve (filter/eq/lfo/waveform all compute one y
 * per x column) at a fixed, small number of points across [x0, xEnd) and
 * connect them with real line segments — smooth, not stepped, and its cost
 * is O(sample count), not O(width). `skipY` breaks the polyline rather than
 * drawing through a region that should read as absent (filter's floor).
 *
 * A run does not simply END at its last non-skipped SAMPLE: it is extended to
 * the refined crossing point, which lies inside the skip region and therefore
 * sits exactly on the boundary (for the filter, the bottom axis).
 *
 * That distinction is the whole bug behind "the cutoff curve doesn't go all
 * the way down, it flashes on and off as you turn". The filter's roll-off
 * occupies about 11% of the span (`dropW`), so only ~3 of the 28 uniform
 * samples ever land inside it. Truncating at the last of those left the tail
 * hanging in mid-air at whatever height that sample happened to have — and as
 * cutoff moves, that sample climbs the roll-off (tail shrinks) until the next
 * sample column crosses in and the tail snaps long again. Measured over one
 * detent at a time (0.005 of range) the endpoint sawtoothed between y=6 and
 * y=13 in a 13px-tall box: the bottom half of the curve visibly appearing and
 * disappearing. Ending on the true crossing makes the tail land on the axis at
 * every value and move smoothly, and costs no extra draw calls.
 */
function drawColumnCurve(ctx, x0, xEnd, yAt, color = 1, skipY = null, samples = 28) {
    const w = xEnd - x0;
    if (w <= 0) return;
    /* EVEN integer spacing. `x0 + (w-1)*(i/(n-1))` looks even and is not: at
     * w=127, n=28 the evaluated columns land 5,4,5,5,4,5,5,4... apart, so
     * every other segment covers 25% more of the curve than its neighbour and
     * the reconstructed slope alternates. On a steep stretch that is directly
     * visible as lumpiness. A fixed stride costs the same number of calls. */
    const n = Math.max(2, Math.min(samples, Math.round(w)));
    const stride = Math.max(1, Math.round(w / (n - 1)));
    let run = [];
    /* x of the most recent skipped sample, so a run that STARTS mid-span
     * (a highpass rising off the floor) begins on the boundary too. */
    let lastSkipX = null;
    const flush = () => { if (run.length >= 2) drawPolyline(ctx, run, color); run = []; };
    for (let i = 0; i < n; i++) {
        const x = Math.min(x0 + w - 1, x0 + i * stride);
        const y = yAt(x);
        if (skipY && skipY(y)) {
            if (run.length) {
                const bx = skipBoundaryX(yAt, skipY, run[run.length - 1][0], x);
                run.push([bx, yAt(Math.round(bx))]);
            }
            flush();
            lastSkipX = x;
            continue;
        }
        if (!run.length && lastSkipX !== null && skipY) {
            const bx = skipBoundaryX(yAt, skipY, x, lastSkipX);
            run.push([bx, yAt(Math.round(bx))]);
        }
        run.push([x, y]);
    }
    flush();
}

/**
 * Draw a column-defined curve at FULL horizontal resolution — one y per pixel
 * column — coalescing equal-y neighbours into a single horizontal run and
 * emitting a vertical riser at each step. This is what `drawWaveCell` already
 * does for the single-knob silhouette, generalised.
 *
 * Why a periodic wave needs this and `drawColumnCurve` will not do:
 * approximating one with ~28 straight segments reads as a POLYGON, not a
 * wave. Sampling a sine every ~5 columns puts each vertex at a different
 * fraction of the curvature, so the run lengths down one flank come out
 * `5,3,2,4,5` where a real sine tapers monotonically — the shape visibly
 * wobbles, and the wobble MOVES as rate or phase changes because the sample
 * grid slides against the waveform. It also slants what should be vertical:
 * a square LFO's edge became a diagonal across one whole sample step, even
 * though drawWaveCell rendered the same square crisply two functions away.
 *
 * Cost is data-dependent rather than fixed, and mostly BETTER than the
 * polyline it replaced: a square is 3-7 calls (vs a flat 27), sample-and-hold
 * 7-15, a saw 27-55. Only the smooth shapes cost more — a sine 47-95, noise
 * up to 147 — and at ~490ns per call that worst case is 72us, which is 0.3%
 * of a frame. Draw the wave honestly; the calls are not the expensive part.
 */
function drawStepCurve(ctx, x0, xEnd, yAt, color = 1) {
    const w = xEnd - x0;
    if (w <= 0) return;

    /* Build first, draw second — so the budget check costs no draw calls. */
    const runs = [];
    let runStart = x0, runY = yAt(x0);
    for (let x = x0 + 1; x < xEnd; x++) {
        const y = yAt(x);
        if (y === runY) continue;
        runs.push([runStart, x - runStart, runY, y]);
        runStart = x; runY = y;
    }
    runs.push([runStart, xEnd - runStart, runY, null]);

    /* No draw-call ceiling here. There used to be one, on the belief that a
     * binding cost 90-100us; measured, it is ~490ns, so the most expensive
     * shape in the vocabulary (noise at full rate, ~147 calls) costs about
     * 72us — 0.3% of a frame. Falling back to a coarse polyline to save that
     * traded a visibly wrong waveform for nothing. See draw_bench.mjs. */

    for (const [rx, rw, ry, nextY] of runs) {
        if (rw > 0) ctx.fillRect(rx, ry, rw, 1, color);
        if (nextY !== null) {
            /* The riser carries only the rows BETWEEN this run and the next.
             * Spanning ry..nextY inclusive re-drew ry in the riser column,
             * which the run had already covered, so every row came out one
             * column too long and the staircase read as a chunky zigzag
             * rather than a line. The run and the riser stay 8-connected at
             * the corner. */
            if (nextY < ry) ctx.fillRect(rx + rw, nextY, 1, ry - nextY, color);
            else ctx.fillRect(rx + rw, ry + 1, 1, nextY - ry, color);
        }
    }
}

/*
 * The band every graphic body draws into: 13 rows starting one below the rect
 * top. THIRTEEN, an odd count, on purpose.
 *
 * A bipolar graphic — LFO, EQ, sample — is drawn as `mid - sample * amp`, so
 * it needs its zero line to be a real ROW. The band used to be 14 rows
 * (topY=rect.y+1, botY=topY+13), which has no centre: `round((1+14)/2)` is 8
 * while the true middle is 7.5, so the whole wave sat half a row low and
 * `amp` was the fractional 6.5. At full depth that put the peak at
 * `round(1.5)=2` — one row short of the top — and the trough at
 * `round(14.5)=15`, one row BELOW the bottom of the box, which is the stray
 * jag that appeared under a triangle's troughs.
 *
 * 13 rows gives an integer centre (topY+6) and an integer amplitude (6), so
 * full depth lands exactly on topY and botY and the axis is a row that
 * actually exists. Same reason BOX_H and LBL_H are odd.
 */
export const VIZ_ROWS = 13;

/*
 * The narrowest cell a graphic can be drawn into.
 *
 * It used to be a hard requirement of `drawSwitch`, which was a tabulated
 * sprite ported pixel-for-pixel from Movy: 26 columns wide, FIXED, because it
 * was a rounded rectangle rasterised at one size and a rasterised curve cannot
 * be stretched and stay round. Below 26 it did not narrow, it hung out of the
 * cell on both sides.
 *
 * SCH-50 `pill-inverted` replaced that sprite with a rectangular pill whose
 * width is `min(24, rect.w - 4)`, so the switch now genuinely scales and the
 * overhang cannot happen. The floor is kept anyway: 26 is about where the rest
 * of the vocabulary — a filter curve, an ADSR — stops being readable, and a
 * caller that has less room than this should stand the graphics down rather
 * than draw a picture nobody can use.
 *
 * The full screen gives a 32px cell, so this only binds on a caller that
 * passes a narrower `rect` — see render_page.mjs, which does exactly that.
 */
export const VIZ_MIN_W = 26;

function band(rect) {
    const topY = rect.y + 1;
    const botY = topY + VIZ_ROWS - 1;
    return { topY, botY, midY: topY + ((VIZ_ROWS - 1) >> 1), amp: (VIZ_ROWS - 1) / 2 };
}

/* ------------------------------------------------------------- ghost fill --
 *
 * SCH-50 `ghost-fill`, and THE ONE THING THAT MATTERS ABOUT IT IS THAT THERE IS
 * EXACTLY ONE OF IT.
 *
 * The envelope, the filter response, the LFO and the sample waveform are
 * pictures of the maths. An ADSR that does not look like an ADSR is simply
 * wrong, and nobody owns the shape of an exponential decay — so the four graphs
 * are allowed to differ in SHAPE and must not differ in TREATMENT. In the
 * catalog that was enforced mechanically: one function object registered in all
 * four sets, so they could not drift. Here it is enforced by there being one
 * `fillCurveMass` and four callers.
 *
 * DO NOT give one graph its own copy to tune. A filter graph that fills
 * differently from an envelope is a filter graph that can misrepresent the
 * filter, which is the failure this construction exists to prevent.
 *
 * It won ALL FOUR sets independently — the single strongest result in the
 * catalog — while the treatment that shipped before it (`thin-stroke`, a bare
 * hairline) ranked 10th / 8th / 7th / 8th. That is the useful finding: the
 * treatment being replaced was not merely undistinctive, it was not liked, so
 * the change is not a cosmetic tax paid for its own sake.
 *
 * The curve becomes an AREA, which is the reading a musician wants — how much
 * of the note is loud, how much of the spectrum passes — rather than a boundary
 * they have to integrate by eye. CHECKER (50%) is the highest density at which
 * the 1px stroke drawn on top stays visibly separate from its own fill.
 *
 * ACCEPTED COST: at a high sustain the mass covers most of a 13-row band, and
 * twelve rows of checker at true size is grey, not texture. The page gets
 * noticeably heavier.
 *
 * NO NOTCHED CORNERS HERE, deliberately, though they are the house idiom
 * everywhere else. A box's corners are a design decision; the corners of a
 * filled curve are DATA — the left edge of a passband, the floor a release
 * lands on — and rounding them off is exactly the misrepresentation above.
 */

/**
 * Fill the mass of a column-defined curve through CHECKER, between the curve
 * and its zero line.
 *
 * @param yAt      (px) => y, the same closure the stroke is drawn from, so the
 *                 fill can never disagree with the line about where the curve is
 * @param baseY    the graph's zero row: the floor for a unipolar graph
 *                 (envelope, filter), the centre for a bipolar one (LFO)
 * @param mirrorAt optional (px) => y for a graph symmetric about its zero line
 *                 (the sample waveform), so the mass spans crest to trough
 *
 * The span is SIGNED for a bipolar graph: a trough below the centre line fills
 * downward. That is the honest reading and the only one that keeps the fill
 * attached to the curve — filling always down to the floor would detach the
 * shape from its ink on every negative half cycle.
 *
 * Runs BEFORE the stroke at every call site. The stroke is solid and the fill
 * is not, so drawing the fill second would punch its lattice through the line.
 */
/**
 * Turn a polyline's breakpoints into the `yAt` closure `fillCurveMass` wants.
 *
 * The envelope is the one graph that is NOT defined per column — it is four
 * straight segments between five vertices — so its fill has to be told the same
 * vertices the strokes are drawn from rather than deriving the shape a second
 * time. Anything past the last vertex rests on the zero line, which is what
 * makes a short release leave the tail of the cell empty instead of filled.
 */
function segmentsYAt(pts, restY) {
    return (px) => {
        for (let i = 0; i < pts.length - 1; i++) {
            const [ax, ay] = pts[i], [bx, by] = pts[i + 1];
            if (px < ax || px > bx) continue;
            if (bx === ax) return Math.round(by);
            return Math.round(ay + (by - ay) * ((px - ax) / (bx - ax)));
        }
        return restY;
    };
}

function fillCurveMass(ctx, x0, xEnd, yAt, baseY, topY, botY, mirrorAt = null) {
    const clip = (y) => (y < topY ? topY : (y > botY ? botY : y));
    for (let x = x0; x < xEnd; x++) {
        const a = clip(yAt(x));
        const b = clip(mirrorAt ? mirrorAt(x) : baseY);
        const lo = a < b ? a : b, hi = a < b ? b : a;
        for (let y = lo; y <= hi; y++) if (CHECKER(x, y)) ctx.fillRect(x, y, 1, 1, 1);
    }
}

function frac(metaIndex, key, values) {
    if (!key) return 0;
    return fractionOf(metaIndex.getOrGuess(key), values ? values[key] : undefined);
}

function optionText(metaIndex, key, values) {
    if (!key) return "";
    const meta = metaIndex.getOrGuess(key);
    const raw = values ? values[key] : undefined;
    if (!meta || !Array.isArray(meta.options)) return "";
    /* Resolves a name-reporting plugin's value too — see enumIndexOf. */
    const idx = enumIndexOf(meta, raw);
    return (idx >= 0 && idx < meta.options.length) ? String(meta.options[idx]) : "";
}

/* -------------------------------------------------------------- envelope */

/**
 * schwung-movy renderer/envelope.ts drawFullAdsr/drawPartialEnv, ported.
 *
 * Full ADSR (4 roles: the group always spans a whole row when it has 4) uses
 * Movy's exact reference geometry (26px attack, 4px+24px decay, a fixed
 * gate-off x, a 33px release) proportionally against `rect.w`, which is 128
 * (Movy's own reference width) whenever this draws a full-width row. Partial
 * envelopes (2-3 roles) use Movy's span-relative formula directly.
 */
export function drawEnvelope(ctx, rect, roles, values, metaIndex) {
    /* Time order, which is draw order. HOLD is here because an AHR envelope is
     * a real shape, not a degenerate ADSR: gate and ducker both declare
     * attack/hold/release and nothing else. Leaving hold out of this list did
     * not drop the group -- it drew the group WITHOUT its middle segment, so
     * the knob was in the span, turning it moved nothing on screen, and the
     * curve quietly lied about the shape. */
    const present = ["attack", "hold", "decay", "sustain", "release"].filter((r) => roles[r]);
    if (present.length < 2) return;

    const x0 = rect.x, x1 = rect.x + rect.w;
    const { topY, botY: bodyBottom } = band(rect);

    /* drawFullAdsr is Movy's fixed ADSR reference geometry -- four named
     * segments, no room for a fifth. Anything else, including a 4-role set
     * that contains hold, goes to the span-relative builder. */
    const isPlainAdsr = present.length === 4 && !roles.hold;
    if (isPlainAdsr) {
        drawFullAdsr(ctx, x0, x1, topY, bodyBottom, roles, values, metaIndex);
    } else {
        drawPartialEnv(ctx, x0, x1, topY, bodyBottom, present, roles, values, metaIndex);
    }
}

function drawFullAdsr(ctx, x0, x1, topY, baseY, roles, values, metaIndex) {
    const a = frac(metaIndex, roles.attack, values);
    const d = frac(metaIndex, roles.decay, values);
    const s = frac(metaIndex, roles.sustain, values);
    const r = frac(metaIndex, roles.release, values);

    const W = x1 - x0;                       // Movy's reference W is 128
    const usableH = baseY - topY;            // 13
    const gateX = x0 + W * (88 / 128);        // fixed note-off reference

    const peakX = x0 + Math.round(a * W * (26 / 128));
    let sustStartX = peakX + W * (4 / 128) + Math.round(d * W * (24 / 128));
    if (sustStartX > gateX - W * (2 / 128)) sustStartX = gateX - W * (2 / 128);
    const susY = baseY - Math.round(s * usableH);
    let relEndX = gateX + W * (4 / 128) + Math.round(r * W * (33 / 128));
    if (relEndX > x1 - 1) relEndX = x1 - 1;

    /* The mass, under the same four segments the strokes draw. `segmentsYAt`
     * interpolates the SAME breakpoints rather than re-deriving the envelope,
     * so the fill cannot disagree with the line about where the curve is —
     * which is the whole contract of fillCurveMass. */
    fillCurveMass(ctx, x0, x1, segmentsYAt([
        [x0, baseY], [peakX, topY], [sustStartX, susY], [gateX, susY], [relEndX, baseY],
    ], baseY), baseY, topY, baseY);

    drawLine(ctx, x0, baseY, peakX, topY);            // attack rise
    drawLine(ctx, peakX, topY, sustStartX, susY);     // decay fall
    drawLine(ctx, sustStartX, susY, gateX, susY);     // sustain plateau
    drawLine(ctx, gateX, susY, relEndX, baseY);       // release fall

    knockoutV(ctx, sustStartX, susY + 1, baseY - 1);
    knockoutV(ctx, gateX, susY + 1, baseY - 1);

    dot(ctx, Math.max(x0, peakX - 1), topY);
    dot(ctx, sustStartX - 1, Math.max(topY, susY - 1));
    dot(ctx, gateX - 1, Math.max(topY, susY - 1));
    dot(ctx, Math.min(x1 - 2, relEndX - 1), baseY - 1);
}

function drawPartialEnv(ctx, leftX, xEnd, topY, baseY, present, roles, values, metaIndex) {
    const rightX = xEnd - 1;
    const usableH = baseY - topY;
    const span = rightX - leftX;

    const has = (r) => present.includes(r);
    const val = {};
    for (const r of present) val[r] = frac(metaIndex, roles[r], values);

    /*
     * A RELEASE ROLE IS EVIDENCE OF A SUSTAIN STAGE, even with no sustain role.
     *
     * "No sustain role" was read as "sustain is zero". For an A/D envelope that
     * is right -- a percussive shape decays to silence and there is nothing to
     * release. It stops being right the moment `release` is declared, because
     * release means "fall from the held level to zero": if the held level were
     * already zero there would be nothing to release from and the role would be
     * meaningless.
     *
     * So an A/D/R was drawn decaying to the floor, no plateau, and a release
     * running along the bottom from one floor point to another -- a flat line.
     * Reported against a Yamaha QY-70 editor, whose XG Multi Part offsets are
     * EG Attack, EG Decay and EG Release and nothing else: the sustain LEVEL
     * lives in the voice and is not addressable, but hold a note and it holds.
     * The stage exists; the editor just cannot read its height.
     *
     * The height is therefore a PICTURE, not a measurement -- half scale, which
     * leaves decay and release both clearly legible. Nothing reads it as a
     * value: it is only geometry, and `val.sustain` stays undefined.
     */
    const IMPLIED_SUSTAIN = 0.5;
    const impliedSustain = !has("sustain") && has("decay") && has("release");
    /* A/D with no release must still fall to the floor -- that case was right. */
    const susY = has("sustain") ? baseY - Math.round(val.sustain * usableH)
               : impliedSustain ? baseY - Math.round(IMPLIED_SUSTAIN * usableH)
               : baseY;
    /* The plateau is what makes the stage visible as HELD rather than as a kink
     * between two falls, so the implied case draws one too. */
    const drawsPlateau = has("sustain") || impliedSustain;

    /*
     * ATTACK IS NOT GUARANTEED.
     *
     * The rise was drawn unconditionally from `val.attack`, and an envelope
     * with no attack role makes that `undefined` -- so peakX is NaN, the NaN
     * reaches line()'s `for(;;)` and its equality break is never satisfied.
     * Not a wrong picture: a HANG, the same one docs/PARAM_PAGES.md records
     * for a partial GRID_GEOM freezing the shadow_ui tick.
     *
     * It was unreachable until knob alignment made these pages drawable:
     * surge declares twelve LFO pages carrying hold/sustain/release and no
     * attack at all, and every one of them was blocked by the row constraint
     * before. A latent renderer bug, exposed rather than caused by the
     * alignment -- and the reason `present` is filtered by ROLE and must never
     * be assumed to contain any particular one.
     *
     * With no attack the shape simply starts at full level, which is what an
     * envelope with no rise means.
     */
    const pts = [];
    let cur;
    if (has("attack")) {
        pts.push([leftX, baseY]);
        cur = Math.min(rightX - 2, leftX + 4 + Math.round(val.attack * span * 0.4));
        pts.push([cur, topY]);
    } else {
        cur = leftX;
        pts.push([cur, topY]);
    }
    /* Hold is a plateau AT THE PEAK, between the attack rise and whatever
     * falls next -- for an AHR (gate, ducker) that is the release. */
    if (has("hold")) {
        const holdEnd = Math.min(rightX - 2, cur + Math.round(val.hold * span * 0.3));
        if (holdEnd > cur) { pts.push([holdEnd, topY]); cur = holdEnd; }
    }
    if (has("decay")) {
        cur = Math.min(rightX - 2, cur + 4 + Math.round(val.decay * span * 0.35));
        pts.push([cur, susY]);
    } else if (has("sustain")) {
        pts.push([cur, susY]);
    }
    if (drawsPlateau) {
        const plateauEnd = has("release") ? Math.round(leftX + span * 0.7) : rightX;
        if (plateauEnd > cur) { pts.push([plateauEnd, susY]); cur = plateauEnd; }
    }
    if (has("release")) {
        const endX = Math.min(rightX, cur + 4 + Math.round(val.release * span * 0.4));
        pts.push([endX, baseY]);
    }

    /* Same mass, same vertices — a 2- or 3-role envelope is the same kind of
     * object as a 4-role one and must not read as a different treatment. */
    fillCurveMass(ctx, leftX, xEnd, segmentsYAt(pts, baseY), baseY, topY, baseY);

    for (let i = 0; i < pts.length - 1; i++) drawLine(ctx, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]);
    for (const [px, py] of pts) dot(ctx, Math.min(xEnd - 2, Math.max(leftX, px - 1)), Math.max(topY, py - 1));
    /*
     * Only where the plateau actually ENDS inside the cell. With no release the
     * plateau runs to rightX, so the marker was clamped to xEnd-2 and drew a
     * rule one pixel in from the edge, dividing the mass from nothing — which is
     * where braids showed it, since braids declares attack/decay/sustain and no
     * release. A boundary at the boundary of the picture marks nothing.
     */
    if (drawsPlateau && cur < rightX - 1) knockoutV(ctx, cur, susY + 1, baseY - 1);
}

/* ----------------------------------------------------------------- filter */

const PASS = 0.62;
const EDGE = 0.10;
const bump = (u, c, w) => Math.exp(-(((u - c) * w) ** 2));

/**
 * Gain 0..1 at horizontal position u (0..1 across the span). Ported verbatim
 * from schwung-movy's filter-curve.ts gainAt — see that file for the shape
 * reasoning (a quarter-ellipse roll-off, a rounded shoulder into the corner).
 */
export function filterGainAt(u, mode, c, r, steep) {
    const cx = EDGE + c * (1 - 2 * EDGE);
    const dropW = steep ? 0.07 : 0.11;
    const pk = r * (1 - PASS);
    const top = PASS + pk;
    const ellipse = (dist) => { const t = dist / dropW; return t >= 1 ? 0 : top * Math.sqrt(1 - t * t); };
    const shoulder = (dist) => PASS + pk * bump(dist, 0, 8);
    switch (mode) {
        case "hp": return u >= cx ? shoulder(u - cx) : ellipse(cx - u);
        case "bp": return Math.min(1, top * bump(u, cx, 5 + r * 4));
        case "notch": return Math.max(0, PASS - PASS * (0.5 + 0.5 * r) * bump(u, cx, 7));
        case "peak": return Math.min(1, PASS * 0.7 + (0.3 + 0.6 * r) * (1 - PASS * 0.7) * bump(u, cx, 6));
        case "ap":
        case "off": return PASS;
        case "lp":
        default: return u <= cx ? shoulder(cx - u) : ellipse(u - cx);
    }
}

/** Selected filter-mode option string -> Movy's mode vocabulary. */
function filterModeOf(text) {
    const s = String(text || "").toLowerCase();
    const hasLP = /lowpass|low pass|\blp\d?\b/.test(s);
    const hasHP = /highpass|high pass|\bhp\d?\b/.test(s);
    if (/ladder/.test(s)) return hasHP ? "hp" : "lp";
    if (hasLP && hasHP) return "bp";
    if (/notch|bandstop|band stop/.test(s)) return "notch";
    if (/bandpass|band pass|\bbpf\b/.test(s)) return "bp";
    if (hasHP) return "hp";
    if (hasLP) return "lp";
    if (/allpass|all ?pass|\bap\b/.test(s)) return "ap";
    if (/peak|bell/.test(s)) return "peak";
    if (/^\s*off\s*$/.test(s)) return "off";
    return "lp";
}

export function drawFilter(ctx, rect, roles, values, metaIndex) {
    const x0 = rect.x, xEnd = rect.x + rect.w;
    const { topY, botY } = band(rect);
    const h = botY - topY;
    const spanW = xEnd - x0;

    dottedH(ctx, x0, xEnd - 1, botY);

    const mode = filterModeOf(optionText(metaIndex, roles.mode, values));
    const cutoff = frac(metaIndex, roles.cutoff, values);
    const resonance = frac(metaIndex, roles.resonance, values);
    const steep = roles.slope ? frac(metaIndex, roles.slope, values) >= 0.5 : false;

    if (mode === "ap" || mode === "off") {
        const y = Math.round(botY - PASS * h);
        dottedH(ctx, x0, xEnd - 1, y);
        return;
    }

    const yAt = (px) => {
        const g = filterGainAt((px - x0) / spanW, mode, cutoff, resonance, steep);
        return Math.max(topY, Math.min(botY, Math.round(botY - g * h)));
    };

    /* The passband as mass, under the response curve. Unipolar: the zero line
     * is the floor, so this is literally the area under the curve. A column
     * already on the floor fills nothing, which is what keeps a stopband empty
     * rather than giving it a one-row lid. */
    fillCurveMass(ctx, x0, xEnd - 1, yAt, botY, topY, botY);

    /* Skip runs that lie flat on the bottom axis so the curve ends where it
     * reaches the floor instead of continuing along it. Inset one pixel on
     * each side so a neighbouring graphic on the same row gets a visible
     * gap. */
    drawColumnCurve(ctx, x0, xEnd - 1, yAt, 1, (y) => y >= botY);
}

/* -------------------------------------------------------------------- lfo */

/** schwung-movy model/lfo-shapes.ts shapeSample ids 0-10 (the ones a Schwung
 * enum can realistically resolve to — the stepped N-level families and the
 * synth-specific glyphs 11+ are not reachable from a plain shape name here). */
export function lfoShapeSample(shape, t) {
    const ph = t - Math.floor(t);
    switch (shape) {
        case 0: return Math.sin(ph * 2 * Math.PI);
        case 1:
            if (ph < 0.25) return ph * 4;
            if (ph < 0.75) return 1 - (ph - 0.25) * 4;
            return -1 + (ph - 0.75) * 4;
        case 2: return ph * 2 - 1;
        case 3: return ph < 0.5 ? 1 : -1;
        /*
         * SAMPLE AND HOLD: a flat step per quarter cycle, at a level that does
         * not repeat. It used to cycle a fixed four-value table, so every cycle
         * drew the identical staircase and it read as a periodic pattern rather
         * than as random. Hashing the ABSOLUTE step index (t, not ph) makes each
         * step independent while staying perfectly stable frame to frame.
         */
        case 4: {
            const k = Math.floor(t * 4);
            return ((((k * 2654435761) >>> 0) % 2000) / 1000) - 1;
        }
        case 6: return 1 - ph * 2;
        case 7: { const k = Math.floor(ph * 37); return ((((k * 2654435761) >>> 0) % 2000) / 1000) - 1; }
        /*
         * SWISHY, Schwung's random WALK (src/host/lfo_common.h): each cycle it
         * interpolates from where it was to a fresh random target. Not noise —
         * noise is what it drew before, and the two look nothing alike.
         */
        case 8: {
            const c = Math.floor(t);
            const f = t - c;
            const at = (i) => ((((i * 2654435761) >>> 0) % 2000) / 1000) - 1;
            const a0 = at(c), a1 = at(c + 1);
            return a0 + (a1 - a0) * f;
        }
        default: return Math.sin(ph * 2 * Math.PI);
    }
}

function lfoShapeIdOf(text) {
    const n = String(text || "").toLowerCase().replace(/[&\s_]+/g, "");
    if (/^(sine|sin|skewedsine)$/.test(n)) return 0;
    if (/^(tri|triangle)$/.test(n)) return 1;
    if (/^(saw|sawtooth|rampup|softsaw|sawup|ramp)$/.test(n)) return 2;
    if (/^(square|sqr|squ|rect|softsquare|pulse|pulsetr|warmpulse)$/.test(n)) return 3;
    if (/^(sh|samplehold|rnd1|s\+h)$/.test(n)) return 4;
    if (/^(rampdown|sawdown)$/.test(n)) return 6;
    if (/^(noise|rand|rnd|random|smoothrandom)$/.test(n)) return 7;
    /* Schwung's own sixth shape (src/host/lfo_common.h): a random WALK that
     * interpolates toward a fresh target each cycle. The smooth-random
     * silhouette is what that looks like; without this it fell through to the
     * default and drew a sine, which is a different waveform entirely. */
    if (/^(swishy|swish|drunk|randomwalk)$/.test(n)) return 8;
    return 0;
}

/**
 * Phase positions where a shape's slope changes, for the shapes that are
 * piecewise LINEAR. Between two of these the wave is a straight line, so it
 * can be drawn as one real Bresenham segment instead of sampled per column.
 *
 * Only the RAMPS are listed. A sine is curved and has no linear stretch. The
 * stepped shapes (square, sample-and-hold, noise) are already drawn minimally
 * by drawStepCurve, whose run coalescing collapses a square to 3-7 calls on
 * its own — there is nothing left to win there.
 *
 * What this buys on a ramp is large: a full-width triangle costs 5 native
 * line calls instead of ~77 coalesced runs, and a saw 3 instead of ~55. The
 * pixels are also very slightly better — an exact segment distributes its
 * treads more evenly than independently rounding each column does, which
 * removes a few of the doubled treads. It does NOT stop a shallow line
 * looking like a staircase: a triangle ramp is 12 rows over 43 columns, so
 * 3-and-4 pixel treads are what that slope IS on a 1-bit display, at any
 * sample rate. See the aspect-ratio note in drawLfo.
 */
const LFO_LINEAR_BREAKPOINTS = {
    1: [0, 0.25, 0.75],   /* triangle */
    2: [0],               /* saw / ramp up  — jumps at the cycle boundary */
    6: [0],               /* ramp down      — likewise */
};

/**
 * Draw a piecewise-linear wave as exact segments between its breakpoints.
 * A breakpoint where the value jumps (a saw's flyback) emits TWO vertices at
 * the same x, so the connecting segment is the vertical edge itself.
 */
function drawLinearWave(ctx, x0, xEnd, shape, cycles, phase, yOf, color = 1) {
    const span = xEnd - x0;
    if (span <= 0 || cycles <= 0) return;
    const EPS = 1e-6;
    const bps = LFO_LINEAR_BREAKPOINTS[shape];

    const ts = [phase];
    for (let c = Math.floor(phase); c <= Math.ceil(phase + cycles); c++) {
        for (const b of bps) {
            const t = c + b;
            if (t > phase + EPS && t < phase + cycles - EPS) ts.push(t);
        }
    }
    ts.push(phase + cycles);
    ts.sort((a, b) => a - b);

    const xOf = (t) => x0 + Math.round(((t - phase) / cycles) * span);
    const pts = [];
    for (let i = 0; i < ts.length; i++) {
        const t = ts[i], x = xOf(t);
        const before = yOf(lfoShapeSample(shape, t - EPS));
        const after = yOf(lfoShapeSample(shape, t + EPS));
        if (i > 0) pts.push([x, before]);
        if (i === 0 || after !== before) pts.push([x, after]);
    }
    drawPolyline(ctx, pts, color);
}

/**
 * schwung-movy renderer/lfo-wave.ts drawLfoWave, ported. Rate -> cycle
 * density, depth -> amplitude, mirroring Movy's `cycles`/`ampScale` fields.
 *
 * On stairstepping: this band is 128x13, about 10:1, so a wave in it has
 * shallow slopes by construction — a triangle ramp covers 12 rows in 43
 * columns. No drawing technique changes that on a 1-bit display; the lever is
 * geometry (a 2-slot wave is 1.8 px/row and reads as a diagonal, a 4-slot one
 * is 3.6 and reads as a staircase). Waveform glyphs that look clean on other
 * small displays are nearly SQUARE; they are not drawn any more cleverly, they
 * just are not being asked to cross 128 columns in 13 rows.
 */
export function drawLfo(ctx, rect, roles, values, metaIndex) {
    const x0 = rect.x, xEnd = rect.x + rect.w;
    const { topY, botY, midY, amp: fullAmp } = band(rect);
    const spanW = xEnd - x0;

    const shape = lfoShapeIdOf(optionText(metaIndex, roles.shape, values));
    const rateFrac = frac(metaIndex, roles.rate, values);
    const phase = roles.phase ? frac(metaIndex, roles.phase, values) : 0;

    /*
     * DEPTH IS SIGNED. `frac` normalises min..max onto 0..1, which for a
     * bipolar -1..1 depth put ZERO at half amplitude and -100% at nearly flat —
     * exactly backwards. Amplitude is |depth| and a negative depth INVERTS the
     * wave, which is what a negative depth does to the modulation.
     */
    const depthMeta = roles.depth ? metaIndex.getOrGuess(roles.depth) : null;
    const depthRaw = (roles.depth && values) ? Number(values[roles.depth]) : NaN;
    const depthScale = depthMeta
        ? Math.max(Math.abs(Number(depthMeta.min) || 0), Math.abs(Number(depthMeta.max) || 1)) || 1
        : 1;
    const depthSigned = Number.isFinite(depthRaw)
        ? Math.max(-1, Math.min(1, depthRaw / depthScale))
        : (frac(metaIndex, roles.depth, values) * 2 - 1);
    const depthFrac = Math.abs(depthSigned);

    /*
     * RATE HAS TO LOOK LIKE RATE. It used to draw 1..2 cycles across the whole
     * width, so a 20 Hz LFO looked almost identical to a 0.1 Hz one — the number
     * changed and the picture did not. Up to eight cycles now, on a square-root
     * curve because the musically useful rates all live in the bottom of a
     * linear 0.1..20 Hz range and would otherwise be indistinguishable.
     */
    const cycles = 1 + Math.sqrt(Math.max(0, Math.min(1, rateFrac))) * 7;

    /*
     * The BASELINE says which way the modulation goes.
     *
     * Bipolar swings either side of the value you dialled, so the baseline sits
     * mid-band and the wave straddles it. Unipolar only ever adds, so the
     * baseline drops to the bottom and the wave sits ON it. That is the one
     * thing about an LFO you can read across a room, and it costs a graphic
     * nothing — the polarity control keeps its own cell on the other row and
     * lends its value through a span:false role.
     *
     * Defaults to bipolar when no polarity role is declared, which is what
     * every existing caller of this graphic gets.
     */
    const unipolar = roles.polarity
        ? /^uni/i.test(String(optionText(metaIndex, roles.polarity, values) || ""))
        : false;
    const depthSign = depthSigned < 0 ? -1 : 1;
    const baseY = unipolar ? botY : midY;
    /* Unipolar has the whole band to rise through, bipolar half of it each way. */
    const amp = Math.max(0.15, depthFrac) * (unipolar ? (botY - topY) : fullAmp);

    dottedH(ctx, x0, xEnd - 1, baseY);

    const yAt = (px) => {
        const u = (px - x0) / spanW;
        const t = u * cycles + phase;
        const sample = lfoShapeSample(shape, t) * depthSign;
        /* Map [-1,1] into [0,1] for unipolar: it offsets upward only. */
        const v = unipolar ? (sample + 1) / 2 : sample;
        return Math.round(baseY - v * amp);
    };

    /* The mass, between the wave and its baseline. Bipolar fills SIGNED — a
     * trough below the centre line fills downward — which is the only reading
     * that keeps the fill attached to the wave through a negative half cycle.
     * Unipolar rests on the floor and fills upward, same as the envelope.
     *
     * Driven off `yAt` for every shape including the ramps, which the STROKE
     * draws through `drawLinearWave` instead. That is not a disagreement: yAt
     * and drawLinearWave evaluate the same `lfoShapeSample` at the same phase,
     * and a fill sampled per column is exactly what a stroke drawn as segments
     * encloses. */
    fillCurveMass(ctx, x0, xEnd - 1, yAt, baseY, topY, botY);

    /* A ramp is straight between its breakpoints, so draw it as real segments;
     * everything else goes per column, because a coarse uniform polyline turns
     * a wave into a different shape. See drawStepCurve. */
    if (LFO_LINEAR_BREAKPOINTS[shape]) {
        const yOf = (raw) => {
            const sample = raw * depthSign;
            return Math.round(baseY - (unipolar ? (sample + 1) / 2 : sample) * amp);
        };
        drawLinearWave(ctx, x0, xEnd - 1, shape, cycles, phase, yOf, 1);
    } else {
        drawStepCurve(ctx, x0, xEnd - 1, yAt, 1);
    }
}

/**
 * schwung-movy renderer/lfo-wave.ts drawWave, ported: the single-knob
 * silhouette. One column per pixel plus a vertical connector to the previous
 * column — a plain Bresenham diagonal reads as slanted steps once the box is
 * this short, so square/pulse edges need the straight riser this gives them.
 *
 * It used to close the cycle afterwards by drawing a connector from the last
 * sample back to the first, at BOTH ends of the box. For a shape that ends
 * where it began (sine, triangle) that was a stub; for one that does not, it
 * was a full-height bar down each side — a saw came out as a ramp inside a
 * box frame, and a square as a rectangle outline. Neither edge is real: the
 * window shows one cycle, and any discontinuity INSIDE it is already drawn by
 * the riser in the loop. A saw simply ramps and stops, which is what a saw
 * looks like.
 */
function drawWaveCell(ctx, x, y, w, h, shape, cycles, morphFrom = null, morphT = 1) {
    const mid = y + (h - 1) / 2, amp = (h - 1) / 2;
    /*
     * ONE CLOSURE, AND THE MORPH LIVES INSIDE IT.
     *
     * The stroke below, the CHECKER mass through fillCurveMass, and the parity
     * assertions in test_param_pages_movy.sh all derive from `yAt` — that is
     * the whole reason the fill and the line cannot disagree about where the
     * curve is. Blending the two shapes at the SAMPLE, before the closure, keeps
     * that property for free; computing the morph a second time anywhere else
     * would break it silently and only at intermediate frames, which is the
     * hardest kind of wrong picture to notice.
     *
     * With `morphFrom` null this is byte-for-byte the expression it replaced.
     */
    const sampleAt = (px) => {
        const ph = ((px - x) / w) * cycles;
        const to = lfoShapeSample(shape, ph);
        return morphFrom === null ? to : lerp(lfoShapeSample(morphFrom, ph), to, morphT);
    };
    const yAt = (px) => Math.round(mid - sampleAt(px) * amp);
    const vline = (px, a, b) => ctx.fillRect(px, Math.min(a, b), 1, Math.abs(a - b) + 1, 1);

    /*
     * The same CHECKER mass the four curve graphs carry, about the same zero
     * line, through the same helper and the same `yAt` closure the stroke uses.
     *
     * This cell is a SHAPE SILHOUETTE — it says which LFO waveform is selected,
     * not what a value is — and it was left plain when the graphs took the fill,
     * on the reasoning that those are two different jobs. On a page they are two
     * different jobs drawn in the same band, next to each other, and the odd one
     * out reads as the one that did not get finished.
     *
     * Bipolar, so the fill is signed about the centre: a trough fills downward.
     * Before the stroke, because the stroke is solid and the fill is not.
     */
    fillCurveMass(ctx, x, x + w, yAt, Math.round(mid), y, y + h - 1);

    let py = yAt(x);
    vline(x, py, py);
    for (let px = x + 1; px < x + w; px++) {
        const ny = yAt(px);
        /* Draw only the rows this column NEWLY occupies. Spanning py..ny
         * inclusive re-draws py, which the previous column already covered, so
         * every step came out two columns wide and a shallow ramp read as a
         * chunky zigzag instead of a line.
         *
         * An EDGE is the exception and keeps py — see EDGE_ROWS. The previous
         * column covered py at px-1, not at px, so on a hard vertical the riser
         * met the rail only diagonally and the corner was chamfered. */
        const edge = Math.abs(ny - py) >= EDGE_ROWS;
        if (ny === py) vline(px, ny, ny);
        else if (ny < py) vline(px, ny, edge ? py : py - 1);
        else vline(px, edge ? py : py + 1, ny);
        py = ny;
    }
}

/**
 * schwung-movy renderer/knob.ts drawWaveCell, ported: the silhouette spans the
 * whole knob box (KW=16, 2px inset each side) — no frame, since resolution at
 * this size is the entire point (a stepped shape only reads as stepped when
 * its levels are more than a pixel apart).
 */
/*
 * ~100ms, THREE OR FOUR FRAMES, and short is the requirement rather than a
 * performance concession.
 *
 * A morph passes through curves that are neither shape — halfway between a
 * square and a saw is a thing with a step in it — and anything on screen long
 * enough to be read as a value WILL be read as one. A slow morph does not look
 * like a transition, it looks like a third waveform in the list. The failure
 * mode of too-fast is that the morph is missed; the failure mode of too-slow is
 * that it lies.
 */
const WAVE_MORPH_MS = 100;

/*
 * A STEP OF THIS MANY ROWS IS AN EDGE, AND AN EDGE KEEPS ITS CORNER.
 *
 * The stroke draws only the rows a column NEWLY occupies, which is what makes a
 * shallow ramp a true 1px staircase instead of a two-column-wide zigzag. At a
 * 1-row step the omitted pixel IS the staircase and must stay omitted.
 *
 * At a 12-row step it is a nick out of a hard vertical edge: the square's
 * falling edge started one row BELOW the top it falls from, so the corner was
 * chamfered and the top rail looked like it stopped a pixel early. Reported
 * from the device as the square missing a pixel at its first turn — and it
 * was, in the sense that matters, though nothing was missing from the top rail
 * itself.
 *
 * TWO is the threshold and it is measured, not chosen for tidiness. At the
 * shipped 24 drawn columns the steps are: square [12], triangle all 1s, saw all
 * 1s, sine 1s with three 2s. So this rule adds exactly ONE pixel to the square
 * and three to the sine, where it is invisible — rendered both ways to confirm
 * that rather than assumed. Dropping to 1 would add 19 pixels to the sine and
 * 23 to the triangle and bring the zigzag back on every shape.
 */
const EDGE_ROWS = 2;

/*
 * THE TRIANGLE IS DRAWN AT A WIDTH ITS SLOPE DIVIDES, NOT AT THE CELL'S.
 *
 * What reads as jagged is not the step SIZE, it is the step size CHANGING. A
 * triangle traverses 4*amp = 24 rows per cycle, so its staircase is uniform
 * only when the drawn width is a multiple of 24. At the grid cell's 28 drawn
 * columns it gets 20 steps of 1 and 4 of 2; at 24 it is 24 steps of 1, and the
 * apex stops being a plateau.
 *
 * Reported from the device as the triangle looking "wrong", and worth stating
 * plainly that it was never the morph: a settled triangle is byte-identical to
 * the last frame of a morph into one, so the jag was there with animation off.
 *
 * A TABLE OF ONE, AND THE ENTRY THAT IS NOT IN IT IS THE FINDING. The saw
 * traverses 2*amp per cycle and looks like the obvious second entry — it was,
 * until the widths were measured against the cells that actually exist rather
 * than against a round number:
 *
 *   drawn 28 (grid, cell 32)   saw {1:1, 2:9, 3:3}  ->  quantized {1:1, 2:10, 3:1}
 *   drawn 25 (knob card, 29)   saw {1:1, 2:12}      ->  quantized {1:1, 2:10, 3:1}
 *
 * A mild gain at one width and a LOSS at the other, for 4px of width each
 * time. The triangle is exactly uniform at every width tested, which is the
 * difference between a rule and a coincidence. Sine has no constant slope for a
 * quantum to fix and square has no slope at all, so neither was ever a
 * candidate.
 *
 * (The first pass had the saw in, on numbers taken against the 30px cell — a
 * width with no pad subtracted and no cell of that size in the tree.)
 */
const WAVE_QUANTUM_ROWS = { 1: 4 };   /* shape id -> cycle travel, in units of `amp` */

export function drawWaveform(ctx, rect, key, values, metaIndex, anim, nowMs) {
    const name = optionText(metaIndex, key, values);
    const shape = lfoShapeIdOf(name);
    const pad = 2;
    const x = rect.x + pad, w = rect.w - pad * 2;
    const y = rect.y + 1, h = VIZ_ROWS;

    let morphFrom = null, morphT = 1;
    if (anim && typeof nowMs === "number") {
        /*
         * TAGGED "s2", NEVER THE BARE NUMBER 2.
         *
         * observe() re-bases a NUMERIC value to where it visually sits when it
         * is retargeted mid-flight, which is right for a slug and catastrophic
         * for a shape id: a fast scroll would hand back 2.4, lfoShapeSample
         * falls through its default at anything unrecognised, and the cell
         * would morph out of a SINE that was never on screen. A non-numeric
         * token makes that re-base return the previous shape untouched, so a
         * retarget morphs from the shape it was heading to — the last thing
         * actually drawn — which at 100ms is at most one frame stale.
         */
        /* The RAW value, not the shape id: `lfoShapeIdOf` falls through to a
         * default for anything it does not recognise, so an unread key resolves
         * to a perfectly ordinary shape and the morph out of it looks exactly
         * like a real one. See observeLanded. */
        const tr = observeLanded(anim, "wave:" + key, values ? values[key] : undefined,
                                 "s" + shape, nowMs, WAVE_MORPH_MS);
        if (tr.moving && typeof tr.from === "string") {
            const f = Number(tr.from.slice(1));
            if (Number.isFinite(f) && f !== shape) { morphFrom = f; morphT = easeOut(tr.t); }
        }
    }

    /*
     * Centred, and floored at one quantum so a cell too narrow to hold a whole
     * one keeps its full width rather than collapsing to nothing. Snapping DOWN
     * costs at most q-1 columns — 6 of 30 for a triangle, invisible in context
     * because the cell already carries 2px of pad on each side.
     *
     * KEYED ON THE DESTINATION SHAPE, INCLUDING MID-MORPH. `shape` is already
     * the destination on the morph's first frame, so the width is constant for
     * the whole blend and changes only at the instant the value does — when
     * every pixel in the cell is changing anyway. Skipping the quantum while
     * morphing looks like the more conservative choice and is the opposite: it
     * would snap the width when the morph settled, which is a second animation
     * nobody asked for, arriving after the one they did.
     */
    let qx = x, qw = w;
    const q = (WAVE_QUANTUM_ROWS[shape] || 0) * ((h - 1) / 2);
    if (q > 0 && w >= q) {
        qw = Math.floor(w / q) * q;
        qx = x + Math.floor((w - qw) / 2);
    }
    if (qw > 0) drawWaveCell(ctx, qx, y, qw, h, shape, 1, morphFrom, morphT);
}

/* --------------------------------------------------------------------- eq */

const shelfLow = (u) => 1 / (1 + Math.exp((u - 0.28) * 11));
const shelfHigh = (u) => 1 / (1 + Math.exp((0.72 - u) * 11));
const bellMid = (u) => Math.exp(-(((u - 0.5) / 0.20) ** 2));
const EQ_WEIGHT = { low: shelfLow, mid: bellMid, high: shelfHigh };

/**
 * schwung-movy renderer/eq-curve.ts drawEqCurve, ported. gains are signed
 * -1..1 (a band's raw dB value normalized against its own declared range).
 */
export function drawEq(ctx, rect, roles, values, metaIndex) {
    const bands = ["low", "mid", "high"].filter((r) => roles[r]);
    if (bands.length === 0) return;

    const x0 = rect.x, xEnd = rect.x + rect.w;
    const { topY, botY, midY, amp } = band(rect);
    const spanW = xEnd - x0;

    dottedH(ctx, x0, xEnd - 1, midY);

    const gains = bands.map((b) => {
        const meta = metaIndex.getOrGuess(roles[b]);
        const raw = Number(values ? values[roles[b]] : 0) || 0;
        const range = Math.max(Math.abs(meta.min || -1), Math.abs(meta.max || 1)) || 1;
        return clamp01((raw / range + 1) / 2) * 2 - 1;
    });
    const gainAt = (u) => {
        let v = 0;
        bands.forEach((b, i) => { v += gains[i] * EQ_WEIGHT[b](u); });
        return Math.max(-1, Math.min(1, v));
    };
    const yAt = (px) => Math.round(midY - gainAt((px - x0) / spanW) * amp);

    drawColumnCurve(ctx, x0, xEnd - 1, yAt, 1);
}

/* ------------------------------------------------------------------ fader */

/**
 * SCH-50 `outline-fill`. Movy's construction was dashed rails, a 3px solid
 * column and a 1px head; the fill is now a BOX — 7px wide, 1px frame, a
 * DIAG_HEAVY (75%) interior, corners notched — standing on the same rails.
 *
 * The strongest fader result in the catalog, 4-0.
 *
 * THE ACCEPTED COLLISION. This borrows the page's own box vocabulary instead of
 * the mixer's, so a fader and an enum square stop being separable by
 * SILHOUETTE — both are now notched framed boxes. Its own note predicted that
 * before any judging, and picking `thin-frame` for the enum square made it live
 * rather than hypothetical: an osirus page renders four framed faders beside a
 * framed MODE/POLY cell.
 *
 * Taken deliberately, with the escape available and declined — a frameless enum
 * square was the runner-up in that set and would have dissolved it. On a page
 * mixing continuous faders with enums the two are distinguishable by CONTENT,
 * not by shape. If that reads badly on hardware the cheap fix is the enum
 * square, not this.
 *
 * REJECTED: `stepped`, six 2-row blocks over the full range, which stays
 * distinct from everything on the page. Declined for what its own note already
 * said — five detents in six move nothing on screen, which is fine for a level
 * grabbed roughly and bad for anything nudged.
 */
export function drawFader(ctx, rect, key, values, metaIndex, baseValues) {
    const meta = metaIndex.getOrGuess(key);
    const normVal = fractionOf(meta, values ? values[key] : undefined);
    const { topY: top, botY: bot } = band(rect); const h = bot - top;
    const cx = Math.round(rect.x + rect.w / 2);

    dashedVRule(ctx, cx - 4, top, h + 1, 1, 1);
    dashedVRule(ctx, cx + 4, top, h + 1, 1, 1);

    /*
     * THE INTERIOR LATTICE IS PHASED BY THE SUB-ROW REMAINDER.
     *
     * The bar is 7px wide in a 13-row band, so a 128-step parameter gets about
     * ten detents per row of travel and NINE IN TEN MOVE NOTHING — measured at
     * 12 of 127 over a full sweep, 13 distinct pictures out of 128. That is the
     * fault the catalog rejected `stepped` for ("five detents in six move
     * nothing on screen"), which the shipped fader turns out to have had worse,
     * and nothing in the still frames the catalog was judged from could show it.
     *
     * `exact` is the fill height before it is rounded to a row, and the
     * fraction thrown away by that rounding is what the phase carries.
     * DIAG_HEAVY has a period of 4, so four phases sit between one row and the
     * next: a detent too small to move the boundary still moves the texture.
     * Measured, that takes 12 of 127 to 44 of 127.
     *
     * It costs nothing to compute — the phase is a function of the value the
     * cell already has, so there is no extra parameter read, and a read is
     * ~2.8ms against a 1.68ms whole-page render.
     *
     * SUBTRACTED, not added, so a rising value shifts the lattice the way the
     * boundary is heading. Adding it reads as the texture sliding DOWN while
     * the bar grows up, which looks like a defect rather than a finer scale.
     *
     * KNOWINGLY BREAKS THE ABSOLUTE-COORDINATE RULE, which exists so a moving
     * shape does not shimmer as its fill re-phases underneath it. Here the
     * re-phasing IS the signal. The cost is that two faders side by side no
     * longer share one lattice; they are separate cells with a rail and a gap
     * between them, so there is no seam for the mismatch to show at.
     */
    const exact = clamp01(normVal) * h;
    const frac = exact - Math.floor(exact);
    const phase = Math.floor(frac * 4) % 4;
    const pattern = (px, py) => ((((px + py - phase) % 4) + 4) % 4) !== 0;

    const y = Math.round(bot - exact);
    const bh = bot - y + 1, bx = cx - 3, bw = 7;
    ctx.fillRect(bx, y, bw, 1, 1);
    ctx.fillRect(bx, bot, bw, 1, 1);
    ctx.fillRect(bx, y, 1, bh, 1);
    ctx.fillRect(bx + bw - 1, y, 1, bh, 1);
    /* At very low values there is no interior left, so it degrades to a 7x2 bar
     * rather than to a frame with a hole punched in it — and the notch is
     * skipped with it, because notching a 2-row box eats half of it. */
    if (bh >= 3) {
        fillDithered(ctx, bx + 1, y + 1, bw - 2, bh - 2, pattern);
        notchCorners(ctx, bx, y, bw, bh);
    }

    /*
     * THE BASE MARK, for a fader a source is driving.
     *
     * Same rule as the sample cell: a viz group COVERS its cells, so
     * drawKnobWidget never runs for them and the modulation dot -- the only
     * thing that says where you SET the value as opposed to where it is right
     * now -- has no way onto the screen. The bar tracks the effective value,
     * so without this a modulated fader is a bar moving on its own with the
     * base nowhere to be seen.
     *
     * OUTSIDE THE RAILS, which is what makes it collision-proof rather than
     * merely tidy. The bar spans cx-3..cx+3 and the dashed rails sit at cx+-4,
     * so cx+-5..6 is the only part of the cell nothing else ever draws in. A
     * mark inside the bar would have to fight the dithered lattice -- whose
     * whole point is that it re-phases as the value moves -- and would be
     * unreadable exactly when the value is moving, which is the only time this
     * mark exists.
     *
     * Two stubs rather than a rule across the cell, matching drawSample: the
     * fill boundary is already a full-width horizontal edge, so a second one
     * would read as a second bar top.
     */
    if (baseValues) {
        const baseNorm = fractionOf(meta, baseValues[key]);
        if (baseNorm !== undefined && baseNorm !== null && !Number.isNaN(baseNorm)) {
            const byy = Math.round(bot - clamp01(baseNorm) * h);
            if (byy >= top && byy <= bot) {
                ctx.fillRect(cx - 6, byy, 2, 1, 1);
                ctx.fillRect(cx + 5, byy, 2, 1, 1);
            }
        }
    }
}

/* ----------------------------------------------------------------- switch */

/*
 * SCH-50 `pill-inverted`. Replaces Movy's tabulated rounded-rectangle sprite.
 *
 * Won its set 5-0 — but the interesting result is second place. `movy-sprite`,
 * the pixel-for-pixel port and the most directly-derived widget in the whole
 * fleet, ALSO scored 5-0 and lost by 0.05 log-strength. On the one control where
 * the resemblance is most concrete, the incumbent is genuinely well-liked. That
 * this option won anyway is what makes the differentiation here free: it is
 * independently authored, it beat the port on its own merits, and no legibility
 * was traded to get there.
 *
 * THE TRACK CARRIES THE STATE, NOT THE SLUG. ON fills the whole track and knocks
 * the slug out of it; OFF leaves the track an empty frame with a solid slug. So
 * the two states differ by most of the widget's AREA rather than by the position
 * of a 9px block, and the cell stays legible at a distance where a slug-only
 * pill is a grey lozenge with a bump.
 *
 * ACCEPTED COST: ON is a dark cell, so a page with several switches on is a row
 * of black blocks.
 *
 * THE 3px INSET IS THE FIX THAT MADE IT WORK AT ALL. Seated one pixel from the
 * wall the slug is 8-connected to it on the row it sits on and the two merge:
 * OFF stopped reading as "a block parked at one end of a track" and started
 * reading as "the left half of this box is thick" — the same picture at both
 * seats, and therefore no switch. Three pixels leaves a clear column at the
 * outer end and the slug is visibly a separate object at both ends of travel.
 */
/*
 * 16 x 9, DOWN FROM 24 x 11, and the inversion is what paid for it.
 *
 * At 24 x 11 this was the heaviest object on a page — wider than the enum
 * square beside it and, in the ON state, a solid block competing with the
 * inverted label strip a held knob puts directly under it. That is a lot of
 * screen for a value with two states.
 *
 * Shrinking a slug-slides-along-a-track switch is risky, because the whole
 * signal is WHERE the slug sits and a shorter track moves it less. This one
 * does not rely on that: ON fills the track and knocks the slug out of it, so
 * the two states differ in INK as well as in position and are separable from
 * across the room at any size. Judged with both states on one page, which is
 * the only view that can answer it — a render of one state cannot.
 *
 * The 2px inset is the floor, not a preference. At 1px the slug is 8-connected
 * to the wall on its own row and the two merge: OFF stops reading as a block
 * parked at one end of a track and starts reading as "the left half of this box
 * is thick", which is the same picture at both seats and therefore no switch at
 * all. That defect had to be fixed once already; 2px keeps a clear column at
 * the outer end at both ends of travel.
 */
const PILL_H = 9, SLUG_W = 5, SLUG_H = 5, SLUG_INSET = 2;

/*
 * THE SWITCH DOES NOT ANIMATE. IT TOGGLES.
 *
 * Two things used to move here, and both are gone. The slug interpolated
 * between its seats over 120ms — about 7px of journey spread over seven
 * frames, which is a smear rather than a movement, and it made the switch look
 * like it was deciding. That went first, on the argument that the FILL was the
 * thing carrying the transition.
 *
 * The fill has now gone the same way, for the reason that outranks the one
 * that kept it: on hardware it is DISTRACTING. A switch is the control you
 * flip most often and least deliberately, and 160ms of the cell inverting
 * under your hand is motion in the corner of the eye every single time —
 * exactly the wrong place to spend attention. Reported from the device as
 * "distracting"; no calibration of the duration fixes a thing that should not
 * be moving.
 *
 * Nothing is lost by it. The two states already differ by most of the widget's
 * AREA (see the pill-inverted note above), so a flip is the loudest possible
 * change even when it happens between two frames. The transition never had to
 * be readable; the STATE does, and it is.
 *
 * `anim` and `nowMs` stay in the signature because drawVizGroup hands every
 * widget the same arguments. They are deliberately unused.
 */
export function drawSwitch(ctx, rect, key, values, metaIndex, _anim, _nowMs) {
    const raw = values ? values[key] : undefined;
    /* Resolves a name-reporting plugin's value too — see enumIndexOf.
     *
     * A bare Number(raw) reads NaN for the "Off"/"On" spelling (and for
     * "No"/"Yes", "Disabled"/"Enabled" — every non-numeric pair detectSwitch
     * accepts), so the knob stayed pinned to the OFF seat no matter what the
     * module reported: the switch drew, but it never moved. The rest of this
     * file already goes through enumIndexOf for exactly this reason; the
     * metaIndex needed for it was already being passed to us and dropped. */
    const meta = metaIndex ? metaIndex.getOrGuess(key) : null;
    const idx = meta ? enumIndexOf(meta, raw) : Math.round(Number(raw));
    const on = idx === 1;

    const { topY } = band(rect);
    const cx = Math.round(rect.x + rect.w / 2);
    /* Capped at 16 so the pill never fills a wide cell edge to edge, and
     * floored against the rect so a narrow one still gets a track. */
    const w = Math.min(16, rect.w - 4);
    const x = cx - (w >> 1), y = topY + 1, h = PILL_H;
    const seatOff = x + SLUG_INSET, seatOn = x + w - SLUG_INSET - SLUG_W;
    const sy = y + SLUG_INSET;

    const sx = on ? seatOn : seatOff;

    /*
     * ON is the track filled WALL TO WALL — not up to the slug's trailing
     * edge, which is what a literal "fill up to the slug" rule does and which
     * left the last two columns of an ON switch drawn as outline. OFF is the
     * same rect as an empty frame: two rails and two walls.
     */
    if (on) {
        ctx.fillRect(x, y, w, h, 1);
    } else {
        ctx.fillRect(x, y, w, 1, 1);                         /* top rail */
        ctx.fillRect(x, y + h - 1, w, 1, 1);                 /* bottom rail */
        ctx.fillRect(x, y, 1, h, 1);                         /* left wall */
        ctx.fillRect(x + w - 1, y, 1, h, 1);                 /* right wall */
    }
    notchCorners(ctx, x, y, w, h);

    /*
     * The slug takes the colour of the ground it is standing on. ON the ground
     * is solid so the slug is a knockout; OFF it is empty so the slug is ink.
     * That inversion is the point of the widget: the two states differ by most
     * of the cell's area rather than by where a 5px block sits.
     */
    ctx.fillRect(sx, sy, SLUG_W, SLUG_H, on ? 0 : 1);
    /* The slug's own corners, in whichever direction softens them against the
     * ground each corner sits on. `notchCorners` clears, which rounds a solid
     * slug; a knockout needs the reverse, four SET pixels, or the hole is the
     * only square-cornered shape on a page where every filled box is softened. */
    for (const [px, py] of [[sx, sy], [sx + SLUG_W - 1, sy],
                            [sx, sy + SLUG_H - 1], [sx + SLUG_W - 1, sy + SLUG_H - 1]]) {
        ctx.fillRect(px, py, 1, 1, on ? 1 : 0);
    }
}

/* ----------------------------------------------------------------- sample */

/**
 * schwung-movy renderer/wav-form.ts drawWavForm, ported. The position marker
 * is the envelope's COMPLEMENT in its own column — inverted rather than drawn
 * as a separate line — which stays the highest-contrast thing in the column
 * whether the sample is loud or quiet there.
 *
 * The envelope is the file's real peaks, decoded by wav_peaks.mjs and advanced
 * from the tick. When there are none, there is no envelope — see below.
 */
export function drawSample(ctx, rect, roles, values, metaIndex, baseValues) {
    const x0 = rect.x, w = rect.w;
    const { topY, botY, midY, amp } = band(rect);

    /*
     * REAL PEAKS OR NOTHING. There is no representative shape.
     *
     * There used to be one: `sin(t*PI) * (0.55 + 0.35*sin(t*23))`, a
     * waveform-shaped thing drawn whenever the peaks were missing. It was
     * justified as degrading gracefully — the cursor and brackets are still
     * true, so why blank the cell — and that reasoning is wrong in the way
     * this codebase has a standing rule about: a read that did not produce an
     * answer must never produce a PICTURE. The rule is written down for
     * parameter reads (see the tri-state contract) and it is the same rule
     * here.
     *
     * What it cost: granny declares `sample_path` in its hierarchy but puts it
     * on no knob, so the sample cell had no file to point at on ANY page and
     * drew the synthetic envelope every time — a picture of a sample that was
     * never loaded, on the flagship granular module. Reported from the device
     * as "no sample was loaded, not sure why it was showing a waveform".
     *
     * A missing envelope now draws the baseline only (halfAt = 0 lights one
     * pixel per column), which is honest and still carries the cursor and the
     * brackets. It also self-corrects: wavPeaks fills in progressively, so the
     * flicker the fallback existed to hide is a few ticks of a centre line.
     *
     * Normalised against the running PEAK, not against full scale, so a quiet
     * sample still uses the full height of the cell. Guarded, because peak is
     * 0 until the first block lands.
     *
     * No I/O here: wavPeaks never reads, and the job is advanced from the tick.
     */
    const file = roles.value && values ? values[roles.value] : null;
    const pk = file ? wavPeaks(String(file)) : null;
    const pts = (pk && !pk.error && pk.points.length) ? resamplePeaks(pk.points, w) : null;
    const scale = (pk && pk.peak > 0) ? 1 / pk.peak : 1;
    const halfAt = (i) => {
        if (!pts) return 0;
        return Math.round(clamp01(pts[Math.min(pts.length - 1, i)] * scale) * amp);
    };
    /*
     * SCH-50 `ghost-fill`, and this is the set where the AXIS IS INVERTED: the
     * waveform already drew SOLID, so `solid-mass` was the incumbent here while
     * being the radical option in the other three. It was the clearest of the
     * candidates at both loud and quiet gain and it was still declined —
     * choosing it would have left the sample cell unchanged while its three
     * siblings all changed, which is the opposite of the coherence this pick
     * exists for. An envelope, a filter, an LFO and a sample now read as the
     * same kind of object.
     *
     * `terrain` was the option this case eliminated, exactly as its own note
     * warned: it fills to the BOTTOM EDGE, so on a MIRRORED graph the quieter
     * the material the MORE of the cell it covers. At 28% gain it was still a
     * hatched slab with the waveform gone entirely. That is backwards, and
     * quiet material is the common case for samples rather than an edge case —
     * it took rendering the comparison at two gains to see it at all.
     */
    const crestAt = (px) => midY - halfAt(px - x0);
    const troughAt = (px) => midY + halfAt(px - x0);
    fillCurveMass(ctx, x0, x0 + w, crestAt, midY, topY, botY, troughAt);
    /* Both flanks stroked. `drawStepCurve` rather than a pixel per column: at a
     * steep transient adjacent columns differ by several rows, and a bare pixel
     * each would read as a dotted outline rather than as the edge of a body. */
    drawStepCurve(ctx, x0, x0 + w, crestAt, 1);
    drawStepCurve(ctx, x0, x0 + w, troughAt, 1);

    /*
     * NO FILE still draws the MARKERS.
     *
     * This used to return early, on the reasoning that a cursor with no file
     * is a playhead pointing into nothing. That is true of the playhead and
     * false of the widget: the empty track, its cursor and its spray fences
     * are the picture of two controls that still exist and are still yours to
     * set. "When no sample is loaded it should be the empty two column
     * widget."
     *
     * The emptiness is reported where it belongs — on the file's own cell,
     * which reads NONE (see displayValue in render_page_movy.mjs). It is not
     * written across the graphic, because centred on the graphic is over the
     * SPRAY cell: "why is spray showing empty? Sample file should be empty."
     */

    /* Column i covers frames [i/w, (i+1)/w), so a marker belongs in
     * floor(p*w). The obvious round(p*(w-1)) disagrees for a quarter of all
     * positions and lands a pixel off the column that will actually play. */
    const colOf = (p) => Math.min(w - 1, Math.floor(clamp01(p) * w));
    const posIn = (src, role) => {
        const k = roles[role];
        if (!k || !src || src[k] === undefined || src[k] === null) return undefined;
        return clamp01(fractionOf(metaIndex.getOrGuess(k), src[k]));
    };
    const posOf = (role) => posIn(values, role);

    /*
     * LOOP BOUNDS FIRST, so the playback cursor draws on top of them — the
     * cursor is the thing that moves and the thing you are usually looking
     * for, and a bound sitting on the same column would otherwise hide it
     * exactly when the two matter most.
     *
     * Tips point INWARD, at the region that repeats. That is how you tell a
     * start from an end with no room for a label, and it is invisible in code
     * review: reversing `dir` still draws two brackets and still satisfies any
     * "are there brackets" check, while reading as a loop that excludes the
     * part it actually plays. test_viz_sample.sh pins the tip COLUMNS.
     */
    const bracket = (p, opening) => {
        if (p === undefined) return;
        const bx = x0 + colOf(p);
        ctx.fillRect(bx, topY, 1, botY - topY + 1, 1);          /* the stem */
        const tipX = bx + (opening ? 1 : -1);
        if (tipX >= x0 && tipX < x0 + w) {
            ctx.fillRect(tipX, topY, 1, 2, 1);
            ctx.fillRect(tipX, botY - 1, 1, 2, 1);
        }
    };
    bracket(posOf("loopStart"), true);
    bracket(posOf("loopEnd"), false);

    const pos = posOf("position");

    /*
     * GRANULAR SPREAD: the region grains are actually drawn from, as a dotted
     * fence either side of the cursor. Dotted rather than solid so it reads as
     * a boundary the cursor may wander past, not as a second cursor.
     *
     * Two behaviours copied from granny's engine rather than guessed:
     *   max_offset = spray * (sample_len - 1)   -> the WHOLE file, not a window
     *   start_idx wraps into [0, len)           -> so the fence wraps too
     * and because the offset is symmetric, ±0.5 already reaches every frame:
     * past that the region cannot grow, so the fences stop at the file edges
     * instead of drifting on and implying a spread the DSP never applies.
     */
    const spray = posOf("spray");
    /* `spray > 0` is inert in every reachable case -- at 0 both fences land on
     * the cursor column and the cursor, drawn last as a solid full-height
     * complement, overwrites them. Kept for intent and to skip the work; the
     * mutation that removes it is an EQUIVALENT mutant, not an untested gap. */
    if (pos !== undefined && spray !== undefined && spray > 0) {
        const wrap = (f) => f - Math.floor(f);
        const full = spray >= 0.5;
        for (const side of [-1, 1]) {
            const at = full ? (side < 0 ? 0 : 1 - 1 / w) : wrap(pos + side * spray);
            const fx = x0 + colOf(at);
            const fh = halfAt(fx - x0);
            for (let yy = topY; yy <= botY; yy++) {
                if (((yy + fx) & 1) !== 0) continue;
                /* Inside the waveform body the fence must be CUT, not added: a
                 * lit pixel over a lit body is invisible. Same complement
                 * technique the cursor uses just below. */
                const inWave = yy >= midY - fh && yy <= midY + fh;
                ctx.fillRect(fx, yy, 1, 1, inWave ? 0 : 1);
            }
        }
    }

    /*
     * The cursor is the envelope's COMPLEMENT in its own column: the sample is
     * cleared there and the space around it is lit. That inverts it over the
     * waveform without ever reading the framebuffer back — and it is
     * self-correcting, which is the point. Through a quiet passage it is a tall
     * bright line; through a loud one it becomes a dark notch cut into the
     * body. Either way it is the highest-contrast thing in the column.
     */
    /*
     * THE BASE MARK: where the knob is SET, when a source is moving it.
     *
     * A graphic COVERS its cells, so `drawKnobWidget` never runs for them and
     * the modulation dot -- the only thing that says "this is yours, that is
     * the LFO" -- has no way onto the screen. That was invisible until a lone
     * `wav_position` started forming a one-cell sample group (5f5fb11c): the
     * cell had been an ordinary knob, with a pointer at the base and a dot at
     * the effective value, and it silently became a waveform whose cursor
     * tracks the effective value with the base nowhere on screen. Reported as
     * "we lost the knob LFO indicator", against mrdrums' Sample Start.
     *
     * So the graphic carries what the widget it replaced carried. The cursor
     * is already the live value; this is its base, and the two together say
     * the same thing the pointer and the dot said.
     *
     * Drawn as STUBS at the band edges rather than a full-height line, and
     * that is the whole design: the cursor is full height and solid, the spray
     * fences are full height and dotted, so a third full-height mark would be
     * a third thing competing at the same weight. Two 2px ticks read as an
     * annotation on the track instead.
     *
     * Same COMPLEMENT technique as the cursor -- a lit pixel over a lit body
     * is invisible, and at a loud peak the body reaches the band edge -- so
     * the stub is cut out of the waveform where it lands inside it.
     *
     * Before the cursor, so that where the LFO passes through its own base the
     * solid cursor wins the column. Nothing is lost by that: the two marks
     * coinciding IS the reading "the source is at the base right now", which
     * is exactly what the cursor alone shows.
     */
    const basePos = baseValues ? posIn(baseValues, "position") : undefined;
    if (basePos !== undefined && pos !== undefined && colOf(basePos) !== colOf(pos)) {
        const bi = colOf(basePos), bh = halfAt(bi), bx = x0 + bi;
        /*
         * A COARSE DASH -- 2 on, 2 off -- and the rhythm is the whole point.
         *
         * This column has to be told apart from the two other vertical marks
         * that can share the cell: the playback cursor is SOLID, and the spray
         * fences are a fine every-other-row dither. Two-on-two-off is legible
         * as a third rhythm in a 13px band, where a single stub pair at the
         * edges (the first cut) was four pixels and read as noise on the frame.
         * Reported from the device as wanting "a different line ... dotted with
         * large dots".
         *
         * Phased from topY, an ABSOLUTE coordinate, so the dash does not crawl
         * as the base moves between columns -- the same rule the fills follow,
         * and the opposite of the fader lattice, where re-phasing IS the
         * signal.
         *
         * Complemented against the waveform body like the cursor: a lit pixel
         * over a lit body is invisible, so inside the envelope the dash is cut
         * OUT of it instead.
         */
        for (let yy = topY; yy <= botY; yy++) {
            if (((yy - topY) & 3) >= 2) continue;
            const inWave = yy >= midY - bh && yy <= midY + bh;
            ctx.fillRect(bx, yy, 1, 1, inWave ? 0 : 1);
        }
    }

    if (pos !== undefined) {
        const mi = colOf(pos);
        const h = halfAt(mi), mx = x0 + mi;
        ctx.fillRect(mx, midY - h, 1, 2 * h + 1, 0);
        if (midY - h > topY) ctx.fillRect(mx, topY, 1, (midY - h) - topY, 1);
        if (midY + h < botY) ctx.fillRect(mx, midY + h + 1, 1, botY - (midY + h), 1);
    }
}

/* --------------------------------------------------------------- dispatch */

const DRAW = {
    [VIZ_ENVELOPE]: (ctx, rect, group, values, metaIndex) => drawEnvelope(ctx, rect, group.roles, values, metaIndex),
    [VIZ_FILTER]: (ctx, rect, group, values, metaIndex) => drawFilter(ctx, rect, group.roles, values, metaIndex),
    [VIZ_LFO]: (ctx, rect, group, values, metaIndex) => drawLfo(ctx, rect, group.roles, values, metaIndex),
    [VIZ_EQ]: (ctx, rect, group, values, metaIndex) => drawEq(ctx, rect, group.roles, values, metaIndex),
    [VIZ_WAVEFORM]: (ctx, rect, group, values, metaIndex, anim, nowMs) =>
        drawWaveform(ctx, rect, group.roles.value, values, metaIndex, anim, nowMs),
    [VIZ_FADER]: (ctx, rect, group, values, metaIndex, anim, nowMs, baseValues) =>
        drawFader(ctx, rect, group.roles.value, values, metaIndex, baseValues),
    [VIZ_SWITCH]: (ctx, rect, group, values, metaIndex, anim, nowMs) =>
        drawSwitch(ctx, rect, group.roles.value, values, metaIndex, anim, nowMs),
    [VIZ_SAMPLE]: (ctx, rect, group, values, metaIndex, anim, nowMs, baseValues) =>
        drawSample(ctx, rect, group.roles, values, metaIndex, baseValues),
};

/**
 * Draw a resolved group (from viz.resolveViz) into `rect`. Unknown kinds are
 * silently skipped, not thrown, so a future kind never crashes an old caller.
 *
 * `anim` (an anim_state.mjs store) and `nowMs` are OPTIONAL TRAILING
 * parameters, appended rather than folded into an options object because these
 * functions are called from several places and none of the existing parameters
 * may be reordered or removed. Omit both and every widget draws exactly the
 * pixels it always did — the animations are opt-in per caller, which is what
 * keeps the harness, the pinned baselines and the device unaffected.
 */
/*
 * `baseValues` is OPTIONAL and is only ever the BASE of a modulated key --
 * null when nothing on the page is modulated, which is the common case and
 * costs the callers nothing. Only drawSample reads it today (see its base
 * mark); the rest of the table ignores the argument, exactly as they already
 * ignore `anim`/`nowMs` when they do not animate.
 */
export function drawVizGroup(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues) {
    /*
     * A custom widget is tried first, and may decline -- an unregistered kind,
     * or one disabled by a previous throw.
     *
     * DECLINING DRAWS NOTHING FOR THIS FRAME, and that is correct rather than a
     * gap. The recovery is not here: it is in resolveViz, where a kind that has
     * become unavailable stops claiming its keys, so the very next resolve hands
     * them to the detector and the built-in draws over them. The alternative --
     * reaching into the DRAW table for a guessed fallback right here -- would
     * mean this function inventing a widget the resolver never chose.
     */
    if (isCustomKind(group.kind)) {
        drawCustom(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues);
        return;
    }
    const fn = DRAW[group.kind];
    if (fn) fn(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues);
}
