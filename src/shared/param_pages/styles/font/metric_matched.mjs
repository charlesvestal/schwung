/**
 * styles/font/metric_matched.mjs — SET 12, option 11: metric-matched —
 * new skeletons inside the shipping font advance widths, to the pixel.
 *
 * WHY THIS ONE IS DIFFERENT. The other ten redraw the letterforms AND the
 * metrics, and every legible one of them comes out wider: AMPLITUDE goes from
 * 42px to 45-47, KEYTRIG from 30px to 33-35, which is past the 32px knob cell
 * it currently fits. The two that are narrower are narrower because they are
 * three columns wide, which is the construction font4x5.mjs exists to replace.
 * So the cost of adopting any of them is a layout pass over the fleet, and the
 * cost is not in the letterforms at all — six of the ten measure IDENTICALLY on
 * short strings and diverge only over a long one.
 *
 * This option removes that trade entirely. Every advance here equals the
 * shipping advance for the same character, so every string in the product
 * renders at exactly the width it does today. Adopting it is a diff to one
 * table and nothing else. tests/host/test_style_catalog.sh asserts the property
 * against font4x5.mjs directly, off the `metricMatched` flag on the option, so
 * it cannot drift once someone nudges a glyph.
 *
 * THE CONSTRUCTION RULE: top-loaded. The shipping face is a neutral grotesque
 * with its waist dead centre and a matching bar at each terminal; this one
 * moves the weight up.
 *
 *   - A crossbar rides at row 1 rather than row 2 — A E F H K N Y Z and the
 *     digits 4 and 9 all carry it one row higher, which gives the face a small
 *     tight head over a long open leg instead of two equal halves.
 *   - A stemmed letter takes a FULL-WIDTH flat head: B C D E F G J P R S all
 *     start on `####` where the shipping font starts on `###.` or `.###`.
 *   - A foot is cut back, so the bottom row is narrower than the top: D closes
 *     to `###.`, L stops one column short, C and G tuck to `.###`.
 *   - The round letters O Q 0 are the stated exception. A flat head needs a
 *     stem to hang on; with no stem it is a rectangle, and a rectangular O one
 *     pixel from a rectangular D is not a face, it is a defect.
 *
 * Two glyphs are legibility overrides rather than rule, and both are places
 * this codebase has already been burnt:
 *
 *   - U closes FLAT (`####`) rather than cut back. font4x5.mjs records the
 *     3-wide font it replaced rendering SAW as SAU, and the rounded option in
 *     this same directory had to buy its U a fifth column after AMPLITUDE came
 *     out AMPLITVDE. At advance 5 there is no fifth column to buy, so the U is
 *     settled by being square-bottomed, which no V ever is.
 *   - I is the forced exception. Its advance is 2, so its body is one pixel
 *     wide and one pixel tall by five — a full stem IS the shipping glyph,
 *     and there is no other way to draw it. It takes the cut foot the rule
 *     gives D and L, expressed the only way a 1px column can: one row short.
 *
 * Glyph format is font4x5.mjs exactly — [advance, yOff, w, h, ...rowBits] with
 * bit0 = leftmost pixel — in CHARS order. Nothing here ships; production draw
 * code imports nothing from this directory.
 */

/* Row bit values, 4-wide: col0=1 col1=2 col2=4 col3=8
 *   ....=0  #...=1  .#..=2  ##..=3  ..#.=4  #.#.=5  .##.=6  ###.=7
 *   ...#=8  #..#=9  .#.#=10 ##.#=11 ..##=12 #.##=13 .###=14 ####=15
 * 5-wide adds col4=16. The picture in each trailing comment is the authored
 * form; the numbers are derived from it. */
export const GLYPHS = [
    [3, 0, 0, 5],                      /* space     advance only */
    [2, 0, 1, 5, 1, 1, 1, 0, 1],       /* !         # # # . # */
    [4, 0, 3, 5, 5, 5, 0, 0, 0],       /* \"        #.# #.# ... ... ... */
    [2, 0, 1, 5, 1, 1, 0, 0, 0],       /* '         # # . . . */
    [3, 0, 2, 5, 2, 1, 1, 1, 2],       /* (         .# #. #. #. .# */
    [3, 0, 2, 5, 1, 2, 2, 2, 1],       /* )         #. .# .# .# #. */
    [4, 0, 3, 5, 0, 2, 7, 2, 0],       /* +         ... .#. ### .#. ... */
    [3, 0, 2, 5, 0, 0, 0, 2, 1],       /* ,         .. .. .. .# #. */
    [4, 0, 3, 5, 0, 0, 7, 0, 0],       /* -         ... ... ### ... ... */
    [2, 0, 1, 5, 0, 0, 0, 0, 1],       /* .         . . . . # */
    [5, 0, 4, 5, 8, 8, 6, 1, 1],       /* /         ...# ...# .##. #... #... */
    [2, 0, 1, 5, 0, 1, 0, 1, 0],       /* :         . # . # . */
    [5, 0, 4, 5, 6, 9, 9, 9, 6],       /* 0         .##. #..# #..# #..# .##. */
    [4, 0, 3, 5, 3, 2, 2, 2, 7],       /* 1         ##. .#. .#. .#. ### */
    [5, 0, 4, 5, 15, 8, 6, 1, 15],     /* 2         #### ...# .##. #... #### */
    [5, 0, 4, 5, 15, 8, 6, 8, 7],      /* 3         #### ...# .##. ...# ###. */
    [5, 0, 4, 5, 9, 15, 8, 8, 8],      /* 4         #..# #### ...# ...# ...# */
    [5, 0, 4, 5, 15, 1, 7, 8, 7],      /* 5         #### #... ###. ...# ###. */
    [5, 0, 4, 5, 14, 1, 15, 9, 6],     /* 6         .### #... #### #..# .##. */
    [5, 0, 4, 5, 15, 8, 4, 2, 2],      /* 7         #### ...# ..#. .#.. .#.. */
    [5, 0, 4, 5, 6, 9, 6, 9, 6],       /* 8         .##. #..# .##. #..# .##. */
    [5, 0, 4, 5, 6, 9, 15, 8, 7],      /* 9         .##. #..# #### ...# ###. */
    [5, 0, 4, 5, 6, 15, 9, 9, 9],      /* A         .##. #### #..# #..# #..# */
    [5, 0, 4, 5, 15, 9, 7, 9, 7],      /* B         #### #..# ###. #..# ###. */
    [5, 0, 4, 5, 15, 1, 1, 1, 14],     /* C         #### #... #... #... .### */
    [5, 0, 4, 5, 15, 9, 9, 9, 7],      /* D         #### #..# #..# #..# ###. */
    [5, 0, 4, 5, 15, 7, 1, 1, 15],     /* E         #### ###. #... #... #### */
    [5, 0, 4, 5, 15, 7, 1, 1, 1],      /* F         #### ###. #... #... #... */
    [5, 0, 4, 5, 15, 1, 13, 9, 14],    /* G         #### #... #.## #..# .### */
    [5, 0, 4, 5, 9, 15, 9, 9, 9],      /* H         #..# #### #..# #..# #..# */
    [2, 0, 1, 5, 1, 1, 1, 1, 0],       /* I         # # # # . */
    [5, 0, 4, 5, 15, 8, 8, 9, 6],      /* J         #### ...# ...# #..# .##. */
    [5, 0, 4, 5, 5, 3, 5, 9, 9],       /* K         #.#. ##.. #.#. #..# #..# */
    [5, 0, 4, 5, 1, 1, 1, 1, 7],       /* L         #... #... #... #... ###. */
    [6, 0, 5, 5, 27, 21, 17, 17, 17],  /* M         ##.## #.#.# #...# #...# #...# */
    [5, 0, 4, 5, 11, 13, 9, 9, 9],     /* N         ##.# #.## #..# #..# #..# */
    [5, 0, 4, 5, 6, 9, 9, 9, 6],       /* O         .##. #..# #..# #..# .##. */
    [5, 0, 4, 5, 15, 9, 7, 1, 1],      /* P         #### #..# ###. #... #... */
    [5, 0, 4, 5, 6, 9, 9, 5, 10],      /* Q         .##. #..# #..# #.#. .#.# */
    [5, 0, 4, 5, 15, 9, 7, 5, 9],      /* R         #### #..# ###. #.#. #..# */
    [5, 0, 4, 5, 15, 1, 14, 8, 7],     /* S         #### #... .### ...# ###. */
    [4, 0, 3, 5, 7, 7, 2, 2, 2],       /* T         ### ### .#. .#. .#. */
    [5, 0, 4, 5, 9, 9, 9, 9, 15],      /* U         #..# #..# #..# #..# #### */
    [6, 0, 5, 5, 17, 17, 10, 10, 4],   /* V         #...# #...# .#.#. .#.#. ..#.. */
    [6, 0, 5, 5, 17, 21, 21, 21, 10],  /* W         #...# #.#.# #.#.# #.#.# .#.#. */
    [5, 0, 4, 5, 9, 9, 6, 9, 9],       /* X         #..# #..# .##. #..# #..# */
    [4, 0, 3, 5, 5, 2, 2, 2, 2],       /* Y         #.# .#. .#. .#. .#. */
    [5, 0, 4, 5, 15, 12, 6, 3, 15],    /* Z         #### ..## .##. ##.. #### */
    [5, 0, 4, 5, 9, 8, 6, 1, 9],       /* %         #..# ...# .##. #... #..# */
    [4, 0, 3, 5, 4, 2, 1, 2, 4],       /* <         ..# .#. #.. .#. ..# */
    [4, 0, 3, 5, 1, 2, 4, 2, 1],       /* >         #.. .#. ..# .#. #.. */
    [4, 0, 3, 5, 0, 7, 0, 7, 0],       /* =         ... ### ... ### ... */
    [5, 0, 4, 5, 15, 8, 6, 0, 2],      /* ?         #### ...# .##. .... .#.. */
    [4, 0, 3, 5, 5, 2, 5, 0, 0],       /* *         #.# .#. #.# ... ... */
    [5, 0, 4, 5, 10, 15, 10, 15, 10],  /* #         .#.# #### .#.# #### .#.# */
    [5, 0, 4, 5, 6, 9, 6, 5, 10],      /* &         .##. #..# .##. #.#. .#.# */
    [5, 0, 4, 5, 0, 0, 0, 0, 15],      /* _         .... .... .... .... #### */
    [5, 0, 4, 5, 1, 1, 6, 8, 8],       /* backslash #... #... .##. ...# ...# */
    [4, 0, 3, 5, 2, 5, 0, 0, 0],       /* ^         .#. #.# ... ... ... */
];
