/**
 * font_big_num.mjs — the big-number face: digits, plus and minus.
 *
 * TRANSCRIBED from schwung-movy's `src/font/glyphs-big.ts`
 * (© 2026 megadake, MIT — https://github.com/DimaDake/schwung-movy), which
 * describes it as a Nokia 13px bitmap font at cap-height 11. Movy uses it for
 * the preset number (`drawPresetValue`, src/renderer/knob.ts) and for the
 * value row. Same relationship as font5x3.mjs, which is also vendored from
 * there.
 *
 * PROVENANCE, STATED RATHER THAN ASSUMED: movy generated this by rasterising an
 * OTF it does not vendor (scripts/generate_font.py), so the underlying typeface
 * is identified only as "Nokia". The bitmap table is MIT via movy; the face it
 * came from is not verified here. Recorded because SCH-50 exists to reduce
 * exactly this kind of exposure, and a derived asset should say what it is.
 *
 * WHY NOT TAMZEN. The first cut of this cell used Tamzen 8x16 — a terminal
 * face, 1px stems, and a slashed zero in every size on disk. At 9 rows in a
 * 15-row box it still read as thin. This is 11 rows with 2px stems and a plain
 * bowl, which is what a value meant to be read across a room wants.
 *
 * ONLY 12 GLYPHS, matching what `bigNumberText` can emit — the digits, the
 * sign, and the "--" an unread value draws. `missingGlyphs` reports anything
 * else rather than drawing it wrong, and tests/host/test_big_number_font.sh
 * sweeps the fleet against that.
 *
 * The advance carries movy's 1px inter-glyph gap folded in: its blitter adds
 * BIG_GAP separately, ours does not, so every advance here is theirs plus one.
 * Glyph format is otherwise unchanged and identical to font4x5.mjs —
 * [advance, yOff, w, h, ...rowBits], bit0 = leftmost pixel.
 */

const CHARS = '0123456789+-';
const G = [
    [10, 0, 9, 11, 62, 127, 99, 99, 99, 99, 99, 99, 99, 127, 62],  /* 0 */
    [9, 0, 8, 11, 48, 56, 60, 60, 48, 48, 48, 48, 48, 48, 48],  /* 1 */
    [10, 0, 9, 11, 62, 127, 99, 96, 112, 56, 28, 14, 7, 127, 127],  /* 2 */
    [10, 0, 9, 11, 62, 127, 99, 96, 60, 124, 96, 96, 99, 127, 62],  /* 3 */
    [10, 0, 9, 11, 48, 56, 60, 54, 55, 51, 127, 127, 48, 48, 48],  /* 4 */
    [10, 0, 9, 11, 63, 63, 3, 3, 63, 127, 96, 96, 99, 127, 62],  /* 5 */
    [10, 0, 9, 11, 62, 127, 3, 63, 127, 99, 99, 99, 99, 127, 62],  /* 6 */
    [10, 0, 9, 11, 127, 127, 48, 48, 24, 24, 12, 12, 12, 12, 12],  /* 7 */
    [10, 0, 9, 11, 62, 127, 99, 99, 62, 127, 99, 99, 99, 127, 62],  /* 8 */
    [10, 0, 9, 11, 62, 127, 99, 99, 99, 127, 126, 96, 99, 127, 62],  /* 9 */
    [9, 3, 8, 6, 12, 12, 63, 63, 12, 12],  /* + */
    [8, 5, 7, 2, 31, 31],  /* - */
];

export const HEIGHT = 11;

function glyphFor(ch) { const i = CHARS.indexOf(ch); return i >= 0 ? G[i] : null; }

export function fontWidth(str) {
    let w = 0; const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) { const g = glyphFor(s[i]); w += g ? g[0] : 0; }
    return w > 0 ? w - 1 : 0;
}

export function fontPrint(ctx, x, y, str, color) {
    let cx = x; const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(s[i]);
        if (!g) continue;
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
}

export function missingGlyphs(str) {
    const out = new Set();
    for (const ch of String(str == null ? "" : str)) if (glyphFor(ch) === null) out.add(ch);
    return out;
}

export const MEASURE = { textWidth: fontWidth };
