/*
 * scene_fader_overlay.mjs -- the A-B slider shown when the scene fader moves
 * anywhere but the Scenes screen (Shift+Vol, a mapped CC), over the shadow
 * UI or over Move's own screen.
 *
 * A morph you cannot see is a sound changing for no visible reason. The panel
 * itself -- where it sits, how it slides -- is footer_panel.mjs, the same band
 * the automation lock map uses; this adds only WHEN (a move, held for
 * HOLD_MS) and the picture. The Scenes screen draws its own fader, so there
 * the panel drops at once.
 *
 * Pure: the clock is handed in, so a test can drive it.
 */
import { createFooterPanel, beginFooterPanel } from "./footer_panel.mjs";

export const HOLD_MS = 1200;           /* stays this long after the last move */

/**
 * io: { now() -> ms }.
 * observe(x, label, suppressed, turned) once per tick: x the fader 0..1,
 *   label { a: "A3", b: "B3" } for the active scene's ends, suppressed = the
 *   Scenes screen is up, turned = the fader was TURNED since the last tick
 *   (Shift+Vol), whether or not x changed -- a turn past either end is
 *   clamped, and it must still show where the fader is.
 * frame() -> null, or { y, payload: { x, label } } -- call once per drawn frame.
 */
export function createSceneFaderOverlay(io) {
    const panel = createFooterPanel();
    let lastX = null;
    let lastMove = -Infinity;
    let want = null;

    function observe(x, label, suppressed, turned) {
        if (!Number.isFinite(x)) return;
        /* The first observation is a baseline, not a move: a boot, a set load
         * or opening the shadow UI must not raise the panel. */
        if (lastX === null) { lastX = x; return; }
        const moved = Math.abs(x - lastX) > 1e-4;
        lastX = x;
        if (suppressed) {
            panel.hide();
            lastMove = -Infinity;
            want = null;
            return;
        }
        const t = io.now();
        if (moved || turned) lastMove = t;
        want = t - lastMove < HOLD_MS ? { x, label } : null;
    }

    /* Show it without a fader move: a Program Change just changed the scene. */
    function raise() { lastMove = io.now(); }

    function frame() { return panel.update(io.now(), want); }
    function busy() { return want !== null || panel.busy(); }

    return { observe, frame, busy, raise };
}

/**
 * ctx: { fillRect(x,y,w,h,c), print(x,y,s,c), textWidth(s) }. Returns the rect
 * it touched (for a blit onto Move's screen), or null.
 *
 *   A3 |=====#------| B3
 */
export function drawSceneFaderOverlay(ctx, fr) {
    if (!fr) return null;
    const rect = beginFooterPanel(ctx, fr.y);
    const { x, label } = fr.payload;
    const ty = fr.y + 2;                /* the footer's own text row when fully up */
    const a = (label && label.a) || "A-";
    const b = (label && label.b) || "B-";
    const v = Math.max(0, Math.min(1, x));
    const bx = 127 - ctx.textWidth(b);
    ctx.print(1, ty, a, 1);
    ctx.print(bx, ty, b, 1);
    /* The Scenes screen's fader at footer size, so it reads as the same one. */
    const tx0 = 1 + ctx.textWidth(a) + 3, tx1 = bx - 3;
    if (tx1 - tx0 >= 8) {
        const cy = ty + 3;
        ctx.fillRect(tx0, cy, tx1 - tx0, 1, 1);
        ctx.fillRect(tx0, cy - 2, 1, 5, 1);
        ctx.fillRect(tx1 - 1, cy - 2, 1, 5, 1);
        const pos = tx0 + 1 + Math.round(v * (tx1 - tx0 - 3));
        ctx.fillRect(tx0, cy - 1, pos - tx0, 3, 1);
        ctx.fillRect(pos - 1, cy - 3, 3, 7, 1);
    }
    return rect;
}
