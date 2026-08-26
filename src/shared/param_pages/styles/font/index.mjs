/**
 * styles/font/index.mjs — SET 12: eleven complete replacements for font4x5.
 *
 * This is the one set in the catalog that is not about style. Nine of the
 * letterforms font4x5.mjs shipped at the time this set was written — A M I T U
 * D E P L — had been traced from another product's screen rather than drawn,
 * and the file said so in its own header and annotated the nine in its table.
 * (It no longer does: the `metric-matched` option below was adopted, and
 * font4x5.mjs now carries its own forms and its own rationale.)
 *
 * Tracing is reproduction of specific expression rather than evocation of a
 * style, which is what separates this set from the knob, fader and fill sets.
 * It is also why replacing the nine is not the fix: the other forty-seven
 * letters were drawn to sit with them, so the SYSTEM is derived even where no
 * individual glyph was traced. Each option below is therefore a complete
 * table for all 57 characters, built from one stated construction rule and
 * carrying nothing forward.
 *
 * The axis is narrow -> wide rather than minimal -> radical, because for a
 * font the thing a reviewer actually has to trade is legibility against how
 * much of a 128px header (or a 32px knob cell) a label consumes:
 *
 *   1-2   four wide, conventional. The safe replacements.
 *   3     three wide, with the three letters that cannot survive it bought
 *         back at four and five.
 *   4-6   four wide, with a rule applied over the whole alphabet: chamfered
 *         bowls, terminal bars, broken counters.
 *   7     three wide with no exceptions at all — the construction this
 *         codebase already tried and rejected once, included so the catalog
 *         measures the claim rather than repeating it.
 *   8-10  wider than the shipping font: a 2px lattice, a diagonal-free
 *         geometry, and a five-column body.
 *   11    OFF the axis, deliberately. Every advance is pinned to the shipping
 *         value, so it is neither narrower nor wider by construction and only
 *         the skeletons change. It was authored last, after the first ten were
 *         measured and every legible one of them turned out 3-5px wider on a
 *         long string -- enough for KEYTRIG to cross the 32px knob cell it
 *         currently fits. That is an advance-width problem rather than a
 *         letterform problem, and this is the option that separates the two.
 *
 * WHAT THE MARKS ARE. Punctuation and the digits are supplied per width
 * family (3, 4 and 5 wide) rather than per option, and several coincide with
 * the shipping font's. That is deliberate and it is not a gap: a `+` or a `:`
 * on a 4x5 grid has one sensible form, nothing about them was measured off
 * anyone's screen, and inventing differences there would add noise to a
 * pairwise comparison that is supposed to be about letterforms. Every LETTER
 * in every option is authored from that option's rule, and
 * tests/host/test_style_catalog.sh asserts the nine annotated glyphs against
 * the shipping table directly.
 *
 * Nothing here ships. Production draw code imports nothing from this
 * directory; `src/shared/param_pages/font4x5.mjs` is still the live font.
 */

import { registerSet, KIND_FONT } from "../index.mjs";

import { GLYPHS as HUMANIST } from "./humanist.mjs";
import { GLYPHS as SQUARE } from "./square.mjs";
import { GLYPHS as NARROW } from "./narrow.mjs";
import { GLYPHS as ROUNDED } from "./rounded.mjs";
import { GLYPHS as SLAB } from "./slab.mjs";
import { GLYPHS as STENCIL } from "./stencil.mjs";
import { GLYPHS as CONDENSED_CAPS } from "./condensed_caps.mjs";
import { GLYPHS as DOT_MATRIX } from "./dot_matrix.mjs";
import { GLYPHS as GEOMETRIC } from "./geometric.mjs";
import { GLYPHS as WIDE } from "./wide.mjs";
import { GLYPHS as METRIC_MATCHED } from "./metric_matched.mjs";

/**
 * The specimen. Not arbitrary, and not a pangram.
 *
 * font4x5.mjs records what went wrong the last time this font was changed:
 * the 3-wide font it replaced "rendered MAIN as 'MAIK', SINE as 'SIKE', SAW
 * as 'SAU'". Those three strings are this codebase's own legibility
 * regression test, so every option is drawn with them and looked at. The
 * remaining strings are the ones a real page carries — a header (AMPLITUDE is
 * the string the original measurement was taken from, and it has to fit
 * 128px), two more parameter labels, and the digits, which is what a value
 * readout is made of.
 */
export const SPECIMEN = ["AMPLITUDE", "MAIN", "SINE", "SAW", "ATTACK", "KICK", "0123456789"];

/**
 * Called from the bottom of ../index.mjs rather than running on import — see
 * the cycle note there. Registering at module scope would read KIND_FONT
 * while it is still in the temporal dead zone.
 */
export function register() {
    return registerSet({
        id: "font",
        title: "Label font — complete 4x5 tables, replacing nine traced letterforms",
        kind: KIND_FONT,
        replaces: "font4x5.mjs",
        /* Eleven, and declared: option 11 is off the narrow -> wide axis the
         * other ten share, so it is an addition to the set rather than a
         * renumbering of it. validateSet refuses an undeclared count. */
        optionCount: 11,
        specimen: SPECIMEN,
        options: [
            {
                position: 1, id: "humanist", name: "Humanist", glyphs: HUMANIST,
                note: "Four-wide caps with one-pixel stems and open apertures: C E G S L stop short of closing, and width follows the letter (I is a bare stem, M U V W take five columns) rather than filling the cell. The nearest thing here to a conventional text face, which makes it the cheapest to adopt and the least distinctive of the ten -- it argues for itself on legibility, not on identity.",
            },
            {
                position: 2, id: "square", name: "Square", glyphs: SQUARE,
                note: "Uniform box construction with flat terminals -- every bar runs the full width of its glyph, O is a closed rectangle, C is a bracket, T carries a two-pixel stem. The heaviest option in the set and the most machine-like; it holds up at a glance from across a room, and its cost is that D and O both want to be the same box, which is why D is given a fifth column -- as are M, V and W, whose diagonals need the room.",
            },
            {
                position: 3, id: "narrow", name: "Narrow", glyphs: NARROW,
                note: "A 3-wide body on a 4px advance, with the counters run the full five rows so bowls stay open. M, N and W are declared exceptions at 4/4/5 columns, because a 3-wide diagonal has to fill the middle column for three rows and that is exactly the collapse that made font5x3 render MAIN as MAIK. The narrowest face here that still passes the legibility strings, and the only one that would let a knob label carry more than seven characters.",
            },
            {
                position: 4, id: "rounded", name: "Rounded", glyphs: ROUNDED,
                note: "Four wide with the corner pixel dropped from every bowl -- O C G D Q S P R read as chamfered octagons rather than rectangles, A comes to a point and T gets a tucked foot. The softest face in the set. Its risk is the inverse of square's, and the U is the proof: a chamfered bowl loses two of a 4x5 O's twelve pixels, and at four columns the U read as a V outright -- AMPLITVDE -- so it is the one glyph here given a fifth column. O, D and 0 still sit closer together than in any other option.",
            },
            {
                position: 5, id: "slab", name: "Slab", glyphs: SLAB,
                note: "Four wide, with the serif expressed the only way a 4px grid allows -- as a full-width bar at each stem terminal. A gains a slab apex, I and L carry head and foot, U closes flat, so every stem in the face starts and ends on a horizontal. T is the forced exception: a T with a foot bar is an I, and drawn that way AMPLITUDE carried two glyphs a pixel apart. The densest option after square, and the one whose texture across a page of labels differs most from the shipping font, because the extra ink is all on the two outer rows rather than distributed.",
            },
            {
                position: 6, id: "stencil", name: "Stencil", glyphs: STENCIL,
                note: "Four wide with a one-pixel break wherever a stroke would close a counter: A B D O P Q R break in the bowl and U breaks in the stem, so every counter stays connected to the outside of the glyph. I is deliberately left whole -- it has no counter to open, and a broken 1px stem is not a stencil I but an exclamation mark, which is what SINE and AMPLITUDE actually rendered as. The most identifiable face in the set at a glance, and the one most likely to be judged on novelty rather than on reading.",
            },
            {
                position: 7, id: "condensed-caps", name: "Condensed caps", glyphs: CONDENSED_CAPS,
                note: "Three columns for every glyph including M N W, which is the exception narrow declines to make. With no room for a diagonal, M N and W are told apart by where their mass sits -- two full rows at the top, the middle and the bottom -- rather than by stroke direction. It clears the old failure (MAIN is not MAIK; K is unmistakable) and it does not clear it by much: in the specimen M reads close to N, and SAW is nearer SAV than SAW. This is the one option with a known prior -- font5x3 attempted 3-wide caps and font4x5 exists because of it -- and it is here so the catalog measures that claim rather than inheriting it.",
            },
            {
                position: 8, id: "dot-matrix", name: "Dot matrix", glyphs: DOT_MATRIX,
                note: "Option 2's skeleton with a blank column inserted between every pair, so each glyph occupies 7 or 9 columns and every lit pixel lands on an even one -- an LED sign. Sharing a skeleton is the point: it is the only option whose identity is a rendering rule rather than a letterform. It is also the one whose cost is unarguable. AMPLITUDE takes 93 of the header's 128 pixels, and dotting a 5-row face is close to the limit of what reads: the square skeleton was chosen only after the rounded one dissolved (a chamfer is one pixel, and one pixel on a 2px lattice is noise), and the advance had to go to w+3 because at w+1 the letterspacing and the lattice had the same rhythm. Rank it on the strength of the idea and expect the reading to argue back.",
            },
            {
                position: 9, id: "geometric", name: "Geometric", glyphs: GEOMETRIC,
                note: "Built from horizontals, verticals and arcs: A is a squared arch rather than a peak, M is a full top bar over three stems, the bowls are circles. K N X Z keep one stepped arm each -- stated rather than hidden, because those four cannot be drawn without a diagonal and still be read, and a face that claimed no diagonals while drawing four would be judged on the claim. The most systematic option, and the one whose A is most obviously not the shipping A.",
            },
            {
                position: 10, id: "wide", name: "Wide", glyphs: WIDE,
                note: "Five columns of body on a 6px advance, so every bowl has a real three-pixel counter and N M W V need no special case -- the only face here where a diagonal moves one column per row. The opposite bet to narrow, and its cost is arithmetic rather than aesthetic: a 32px knob cell holds about five characters instead of seven, so adopting it means shortening labels, not just redrawing them.",
            },
            {
                position: 11, id: "metric-matched", name: "Metric-matched", glyphs: METRIC_MATCHED,
                metricMatched: true,
                note: "Every advance pinned to the shipping value, to the pixel: AMPLITUDE is still 42px, KEYTRIG still 30, so adopting it is a diff to one table and no layout pass at all. The letterforms inside those boxes are top-loaded instead of centred -- crossbars ride at row 1, stemmed letters take a full-width flat head, and feet are cut back, which reads as a small tight head over a long leg rather than as two equal halves. Two overrides are stated: U closes flat because a cut-back U is the AMPLITVDE failure this directory has already had once, and I is a 1px column at advance 2 where a full stem IS the shipping glyph, so it takes its cut as a short foot. The property is asserted by the test suite off the metricMatched flag, not assumed.",
            },
        ],
    });
}
