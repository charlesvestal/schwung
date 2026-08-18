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
 * Measured on device: a bare fillRect/print binding costs roughly 90-100us —
 * not the few microseconds a 1-bit blit would suggest, because every one
 * crosses a QuickJS->C boundary (see render_page.mjs's own note on this). A
 * page with a graphic drawn one pixel at a time (Movy's own technique, which
 * its target hardware apparently affords) measured 27-45ms+ here, which is
 * exactly the "lower fps on fast knob turns" this was built to fix — Movy's
 * math changing every tick, redrawn every tick, at a cost per call this
 * device does not have to spare.
 *
 * schwung already has a NATIVE `draw_line(x0,y0,x1,y1,color)` binding
 * (src/host/js_display.c) — the whole Bresenham walk happens in C, so it
 * costs the SAME ~90-100us as a single fillRect regardless of the line's
 * length. That changes the right fix entirely: instead of degrading a curve
 * to fewer SAMPLE POINTS (tried, reverted — visible staircasing) or
 * coalescing pixel runs in JS (helps flat regions only, not a steep ramp,
 * which still costs one call per pixel), a curve is drawn as a small,
 * FIXED number of real connecting line segments between sample points —
 * true diagonals, not axis-aligned treads and risers, so it still looks
 * like a curve rather than stairs — each segment costing one call
 * regardless of its length or slope. `ctx.line` is optional: callers that
 * don't provide a native one (headless tests, a tool with no such binding)
 * fall back to a coalesced-run JS Bresenham that draws the identical
 * pixels at a real (if lesser) cost saving.
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
 * Sample a column-defined curve (filter/eq/lfo/waveform all compute one y
 * per x column) at a fixed, small number of points across [x0, xEnd) and
 * connect them with real line segments — smooth, not stepped, and its cost
 * is O(sample count), not O(width). `skipY` breaks the polyline rather than
 * drawing through a region that should read as absent (filter's floor).
 */
function drawColumnCurve(ctx, x0, xEnd, yAt, color = 1, skipY = null, samples = 28) {
    const w = xEnd - x0;
    if (w <= 0) return;
    const n = Math.max(2, Math.min(samples, Math.round(w)));
    let run = [];
    const flush = () => { if (run.length >= 2) drawPolyline(ctx, run, color); run = []; };
    for (let i = 0; i < n; i++) {
        const x = x0 + (w - 1) * (i / (n - 1));
        const y = yAt(Math.round(x));
        if (skipY && skipY(y)) { flush(); continue; }
        run.push([x, y]);
    }
    flush();
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
    const idx = Math.round(Number(raw));
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
    const present = ["attack", "decay", "sustain", "release"].filter((r) => roles[r]);
    if (present.length < 2) return;

    const x0 = rect.x, x1 = rect.x + rect.w;
    const topY = rect.y + 1, bodyBottom = rect.y + 14;   // Movy: rowY+1 .. rowY+14

    if (present.length === 4) {
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

    const pts = [[leftX, baseY]];
    const peakX = Math.min(rightX - 2, leftX + 4 + Math.round(val.attack * span * 0.4));
    pts.push([peakX, topY]);
    let cur = peakX;
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
    const topY = rect.y + 1, botY = topY + 13;
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
        case 4: { const steps = [0.3, -0.7, 0.85, -0.35]; return steps[Math.floor(ph * steps.length) % steps.length]; }
        case 6: return 1 - ph * 2;
        case 7: { const k = Math.floor(ph * 37); return ((((k * 2654435761) >>> 0) % 2000) / 1000) - 1; }
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
    return 0;
}

/**
 * schwung-movy renderer/lfo-wave.ts drawLfoWave, ported. Rate -> cycle
 * density, depth -> amplitude, mirroring Movy's `cycles`/`ampScale` fields.
 */
export function drawLfo(ctx, rect, roles, values, metaIndex) {
    const x0 = rect.x, xEnd = rect.x + rect.w;
    const topY = rect.y + 1, botY = topY + 13;
    const spanW = xEnd - x0;
    const baseY = Math.round((topY + botY) / 2);   // always bipolar here — no unipolar/mode role in our contract

    const shape = lfoShapeIdOf(optionText(metaIndex, roles.shape, values));
    const rateFrac = frac(metaIndex, roles.rate, values);
    const depthFrac = frac(metaIndex, roles.depth, values);
    const phase = roles.phase ? frac(metaIndex, roles.phase, values) : 0;
    const cycles = 1 + rateFrac;                    // 1..2, matching Movy's default range
    const amp = Math.max(0.15, depthFrac) * (botY - topY) / 2;

    dottedH(ctx, x0, xEnd - 1, baseY);

    const yAt = (px) => {
        const u = (px - x0) / spanW;
        const t = u * cycles + phase;
        return Math.round(baseY - lfoShapeSample(shape, t) * amp);
    };

    drawColumnCurve(ctx, x0, xEnd - 1, yAt, 1);
}

/**
 * schwung-movy renderer/lfo-wave.ts drawWave, ported: the single-knob
 * silhouette. One column per pixel plus a vertical connector to the previous
 * column — a plain Bresenham diagonal reads as slanted steps once the box is
 * this short, so square/pulse edges need the straight riser this gives them.
 */
function drawWaveCell(ctx, x, y, w, h, shape, cycles) {
    const mid = y + (h - 1) / 2, amp = (h - 1) / 2;
    const yAt = (px) => Math.round(mid - lfoShapeSample(shape, ((px - x) / w) * cycles) * amp);
    const vline = (px, a, b) => ctx.fillRect(px, Math.min(a, b), 1, Math.abs(a - b) + 1, 1);

    const firstY = yAt(x);
    let py = firstY;
    vline(x, py, py);
    for (let px = x + 1; px < x + w; px++) {
        const ny = yAt(px);
        vline(px, py, ny);
        py = ny;
    }
    if (py !== firstY) {
        vline(x, py, firstY);
        vline(x + w - 1, py, firstY);
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
    const y = rect.y + 1, h = 14;   // Movy: ky+1, KW-2
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
    const topY = rect.y + 1, botY = topY + 13;
    const midY = Math.round((topY + botY) / 2);
    const amp = (botY - topY) / 2;
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
    const top = rect.y + 1, bot = top + 13, h = bot - top;
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

export function drawSwitch(ctx, rect, key, values) {
    const raw = values ? values[key] : undefined;
    const on = Math.round(Number(raw)) === 1;
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
    const topY = rect.y + 1, botY = topY + 13;
    const midY = Math.round((topY + botY) / 2);
    const amp = Math.min(midY - topY, botY - midY);

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

    if (roles.position && values && values[roles.position] !== undefined) {
        const posMeta = metaIndex.getOrGuess(roles.position);
        const posFrac = clamp01(fractionOf(posMeta, values[roles.position]));
        const mi = Math.min(w - 1, Math.floor(posFrac * w));
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
