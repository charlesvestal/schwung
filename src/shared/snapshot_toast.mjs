/*
 * The snapshot / recall toast.
 *
 * Lives here rather than in sampler_overlay.mjs because that file draws the
 * SHIM's overlays — its `OVERLAY_*` values are a mirror of shadow_constants.h,
 * and its state arrives from /schwung-overlay. This toast has no shim state at
 * all: shadow_ui raises it itself and clears it on a frame count, using the
 * `display_overlay` field that already composites JS pixels over Move's native
 * screen. Adding a fifth OVERLAY_* constant would have implied a shim field
 * that does not exist.
 *
 * The box is sized from MEASURED text, not from `text.length * 6`. The font is
 * proportional — the neighbouring set-page toast estimates and gets away with
 * it only because "Page 3/8" is short and the box is generously wide. A help
 * audit already found a character-count budget cutting names that fit fine, so
 * the estimate is not repeated here.
 */

import { drawRect } from '/data/UserData/schwung/shared/menu_layout.mjs';

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

const PAD_X = 8;        /* horizontal padding either side of the widest line */
const PAD_Y = 6;        /* vertical padding above the first / below the last */
const LINE_H = 11;
const MIN_W = 60;       /* so a one-word toast is not a sliver */
const MAX_W = SCREEN_WIDTH - 8;

/* Measure through the host binding when there is one. Falls back to a 6px
 * monospace estimate so this module can be exercised in bare node, where the
 * geometry is still worth asserting even though the widths are approximate. */
function measure(s) {
    return (typeof text_width === "function")
        ? text_width(String(s))
        : String(s).length * 6;
}

/*
 * Where the box lands, given its lines. Split out from the drawing so a test
 * can assert the geometry without a framebuffer — which is the half that can
 * be wrong invisibly (a box that clips, or one narrower than its text).
 */
export function toastGeometry(lines, measureFn) {
    const m = measureFn || measure;
    const rows = (lines || []).filter(l => l !== null && l !== undefined && l !== "");
    let widest = 0;
    for (const l of rows) { const w = m(l); if (w > widest) widest = w; }

    let boxW = widest + PAD_X * 2;
    if (boxW < MIN_W) boxW = MIN_W;
    if (boxW > MAX_W) boxW = MAX_W;
    const boxH = rows.length * LINE_H + PAD_Y * 2;

    return {
        rows,
        x: Math.floor((SCREEN_WIDTH - boxW) / 2),
        y: Math.floor((SCREEN_HEIGHT - boxH) / 2),
        w: boxW,
        h: boxH,
        /* Non-zero when the text does not fit the widest box we allow. The
         * caller cannot see this by looking at the screen — an overflowing
         * line is drawn off the edge with no error — so it is reported. */
        clipped: rows.filter(l => m(l) + PAD_X * 2 > MAX_W).length,
    };
}

/* Draw the toast. Returns the geometry so the caller can hand the same rect to
 * shadow_set_display_overlay — the blit rect and the drawn box must be the
 * same rectangle, and computing it twice is how they drift apart. */
export function drawSnapshotToast(lines) {
    const g = toastGeometry(lines);
    fill_rect(g.x, g.y, g.w, g.h, 0);
    drawRect(g.x, g.y, g.w, g.h, 1);
    for (let i = 0; i < g.rows.length; i++) {
        const text = g.rows[i];
        const tx = g.x + Math.floor((g.w - measure(text)) / 2);
        print(tx, g.y + PAD_Y + i * LINE_H + LINE_H - 3, text, 1);
    }
    return g;
}
