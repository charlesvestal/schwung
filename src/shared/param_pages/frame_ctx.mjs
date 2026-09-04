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

    return {
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

        /* Measurement, not drawing -- no translation to do. */
        textWidth(text) { return ctx.textWidth(text); },

        /** How many draw calls were clipped, truncated or dropped. */
        clipped() { return clipCount; },
    };
}
