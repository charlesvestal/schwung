/**
 * frame_ctx.mjs — a drawing context scoped to one frame.
 *
 * PURE, like the rest of this library: it wraps whatever ctx it is handed and
 * draws nothing itself.
 *
 * A CUSTOM WIDGET CANNOT EXPRESS A SCREEN COORDINATE. That is the point of
 * this file, and it is stronger than clipping-as-safety-net: (0,0) is the
 * frame top-left and there is no accessor that reaches absolute space, so
 * drawing outside your frame stops being a rule an author must follow and
 * becomes something they cannot write down.
 *
 * It has to be that strong because the rect is unstable three ways:
 *
 *   render_page.mjs:619   cellW = floor(rect.w / COLS), caller-dependent
 *   render_page.mjs:116   rowH is DYNAMIC, and computeGeom picks the whole
 *                         render mode from it (dial -> shrinking radius ->
 *                         bar-value -> bar-label -> bar-only)
 *   render_page_movy.mjs  a fixed 32x15, whose own comment warns that 15 is
 *                         only right because both grid gaps happen to be 15px
 *   render_page.mjs:671   Math.min(g.slotSpan, COLS - col) silently CLAMPS a
 *                         two-slot group near the right edge
 *
 * The same widget can be handed any of those, so pixel coordinates authored
 * against one of them are wrong in the others.
 *
 * NO READS. The context carries no getParam and holds no reference to anything
 * that has one. PARAM_PAGES.md forbids a read on the draw path (~2.8ms, against
 * a 1.68ms whole-page render); here that is enforced by construction rather
 * than by review, because values arrive as an argument.
 *
 * THE PRIMITIVES ARE IMPLEMENTED HERE, NOT DELEGATED. The context the host
 * hands the grid carries setPixel / line / fillCircle / drawCircle / drawArc
 * alongside fillRect, and for a while this file offered only the three that
 * render() documents -- so a module drawer that wanted a line had to build
 * Bresenham on fillRect, while the built-in widget beside it called the host.
 *
 * They are NOT passed through to the parent, for two reasons that both matter:
 *
 *   - a delegated call draws in the PARENT's coordinates with the parent's own
 *     implementation, so nothing here could clip it. One `line` and the central
 *     guarantee of this file is gone. Built on the clipped fillRect below,
 *     clipping is not a rule they follow, it is a thing they cannot avoid.
 *   - the host builds several of them as `typeof draw_line === "function" ?
 *     draw_line : undefined`, so delegating would make availability depend on
 *     the caller and force every drawer to feature-detect. Implemented here,
 *     they are always present -- including for a node test, which has no host.
 *
 * The algorithms are ported from js_display.c deliberately rather than
 * reinvented: a widget's arc sits beside the grid's own arc knobs, and two
 * circles drawn by different maths on a 1-bit display do not look like the same
 * object. The row/column union in drawArc is the one that avoids stranded
 * pixels at the compass points -- see that file for the three constructions
 * that are wrong.
 *
 * THE COST IS BINDINGS. Each is a run of fillRect calls where the host would
 * make one crossing, so a filled circle of r=8 is ~17 calls rather than 1. That
 * is the price of clipping being structural, and it is the right trade at cell
 * and card sizes; a drawer filling a large disc every frame should reach for a
 * rect.
 *
 * clipped() COUNTS ATTEMPTED OVERFLOW rather than hiding it. A fixed-width row
 * cannot report that it overflowed, which is exactly how nine Master FX boxes
 * came to be drawn 86px off-screen with no error at all
 * (tests/host/test_master_fx_diagram_fit.sh). A widget that tries to leave its
 * frame is a red test, not a silent absorption.
 */

/** Clip a rect given in frame-local coordinates to the frame. */
function clipToFrame(fx, fy, fw, fh, w, h) {
    let x0 = fx, y0 = fy, x1 = fx + fw, y1 = fy + fh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return null;
    return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}

/**
 * @param {object} ctx    parent context: { fillRect, print, textWidth }
 * @param {object} frame  { x, y, w, h } in the parent's coordinates
 * @returns {object} a frame-local context
 */
export function frameCtx(ctx, frame) {
    const ox = Math.round(frame.x), oy = Math.round(frame.y);
    const w = Math.max(0, Math.round(frame.w)), h = Math.max(0, Math.round(frame.h));
    let clipCount = 0;

    const self = {
        width: w,
        height: h,

        fillRect(x, y, rw, rh, color) {
            const fx = Math.round(x), fy = Math.round(y);
            const fw = Math.round(rw), fh = Math.round(rh);
            const r = clipToFrame(fx, fy, fw, fh, w, h);
            if (!r) { clipCount++; return; }
            if (r.x !== fx || r.y !== fy || r.w !== fw || r.h !== fh) clipCount++;
            ctx.fillRect(ox + r.x, oy + r.y, r.w, r.h, color);
        },

        /* Text cannot be partially clipped with fillRect/print alone, so it is
         * TRUNCATED to what fits -- the same choice labelForCell already makes.
         * A half-drawn glyph run reads as a broken renderer; a short label
         * reads as a short label. */
        print(x, y, text, color) {
            const fx = Math.round(x), fy = Math.round(y);
            if (fx < 0 || fy < 0 || fx >= w || fy >= h) { clipCount++; return; }
            let s = String(text);
            const budget = w - fx;
            if (ctx.textWidth(s) > budget) {
                clipCount++;
                while (s.length > 0 && ctx.textWidth(s) > budget) s = s.slice(0, -1);
                if (!s) return;
            }
            ctx.print(ox + fx, oy + fy, s, color);
        },

        /* ---------------------------------------------------------------
         * Primitives, all routed through this object's own fillRect, so every
         * one of them is translated and clipped by construction. `self` is the
         * context being built; the methods call each other through it rather
         * than through a captured local so a caller that wraps or spies on
         * fillRect sees every write.
         * --------------------------------------------------------------- */

        setPixel(x, y, color) { self.fillRect(x, y, 1, 1, color); },

        /* Bresenham, matching render_page.mjs's `line`. */
        line(x0, y0, x1, y1, color) {
            let ax = Math.round(x0), ay = Math.round(y0);
            const bx = Math.round(x1), by = Math.round(y1);
            let dx = Math.abs(bx - ax), sx = ax < bx ? 1 : -1;
            let dy = -Math.abs(by - ay), sy = ay < by ? 1 : -1;
            let err = dx + dy;
            /* A run this long cannot be a real drawing on a 128x64 panel; it is
             * a NaN or a runaway, and without the bound it would spin. */
            for (let guard = 0; guard < 4096; guard++) {
                self.fillRect(ax, ay, 1, 1, color);
                if (ax === bx && ay === by) break;
                const e2 = 2 * err;
                if (e2 >= dy) { err += dy; ax += sx; }
                if (e2 <= dx) { err += dx; ay += sy; }
            }
        },

        /* js_display.c fill_circle: every pixel inside the radius. Emitted as
         * one fillRect per ROW rather than per pixel -- same pixels, ~2r calls
         * instead of ~pi*r*r. */
        fillCircle(cx, cy, r, color) {
            const x = Math.round(cx), y = Math.round(cy), rr = Math.round(r);
            if (!(rr >= 0)) return;
            for (let dy = -rr; dy <= rr; dy++) {
                const half = Math.floor(Math.sqrt(rr * rr - dy * dy));
                self.fillRect(x - half, y + dy, half * 2 + 1, 1, color);
            }
        },

        drawCircle(cx, cy, r, color) { self.drawArc(cx, cy, r, 0, 360, color); },

        /*
         * js_display.c draw_arc, ported exactly.
         *
         * Angles are read the way a knob is: 0 at twelve o`clock, increasing
         * CLOCKWISE. One pixel per row and one per column, unioned -- rows
         * alone strand a lone pixel at twelve and six, columns alone at three
         * and nine.
         */
        drawArc(cx, cy, r, startDeg, sweepDeg, color) {
            const x = Math.round(cx), y = Math.round(cy), rr = Math.round(r);
            if (!(rr >= 0)) return;
            if (rr === 0) { self.fillRect(x, y, 1, 1, color); return; }
            let sweep = Number(sweepDeg);
            if (!(sweep > 0)) return;
            if (sweep >= 360) sweep = 360;
            const start = ((Math.round(Number(startDeg) || 0) % 360) + 360) % 360;

            const plot = (dx, dy) => {
                if (sweep < 360) {
                    let a = Math.atan2(dx, -dy) * 180 / Math.PI;
                    if (a < 0) a += 360;
                    let delta = a - start;
                    if (delta < 0) delta += 360;
                    if (delta > sweep) return;
                }
                self.fillRect(x + dx, y + dy, 1, 1, color);
            };

            for (let dy = -rr; dy <= rr; dy++) {
                const dx = Math.round(Math.sqrt(rr * rr - dy * dy));
                plot(dx, dy);
                if (dx !== 0) plot(-dx, dy);
            }
            for (let dx = -rr; dx <= rr; dx++) {
                const dy = Math.round(Math.sqrt(rr * rr - dx * dx));
                plot(dx, dy);
                if (dy !== 0) plot(dx, -dy);
            }
        },

        /* Measurement, not drawing -- no translation to do. */
        textWidth(text) { return ctx.textWidth(text); },

        /** How many draw calls were clipped, truncated or dropped. */
        clipped() { return clipCount; },
    };
    return self;
}
