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

import { clamp01, fractionOf, line } from "./render_page.mjs";
import {
    VIZ_ENVELOPE, VIZ_FILTER, VIZ_LFO, VIZ_WAVEFORM, VIZ_FADER, VIZ_SWITCH, VIZ_EQ, VIZ_SAMPLE,
} from "./viz.mjs";
import { enumIndexOf } from "./param_meta.mjs";

/* -------------------------------------------------------------- primitives */

/* schwung-movy renderer/primitives.ts: dot / dottedV / dottedH. */
function dot(ctx, x, y) { ctx.fillRect(x, y, 2, 2, 1); }
function dottedV(ctx, x, y0, y1) {
    const lo = Math.min(y0, y1), hi = Math.max(y0, y1);
    for (let y = lo; y <= hi; y += 2) ctx.fillRect(x, y, 1, 1, 1);
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
 * Every other body scales horizontally against `rect.w`, but `drawSwitch` is a
 * tabulated sprite ported pixel-for-pixel from Movy — 26 columns wide, fixed,
 * because it is a circle and a circle rasterised at one size cannot be
 * stretched to another and stay round. Below 26 it does not narrow, it hangs
 * out of the cell on both sides.
 *
 * The full screen gives a 32px cell, so this only binds on a caller that
 * passes a narrower `rect` — see render_page.mjs, which stands the graphics
 * down rather than let one overhang.
 */
export const VIZ_MIN_W = 26;

function band(rect) {
    const topY = rect.y + 1;
    const botY = topY + VIZ_ROWS - 1;
    return { topY, botY, midY: topY + ((VIZ_ROWS - 1) >> 1), amp: (VIZ_ROWS - 1) / 2 };
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

    drawLine(ctx, x0, baseY, peakX, topY);            // attack rise
    drawLine(ctx, peakX, topY, sustStartX, susY);     // decay fall
    drawLine(ctx, sustStartX, susY, gateX, susY);     // sustain plateau
    drawLine(ctx, gateX, susY, relEndX, baseY);       // release fall

    dottedV(ctx, sustStartX, susY, baseY);
    dottedV(ctx, gateX, susY, baseY);

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
    const susY = has("sustain") ? baseY - Math.round(val.sustain * usableH) : baseY;

    /*
     * ATTACK IS NOT GUARANTEED.
     *
     * The rise was drawn unconditionally from `val.attack`, and an envelope
     * with no attack role makes that `undefined` -- so peakX is NaN, the NaN
     * reaches line()'s `for(;;)` and its equality break is never satisfied.
     * Not a wrong picture: a HANG, the same one CLAUDE.md records for a
     * partial GRID_GEOM freezing the shadow_ui tick.
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
    if (has("sustain")) {
        const plateauEnd = has("release") ? Math.round(leftX + span * 0.7) : rightX;
        if (plateauEnd > cur) { pts.push([plateauEnd, susY]); cur = plateauEnd; }
    }
    if (has("release")) {
        const endX = Math.min(rightX, cur + 4 + Math.round(val.release * span * 0.4));
        pts.push([endX, baseY]);
    }

    for (let i = 0; i < pts.length - 1; i++) drawLine(ctx, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]);
    for (const [px, py] of pts) dot(ctx, Math.min(xEnd - 2, Math.max(leftX, px - 1)), Math.max(topY, py - 1));
    if (has("sustain")) dottedV(ctx, Math.min(xEnd - 2, cur), susY, baseY);
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
 * is 3.6 and reads as a staircase). Elektron's own waveform glyphs look clean
 * because they are nearly square, not because they are drawn differently.
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
function drawWaveCell(ctx, x, y, w, h, shape, cycles) {
    const mid = y + (h - 1) / 2, amp = (h - 1) / 2;
    const yAt = (px) => Math.round(mid - lfoShapeSample(shape, ((px - x) / w) * cycles) * amp);
    const vline = (px, a, b) => ctx.fillRect(px, Math.min(a, b), 1, Math.abs(a - b) + 1, 1);

    let py = yAt(x);
    vline(x, py, py);
    for (let px = x + 1; px < x + w; px++) {
        const ny = yAt(px);
        /* Draw only the rows this column NEWLY occupies. Spanning py..ny
         * inclusive re-draws py, which the previous column already covered, so
         * every step came out two columns wide and a shallow ramp read as a
         * chunky zigzag instead of a line. Excluding py leaves a true 1px
         * staircase, and a steep step still gets its full riser. */
        if (ny === py) vline(px, ny, ny);
        else if (ny < py) vline(px, ny, py - 1);
        else vline(px, py + 1, ny);
        py = ny;
    }
}

/**
 * schwung-movy renderer/knob.ts drawWaveCell, ported: the silhouette spans the
 * whole knob box (KW=16, 2px inset each side) — no frame, since resolution at
 * this size is the entire point (a stepped shape only reads as stepped when
 * its levels are more than a pixel apart).
 */
export function drawWaveform(ctx, rect, key, values, metaIndex) {
    const name = optionText(metaIndex, key, values);
    const shape = lfoShapeIdOf(name);
    const pad = 2;
    const x = rect.x + pad, w = rect.w - pad * 2;
    const y = rect.y + 1, h = VIZ_ROWS;
    if (w > 0) drawWaveCell(ctx, x, y, w, h, shape, 1);
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

/** schwung-movy renderer/knob.ts drawFader, ported: dotted rails + a filled
 * column + a 1px head, always filling from the bottom. */
export function drawFader(ctx, rect, key, values, metaIndex) {
    const meta = metaIndex.getOrGuess(key);
    const normVal = fractionOf(meta, values ? values[key] : undefined);
    const { topY: top, botY: bot } = band(rect); const h = bot - top;
    const cx = rect.x + rect.w / 2;

    for (let y = top; y <= bot; y += 2) {
        ctx.fillRect(Math.round(cx - 4), y, 1, 1, 1);
        ctx.fillRect(Math.round(cx + 4), y, 1, 1, 1);
    }
    const y = Math.round(bot - clamp01(normVal) * h);
    if (y < bot) ctx.fillRect(Math.round(cx - 1), y, 3, bot - y + 1, 1);
    ctx.fillRect(Math.round(cx - 3), y, 7, 1, 1);
}

/* ----------------------------------------------------------------- switch */

/* schwung-movy renderer/knob.ts drawSwitch: one circle at three radii,
 * tabulated because it redraws every frame for every knob. */
const SW_X = [4, 2, 1, 1, 0, 0, 0, 1, 1, 2, 4];
const SW_W = [18, 22, 24, 24, 26, 26, 26, 24, 24, 22, 18];
const SW_IN_X = [3, 1, 1, 0, 0, 0, 1, 1, 3];
const SW_IN_W = [18, 22, 22, 24, 24, 24, 22, 22, 18];
const SW_KN_X = [3, 1, 1, 0, 0, 0, 1, 1, 3];
const SW_KN_W = [3, 7, 7, 9, 9, 9, 7, 7, 3];

export function drawSwitch(ctx, rect, key, values, metaIndex) {
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
    const kx = Math.round(rect.x + rect.w / 2 - 8), ky = rect.y;

    const x = kx - 5, y = ky + 2;
    for (let i = 0; i < 11; i++) ctx.fillRect(x + SW_X[i], y + i, SW_W[i], 1, 1);
    if (!on) for (let i = 0; i < 9; i++) ctx.fillRect(x + 1 + SW_IN_X[i], y + 1 + i, SW_IN_W[i], 1, 0);
    const seat = on ? x + 16 : x + 1;
    const v = on ? 0 : 1;
    for (let i = 0; i < 9; i++) ctx.fillRect(seat + SW_KN_X[i], y + 1 + i, SW_KN_W[i], 1, v);
}

/* ----------------------------------------------------------------- sample */

/**
 * schwung-movy renderer/wav-form.ts drawWavForm, ported. The position marker
 * is the envelope's COMPLEMENT in its own column — inverted rather than drawn
 * as a separate line — which stays the highest-contrast thing in the column
 * whether the sample is loud or quiet there.
 *
 * No decoded audio is available here (see docs/plans/2026-08-16-next-sessions.md
 * item 7 — WAV/AIFF parsing is its own, larger task), so `points` is a
 * representative envelope shape rather than the real waveform; the marker
 * technique itself is real Movy, not a placeholder.
 */
export function drawSample(ctx, rect, roles, values, metaIndex) {
    const x0 = rect.x, w = rect.w;
    const { topY, botY, midY, amp } = band(rect);

    const halfAt = (i) => {
        const t = i / Math.max(1, w);
        const v = Math.abs(Math.sin(t * Math.PI)) * (0.55 + 0.35 * Math.sin(t * 23));
        return Math.round(clamp01(v) * amp);
    };
    for (let i = 0; i < w; i++) {
        const h = halfAt(i);
        if (h <= 0) ctx.fillRect(x0 + i, midY, 1, 1, 1);
        else ctx.fillRect(x0 + i, midY - h, 1, 2 * h + 1, 1);
    }

    /* Column i covers frames [i/w, (i+1)/w), so a marker belongs in
     * floor(p*w). The obvious round(p*(w-1)) disagrees for a quarter of all
     * positions and lands a pixel off the column that will actually play. */
    const colOf = (p) => Math.min(w - 1, Math.floor(clamp01(p) * w));
    const posOf = (role) => {
        const k = roles[role];
        if (!k || !values || values[k] === undefined || values[k] === null) return undefined;
        return clamp01(fractionOf(metaIndex.getOrGuess(k), values[k]));
    };

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

    /*
     * The cursor is the envelope's COMPLEMENT in its own column: the sample is
     * cleared there and the space around it is lit. That inverts it over the
     * waveform without ever reading the framebuffer back — and it is
     * self-correcting, which is the point. Through a quiet passage it is a tall
     * bright line; through a loud one it becomes a dark notch cut into the
     * body. Either way it is the highest-contrast thing in the column.
     */
    const pos = posOf("position");
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
    [VIZ_WAVEFORM]: (ctx, rect, group, values, metaIndex) => drawWaveform(ctx, rect, group.roles.value, values, metaIndex),
    [VIZ_FADER]: (ctx, rect, group, values, metaIndex) => drawFader(ctx, rect, group.roles.value, values, metaIndex),
    [VIZ_SWITCH]: (ctx, rect, group, values, metaIndex) => drawSwitch(ctx, rect, group.roles.value, values, metaIndex),
    [VIZ_SAMPLE]: (ctx, rect, group, values, metaIndex) => drawSample(ctx, rect, group.roles, values, metaIndex),
};

/** Draw a resolved group (from viz.resolveViz) into `rect`. Unknown kinds are
 * silently skipped, not thrown, so a future kind never crashes an old caller. */
export function drawVizGroup(ctx, rect, group, values, metaIndex) {
    const fn = DRAW[group.kind];
    if (fn) fn(ctx, rect, group, values, metaIndex);
}
