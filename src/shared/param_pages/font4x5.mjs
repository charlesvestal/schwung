/**
 * font4x5.mjs — a proportional 5-tall font for the Movy knob grid.
 *
 * The metrics are cut for this grid and for nothing else. A 128px header has
 * to carry a real parameter name, so: five rows tall, proportional advance
 * with the 1px inter-glyph gap folded in, a typical cap 4 wide on a 5px
 * advance. The number in brackets is the glyph WIDTH; the advance is one more.
 *
 *      A(4)   M(5)   I(1)  T(3)   U(4)      5 rows tall
 *      .##.   ##.##   #    ###    #..#      proportional width, 1px gap
 *      ####   #.#.#   #    ###    #..#      typical cap 4 wide -> 5px advance
 *      #..#   #...#   #    .#.    #..#
 *      #..#   #...#   #    .#.    #..#
 *      #..#   #...#   .    .#.    ####
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
 * THE LETTERFORMS (SCH-50). The skeletons below are the `metric-matched`
 * option from the SCH-50 catalog, adopted because it is the only redraw in
 * that set that costs no layout pass: **every advance equals the advance it
 * replaced, to the pixel**, so every string in the product renders at exactly
 * the width it did before. The metrics above did not move and must not — a
 * 4-wide cap on a 5px advance is the geometry every cell in this grid was cut
 * for, and changing it moves text everywhere. What changed is what fills that
 * box.
 *
 * THE CONSTRUCTION RULE: top-loaded. The face this replaces was a neutral
 * grotesque with its waist dead centre and a matching bar at each terminal;
 * this one moves the weight up.
 *
 *   - A crossbar rides at row 1 rather than row 2 — A E F H K N Y Z and the
 *     digits 4 and 9 all carry it one row higher, giving a small tight head
 *     over a long open leg instead of two equal halves.
 *   - A stemmed letter takes a FULL-WIDTH flat head: B C D E F G J P R S all
 *     start on `####` where the old face started on `###.` or `.###`.
 *   - A foot is cut back, so the bottom row is narrower than the top: D closes
 *     to `###.`, L stops one column short, C and G tuck to `.###`.
 *   - The round letters O Q 0 are the stated exception. A flat head needs a
 *     stem to hang on; with no stem it is a rectangle, and a rectangular O one
 *     pixel from a rectangular D is not a face, it is a defect.
 *
 * Two glyphs are legibility overrides rather than rule, and both are places
 * this codebase has already been burnt:
 *
 *   - U closes FLAT (`####`) rather than cut back. The 3-wide font this file
 *     replaced rendered SAW as SAU, and a rounded candidate in the catalog had
 *     to buy its U a fifth column after AMPLITUDE came out AMPLITVDE. At
 *     advance 5 there is no fifth column to buy, so the U is settled by being
 *     square-bottomed, which no V ever is.
 *   - I is the forced exception. Its advance is 2, so its body is one pixel
 *     wide by five tall — a full stem IS the only way to draw it. It takes the
 *     cut foot the rule gives D and L, expressed the only way a 1px column
 *     can: one row short.
 */

export const CHARS = " !\"'()+,-./:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%<>=?*#&_\\^";

/* Row bit values, 4-wide: col0=1 col1=2 col2=4 col3=8
 *   ....=0  #...=1  .#..=2  ##..=3  ..#.=4  #.#.=5  .##.=6  ###.=7
 *   ...#=8  #..#=9  .#.#=10 ##.#=11 ..##=12 #.##=13 .###=14 ####=15
 * 5-wide adds col4=16. */
const G = [
    [3, 0, 0, 5],                       /* space     advance only */
    [2, 0, 1, 5, 1, 1, 1, 0, 1],        /* !         # # # . # */
    [4, 0, 3, 5, 5, 5, 0, 0, 0],        /* "         #.# #.# ... ... ... */
    [2, 0, 1, 5, 1, 1, 0, 0, 0],        /* '         # # . . . */
    [3, 0, 2, 5, 2, 1, 1, 1, 2],        /* (         .# #. #. #. .# */
    [3, 0, 2, 5, 1, 2, 2, 2, 1],        /* )         #. .# .# .# #. */
    [4, 0, 3, 5, 0, 2, 7, 2, 0],        /* +         ... .#. ### .#. ... */
    [3, 0, 2, 5, 0, 0, 0, 2, 1],        /* ,         .. .. .. .# #. */
    [4, 0, 3, 5, 0, 0, 7, 0, 0],        /* -         ... ... ### ... ... */
    [2, 0, 1, 5, 0, 0, 0, 0, 1],        /* .         . . . . # */
    [5, 0, 4, 5, 8, 8, 6, 1, 1],        /* /         ...# ...# .##. #... #... */
    [2, 0, 1, 5, 0, 1, 0, 1, 0],        /* :         . # . # . */

    [5, 0, 4, 5, 6, 9, 9, 9, 6],        /* 0         .##. #..# #..# #..# .##. */
    [4, 0, 3, 5, 3, 2, 2, 2, 7],        /* 1         ##. .#. .#. .#. ### */
    [5, 0, 4, 5, 15, 8, 6, 1, 15],      /* 2         #### ...# .##. #... #### */
    [5, 0, 4, 5, 15, 8, 6, 8, 7],       /* 3         #### ...# .##. ...# ###. */
    [5, 0, 4, 5, 9, 15, 8, 8, 8],       /* 4         #..# #### ...# ...# ...# */
    [5, 0, 4, 5, 15, 1, 7, 8, 7],       /* 5         #### #... ###. ...# ###. */
    [5, 0, 4, 5, 14, 1, 15, 9, 6],      /* 6         .### #... #### #..# .##. */
    [5, 0, 4, 5, 15, 8, 4, 2, 2],       /* 7         #### ...# ..#. .#.. .#.. */
    [5, 0, 4, 5, 6, 9, 6, 9, 6],        /* 8         .##. #..# .##. #..# .##. */
    [5, 0, 4, 5, 6, 9, 15, 8, 7],       /* 9         .##. #..# #### ...# ###. */

    [5, 0, 4, 5, 6, 9, 15, 9, 9],       /* A         .##. #..# #### #..# #..# */
    [5, 0, 4, 5, 15, 9, 7, 9, 7],       /* B         #### #..# ###. #..# ###. */
    [5, 0, 4, 5, 15, 1, 1, 1, 14],      /* C         #### #... #... #... .### */
    [5, 0, 4, 5, 7, 9, 9, 9, 7],        /* D         ###. #..# #..# #..# ###. */
    [5, 0, 4, 5, 15, 1, 7, 1, 15],      /* E         #### #... ###. #... #### */
    [5, 0, 4, 5, 15, 7, 1, 1, 1],       /* F         #### ###. #... #... #... */
    [5, 0, 4, 5, 15, 1, 13, 9, 14],     /* G         #### #... #.## #..# .### */
    [5, 0, 4, 5, 9, 15, 9, 9, 9],       /* H         #..# #### #..# #..# #..# */
    [2, 0, 1, 5, 1, 1, 1, 1, 1],        /* I         # # # # # */
    [5, 0, 4, 5, 15, 8, 8, 9, 6],       /* J         #### ...# ...# #..# .##. */
    [5, 0, 4, 5, 5, 3, 5, 9, 9],        /* K         #.#. ##.. #.#. #..# #..# */
    [5, 0, 4, 5, 1, 1, 1, 1, 15],       /* L         #... #... #... #... #### */
    [6, 0, 5, 5, 17, 27, 21, 17, 17],   /* M         #...# ##.## #.#.# #...# #...# */
    [5, 0, 4, 5, 11, 13, 9, 9, 9],      /* N         ##.# #.## #..# #..# #..# */
    [5, 0, 4, 5, 6, 9, 9, 9, 6],        /* O         .##. #..# #..# #..# .##. */
    [5, 0, 4, 5, 7, 9, 7, 1, 1],        /* P         ###. #..# ###. #... #... */
    [5, 0, 4, 5, 6, 9, 9, 5, 10],       /* Q         .##. #..# #..# #.#. .#.# */
    [5, 0, 4, 5, 15, 9, 7, 5, 9],       /* R         #### #..# ###. #.#. #..# */
    [5, 0, 4, 5, 15, 1, 14, 8, 7],      /* S         #### #... .### ...# ###. */
    [4, 0, 3, 5, 7, 2, 2, 2, 2],        /* T         ### .#. .#. .#. .#. */
    [5, 0, 4, 5, 9, 9, 9, 9, 6],        /* U         #..# #..# #..# #..# .##. */
    [6, 0, 5, 5, 17, 17, 10, 10, 4],    /* V         #...# #...# .#.#. .#.#. ..#.. */
    [6, 0, 5, 5, 17, 21, 21, 21, 10],   /* W         #...# #.#.# #.#.# #.#.# .#.#. */
    [5, 0, 4, 5, 9, 9, 6, 9, 9],        /* X         #..# #..# .##. #..# #..# */
    [4, 0, 3, 5, 5, 2, 2, 2, 2],        /* Y         #.# .#. .#. .#. .#. */
    [5, 0, 4, 5, 15, 12, 6, 3, 15],     /* Z         #### ..## .##. ##.. #### */

    [5, 0, 4, 5, 9, 8, 6, 1, 9],        /* %         #..# ...# .##. #... #..# */
    [4, 0, 3, 5, 4, 2, 1, 2, 4],        /* <         ..# .#. #.. .#. ..# */
    [4, 0, 3, 5, 1, 2, 4, 2, 1],        /* >         #.. .#. ..# .#. #.. */
    [4, 0, 3, 5, 0, 7, 0, 7, 0],        /* =         ... ### ... ### ... */
    [5, 0, 4, 5, 15, 8, 6, 0, 2],       /* ?         #### ...# .##. .... .#.. */
    [4, 0, 3, 5, 5, 2, 5, 0, 0],        /* *         #.# .#. #.# ... ... */
    [5, 0, 4, 5, 10, 15, 10, 15, 10],   /* #         .#.# #### .#.# #### .#.# */
    [5, 0, 4, 5, 6, 9, 6, 5, 10],       /* &         .##. #..# .##. #.#. .#.# */
    [5, 0, 4, 5, 0, 0, 0, 0, 15],       /* _         .... .... .... .... #### */
    [5, 0, 4, 5, 1, 1, 6, 8, 8],        /* \         #... #... .##. ...# ...# */
    [4, 0, 3, 5, 2, 5, 0, 0, 0],        /* ^         .#. #.# ... ... ... */
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

/* Test-only: tests/host/test_style_catalog.sh asserts that no font option —
 * this shipping table included — reproduces any of the nine retired
 * letterforms SCH-50 replaced. The assertion cannot be written without reading
 * the table it compares against, and there is no other reader — nothing on the
 * draw path imports either of these two names. Additive; no behaviour changes. */
export const GLYPHS_FOR_TEST = G;
