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

/*
 * `print(x, y, ...)` takes y as the GLYPH TOP, not a baseline — js_display_print
 * walks the glyph's rows from y downward. Treating it as a baseline (y + line
 * height - 3, the shape that reads naturally) put a 7px glyph 14px down a 23px
 * box: text jammed against the bottom edge with a band of empty space above
 * it, which is what "not centred" looks like on hardware. Everything below is
 * measured from the top.
 */
const GLYPH_H = 7;      /* inked height of the font, measured, not assumed */
const LINE_GAP = 4;
const PAD_X = 8;
const PAD_Y = 6;

/*
 * FIXED width. It was sized to its content, so the box was 100px wide for
 * "Snapshot saved" and 118px for "Snapshot restored" — the toast changed shape
 * depending on which thing had happened, which reads as a broken widget rather
 * than as information. 118 is what the longest message needs (102px of text
 * plus the padding); every shorter message centres inside it.
 */
const BOX_W = 118;
const CONTENT_W = BOX_W - PAD_X * 2;

/* Measure through the host binding when there is one. text_width and print
 * share g_font in js_display.c, so they cannot disagree. Falls back to a 6px
 * monospace estimate only in bare node, where the geometry is still worth
 * asserting even though the widths are approximate. */
function measure(s) {
    return (typeof text_width === "function")
        ? text_width(String(s))
        : String(s).length * 6;
}

/*
 * Where the box lands, given its lines. Split out from the drawing so a test
 * can assert the geometry without a framebuffer — which is the half that can
 * be wrong invisibly (a box that clips, or text sitting off-centre in it).
 */
export function toastGeometry(lines, measureFn) {
    const m = measureFn || measure;
    const rows = (lines || []).filter(l => l !== null && l !== undefined && l !== "");

    const contentH = rows.length * GLYPH_H + Math.max(0, rows.length - 1) * LINE_GAP;
    const boxH = contentH + PAD_Y * 2;

    return {
        rows,
        x: Math.floor((SCREEN_WIDTH - BOX_W) / 2),
        y: Math.floor((SCREEN_HEIGHT - boxH) / 2),
        w: BOX_W,
        h: boxH,
        /* Top of each line, absolute. Computed here rather than in the draw
         * loop so a test can check the text is vertically centred without
         * rendering anything. */
        lineTops: rows.map((_, i) =>
            Math.floor((SCREEN_HEIGHT - boxH) / 2) + PAD_Y + i * (GLYPH_H + LINE_GAP)),
        /* Non-zero when a line does not fit the box. The caller cannot see this
         * by looking at the screen — an overflowing line is drawn past the
         * border with no error — so it is reported. */
        clipped: rows.filter(l => m(l) > CONTENT_W).length,
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
        print(tx, g.lineTops[i], text, 1);
    }
    return g;
}
