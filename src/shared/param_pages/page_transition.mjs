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
 * COVERAGE IS NOT ERASURE, and the paragraph above is only about coverage.
 * The union of the two pages contains [0, 128), which is why no clip rect is
 * needed -- but neither renderer paints a background, so ink survives wherever
 * the pages happen not to ink. The caller must still clear the body band
 * before calling drawSlide, exactly as it does before an ordinary render.
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
 *
 * The table is the set of methods the two real contexts actually PROVIDE --
 * the device ctx built in src/shadow/shadow_ui_param_pages.mjs and the
 * harness ctx in tools/param-pages/harness.mjs -- not a wish list. In
 * particular the device binds its native draw_line to the key `line`, so a
 * `drawLine` entry would wrap a key that exists on no real context; `drawRect`
 * is provided by neither. Both were dropped as dead entries.
 *
 * `drawCircle` is here although no renderer calls it today: both contexts
 * provide it, and render_page_movy.mjs carries a comment claiming it prefers
 * the native drawCircle binding while the shipped code goes through drawArc.
 * If that preference returns, an unwrapped drawCircle would draw at the wrong
 * x. One table entry is cheaper than that bug.
 */
const WRAP = {
    fillRect:   (f, dx) => (x, y, w, h, v) => f(x + dx, y, w, h, v),
    print:      (f, dx) => (x, y, t, c) => f(x + dx, y, t, c),
    /* No coordinates: a measurement is the same wherever it is drawn. It must
     * still be forwarded -- a missing textWidth makes every centred string
     * lay out against undefined. */
    textWidth:  (f) => (t) => f(t),
    setPixel:   (f, dx) => (x, y, v) => f(x + dx, y, v),
    line:       (f, dx) => (x0, y0, x1, y1, v) => f(x0 + dx, y0, x1 + dx, y1, v),
    fillCircle: (f, dx) => (cx, cy, r, v) => f(cx + dx, cy, r, v),
    drawCircle: (f, dx) => (cx, cy, r, v) => f(cx + dx, cy, r, v),
    drawArc:    (f, dx) => (cx, cy, r, a, s, v) => f(cx + dx, cy, r, a, s, v),
};

/** A view of `ctx` shifted `dx` pixels horizontally. */
export function translateCtx(ctx, dx) {
    /*
     * Identity at zero, by object, so the ordinary no-transition path is
     * literally the un-proxied one and cannot drift from it.
     *
     * `!dx` RATHER THAN `dx === 0`, DELIBERATELY, and it is load-bearing:
     * `!NaN` is true, so a NaN offset returns the unproxied ctx and the frame
     * is merely garbled. Tighten this to `dx === 0` and a NaN instead reaches
     * every wrapped coordinate -- and NaN coordinates reach line()'s for(;;)
     * whose equality break is never satisfied, which is a FROZEN TICK, a
     * failure this codebase has already had (see the drawPartialEnv note in
     * CLAUDE.md). The advances guard NaN at source; this is the second line.
     */
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
    /*
     * The epsilon is DEFENSIVE, not a recorded failure. It keeps a position
     * left at 2.9999999 from reading as base 2 with frac 0.9999 -- one frame
     * of the previous page flashing at the moment of arrival.
     *
     * No CURRENT caller can produce that value: both advances snap exactly
     * onto the integer target once they are within SNAP_PAGES, which is three
     * orders of magnitude coarser than this epsilon, so a position that close
     * to an integer has already been replaced by the integer. It is here for a
     * future advance (a spring, an overshoot) that does not snap.
     */
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
 * `to` is derived by ADDITION, never rounded in its own right. The natural
 * refactor -- `to: -Math.round((frac - 1) * width)` -- looks symmetrical and
 * is not: the two roundings can disagree by one, giving a 127 or 129 gap, i.e.
 * a one-pixel seam of stale ink or a one-pixel column of a page overwritten.
 *
 * There is no direction argument: travelling backwards is `pos` DECREASING,
 * and `frac` is still the fraction between `base` and `base + 1`.
 */
export function slideOffsets(frac, width) {
    const from = -Math.round(frac * width);
    return { from, to: from + width };
}

/**
 * How close to the target counts as arrived: ONE PIXEL at the rendered width.
 *
 * IT WAS HALF A PIXEL, AND THAT WAS REPORTED FROM THE DEVICE AS THE PAGE
 * "GOING ONE PIXEL TOO FAR AND SNAPPING BACK". The old reasoning sounded right
 * and was subtly wrong: half a pixel is where `slideOffsets`' `Math.round`
 * stops producing a different offset, so below it the frame is pixel-identical
 * and the motion really is over.
 *
 * What it misses is the band JUST ABOVE it. While the remaining travel sits
 * between 0.5px and 1.5px, Math.round renders the incoming page ONE PIXEL
 * SHORT of home -- correct arithmetic, and a picture with a 1px sliver of the
 * OUTGOING page still showing at the left edge. An exponential tail dawdles
 * through that band: at 160ms it drew x=1 for two consecutive frames (~34ms)
 * and then jumped the last pixel. Held-then-jumped is what the eye reads as an
 * overshoot.
 *
 * One pixel is the honest threshold, because one pixel is the smallest thing
 * the panel can express.
 *
 * `EASED_TAUS_TO_SNAP` derives from this, so changing it changes what every
 * duration means -- intended, and why the derivation is in code not a literal.
 *
 * 128 is hardcoded here while `slideOffsets` takes `width` as a parameter.
 * That asymmetry is deliberate and bounded: 128 is the only width this ships
 * at (it is the panel), and the parameter exists so the filming tool can say
 * so explicitly rather than so the width can vary. If a second width ever
 * appears, thread it through instead of scaling this constant.
 */
const SNAP_PAGES = 1 / 128;

/** Is a page change still in flight at scroll position `pos`? */
export function isSliding(pos) {
    /*
     * The gate for "stop compositing", the same role settled() plays in
     * anim_state.mjs -- which calls the idle redraw "the single largest cost
     * of animating anything here". Ours is larger still: a composited frame is
     * TWO full page renders (measured at 1.96x a settled one), and at frac 0
     * the second is drawn entirely offscreen at dx 128, so it is pure waste.
     *
     * THE TEST-FACING SPELLING OF THAT GATE, and nothing more -- do not read
     * it as the production one. page_controller.mjs`s render() needs `base`
     * and `frac` anyway, so it destructures scrollFrame and tests
     * `frac !== 0` inline; calling this as well would be a second scrollFrame
     * per frame to learn what it already knows. It stays because a caller that
     * only wants the yes/no should not have to know that frac 0 is the
     * settled case, and because the tests say `isSliding` where they mean it.
     */
    return isFinite(pos) && scrollFrame(pos).frac !== 0;
}

/**
 * Guard shared by both advances.
 *
 * SAME SHAPE AS anim_state.mjs's `progress`, which carries
 * `if (!(dt >= 0)) return 1;` under the comment "clock went backwards:
 * settle" for exactly this case -- and for the same reason: a backwards
 * clock means we have lost
 * the ability to PACE, and finishing is better than travelling the wrong way.
 * Without it a negative dt is read as a velocity: measured,
 * advanceEased(0, 1, -500, 200) returns -1807, because 1 - exp(+dt/tau) is
 * unbounded below.
 *
 * NaN is guarded here too because it is ABSORBING. `pos` is stored state, and
 * once it is NaN both escape hatches in the advances fail open --
 * `Math.abs(NaN) <= SNAP` is false and `step >= Math.abs(d)` is false -- so
 * every subsequent frame is NaN as well, permanently, until the view is
 * re-entered. Settling makes it self-heal on the very next frame.
 */
function cannotPace(pos, dtMs, ms) {
    return !(ms > 0) || !(dtMs >= 0) || !isFinite(pos) || !isFinite(dtMs);
}

/**
 * Constant-velocity advance: one page per `ms`.
 *
 * Both advances SNAP EXACTLY onto the target rather than approaching it. An
 * eased chase is naturally asymptotic, and a position a thousandth of a page
 * from home would leave the transition running for ever — two page renders
 * every frame, on a screen that has visibly finished moving.
 *
 * `ms` of 0 arrives immediately, which is how the animation is switched off:
 * the position is always the target, always an integer, and every frame is the
 * ordinary un-composited draw.
 */
export function advanceLinear(pos, target, dtMs, ms) {
    if (cannotPace(pos, dtMs, ms)) return target;
    const d = target - pos;
    if (Math.abs(d) <= SNAP_PAGES) return target;
    const step = dtMs / ms;
    if (step >= Math.abs(d)) return target;
    const next = d > 0 ? pos + step : pos - step;
    /*
     * A STEP THAT CANNOT MOVE THE PICTURE ENDS THE SLIDE.
     *
     * The distance threshold is not enough on its own: an exponential tail
     * keeps returning positions further than a pixel from home while the STEP
     * between them has already fallen below one pixel, so Math.round renders
     * two consecutive frames identically. Measured at 280ms eased: ...3 2 2 1 0.
     *
     * Steps are non-increasing for both advances (constant for linear, decaying
     * for eased), so once one step is invisible every later one is too -- there
     * is no progress left to draw. A duration so long that even the FIRST step
     * is sub-pixel correctly becomes a cut; at 16.7ms/frame that is past 2s.
     */
    /* ...but ONLY when time actually advanced. A zero-size step from dt=0
     * means NO TIME HAS PASSED, not "the animation is over" -- and dt=0 is
     * the COMMON case here, because this device's clock is quantized to
     * ~11-12ms against a ~17ms tick, so a jog and its tick often land inside
     * the same quantum. Conflating the two ended the slide on the first frame
     * of most page changes: measured on hardware as dt=0ms with the position
     * jumping straight to the target, drawing zero composited frames. */
    if (dtMs > 0 && Math.abs(next - pos) < SNAP_PAGES) return target;
    return next;
}

/**
 * How many time constants an exponential decay needs to reach the snap.
 *
 * DERIVED FROM SNAP_PAGES, NEVER WRITTEN AS A NUMBER, because the two are the
 * same fact seen twice: the decay is `exp(-t/tau)` and `advanceEased` arrives
 * when the remaining distance falls to SNAP_PAGES, so arrival is at
 * `t = tau * ln(1 / SNAP_PAGES)` — today ln(256) = 5.545.
 *
 * Hardcode 5.545 and a future change to the snap threshold silently changes
 * what every duration in the codebase MEANS, with no test touching either
 * constant and nothing to read that says so. The coupling is the reason this
 * line exists.
 */
const EASED_TAUS_TO_SNAP = Math.log(1 / SNAP_PAGES);

/**
 * Exponential advance: velocity proportional to the distance left.
 *
 * `ms` IS THE SETTLE TIME — the moment the position lands exactly on the
 * target — and that is a correction, not a restatement.
 *
 * It used to be `tau = ms / 3`, on the reasoning that three time constants is
 * 95% of the travel and therefore "about `ms`". The remaining 5% is not free:
 * it still has to walk down to SNAP_PAGES, which is another 2.5 time
 * constants, so the real settle was 1.85x the number written at the call site.
 * Filming it at the device's true cadence is what exposed this
 * (tools/param-pages/movie.mjs) — a nominal 200ms measured 364ms over 20
 * frames, SEVEN of which had moved less than half a pixel and so were
 * pixel-identical repaints at two full page renders each.
 *
 * That made `ms` incomparable between the two advances: advanceLinear's 200
 * really was 200. Now both mean the same thing, which is what lets SLIDE_MS be
 * a single number rather than a number plus a note about which curve it is for.
 *
 * Frame-rate independent by construction (`1 - exp(-dt/tau)`), which matters
 * here because this device's clock is quantized to ~11-12ms and the tick
 * interval is not constant — a naive `pos += d * k` would move at a speed that
 * depended on how the clock happened to round.
 */
export function advanceEased(pos, target, dtMs, ms) {
    if (cannotPace(pos, dtMs, ms)) return target;
    const d = target - pos;
    if (Math.abs(d) <= SNAP_PAGES) return target;
    const next = pos + d * (1 - Math.exp(-dtMs / (ms / EASED_TAUS_TO_SNAP)));
    if (Math.abs(target - next) <= SNAP_PAGES) return target;
    /*
     * A STEP THAT CANNOT MOVE THE PICTURE ENDS THE SLIDE.
     *
     * The distance threshold is not enough on its own: an exponential tail
     * keeps returning positions further than a pixel from home while the STEP
     * between them has already fallen below one pixel, so Math.round renders
     * two consecutive frames identically. Measured at 280ms eased: ...3 2 2 1 0.
     *
     * Steps are non-increasing for both advances (constant for linear, decaying
     * for eased), so once one step is invisible every later one is too -- there
     * is no progress left to draw. A duration so long that even the FIRST step
     * is sub-pixel correctly becomes a cut; at 16.7ms/frame that is past 2s.
     */
    /* ...but ONLY when time actually advanced. A zero-size step from dt=0
     * means NO TIME HAS PASSED, not "the animation is over" -- and dt=0 is
     * the COMMON case here, because this device's clock is quantized to
     * ~11-12ms against a ~17ms tick, so a jog and its tick often land inside
     * the same quantum. Conflating the two ended the slide on the first frame
     * of most page changes: measured on hardware as dt=0ms with the position
     * jumping straight to the target, drawing zero composited frames. */
    if (dtMs > 0 && Math.abs(next - pos) < SNAP_PAGES) return target;
    return next;
}

/**
 * Draw one composite frame: both pages, then the fixed chrome over them.
 *
 * Chrome LAST because the bank bar and the footer do not travel -- the bar is
 * the page indicator and cannot be unreadable for the duration of the page
 * change it is reporting.
 *
 * PRECONDITION: the caller has already cleared the body band. The two pages
 * COVER [0, 128) between them, which is what makes a clip rect unnecessary,
 * but covering is not erasing -- neither renderer paints a background, so any
 * pixel neither page inks keeps whatever was there before.
 */
export function drawSlide(ctx, { fromDx, toDx, drawFrom, drawTo, drawChrome }) {
    drawFrom(translateCtx(ctx, fromDx));
    drawTo(translateCtx(ctx, toDx));
    if (drawChrome) drawChrome(ctx);
}

/**
 * Duration and advance — the values the controller ships with.
 *
 * Chosen from filmed GIFs at the device's real cadence, not from a preview at
 * a frame rate the hardware cannot produce -- this device's clock is quantized
 * to ~11-12ms and the grid ticks at ~55Hz. See tools/param-pages/movie.mjs.
 *
 * 90ms IS ABOUT FIVE FRAMES, AND THAT IS THE POINT: the brief was "just enough
 * to get a feel for what's happening". The eased curve front-loads the travel,
 * so the first frame covers roughly half the screen and the rest is the tail —
 * which reads as snap rather than as motion, and was chosen over the linear
 * 90ms (an even ~26px per frame, filmed as slide-090-linear) with that
 * difference visible in both GIFs.
 *
 * These are constants, not settings: there is no setter and no caller that
 * passes anything else. SLIDE_MS is the value a Global Settings toggle would
 * drive if one is ever added, and 0 is the value that would disable the
 * animation (see advanceLinear -- a duration of 0 arrives immediately).
 */
export const SLIDE_MS = 160;
export const advanceScroll = advanceEased;
