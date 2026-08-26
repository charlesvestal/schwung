/**
 * page_transition.mjs — sliding one page off the screen while the next slides on.
 *
 * WHY THIS NEEDS NO CLIPPING, which is the whole reason it is cheap.
 *
 * Every draw both renderers make goes through the injected ctx, and
 * js_display_set_pixel (src/host/js_display.c) already discards anything
 * outside the 128x64 buffer. Render the outgoing page at dx = -N and the
 * incoming at dx = 128 - N and they occupy [-N, 128-N) and [128-N, 256-N):
 * they ABUT EXACTLY and the screen bounds do the rest. No clip rect, no C
 * change, no offscreen framebuffer, no JS blit.
 *
 * Two properties hold that up and must not quietly stop being true:
 *   - the two pages never overlap (slideOffsets is what guarantees it, and
 *     tests/host/test_page_transition_proxy.sh pins it at every t);
 *   - the body's ink stops at row 54 (ROW1_Y 33 + LBL1_Y 48 + a 7-row label),
 *     with RULE_Y 55 belonging to the footer, which the sliding passes suppress.
 *
 * Pure: no state, no device globals, no clock. Time is passed in, exactly as
 * anim_state.mjs takes `now` -- which is what lets tools/param-pages/movie.mjs
 * film a transition deterministically instead of racing a real clock.
 */

/**
 * How each draw method is translated.
 *
 * A method NOT in this table is deliberately OMITTED from the proxy rather
 * than passed through. Both renderers treat `line` and `drawArc` as optional
 * and fall back to a JS Bresenham drawn through ctx.fillRect, which IS
 * translated -- so omission degrades to correct-but-slower, while an
 * untranslated passthrough would draw in the wrong place. Neither failure
 * raises, so the direction of the safe default matters.
 */
const WRAP = {
    fillRect:   (f, dx) => (x, y, w, h, v) => f(x + dx, y, w, h, v),
    print:      (f, dx) => (x, y, t, c) => f(x + dx, y, t, c),
    /* No coordinates: a measurement is the same wherever it is drawn. It must
     * still be forwarded -- a missing textWidth makes every centred string
     * lay out against undefined. */
    textWidth:  (f) => (t) => f(t),
    setPixel:   (f, dx) => (x, y, v) => f(x + dx, y, v),
    drawRect:   (f, dx) => (x, y, w, h, v) => f(x + dx, y, w, h, v),
    line:       (f, dx) => (x0, y0, x1, y1, v) => f(x0 + dx, y0, x1 + dx, y1, v),
    drawLine:   (f, dx) => (x0, y0, x1, y1, v) => f(x0 + dx, y0, x1 + dx, y1, v),
    fillCircle: (f, dx) => (cx, cy, r, v) => f(cx + dx, cy, r, v),
    drawArc:    (f, dx) => (cx, cy, r, a, s, v) => f(cx + dx, cy, r, a, s, v),
};

/** A view of `ctx` shifted `dx` pixels horizontally. */
export function translateCtx(ctx, dx) {
    /* Identity at zero, by object, so the ordinary no-transition path is
     * literally the un-proxied one and cannot drift from it. */
    if (!ctx || !dx) return ctx;
    const out = {};
    for (const name of Object.keys(WRAP)) {
        const fn = ctx[name];
        if (typeof fn === "function") out[name] = WRAP[name](fn.bind(ctx), dx);
    }
    return out;
}

/**
 * THE SLIDE IS A POSITION, NOT A FROM/TO PAIR — and that is what makes a fast
 * jog chase instead of stutter.
 *
 * The obvious model is `{ fromIndex, toIndex, startMs }`, and it cannot chase.
 * When a jog retargets mid-slide, the page that was arriving is somewhere out
 * at +80px; a fresh from/to pair puts its outgoing page at 0, so the picture
 * SNAPS up to a full screen width backwards on the retarget frame. That is the
 * exact discontinuity chase exists to prevent.
 *
 * So the state is a single continuous `pos`, in page units, easing toward a
 * target index. Retargeting changes the target; the position carries on from
 * wherever it is. `s.pageIndex` remains the logical page — the one input, LEDs
 * and the screen reader belong to — and `pos` is purely what is drawn.
 */

/** Which two pages are on screen at scroll position `pos`, and how far along. */
export function scrollFrame(pos) {
    /* The epsilon keeps a position that arithmetic left at 2.9999999 from
     * reading as base 2 with frac 0.9999 -- one frame of the previous page
     * flashing at the moment of arrival. */
    const base = Math.floor(pos + 1e-6);
    const frac = pos - base;
    return { base, frac: frac < 1e-6 ? 0 : frac };
}

/**
 * Where the two pages sit at `frac` of the way between them.
 *
 * The offsets always differ by exactly one `width` — that is the no-overlap
 * guarantee the whole approach rests on, and it is asserted across the range
 * rather than at the endpoints.
 *
 * There is no direction argument: travelling backwards is `pos` DECREASING,
 * and `frac` is still the fraction between `base` and `base + 1`.
 */
export function slideOffsets(frac, width) {
    const from = -Math.round(frac * width);
    return { from, to: from + width };
}

/** How close to the target counts as arrived: under a quarter of a pixel. */
const SNAP_PAGES = 0.002;

/**
 * Constant-velocity advance: one page per `ms`.
 *
 * Both advances SNAP EXACTLY onto the target rather than approaching it. An
 * eased chase is naturally asymptotic, and a position a thousandth of a page
 * from home would leave the transition running for ever — two page renders
 * every frame, on a screen that has visibly finished moving.
 */
export function advanceLinear(pos, target, dtMs, ms) {
    if (!(ms > 0)) return target;
    const d = target - pos;
    if (Math.abs(d) <= SNAP_PAGES) return target;
    const step = dtMs / ms;
    if (step >= Math.abs(d)) return target;
    return d > 0 ? pos + step : pos - step;
}

/**
 * Exponential advance: velocity proportional to the distance left.
 *
 * Frame-rate independent by construction (`1 - exp(-dt/tau)`), which matters
 * here because this device's clock is quantized to ~11-12ms and the tick
 * interval is not constant — a naive `pos += d * k` would move at a speed that
 * depended on how the clock happened to round.
 */
export function advanceEased(pos, target, dtMs, ms) {
    if (!(ms > 0)) return target;
    const d = target - pos;
    if (Math.abs(d) <= SNAP_PAGES) return target;
    /* tau chosen so the visible motion is over in about `ms`: three time
     * constants is 95% of the distance. */
    const next = pos + d * (1 - Math.exp(-dtMs / (ms / 3)));
    return Math.abs(target - next) <= SNAP_PAGES ? target : next;
}

/**
 * Draw one composite frame: both pages, then the fixed chrome over them.
 *
 * Chrome LAST because the bank bar and the footer do not travel -- the bar is
 * the page indicator and cannot be unreadable for the duration of the page
 * change it is reporting.
 */
export function drawSlide(ctx, { fromDx, toDx, drawFrom, drawTo, drawChrome }) {
    drawFrom(translateCtx(ctx, fromDx));
    drawTo(translateCtx(ctx, toDx));
    if (drawChrome) drawChrome(ctx);
}

/**
 * Duration and advance.
 *
 * Chosen from filmed GIFs at the device's real cadence, not from a preview at
 * a frame rate the hardware cannot produce -- this device's clock is quantized
 * to ~11-12ms and the grid ticks at ~55Hz, so a 128px slide is about a dozen
 * real frames of ~12px each. See tools/param-pages/movie.mjs.
 *
 * SLIDE_MS of 0 disables the slide: advanceScroll returns the target
 * immediately, the position is always an integer, and every frame is the
 * ordinary un-composited draw.
 */
export const SLIDE_MS = 200;
export const advanceScroll = advanceLinear;
