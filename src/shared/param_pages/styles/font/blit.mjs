/**
 * styles/font/blit.mjs — font4x5.mjs's own draw and measure loops, with the
 * glyph table lifted out into a parameter.
 *
 * The shipping printer closes over its own `G`, so there is no way to draw a
 * candidate table with it. This is that function with `G` passed in and
 * nothing else changed — same run-length scan, same `advance already carries
 * the inter-glyph gap` measurement — so a specimen rendered here is what the
 * device would put on the OLED if the table were swapped in.
 *
 * Keeping it a copy rather than reworking font4x5.mjs is deliberate: the only
 * change this task is allowed to make to that file is additive, because it is
 * on the shipping draw path and this directory is not.
 *
 * Node-only in practice. Nothing here ships to the device.
 */

export const CHARS = " !\"'()+,-./:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%<>=?*#&_\\^";

const FALLBACK_ADV = 5;

function glyphFor(glyphs, ch) {
    const i = CHARS.indexOf(ch);
    return i >= 0 ? glyphs[i] : null;
}

export function fontWidth(glyphs, str) {
    let w = 0;
    const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(glyphs, s[i]);
        w += g ? g[0] : FALLBACK_ADV;
    }
    return w > 0 ? w - 1 : 0;
}

export function fontPrint(ctx, x, y, str, color, glyphs) {
    let cx = x;
    const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(glyphs, s[i]);
        if (!g) { cx += FALLBACK_ADV; continue; }
        const yOff = g[1], w = g[2], h = g[3];
        for (let row = 0; row < h; row++) {
            const bits = g[4 + row];
            if (!bits) continue;
            let col = 0;
            while (col < w) {
                if (bits & (1 << col)) {
                    const start = col;
                    while (col < w && (bits & (1 << col))) col++;
                    ctx.fillRect(cx + start, y + yOff + row, col - start, 1, color);
                } else col++;
            }
        }
        cx += g[0];
    }
    return cx;
}
