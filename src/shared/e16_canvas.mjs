/**
 * e16_canvas.mjs — an SSD1306 framebuffer that speaks the same draw-context
 * interface `src/shared/param_pages/` already renders through
 * (`fillRect`, `print`, `textWidth`, plus `drawLine`/`clear`).
 *
 * The OXI E16 takes a raw 1024-byte SSD1306 page/column buffer over SysEx.
 * Rather than growing a second drawing stack for it, this gives the E16 a
 * ctx implementation that IS that buffer: the fleet's existing page
 * renderers (render_page_movy.mjs and friends) can draw into it exactly as
 * they draw into the harness's PNG framebuffer or the device's real OLED —
 * one code path, three destinations.
 *
 * Font: font4x5 (`./param_pages/font4x5.mjs`), not the device's native 5x7.
 * render_page_movy.mjs — "the one the device uses" (CLAUDE.md) — measures
 * and draws its labels through `fontWidth4x5`/`fontPrint4x5` directly,
 * bypassing ctx.textWidth/ctx.print entirely. Backing THIS canvas's
 * print/textWidth with the same two functions, rather than reimplementing
 * font metrics, is what makes "a page measures the same on both screens"
 * true by construction instead of by coincidence: there is exactly one
 * source for what a string of grid text measures, same as the wav-format
 * table has exactly one source for which bytes are the samples.
 *
 * SSD1306 page/column layout (matches the device's real panel, and is why
 * this buffer can be handed to the E16 unmodified): 8 pages of 128 columns,
 * byte index = (y >> 3) * 128 + x, bit = y & 7 with bit 0 the TOPMOST pixel
 * of that page's row. Bits are set directly into the packed buffer — no
 * separate x/y pixel array — so there is nothing else that could hold a
 * transposed copy of the frame.
 */

import { fontWidth4x5, fontPrint4x5 } from "./param_pages/font4x5.mjs";
import { asciiFold } from './param_pages/render_page.mjs';

const WIDTH = 128;
const HEIGHT = 64;
const BUFFER_SIZE = 1024; /* (WIDTH * HEIGHT) / 8 */

/*
 * font4x5 has NO LOWERCASE. Its CHARS run is uppercase, digits and
 * punctuation, so `print("cutoff")` draws precisely nothing -- and
 * `fontWidth4x5` still returns a width for it, so a layout reserves space for
 * glyphs that never appear. Measured on hardware 2026-09-10: the E16 drew its
 * header bar and a grid of identical value readouts with every parameter name
 * missing, which reads as a corrupted framebuffer rather than as absent text.
 *
 * render_page_movy.mjs has always folded and upper-cased before drawing
 * (`caps()`), which is why the knob grid never showed this. Doing it HERE
 * rather than in the callers keeps print() and textWidth() describing the same
 * string -- the width lie is half the bug, and a caller that upper-cases for
 * one and not the other reintroduces it.
 */
function caps(s) {
    return asciiFold(String(s === null || s === undefined ? "" : s)).toUpperCase();
}

export function createCanvas() {
    const buf = new Uint8Array(BUFFER_SIZE);

    /* Every write goes through here. Bounds-checked so a page that overflows
     * 128x64 clips silently instead of throwing or wrapping into the wrong
     * byte — matching how the harness and the real device both behave. */
    const setPixel = (x, y, color) => {
        x |= 0; y |= 0;
        if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
        const byteIdx = (y >> 3) * WIDTH + x;
        const bit = y & 7;
        if (color) buf[byteIdx] |= (1 << bit);
        else buf[byteIdx] &= ~(1 << bit);
    };

    const fillRect = (x, y, w, h, color) => {
        x |= 0; y |= 0; w |= 0; h |= 0;
        for (let yy = y; yy < y + h; yy++) {
            for (let xx = x; xx < x + w; xx++) setPixel(xx, yy, color);
        }
    };

    /* font4x5 draws by calling ctx.fillRect itself (it runs solid spans, not
     * pixel-by-pixel), so handing it THIS canvas's own fillRect is enough —
     * no separate glyph blitter to keep in sync with the one the grid uses. */
    /* Both go through caps() -- see the note above it. Keeping them in step is
     * the point: a width that describes a string the print cannot draw is how
     * this hid. */
    const print = (x, y, text, color) => fontPrint4x5({ fillRect }, x, y, caps(text), color);
    const textWidth = (text) => fontWidth4x5(caps(text));

    /* Bresenham, ported from js_display_draw_line the same way the harness's
     * ctx.line does — horizontal/vertical runs short-circuited, general case
     * walked one pixel at a time through the bounds-checked setPixel. */
    const drawLine = (x0, y0, x1, y1, color) => {
        x0 |= 0; y0 |= 0; x1 |= 0; y1 |= 0;
        let dx = x1 - x0, dy = y1 - y0;
        if (dx === 0 && dy === 0) { setPixel(x0, y0, color); return; }
        if (dx === 0) {
            for (let y = Math.min(y0, y1); y <= Math.max(y0, y1); y++) setPixel(x0, y, color);
            return;
        }
        if (dy === 0) {
            for (let x = Math.min(x0, x1); x <= Math.max(x0, x1); x++) setPixel(x, y0, color);
            return;
        }
        const sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
        dx = Math.abs(dx); dy = Math.abs(dy);
        let err = dx - dy;
        for (;;) {
            setPixel(x0, y0, color);
            if (x0 === x1 && y0 === y1) break;
            const e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    };

    const clear = () => buf.fill(0);

    /* Returns the LIVE buffer, not a copy — callers that want a snapshot
     * before the next draw should slice it themselves. Exactly 1024 bytes,
     * always: it is never resized after creation. */
    const toBuffer = () => buf;

    return {
        width: WIDTH, height: HEIGHT,
        /* the shared param_pages draw-context surface */
        fillRect, print, textWidth, drawLine, clear, setPixel,
        toBuffer,
    };
}
