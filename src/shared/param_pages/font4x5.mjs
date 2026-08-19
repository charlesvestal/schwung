/**
 * font4x5.mjs — a proportional 5-tall font for the Movy/Elektron knob grid.
 *
 * Measured off Elektron's own UI rather than guessed. Recovering their 128x64
 * screen from a 4x screenshot and segmenting the header text gives:
 *
 *      A(4)   M(5)   I(1)  T(3)   U(4)      5 rows tall
 *      .##.   #...#   #    ###    #..#      proportional width, 1px gap
 *      #..#   ##.##   #    .#.    #..#      typical cap 4 wide -> 5px advance
 *      ####   #.#.#   #    .#.    #..#
 *      #..#   #...#   #    .#.    #..#
 *      #..#   #...#   #    .#.    .##.
 *
 * This sits deliberately between the two fonts this grid tried first, both of
 * which were wrong in opposite directions:
 *
 *   - The device's own 5x7 is TWO ROWS TALLER, which is what forced an 8-row
 *     label band and left no vertical gutter between a label and the knob row
 *     under it. Its 6px monospaced advance also fits only ~5 characters in a
 *     32px cell.
 *   - font5x3 (Movy's condensed font, 3 wide) fixed the height but at 3px the
 *     letterforms collapse: N and K differ only in which row their bar sits
 *     on, A and M by a single top-row pixel, and W reads as U. Real pages
 *     rendered MAIN as "MAIK", SINE as "SIKE", SAW as "SAU".
 *
 * Four pixels is enough for an unambiguous N (`#..#/##.#/#.##/#..#`) against
 * K (`#..#/#.#./##../#.#.`), and proportional advance is doing real work: `I`
 * is one pixel wide, which is most of why a word like AMPLITUDE fits a header.
 *
 * Glyph format matches font5x3.mjs exactly — [advance, yOff, w, h, ...rowBits]
 * with bit0 = leftmost pixel — so the two blit identically and a caller can
 * swap one for the other. font5x3 is still the right font for the enum SQUARE:
 * two stacked lines in a 16px box need 3-wide glyphs to fit three characters
 * per line, which 4-wide cannot.
 *
 * Letterforms for A M I T U D E P L are Elektron's, read straight off the
 * screen. The rest are authored to match their weight and construction.
 */

const CHARS = " !\"'()+,-./:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%<>=?*#&_\\^";

/* Row bit values, 4-wide: col0=1 col1=2 col2=4 col3=8
 *   ....=0  #...=1  .#..=2  ##..=3  ..#.=4  #.#.=5  .##.=6  ###.=7
 *   ...#=8  #..#=9  .#.#=10 ##.#=11 ..##=12 #.##=13 .###=14 ####=15
 * 5-wide adds col4=16. */
const G = [
    [3, 0, 0, 5],                       /* ' ' — advance only */
    [2, 0, 1, 5, 1, 1, 1, 0, 1],        /* ! */
    [4, 0, 3, 5, 5, 5, 0, 0, 0],        /* " */
    [2, 0, 1, 5, 1, 1, 0, 0, 0],        /* ' */
    [3, 0, 2, 5, 2, 1, 1, 1, 2],        /* ( */
    [3, 0, 2, 5, 1, 2, 2, 2, 1],        /* ) */
    [4, 0, 3, 5, 0, 2, 7, 2, 0],        /* + */
    [3, 0, 2, 5, 0, 0, 0, 2, 1],        /* , */
    [4, 0, 3, 5, 0, 0, 7, 0, 0],        /* - */
    [2, 0, 1, 5, 0, 0, 0, 0, 1],        /* . */
    [5, 0, 4, 5, 8, 8, 6, 1, 1],        /* / */
    [2, 0, 1, 5, 0, 1, 0, 1, 0],        /* : */

    [5, 0, 4, 5, 6, 9, 9, 9, 6],        /* 0 */
    [4, 0, 3, 5, 2, 3, 2, 2, 7],        /* 1 */
    [5, 0, 4, 5, 7, 8, 6, 1, 15],       /* 2 */
    [5, 0, 4, 5, 7, 8, 6, 8, 7],        /* 3 */
    [5, 0, 4, 5, 9, 9, 15, 8, 8],       /* 4 */
    [5, 0, 4, 5, 15, 1, 7, 8, 7],       /* 5 */
    [5, 0, 4, 5, 14, 1, 7, 9, 6],       /* 6 */
    [5, 0, 4, 5, 15, 8, 4, 2, 1],       /* 7 */
    [5, 0, 4, 5, 6, 9, 6, 9, 6],        /* 8 */
    [5, 0, 4, 5, 6, 9, 14, 8, 7],       /* 9 */

    [5, 0, 4, 5, 6, 9, 15, 9, 9],       /* A  (Elektron) */
    [5, 0, 4, 5, 7, 9, 7, 9, 7],        /* B */
    [5, 0, 4, 5, 14, 1, 1, 1, 14],      /* C */
    [5, 0, 4, 5, 7, 9, 9, 9, 7],        /* D  (Elektron) */
    [5, 0, 4, 5, 15, 1, 7, 1, 15],      /* E  (Elektron) */
    [5, 0, 4, 5, 15, 1, 7, 1, 1],       /* F */
    [5, 0, 4, 5, 14, 1, 13, 9, 14],     /* G */
    [5, 0, 4, 5, 9, 9, 15, 9, 9],       /* H */
    [2, 0, 1, 5, 1, 1, 1, 1, 1],        /* I  (Elektron) */
    [5, 0, 4, 5, 12, 8, 8, 9, 6],       /* J */
    [5, 0, 4, 5, 9, 5, 3, 5, 9],        /* K */
    [5, 0, 4, 5, 1, 1, 1, 1, 15],       /* L  (Elektron) */
    [6, 0, 5, 5, 17, 27, 21, 17, 17],   /* M  (Elektron) */
    [5, 0, 4, 5, 9, 11, 13, 9, 9],      /* N */
    [5, 0, 4, 5, 6, 9, 9, 9, 6],        /* O */
    [5, 0, 4, 5, 7, 9, 7, 1, 1],        /* P  (Elektron) */
    [5, 0, 4, 5, 6, 9, 9, 5, 10],       /* Q */
    [5, 0, 4, 5, 7, 9, 7, 5, 9],        /* R */
    [5, 0, 4, 5, 14, 1, 6, 8, 7],       /* S */
    [4, 0, 3, 5, 7, 2, 2, 2, 2],        /* T  (Elektron) */
    [5, 0, 4, 5, 9, 9, 9, 9, 6],        /* U  (Elektron) */
    [6, 0, 5, 5, 17, 17, 17, 10, 4],    /* V */
    [6, 0, 5, 5, 17, 17, 21, 21, 10],   /* W */
    [5, 0, 4, 5, 9, 9, 6, 9, 9],        /* X */
    [4, 0, 3, 5, 5, 5, 2, 2, 2],        /* Y */
    [5, 0, 4, 5, 15, 8, 6, 1, 15],      /* Z */

    [5, 0, 4, 5, 9, 8, 6, 1, 9],        /* % */
    [4, 0, 3, 5, 4, 2, 1, 2, 4],        /* < */
    [4, 0, 3, 5, 1, 2, 4, 2, 1],        /* > */
    [4, 0, 3, 5, 0, 7, 0, 7, 0],        /* = */
    [5, 0, 4, 5, 7, 8, 6, 0, 2],        /* ? */
    [4, 0, 3, 5, 5, 2, 5, 0, 0],        /* * */
    [5, 0, 4, 5, 10, 15, 10, 15, 10],   /* # */
    [5, 0, 4, 5, 6, 9, 6, 5, 10],       /* & */
    [5, 0, 4, 5, 0, 0, 0, 0, 15],       /* _ */
    [5, 0, 4, 5, 1, 1, 6, 8, 8],        /* \ */
    [4, 0, 3, 5, 2, 5, 0, 0, 0],        /* ^ */
];

const FALLBACK_ADV = 5;

function glyphFor(ch) {
    const i = CHARS.indexOf(ch);
    return i >= 0 ? G[i] : null;
}

export const FONT4_HEIGHT = 5;

export function fontWidth4x5(str) {
    let w = 0;
    const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(s[i]);
        w += g ? g[0] : FALLBACK_ADV;
    }
    /* The advance already carries the 1px inter-glyph gap; the last glyph does
     * not need one, so a measured string is one pixel narrower than the sum. */
    return w > 0 ? w - 1 : 0;
}

export function fontPrint4x5(ctx, x, y, str, color) {
    let cx = x;
    const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(s[i]);
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
}

/** Characters this font cannot draw — a missing glyph renders as nothing. */
export function missingGlyphs4x5(str) {
    const out = new Set();
    for (const ch of String(str == null ? "" : str)) if (glyphFor(ch) === null) out.add(ch);
    return out;
}

/** Measuring stand-in for render_page.mjs's fitText/shortenLabel, which
 *  measure through `ctx.textWidth`. */
export const FONT4_MEASURE = { textWidth: fontWidth4x5 };
