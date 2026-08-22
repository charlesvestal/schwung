/**
 * font5x3.mjs — schwung-movy's condensed 5x3 font, ported
 * (`src/font/{glyphs5x3,index5x3,blit}.ts`, © 2026 megadake, MIT —
 * https://github.com/DimaDake/schwung-movy).
 *
 * Movy uses this only for the enum square (a 16x16 knob box): the device's
 * regular 5x7 font cannot fit two lines in that space, so Movy rasterised a
 * second, smaller font and shows two short abbreviated words stacked
 * ("LOW"/"PAS" for "Low Pass") instead of one truncated line. Nothing else
 * in this library uses it — the header, labels and every other string still
 * go through the device's own 5x7 print(), same as render_page.mjs.
 *
 * Glyph format: [advance, yOff, w, h, ...rowBits], bit0 = leftmost pixel —
 * identical encoding to Movy's source, copied verbatim rather than
 * re-derived, so the glyph shapes are exactly what Movy ships.
 */

/* The last three are NOT from Movy — see G5_EXTRA below. */
const CHARS5 = ' !"\'()+,-./:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%<>=?*#&_\\^';

const G5 = [
  [4,0,0,0],// ' '
  [4,0,3,5,1,1,1,0,1],// '!'
  [4,0,3,5,5,5,0,0,0],// '"'
  [4,0,3,5,2,2,0,0,0],// '\''
  [4,0,3,5,2,1,1,1,2],// '('
  [4,0,3,5,1,2,2,2,1],// ')'
  [4,0,3,5,0,2,7,2,0],// '+'
  [4,0,3,5,0,0,3,3,2],// ','
  [4,0,3,5,0,0,7,0,0],// '-'
  [4,0,3,5,0,0,0,3,3],// '.'
  [4,0,3,5,4,4,2,1,1],// '/'
  [4,0,3,5,3,3,0,3,3],// ':'
  [4,0,3,5,7,5,5,5,7],// '0'
  [4,0,3,5,3,2,2,2,2],// '1'
  [4,0,3,5,7,4,7,1,7],// '2'
  [4,0,3,5,7,4,6,4,7],// '3'
  [4,0,3,5,5,5,7,4,4],// '4'
  [4,0,3,5,7,1,7,4,7],// '5'
  [4,0,3,5,7,1,7,5,7],// '6'
  [4,0,3,5,7,4,4,4,4],// '7'
  [4,0,3,5,7,5,7,5,7],// '8'
  [4,0,3,5,7,5,7,4,7],// '9'
  [4,0,3,5,2,7,5,5,5],// 'A'
  [4,0,3,5,7,5,3,5,7],// 'B'
  [4,0,3,5,7,1,1,1,7],// 'C'
  [4,0,3,5,3,5,5,5,3],// 'D'
  [4,0,3,5,7,1,3,1,7],// 'E'
  [4,0,3,5,7,1,3,1,1],// 'F'
  [4,0,3,5,7,1,5,5,7],// 'G'
  [4,0,3,5,5,5,7,5,5],// 'H'
  [4,0,3,5,7,2,2,2,7],// 'I'
  [4,0,3,5,4,4,4,5,7],// 'J'
  [4,0,3,5,5,5,3,5,5],// 'K'
  [4,0,3,5,1,1,1,1,7],// 'L'
  [4,0,3,5,5,7,5,5,5],// 'M'
  [4,0,3,5,5,3,5,5,5],// 'N'
  [4,0,3,5,7,5,5,5,7],// 'O'
  [4,0,3,5,7,5,7,1,1],// 'P'
  [4,0,3,5,3,5,5,7,2],// 'Q'
  [4,0,3,5,7,5,3,5,5],// 'R'
  [4,0,3,5,6,1,2,4,3],// 'S'
  [4,0,3,5,7,2,2,2,2],// 'T'
  [4,0,3,5,5,5,5,5,7],// 'U'
  [4,0,3,5,5,5,5,5,2],// 'V'
  [4,0,3,5,5,5,5,7,7],// 'W'
  [4,0,3,5,5,5,2,5,5],// 'X'
  [4,0,3,5,5,5,7,2,2],// 'Y'
  [4,0,3,5,7,4,2,1,7],// 'Z'
  [4,0,3,5,5,4,2,1,5],// '%'
  [4,0,3,5,4,2,1,2,4],// '<'
  [4,0,3,5,1,2,4,2,1],// '>'
  [4,0,3,5,0,7,0,7,0],// '='
  [4,0,3,5,7,4,6,0,2],// '?'
  [4,0,3,5,2,7,2,5,0],// '*'

  /* --- not from Movy ------------------------------------------------------
   * Movy's set covers its own module fleet. Drawing Schwung's whole fleet
   * (3422 labels + values across 76 modules) through this font needs exactly
   * five more characters, and every one of them carries meaning that folding
   * would destroy: '#' is a note name (C#, D#), '&' is an LFO shape (S&H),
   * '_' survives in file stems and raw enum options (LEAP_OUTWARD), and '\'
   * and '^' are shape GLYPHS in their own right — several modules name their
   * LFO waveforms "/\-_" and "SINE^". Dropping any of them would silently
   * render "C" for "C#" or an empty box for a waveform name. Same 3x5 cell,
   * bit0 = leftmost, so they measure and blit identically to the ported
   * glyphs. tests/host/test_param_pages_movy.sh sweeps every label, value and
   * enum option in the fleet and fails if a sixth is ever needed. */
  [4,0,3,5,5,7,5,7,5],// '#'
  [4,0,3,5,2,5,2,5,6],// '&'
  [4,0,3,5,0,0,0,0,7],// '_'
  [4,0,3,5,1,1,2,4,4],// '\\'  (the mirror of '/')
  [4,0,3,5,2,5,0,0,0],// '^'
];

const FALLBACK_ADV = 4;
const GAP = 0;

function glyphFor(ch) {
    const idx = CHARS5.indexOf(ch);
    return idx >= 0 ? G5[idx] : null;
}

export const FONT5_HEIGHT = 5;

/** Per-character advance, so callers can size a cell without measuring. */
export const FONT5_ADVANCE = FALLBACK_ADV;

/**
 * Characters in `str` this font cannot draw. A missing glyph advances but
 * renders as NOTHING, exactly like the device font's own missing glyphs — so
 * this is the same class of silent bug `harness.missingGlyphs` catches for
 * 5x7 text, and needs the same guard now that labels no longer go through
 * ctx.print(). Feed it uppercased, ascii-folded text: this font has no
 * lowercase by design.
 */
export function missingGlyphs5x3(str) {
    const out = new Set();
    for (const ch of String(str == null ? "" : str)) if (glyphFor(ch) === null) out.add(ch);
    return out;
}

/**
 * A measuring stand-in for `render_page.mjs`'s fitText/shortenLabel, which
 * measure through `ctx.textWidth`. Passing this instead of the real draw
 * context runs all of that label-shortening logic against the condensed font
 * — no second implementation to keep in step.
 */
export const FONT5_MEASURE = { textWidth: fontWidth5x3 };

/** Fold to what this font can actually draw: ASCII, uppercase. */
export function upper5x3(str) {
    return String(str == null ? "" : str).toUpperCase();
}

export function fontWidth5x3(str) {
    let w = 0;
    const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(s[i]);
        w += g ? g[0] : FALLBACK_ADV;
        if (i < s.length - 1) w += GAP;
    }
    return w;
}

export function fontPrint5x3(ctx, x, y, str, color) {
    let cx = x;
    const s = String(str == null ? "" : str);
    for (let i = 0; i < s.length; i++) {
        const g = glyphFor(s[i]);
        if (!g) { cx += FALLBACK_ADV; continue; }
        const yOff = g[1], w = g[2], h = g[3];
        for (let row = 0; row < h; row++) {
            const bits = g[4 + row];
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
        if (i < s.length - 1) cx += GAP;
    }
}

/**
 * schwung-movy renderer/shorten.ts enumSquareLines, ported verbatim: an enum
 * option's display text -> the two 3-char lines the box shows. A bare number
 * (an octave, a voice count) stays whole on the first line rather than being
 * split, so "-3" is never mistaken for "3".
 */
export function enumSquareLines(value) {
    const num = String(value == null ? "" : value).trim();
    if (/^[+-]?\d+$/.test(num)) return [num, ""];
    /*
     * "_" always separates. "-" separates only BETWEEN two word characters --
     * "low-cut" is two words, but "+-1" is one value meaning plus-or-minus one,
     * and splitting it produced "+" on one line and "1" on the other, which
     * drops the minus entirely. eucalypso and superarp both declare it.
     */
    const parts = String(value).toUpperCase()
        .replace(/_/g, " ")
        .replace(/(?<=[A-Z0-9])-(?=[A-Z0-9])/g, " ")
        .trim().split(/\s+/);
    if (parts.length >= 2) {
        return [parts[0].substring(0, 3), parts[1].substring(0, 3)];
    }
    const w = parts[0] || "";

    /*
     * A "+" INSIDE a word is a break opportunity, and the "+" itself must stay
     * visible.
     *
     * Junologue Chorus declares modes I / I+II / II. With no space or
     * underscore to break on, "I+II" fell to the blind 3+3 slice below and
     * came out as "I+I" over "I" -- which is not "I+II" split badly, it is a
     * DIFFERENT VALUE on screen, and the top line is indistinguishable from
     * mode "I".
     *
     * The "+" carries the meaning ("both", "plus an octave"), so it is
     * attached to whichever line has room for it rather than dropped: "I+"/"II"
     * where the left side fits in three, "OSC"/"+2" where it does not. Dropping
     * it would make "Osc1+2" and "Osc1 2" identical.
     *
     * A LEADING or TRAILING "+" is not a break -- "+1", "+3rd", "+Oct" and
     * "Comb+" are single tokens whose "+" is a sign or a suffix, and splitting
     * them would put a bare "+" on a line by itself.
     */
    const plus = w.indexOf("+");
    if (plus > 0 && plus < w.length - 1) {
        const left = w.substring(0, plus + 1);          /* includes the + */
        const right = w.substring(plus + 1);
        return left.length <= 3
            ? [left, right.substring(0, 3)]
            : [w.substring(0, 3), ("+" + right).substring(0, 3)];
    }

    return [w.substring(0, 3), w.substring(3, 6)];
}
