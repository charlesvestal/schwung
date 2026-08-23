/**
 * render_page_movy.mjs — draw one param page in schwung-movy's own layout.
 *
 * A direct port of schwung-movy's knob-grid rendering
 * (`src/renderer/{header,knob,label}.ts`, © 2026 megadake, MIT —
 * https://github.com/DimaDake/schwung-movy), not a new design: the header,
 * the bank bar, the arc knob, the enum square, and the per-column label cell
 * are Movy's actual pixel geometry, adapted to read from this library's own
 * page/metaIndex/values/viz data instead of Movy's ParamVM. The five graphic
 * bodies (envelope/filter/lfo/eq/sample) are the SAME functions render_page.mjs
 * uses, from viz_draw.mjs — also ported from Movy — called here with the
 * exact 16px-tall row boxes Movy itself draws them into.
 *
 * This is a SEPARATE renderer from render_page.mjs's own dial/bar grid, not a
 * third `layout` value on it: the two do not share cell geometry (Movy's
 * fixed ROW0_Y/LBL0_Y/ROW1_Y/LBL1_Y rows vs. the dial/bar grid's dynamic row
 * height) and mixing them into one function would make neither read as
 * either original. Selected via `page_controller.mjs`'s `LAYOUT_MOVY`.
 *
 * Same library rules as render_page.mjs: no param I/O, no screen ownership
 * (draws through the injected ctx only), no input handling. One exception to
 * "no font of its own": the enum square's two-line abbreviation needs Movy's
 * condensed 5x3 font (font5x3.mjs, ported) to fit — the device's regular 5x7
 * font cannot show two lines in a 16px box, which is the entire reason that
 * font exists. Everywhere else (header, labels) still goes through the
 * device's own print(), same as the dial/bar grid.
 */

import { KIND_ENUM, KIND_OPAQUE, enumIndexOf } from "./param_meta.mjs";
import { formatParamValue } from "../param_format.mjs";
import { asciiFold, fitText, shortenLabel, line, circle } from "./render_page.mjs";
import { drawVizGroup } from "./viz_draw.mjs";
import { enumSquareLines } from "./font5x3.mjs";
import { fontPrint as tzPrint, fontWidth as tzWidth, HEIGHT as TZ_H } from "./font_tamzen6x12.mjs";
import { fontWidth4x5, fontPrint4x5, FONT4_HEIGHT, FONT4_MEASURE } from "./font4x5.mjs";
import { fontWidth5x3, fontPrint5x3 } from "./font5x3.mjs";

/**
 * Text is set in caps with labels abbreviated to a mnemonic. The HEADER and the
 * opaque box use the device-height Tamzen; the LABEL CELLS and the enum square
 * use font4x5.
 *
 * On hardware the 5-row font read as too small — legibility mattered more
 * than knob size, and the Move panel is physically smaller than the Elektron
 * one this grid was measured against, so matching its pixel geometry does not
 * reproduce its apparent size. The device font is 40% taller (7 rows vs 5).
 *
 * That was the state until 2026-08-20, when the hint FOOTER needed eight rows
 * and the label bands were the only non-widget source of them. Labels went back
 * to font4x5 (LBL_H 9 -> 7). It costs almost nothing horizontally, because the
 * labels are already mnemonics: at a 6px monospaced advance a 4-character label
 * is 24px in a 30px cell, against 23px in font4x5, whose M is 6 wide too. The
 * header keeps Tamzen, so the module and page name are still full height.
 *
 * The enum SQUARE stays on font4x5: two stacked 7-row lines need 15 rows and
 * the box interior is 13, so the small font is the only thing that fits there
 * — which is exactly what Movy used it for.
 *
 * The rejected option is font5x3, Movy's own condensed font: at 3px wide N/K,
 * A/M and W/U collapse into each other, and real pages drew MAIN as "MAIK"
 * and SAW as "SAU". font4x5 fixed that and is still what the enum square
 * uses; it is only the LABELS that wanted more height.
 */
/*
 * The label cap, in M-widths.
 *
 * It is a CAP, not a fit, and it was stricter than the space available:
 * labelWidth is min(cellW - 2, fontWidth4x5("M" * LABEL_CHARS)), so a 4-across
 * grid offered 30px of cell and the cap allowed 23. "Clear" is 24px and drew
 * as "CLEA" -- reported from the device.
 *
 * 4 is a leftover. It dates from when labels were set in the 6px-advance
 * Tamzen, where four characters genuinely was 24px and all that fit. Labels
 * moved to the PROPORTIONAL font4x5 in the change described above and the 4
 * came with them, even though M is now the widest glyph in the font rather
 * than the only width -- so measuring in Ms is pessimistic for every real
 * word. "Clear" is five characters in 24px because L and E are narrow.
 *
 * 5 gives 29px, still inside the 30px cell, so nothing can overflow: the
 * min() against cellW is what actually guarantees that, and it is unchanged.
 * 6 would be 35px, at which point the cap is inert and the cell width decides
 * everything -- which throws away the mnemonic discipline this constant exists
 * to impose.
 *
 * Exported so a test can derive the same budget rather than hard-coding a
 * pixel number that a cap change would silently invalidate.
 */
export const LABEL_CHARS = 5;

/** Uppercase, ascii-folded — the Elektron register. font4x5 has no lowercase. */
function caps(s) { return asciiFold(String(s == null ? "" : s)).toUpperCase(); }
/** fitText/shortenLabel measure through ctx.textWidth; hand them Tamzen. */
const TZ_MEASURE = { textWidth: tzWidth };
function fitDev(ctx, s, maxWidth) { return caps(fitText(TZ_MEASURE, caps(s), maxWidth)); }

/*
 * What a cell or the held-knob strip should SHOW for a value.
 *
 * formatParamValue is a numeric formatter: handed the empty string it returns
 * "0.00", which is what an unset filepath became in the header strip — holding
 * granny's Sample File read "0.00", a number, for a param that has no numeric
 * meaning and cannot be turned. The box beneath it already showed "--", so the
 * two disagreed about the same param.
 *
 * An opaque value is a path or a string: show its tail, which is the part that
 * identifies it, and "--" when there is nothing set.
 */
function displayValue(raw, meta) {
    if (raw === null || raw === undefined) return "--";
    if (meta && meta.kind === KIND_OPAQUE) {
        return String(raw).split("/").pop() || "--";
    }
    return formatParamValue(raw, meta);
}

/**
 * The synth vocabulary, abbreviated the way hardware does it.
 *
 * A character budget alone is not an abbreviation: truncating "Attack" to four
 * gives "ATTA", which is worse than useless next to "ATTE"nuation. Elektron
 * ships a fixed mnemonic per concept — ATK, DEC, SUS, REL — and that is what
 * makes a row of four-letter labels scannable rather than a row of stumps.
 *
 * These are applied per WORD and then handed to the existing shortenLabel,
 * which already knows how to squeeze a multi-word name (initials for the
 * leading words, last word kept longest, trailing indices preserved). So
 * "Filter Envelope" becomes "FLT ENV" and then "FENV", and "Osc 1 Octave"
 * keeps its 1. Anything not in the table falls through to that same logic
 * unchanged — this table only has to cover the vocabulary that actually
 * recurs across the fleet, not every possible name.
 */
const WORD_ABBREV = {
    /* Added when the fleet capture went from 76 to 95 modules. Each of these
     * was rendering as a vowel-stripped run -- BNDPSS, DSTNTN, CMPLXT -- which
     * is what the table exists to prevent. */
    bandpass: "BPF", highpass: "HPF", lowpass: "LPF", bandwidth: "BW",
    bitcrusher: "CRU", crusher: "CRU", character: "CHR", channels: "CHN",
    complexity: "CPX", destination: "DES", multiplier: "MUL",
    modulations: "MOD", generations: "GNS", progression: "PRG",
    recordings: "REC", resonators: "RSN", smoothing: "SMO",
    passthru: "PTH", passthrough: "PTH", fallthrough: "FAL",
    configuration: "CFG", soundfont: "SF", category: "CAT",
    attack: "ATK", decay: "DEC", sustain: "SUS", release: "REL", hold: "HLD",
    envelope: "ENV", env: "ENV", amount: "AMT", amt: "AMT", depth: "DPT",
    cutoff: "CUT", frequency: "FRQ", freq: "FRQ", resonance: "RES", reso: "RES",
    filter: "FLT", resonant: "RES", slope: "SLP",
    oscillator: "OSC", waveform: "WAV", wave: "WAV", shape: "SHP",
    pitch: "PIT", tune: "TUN", detune: "DET", fine: "FIN", coarse: "CRS",
    octave: "OCT", transpose: "TRN", semitone: "SEM", glide: "GLD",
    portamento: "GLD", noise: "NSE", spread: "SPR", offset: "OFS",
    position: "POS", threshold: "THR", ratio: "RAT", knee: "KNE",
    modulation: "MOD", velocity: "VEL", pressure: "PRS", aftertouch: "AFT",
    sensitivity: "SNS", amplitude: "AMP", volume: "VOL", level: "LEV",
    balance: "BAL", panning: "PAN", width: "WID", phase: "PHS",
    feedback: "FBK", delay: "DLY", reverb: "REV", chorus: "CHO",
    flanger: "FLG", phaser: "PHR", tremolo: "TRM", vibrato: "VIB",
    overdrive: "OVR", distortion: "DST", saturation: "SAT", drive: "DRV",
    compressor: "CMP", limiter: "LIM", damping: "DMP", diffusion: "DIF",
    sample: "SMP", start: "STR", length: "LEN", reverse: "RVS",
    speed: "SPD", random: "RND", quantize: "QNT", divide: "DIV",
    portion: "PRT", channel: "CHN", output: "OUT", input: "IN",

    /*
     * Second pass, chosen by measuring the fleet rather than by guessing.
     *
     * Of the labels the cell still cuts, 371 distinct words of five or more
     * characters had no entry, and the damage they do is not truncation -- it
     * is that shortenLabel falls back to dropping vowels, which turns a word
     * into a non-word. "Rotation" became ROTATN, "Mod Sens A" became MOSEA,
     * "Wave Group" became WGROU. A mnemonic is both SHORTER and readable,
     * which is the whole argument for a table over a wider cell: widening the
     * cell to fit ROTATN buys nothing, because ROTATN was never the goal.
     *
     * Ordered by fleet frequency, most damaging first. `scene` alone appears
     * 84 times.
     */
    voice: "VCE", voices: "VCES", 
    trigger: "TRG", retrigger: "RTG", retrig: "RTG", 
    scaling: "SCLG", rotation: "ROT",
    rotate: "ROT", rhythm: "RHY", 
    density: "DNS", unison: "UNI", macro: "MCR", operator: "OP",
    right: "RGT", left: "LFT", deform: "DFM", unipolar: "UNP", bipolar: "BIP",
    division: "DIV", matrix: "MTX", color: "CLR", colour: "CLR", 
    porta: "GLD", branch: "BRN", enabled: "EN", enable: "EN", 
    settings: "SET", stereo: "STO", distort: "DST", keytrack: "KTK",
    cycle: "CYC", general: "GEN", polyphony: "POLY",
    sends: "SND", send: "SND", expression: "EXP", switch: "SW", group: "GRP",
    number: "NUM", reset: "RST", advanced: "ADV",
    predelay: "PDLY", humanize: "HUM", sweep: "SWP", control: "CTL",
    break: "BRK", point: "PNT", assign: "ASN", performance: "PERF",
    perform: "PERF", route: "RTE", source: "SRC", grain: "GRN",
    spectra: "SPC", motion: "MTN", capture: "CAP", oscillators: "OSCS",
    harmonics: "HRM", brightness: "BRT",
    transport: "TRS", compress: "CMP", flutter: "FLU", stutter: "STU",
    octaves: "OCTS", reson: "RES",
};

/*
 * Words that DELIBERATELY share an abbreviation.
 *
 * Two different words mapping to one mnemonic is normally a bug -- the label
 * stops identifying the parameter -- so the test treats a collision as a
 * failure unless it is declared here. Most groups are morphological (freq /
 * frequency), but not all: portamento, porta and glide are three spellings of
 * one control, and no prefix rule would catch that.
 *
 * Exported for tests/host/test_label_abbrev.sh, which is the only consumer.
 */
export const ABBREV_SYNONYMS = Object.freeze([
    ["envelope", "env"],
    ["modulations", "modulation"],
    ["channels", "channel"],
    ["bitcrusher", "crusher"],
    ["passthru", "passthrough"],
    ["amount", "amt"],
    ["frequency", "freq"],
    ["resonance", "reso", "resonant", "reson"],
    ["glide", "portamento", "porta"],
    /* Two spellings of one word. Without the group they render COLOR and
     * COLOUR side by side -- the drift this list exists to stop, and the
     * reason a word that already fits can still need a table entry. */
    ["color", "colour"],
    ["divide", "division"],
    ["distortion", "distort"],
    ["compressor", "compress"],
    ["retrigger", "retrig"],
    ["enable", "enabled"],
    ["send", "sends"],
    ["perform", "performance"],
    ["rotation", "rotate"],
    ["waveform", "wave"],
]);

/** Exported for the same test; not part of the drawing API. */
export { WORD_ABBREV };

/**
 * The label a cell actually shows: abbreviate per word, then squeeze to fit.
 *
 * Exported because the test has to measure the REAL thing. Re-deriving these
 * two lines in the test would let the table and the assertions about it drift
 * apart, which is the failure mode where a green suite proves nothing.
 *
 * @param {string} text    the declared name or key
 * @param {number} cellW   cell width in px; the grid is 32, the knob card 29
 */
export function labelForCell(text, cellW = CELL_W) {
    /*
     * The CAP governs, not the cell.
     *
     * This was min(cellW - 2, cap), and the -2 made the label depend on which
     * surface was drawing it. The knob card runs this same row renderer at a
     * 29px cell against the grid's 32px, so the card's budget came out 27px
     * against the grid's 29px and 39% of fleet labels rendered differently in
     * the two -- the label changed under your finger as you touched the knob.
     *
     * "Touching a knob should not change its label" is the rule, and it is
     * worth more than the margin: the label you learn has to be the label you
     * see. The min() against the cell stays as an overflow guard for any
     * future surface narrower than the cap; at 29px cap and 29px card cell it
     * no longer binds.
     *
     * The cost is real and was measured rather than waved past: at the card's
     * 29px cell a full-width label leaves no gutter, and 24% of adjacent
     * label pairs in the fleet come within 2px of each other IN THE CARD. The
     * grid is unaffected -- its 32px cell was never the binding constraint,
     * which is why this change moves no grid pixels at all.
     */
    const labelWidth = Math.min(cellW, fontWidth4x5("M".repeat(LABEL_CHARS)));

    /*
     * The table applies UNCONDITIONALLY, even to a word that would have fit.
     *
     * "Abbreviate only when it does not fit" looks like a strict improvement
     * and is not: it splits a family. ATK / DEC / SUS / REL is one set, but
     * "Attack" does not fit and "Decay" does, so the conditional version draws
     * ATK / DECAY / SUS / REL down a row of four knobs. The consistency IS the
     * legibility here -- that is the whole reason this table exists rather
     * than a wider cell.
     *
     * The corollary is a rule for adding entries, enforced by
     * tests/host/test_label_abbrev.sh: do not table a word that already fits
     * unless it belongs to a family whose other members do not. Tempo, Latch
     * and Swing fit; tabling them bought nothing and cost the real word.
     */
    /*
     * CAPS BEFORE MEASURING.
     *
     * The cell draws upper case, so the width that matters is the upper-case
     * one. Capitalising AFTER shortenLabel measured the mixed-case string
     * against the budget, and the two widths differ in both directions:
     *
     *   - upper case is narrower for ordinary words ("Pitch" 24px, "PITCH"
     *     20px), so a label was cut to fit a width it would have fitted
     *     anyway -- PITC, GRAI, DRIV. 351 fleet labels lose a character.
     *   - but WIDER once shortenLabel has devowelled, because the letters it
     *     keeps are the narrow lowercase ones ("Ecldrm" fits, "ECLDRM" is
     *     30px in a 29px cell). 68 fleet labels currently overrun their
     *     budget and eat the gutter between neighbouring cells.
     *
     * The second half is the one that is invisible in code review, and it is
     * why this is a bug rather than a missed optimisation.
     *
     * The value path next door already had it right -- fitDev caps before it
     * measures -- so the label path was the outlier, not the precedent.
     *
     * Safe to fold case first: shortenLabel's only case-sensitive step is
     * devowel, whose vowel test is /[aeiou]/i.
     */
    return shortenLabel(LBL_MEASURE, caps(preAbbreviate(text, labelWidth)), labelWidth);
}

/**
 * What the word pass produced, BEFORE any squeezing to fit.
 *
 * The reference the test compares labelForCell against, and it has to be a
 * separate function rather than labelForCell with a huge cell: the width is
 * `min(cellW - 2, LABEL_CHARS worth of Ms)`, so a huge cell still clamps to
 * the same cap and the comparison is a value against itself. That mistake
 * reported 2665/2665 labels complete and proved nothing.
 */
export function labelUnsqueezed(text) {
    return caps(preAbbreviate(text));
}

/**
 * word -> the first-listed member of its synonym group, or itself.
 *
 * Built once. The GROUP is the unit of the decision below, not the word: if
 * one spelling of a control expands to its full form and another does not,
 * the same parameter reads differently depending on which spelling its module
 * happened to use -- Amount/AMOUNT next to Amt/AMT. That is the exact drift
 * ABBREV_SYNONYMS exists to prevent.
 */
const ABBREV_CANONICAL = (() => {
    const m = new Map();
    for (const g of ABBREV_SYNONYMS) for (const w of g) m.set(w, g[0]);
    return m;
})();

/**
 * Word-level pass before shortenLabel.
 *
 * A mnemonic is a CONSOLATION for not fitting, never an improvement on the
 * real word: nobody reads PIT more easily than PITCH. So the table applies
 * only when the real word will not fit the cell -- ATK stays ATK because
 * ATTACK does not fit, while PITCH, CUTOFF, DECAY and OCTAVE come back.
 *
 * Decided per GROUP, not per word. Taking each word on its own merits looks
 * equivalent and is not: Amount would expand to AMOUNT while Amt stayed AMT,
 * and Glide would become GLIDE while Portamento stayed GLD -- one control
 * reading two ways depending on the spelling its module used. The group's
 * canonical word decides for every member, so they agree or they all fall
 * back together.
 *
 * `budget` is the cell's label width. Zero or absent means "no budget known",
 * which keeps the old unconditional behaviour for any caller that has not got
 * one -- the table is still the safe default.
 */
function preAbbreviate(label, budget) {
    const s = asciiFold(String(label == null ? "" : label)).trim();
    if (!s) return "";
    const parts = s.split(/[\s_]+/).filter(Boolean);

    /*
     * The expansion is for SINGLE-word labels only.
     *
     * The budget belongs to the whole label, so on a multi-word name no word
     * actually has it -- and handing each one the full budget makes the pair
     * LONGER, which shortenLabel then squeezes into something worse than the
     * mnemonics would have produced. "Wave Group" is the case: expanding
     * `group` to GROUP gives "WAVE GROUP" and a rendered WGROU, against WGRP
     * from WAV + GRP. Mnemonics are exactly what a multi-word label needs,
     * which is what the table was built for ("Filter Envelope" -> FLT ENV ->
     * FENV).
     */
    const expandable = parts.length === 1 && budget > 0;

    return parts
        .map((w) => {
            const lw = w.toLowerCase();
            const abbrev = WORD_ABBREV[lw];
            if (!abbrev) return w;
            if (!expandable) return abbrev;
            const canonical = (ABBREV_CANONICAL.get(lw) || lw).toUpperCase();
            return fontWidth4x5(canonical) <= budget ? canonical : abbrev;
        })
        .join(" ");
}

/** Selects this renderer from page_controller.mjs's `setLayout`. */
export const LAYOUT_MOVY = "movy";

/*
 * Vertical rhythm, re-cut against Elektron's measured bands.
 *
 * Movy's original constants assumed a 7-row font and packed the bands flush:
 * LBL0_Y(27) + 8 === ROW1_Y(35), i.e. a label and the knob row below it
 * touched with a ZERO-pixel gutter, while five rows sat unused at the bottom
 * of the screen. All the slack existed; it was in the wrong place.
 *
 * Elektron's screen, recovered from a 4x capture, puts a 1-3px gutter between
 * every band. With font4x5 the label band drops from 8 rows to 6, which pays
 * for a 2px gutter on both sides of every label.
 *
 * THE FOOTER (2026-08-20) is bought from that rhythm rather than from the
 * widgets. Before it, the screen was exactly full — LBL1_Y(55) + LBL_H(9) ===
 * 64 — so a hint bar had to come from somewhere, and the two candidates were
 * the widget rows and the label bands. Shrinking the widget rows is the one
 * change that cannot be made: a viz body occupies rect.y+1..+14 and a box is
 * BOX_H(15), so anything under 15 stands the graphics down (see the minimum-cell
 * rule in render_page.mjs). The label bands can pay instead, by going back to
 * font4x5 — 5 glyph rows, so LBL_H 9 -> 7 with a clear row each side — and the
 * gutters give up the rest:
 *
 *      0..6    header      (7)   text at y=0, 7 tall
 *      7       bank bar    (1)
 *      8..9    gutter      (2)   was 4
 *     10..24   knob row 0  (15)  UNCHANGED height — graphics still fit
 *     25..31   label row 0 (7)   was 9 (tamzen); font4x5 + 1 clear row each side
 *     32       gutter      (1)   was 2
 *     33..47   knob row 1  (15)  UNCHANGED height
 *     48..54   label row 1 (7)   was 9
 *     55       rule        (1)
 *     56..63   FOOTER      (8)
 *
 * Eight rows found, none of them taken from a widget. The knob itself is only
 * ~12 rows tall now that the arc is open at the bottom, so a knob row carries
 * more air below it than a graphic row does; the 15 is held for the graphics.
 */
export const W = 128;
/*
 * The header BAND. Glyphs are printed at y=0 rather than y=1: the OLED has a
 * physical bezel, so the top row of the screen is already visually inset and
 * spending a pixel row on margin there buys nothing. That leaves the band one
 * row taller than the 7-row font, with the spare row BELOW the text.
 *
 * BAR_Y is then one further row down, so row HEADER_H is always dark. Without
 * it the bank bar butted straight against the bottom row of every glyph, and
 * with the header inverted the solid band merged into the bar into one thick
 * smudge.
 */
export const HEADER_H = 7;
export const BAR_Y = HEADER_H;       /* no separator row — the band has its own */
/* One row of gutter here, not two — the bank bar is itself a separator, so
 * this is the cheapest row on the screen to give back to the header. */
export const ROW0_Y = 10;
export const LBL0_Y = 25;
export const ROW1_Y = 33;
export const LBL1_Y = 48;
export const CELL_W = 32;
/**
 * Where a row's cells sit.
 *
 * The grid draws four 32px cells from x=0, which is what every constant here
 * was written against. The chain editor's knob card (knob_card.mjs) draws the
 * SAME row inside a bordered box, so it needs a narrower cell at an offset
 * origin. Rather than a second copy of the row — which is how the two
 * renderers in this directory came to disagree about a pixel — the origin and
 * the width became parameters with the grid's own values as the default.
 *
 * Nothing else changes: the widgets, the fonts, the label budget and the
 * touched-cell inversion are all the same code, so a card cell and a grid cell
 * cannot drift apart.
 *
 * Frozen because it is the shared default every caller who omits `geom`
 * receives the SAME object for — a caller that mutated it (rather than
 * passing its own) would move every other page's cells too.
 */
export const GRID_GEOM = Object.freeze({ x0: 0, cellW: CELL_W });
/*
 * `cellW` has a floor: `Math.floor((cellW - ENUM_W) / 2)` goes negative below
 * ENUM_W (20) and the same for KW (17) in the knob widget, spilling a widget
 * into its neighbour with no clip count and no error to say so. Nothing here
 * enforces it -- the card's 29px cell clears it with room to spare -- but a
 * future geometry needs to stay at or above ENUM_W.
 */
const cellLeft = (g, col) => g.x0 + col * g.cellW;
/* The hint footer, and the rule that separates it from the last label band. */
export const RULE_Y = 55;
export const FOOTER_Y = 56;
export const FOOTER_H = 8;
/*
 * ODD, for the same reason BOX_H is: 5 glyph rows centred in the band leave a
 * remainder that only splits evenly when the band is odd. At 6 the touched
 * cell inverted rows 29-34 while the glyphs sat on 29-33 — no clear row at all
 * above the text, so the letters ran straight into the top edge of the
 * highlight and the whole strip read as a smudge. 7 gives one clear row on
 * each side, which is what makes an inverted run legible.
 *
 * The two rows this costs come back from the widget rows, which drop 16 -> 15:
 * a viz body occupies rect.y+1..rect.y+14 and a box is BOX_H (15), so 15 is
 * all either ever needed.
 *
 * 2026-08-20: back to 7 with font4x5 labels, to buy the footer. The band is
 * the only place on the screen with rows to give that is not a widget — see
 * the vertical rhythm note above. The HEADER and the opaque box keep the
 * device-height Tamzen; it is only the label cells that shrink.
 */
export const LBL_H = 7;
export const KW = 17;
/*
 * The enum square is WIDER than a knob box, because it is the one widget whose
 * content is text and text has a minimum size.
 *
 * At KW=16 the frame eats 2 and the interior is 14, but a three-glyph line in
 * font4x5 measures 14-17px ("LOW" is 15, since W is a 5-wide glyph): the text
 * did not merely touch the frame, it overflowed it, and the centering rounded
 * to a NEGATIVE offset that started the first glyph on top of the left frame
 * column. 20 gives an 18px interior, and reserving a 1px margin inside each
 * frame leaves 16px of text — enough for any three glyphs this font has, with
 * clear space on both sides.
 *
 * The knob keeps KW: it is a circle with nothing to overflow, and 15px of
 * ring in a 16px box is already the right weight.
 */
export const ENUM_W = 20;
/** Frame (1) + margin (1) on each side. */
export const ENUM_TEXT_W = ENUM_W - 4;
/*
 * Height of a text-bearing box, and it is ODD on purpose.
 *
 * Both things these boxes hold are an odd number of rows tall: one line is 5,
 * two lines are 11 (5 + a 1px gap + 5). Centring an odd content height in an
 * even interior always leaves an odd remainder, which cannot split evenly —
 * at h=16 the interior was 14, so a two-line square got 1px of margin above
 * and 2px below, and a one-line box got 4 above and 5 below.
 *
 * h=15 makes the interior 13: two lines leave 2 (1 and 1), one line leaves 8
 * (4 and 4). Both centre exactly. It also still fits the 16-row band with a
 * row to spare, which h=17 would not.
 */
export const BOX_H = 15;
export const TOAST_Y = 55;

/* ------------------------------------------------------------- primitives */

/* Native draw_line (src/host/js_display.c) costs one QuickJS->C crossing
 * regardless of length, unlike this library's own JS Bresenham — see
 * viz_draw.mjs's note on the same tradeoff. Prefer it when the caller
 * provides one. */
function drawLine(ctx, x0, y0, x1, y1) {
    if (typeof ctx.line === "function") ctx.line(Math.round(x0), Math.round(y0), Math.round(x1), Math.round(y1), 1);
    else line(ctx, Math.round(x0), Math.round(y0), Math.round(x1), Math.round(y1), 1);
}

/**
 * Left edge for a run of `w` pixels centred in the span [x0, x0+span-1].
 *
 * Integer arithmetic on the SPAN, not floating-point around a midpoint. The
 * opaque box used to centre on `kx + KW/2`, but a 16px box spanning
 * kx..kx+15 has its centre at kx+7.5 — that half pixel turned into a whole
 * one every time the leftover was odd, and "KIC" sat 3px from the left frame
 * and 2px from the right. An odd leftover still cannot split evenly; what
 * this guarantees is that the extra pixel always goes to the RIGHT, the same
 * way for every widget, instead of depending on which side rounding fell.
 */
function centreX(x0, span, w) {
    return x0 + Math.floor((span - w) / 2);
}

/**
 * Tamzen 6x12, trimmed to its cap window — 7 rows, 6px advance, which is
 * metrically IDENTICAL to the device 5x7 it replaces. Same band height, same
 * label widths, so nothing in the layout moves; only the letterforms change.
 *
 * The device font is noopkat/oled-font-5x7, the stock font every SSD1306
 * project uses, and it looks it: a flat-topped A, a K/X that blur together, a
 * Q whose tail wanders outside the glyph. Tamzen (a cleaned-up Tamsyn) was
 * already vendored in fonts/tamzen/ and unused. Its A has a pointed apex, its
 * W a real double-V, and scripts/bdf_to_font.py redraws Q with the tail
 * closed inside the bowl — necessary here because the touched label draws its
 * band inverted, so a descender would be black on black and vanish.
 */
const FONT_H = TZ_H;
/* Labels alone are set in font4x5 (see LBL_H) — the header, the opaque box and
 * the empty-page string all stay on the device-height Tamzen. */
const LBL_FONT_H = FONT4_HEIGHT;
const LBL_MEASURE = FONT4_MEASURE;

function centeredText(ctx, x0, span, y, text, color) {
    const t = caps(text);
    tzPrint(ctx, centreX(x0, span, tzWidth(t)), y, t, color);
}

/**
 * schwung-movy renderer/header.ts drawHeader, ported.
 *
 * Movy's own HEADER_H (7) matches ITS font's glyph height. Schwung's device
 * font is a 5x7 bitmap printed one row lower (`print(x, 1, ...)`, to clear
 * the top edge of the display), so its glyphs occupy rows 1-7 while a 7-row
 * inverted band covers only rows 0-6 — the bottom row of every character
 * fell outside the highlight and read as a black bar under the text. The
 * band is therefore drawn one row taller than the font, the same fix
 * render_page.mjs's drawTouchStrip already uses.
 *
 * The header is also where the width matters most: it carries the touched
 * knob's FULL parameter name next to its value, which is the answer to a
 * label being abbreviated at all.
 */
/** Clear pixels between the header's two sides, so they never read as one word. */
export const HEADER_GAP = 4;
/** The title's floor: the old fixed 55%, so the worst case is what shipped. */
export const HEADER_MIN_LEFT = Math.floor(W * 0.55);

export function drawHeader(ctx, left, right, inverted = false) {
    /* font4x5, not the label face: the header is secondary text (the slot
     * title, the page name, and the touched parameter's full name and value),
     * so it can afford to be smaller than the thing you read at a glance. A
     * 5-row glyph at y=1 sits in a 7-row band with one clear row above and
     * one below — which is what the touched HIGHLIGHT needs to be legible,
     * and what it did not have while a 7-row font filled an 8-row band edge
     * to edge.
     *
     * Tamzen's own 5-row face was the obvious candidate and is the wrong one:
     * its advance equals its ink width, so adjacent glyphs touch, and the
     * header string overflowed 124px of usable width at 129. font4x5 is
     * proportional with a real gap and the same string measures 106.
     *
     * Two rows come back from this — one from the shorter band, one from the
     * separator row below it, which is no longer needed: the band already
     * carries a clear row under its glyphs, so nothing butts against the bank
     * bar. Both go to the gutter above the first widget row. */
    if (inverted) ctx.fillRect(0, 0, W, HEADER_H, 1);
    const color = inverted ? 0 : 1;
    const fit5 = (t, maxW) => caps(fitText(FONT4_MEASURE, caps(t), maxW));

    /*
     * The split between the two sides is MEASURED, not fixed.
     *
     * It used to be a flat 55% for the left and 60% for the right, which is
     * two problems. The right side is usually a page name and those are
     * short -- 29px at the fleet median -- so the left was capped at 70px
     * while a third of the bar sat empty, and a long title was cut for room
     * nobody was using. That is what made an airwindows effect read as
     * "MFX > BRIGHTAMBI": the name is 94px and there was 104px of bar.
     *
     * And 55 + 60 is 115%, so with a long page name the two sides OVERLAPPED
     * and drew glyphs through each other. Measuring the right first and
     * giving the left the remainder makes that unrepresentable.
     *
     * HEADER_MIN_LEFT is the floor, so a very long page name cannot squeeze
     * the title to nothing -- the right gives ground first. It is the old
     * 55%, so the worst case is exactly what shipped before.
     */
    let r = right ? fit5(right, Math.floor(W * 0.6)) : "";
    let rw = r ? fontWidth4x5(r) : 0;
    if (rw && W - 4 - rw - HEADER_GAP < HEADER_MIN_LEFT) {
        r = fit5(right, Math.max(0, W - 4 - HEADER_MIN_LEFT - HEADER_GAP));
        rw = r ? fontWidth4x5(r) : 0;
    }
    const l = fit5(left, W - 4 - (rw ? rw + HEADER_GAP : 0));
    fontPrint4x5(ctx, 2, 1, l, color);
    if (r) fontPrint4x5(ctx, W - rw - 2, 1, r, color);
}

/**
 * schwung-movy renderer/header.ts drawBankBar, ported. One segment per page,
 * the current one double height; `groups` (one id per page) draws a 1px gap
 * where the bank changes, same idea as render_page.mjs's own page rule.
 */
export function drawBankBar(ctx, pageIndex, pageCount, groups) {
    if (pageCount <= 1) return;

    if (pageCount > W) {
        ctx.fillRect(0, BAR_Y, W, 1, 1);
        const x = Math.min(W - 1, Math.floor(pageIndex * W / pageCount));
        ctx.fillRect(x, BAR_Y, 1, 2, 1);
        return;
    }

    const gap = new Array(pageCount).fill(0);
    const useGroups = !!groups && groups.length === pageCount;
    const bounds = [];
    for (let b = 1; b < pageCount; b++) {
        if (!useGroups || groups[b] !== groups[b - 1]) bounds.push(b);
    }
    const keep = Math.min(bounds.length, Math.max(0, W - pageCount));
    for (let i = 0; i < keep; i++) gap[bounds[Math.floor(i * bounds.length / keep)]] = 1;

    const area = W - keep;
    const edge = (b) => Math.floor(b * area / pageCount);

    let x = 0;
    for (let b = 0; b < pageCount; b++) {
        x += gap[b];
        const segW = edge(b + 1) - edge(b);
        const h = b === pageIndex ? 2 : 1;
        if (segW > 0) ctx.fillRect(x, BAR_Y, segW, h, 1);
        x += segW;
    }
}

/* ------------------------------------------------------------------ knobs */

/**
 * schwung-movy renderer/knob.ts drawCircleBorder + drawArcKnob, ported. The
 * outline prefers the native `drawCircle(cx,cy,r,color)` binding
 * (src/host/js_display.c: js_display_draw_circle) when the caller provides
 * one — the whole midpoint walk happens in C, so a ring costs ONE
 * QuickJS->C crossing instead of the ~40 fillRect calls the same walk costs
 * in JS for r=7, and 8 of these draw on every knob page. Without it, falls
 * back to the identical midpoint walk in JS: same pixels, higher cost.
 *
 * It used to fake the ring out of the `fillCircle` binding instead — a disk
 * of radius r, then a disk of radius r-1 in the background colour punched
 * through the middle. That is NOT a ring. At each cardinal the two disks
 * reach the same column extent (floor(sqrt(r*r-1)) === r-1 for every r we
 * draw), so the ring loses the pixel just inside the extreme one and the
 * extreme pixel is stranded over a gap: four detached dots sitting outside
 * an otherwise flat-sided outline, which is what "the circles have little
 * dots on the outside" was. No integer radius escapes it — it is a property
 * of the difference of two rasterised disks, not of this size. The headless
 * harness never showed it because its draw context supplied no fillCircle,
 * so previews and snapshots only ever exercised the JS fallback;
 * `tools/param-pages/harness.mjs` now offers the native primitives too.
 *
 * The JS fallback below is NOT a midpoint/Bresenham walk, for the same
 * reason: at r=7 that strands a lone pixel at each of the four compass
 * points, one row proud of the run behind it, which reads as a spike on the
 * outside of the circle. Both paths use the distance-rounding ring instead —
 * see js_display_draw_arc. Keep them identical: the harness renders the
 * fallback, the device renders the native one, and a preview that disagrees
 * with the OLED is how the original bug survived review.
 *
 * The track is an OPEN ARC, not a closed ring, and the pointer FLOATS clear
 * of both the centre and the rim. Both come from the Elektron UI this grid
 * imitates, and both carry information a circle does not:
 *
 *   - The gap marks the ends of travel. Drawing a full 360 ring while the
 *     pointer only sweeps 300 leaves 60 degrees of track the value can never
 *     reach, and says the control wraps around when it does not. Reusing the
 *     pointer's own numbers (KNOB_START_DEG / KNOB_SWEEP_DEG) makes the two
 *     agree by construction.
 *   - A pointer welded from the exact centre to the rim reads as a clock hand
 *     or a pie slice. Elektron's is a short stroke floating between about a
 *     quarter and four-fifths of the radius, which reads as an indicator.
 */
/*
 * Measured off Elektron's screen (128x64, recovered from a 4x capture):
 *
 *   track    14px across, open at the bottom. The last drawn pixels sit at
 *            dx=+-5.5, dy=+3.5 from the centre — 237.5 degrees — and the row
 *            below, which a closed circle would fill, is empty. That puts the
 *            arc boundary between 226 and 237.5, so ~230 start / ~260 sweep.
 *   pointer  a stroke from the CENTRE outward to about 0.85r, not a floating
 *            segment: their DEC knob at midpoint is a 6px vertical run
 *            starting one pixel off centre. Travel bottoms out at 225 (their
 *            ATK pointer tip is exactly on that diagonal), so 270 degrees.
 *
 * The 5-degree inset between the pointer travel and the track is theirs, not
 * a rounding artefact: at either extreme the pointer aims just past the end
 * of the track, into the gap.
 */
export const KNOB_R = 8;
const KNOB_START_DEG = 225;
const KNOB_SWEEP_DEG = 270;
const ARC_START_DEG = 230;
const ARC_SWEEP_DEG = 260;
const POINTER_INNER = 0.0;
const POINTER_OUTER = 0.85;
/*
 * Modulation dot centre radius.
 *
 * The dot should HUG the inside of the ring, which is a tighter constraint
 * than "somewhere inside it". The ring sits at KNOB_R and the pointer tip at
 * KNOB_R * POINTER_OUTER (6.8 at r=8), so there is barely a pixel of clear
 * track between them. The mark is a 3-wide plus (half-width 1), so centred
 * here it spans r-2 .. r: far enough out to read as riding the arc rather
 * than floating in the middle of the knob, and one pixel clear of the ring at
 * every angle so it never overwrites it and breaks the circle's silhouette.
 */
const MOD_DOT_R = KNOB_R - 2;

/**
 * The modulation dot: where a modulated param actually IS right now, riding
 * the arc, while the pointer keeps showing the base you dialled in.
 *
 * Two values on one knob is the point. With the pointer chasing an LFO you
 * lose sight of what you set — and turning the knob edits the base, not what
 * you were watching. This is how the hardware this grid is modelled on does
 * it, and it is also the cheap way round: the base only moves when you turn
 * the knob, so only the dot needs live data.
 *
 * A five-pixel PLUS, not a circle and not a square block. At r=7 a rasterised
 * disc of any useful size either disappears into the 1px arc or swamps it,
 * and the plus is five fill_rects (487ns each) against a circle's scan. Drawn
 * just inside the arc radius so it reads as travelling along the track rather
 * than floating. The size and shape are argued in the body — both follow from
 * the fact that an even-sized mark cannot centre on a pixel.
 */
function drawModDot(ctx, kx, ky, normVal) {
    const cx = kx + KNOB_R, cy = ky + KNOB_R;
    /*
     * INSIDE the ring, not on it. Centred on the arc radius the 2x2 straddles
     * the 1px ring — half its pixels land outside the circle, so as the dot
     * travels it reads as lumps growing out of the rim rather than as a marker
     * moving round a track, and at the shoulders it merges with the ring
     * entirely. Sitting it wholly inside keeps the circle's silhouette intact
     * and the dot separately legible.
     *
     * MOD_DOT_R is the ring radius less the dot's own half-width and a pixel
     * of clearance, which is what stops it touching the ring at any angle.
     */
    const rad = (KNOB_START_DEG + normVal * KNOB_SWEEP_DEG) * Math.PI / 180;
    const x = Math.round(cx + MOD_DOT_R * Math.sin(rad));
    const y = Math.round(cy - MOD_DOT_R * Math.cos(rad));
    /*
     * ONE pixel, not a 2x2 — because an even-sized mark cannot be centred.
     *
     * A 2x2 spanning (a,b)..(a+1,b+1) has its centroid at (a+0.5, b+0.5), so
     * whichever way it is rounded it lands half a pixel off the angle it is
     * meant to be showing, and at the cardinal angles floating point decides
     * the tie: at 50% the true centre is exactly cx, sin() returns -2.4e-16
     * rather than 0, and the block rounded a whole pixel LEFT of the knob
     * centre. That is the mark visibly not sitting on its track.
     *
     * An odd-sized mark centres exactly on a pixel at every angle, so the
     * shape is a PLUS: the centre pixel plus its four orthogonal neighbours.
     *
     * One pixel centred correctly was still too faint to track against a 1px
     * ring and a pointer. A full 3x3 is the other obvious odd size and it is
     * too heavy — nine pixels on a knob whose radius is eight reads as a
     * blob, which is what the 2x2 already failed at. The plus is five pixels,
     * three across, and its diagonal gaps keep it visually distinct from the
     * solid ring it travels next to rather than merging into it.
     */
    ctx.fillRect(x, y, 1, 1, 1);
    ctx.fillRect(x - 1, y, 1, 1, 1);
    ctx.fillRect(x + 1, y, 1, 1, 1);
    ctx.fillRect(x, y - 1, 1, 1, 1);
    ctx.fillRect(x, y + 1, 1, 1, 1);
}

/**
 * A parameter's value as 0..1, or null when it has not been read.
 *
 * Extracted from drawKnobWidget, where it was an inline expression, because the
 * knob LEDs light from the same number (knob_leds.mjs). Two copies would let a
 * knob whose arc sits at three-quarters light as if it were at a third, and
 * nothing on screen would say which of them was lying.
 *
 * NULL IS NOT ZERO. An unserved key reads back as "" and Number("") is 0, which
 * is finite — so a parameter that was never answered would confidently
 * normalise to the bottom of its range. That is the same collapse the tri-state
 * read contract exists to prevent, so it is refused here and each caller says
 * what it wants: the arc points at 0 (it must point somewhere), the LED goes
 * dark (an unlit knob already means "nothing to turn here").
 *
 * An ENUM normalises across its OPTIONS, not its min/max — it usually declares
 * neither, and "option 3 of 5" is the only reading of an enum that a brightness
 * can carry.
 *
 * NOT render_page.mjs's fractionOf, and the two must not be merged without
 * deciding which of these they are:
 *
 *   fractionOf   "where on the declared RANGE does this value sit", for the
 *                VIZ shapes — an envelope, a filter curve, a fader. It returns
 *                0 on failure because a shape has to be drawn somewhere, and
 *                it measures an enum against min/max, which for the shapes is
 *                what their geometry expects.
 *   normalizedOf "how full is this control, or unknown", for the KNOB — arc,
 *                modulation dot, indicator LED. It measures an enum across its
 *                option list, because "option 3 of 5" is the only reading a
 *                pointer or a brightness can carry, and it distinguishes
 *                unread from zero because the LED must go dark rather than
 *                claim the bottom of the range.
 *
 * Their pixels are separately baselined; collapsing them would move the viz
 * shapes to answer a question they were not drawn for.
 */
export function normalizedOf(meta, raw) {
    if (!meta) return null;
    if (raw === null || raw === undefined || raw === "") return null;
    if (Array.isArray(meta.options) && meta.options.length > 0) {
        /* enumIndexOf, NOT Number(): a plugin may report an enum by NAME, and
         * Number("Room") is NaN. Getting this wrong would have left every
         * name-reporting enum unlit — the exact class of bug the chord wire
         * format already cost this codebase once. */
        const idx = enumIndexOf(meta, raw);
        if (idx < 0) return null;
        if (meta.options.length === 1) return 0;
        return Math.max(0, Math.min(1, idx / (meta.options.length - 1)));
    }
    const num = Number(raw);
    if (!isFinite(num)) return null;
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    if (!(max > min)) return null;
    return Math.max(0, Math.min(1, (num - min) / (max - min)));
}

function drawArcKnob(ctx, kx, ky, normVal) {
    const cx = kx + KNOB_R, cy = ky + KNOB_R, r = KNOB_R;
    if (typeof ctx.drawArc === "function") {
        ctx.drawArc(cx, cy, r, ARC_START_DEG, ARC_SWEEP_DEG, 1);
    } else {
        /* One pixel per ROW and one per COLUMN, unioned — see
         * js_display_draw_arc. A distance-rounded annulus is 1.41px wide at
         * 45 degrees, which stacks into a visible blob at each shoulder. */
        const inSweep = (dx, dy) => {
            if (ARC_SWEEP_DEG >= 360) return true;
            let a = Math.atan2(dx, -dy) * 180 / Math.PI;
            if (a < 0) a += 360;
            let d = a - ((ARC_START_DEG % 360) + 360) % 360;
            if (d < 0) d += 360;
            return d <= ARC_SWEEP_DEG;
        };
        const plot = (dx, dy) => { if (inSweep(dx, dy)) ctx.fillRect(cx + dx, cy + dy, 1, 1, 1); };
        for (let dy = -r; dy <= r; dy++) {
            const dx = Math.round(Math.sqrt(r * r - dy * dy));
            plot(dx, dy); if (dx !== 0) plot(-dx, dy);
        }
        for (let dx = -r; dx <= r; dx++) {
            const dy = Math.round(Math.sqrt(r * r - dx * dx));
            plot(dx, dy); if (dy !== 0) plot(dx, -dy);
        }
    }
    const rad = (KNOB_START_DEG + normVal * KNOB_SWEEP_DEG) * Math.PI / 180;
    const sin = Math.sin(rad), cos = Math.cos(rad);
    drawLine(ctx,
        Math.round(cx + r * POINTER_INNER * sin), Math.round(cy - r * POINTER_INNER * cos),
        Math.round(cx + r * POINTER_OUTER * sin), Math.round(cy - r * POINTER_OUTER * cos));
}

/**
 * schwung-movy renderer/knob.ts drawEnumSquare, adapted: Movy uses a custom
 * condensed 5x3 font to fit two short lines in the 16px box; this device only
 * has the one 5x7 font, so the option text is fit on a single centred line
 * instead. The frame and box geometry are Movy's.
 */
/** Trim a line until it fits, measured — never assume N glyphs are N*advance
 *  wide, because this font is proportional (I is 1px, W is 5). */
function fitLine(text, maxWidth) {
    let t = String(text || "");
    while (t.length > 1 && fontWidth4x5(t) > maxWidth) t = t.slice(0, -1);
    return fontWidth4x5(t) > maxWidth ? "" : t;
}

/*
 * A TRIGGER is a BANG, not a square.
 *
 * Reusing the enum square said "this is an enum", which is the one thing a
 * momentary action is not — reported from the device as "this shouldn't be a
 * square. It's different."
 *
 * A circle because that is what a momentary action looks like in music
 * software: Max and Pd both draw a bang as a circle, so the shape already
 * means "click me and something happens" to the people using this. It is also
 * unambiguous against everything else on the grid — the dial is a circle WITH
 * a pointer and a gap, the enum is a rectangle, the fader is a column.
 *
 * FILLED while firing, and that matters more than it looks: a trigger has no
 * state, so nothing else on the screen changes when you click it. Without the
 * flash there is no confirmation that anything happened at all.
 */
/*
 * A TRIGGER is a PUSH BUTTON, not a square and not a value.
 *
 * Reusing the enum square said "this is an enum", which is the one thing a
 * momentary action is not. Drawn as a cap ellipse with two straight sides down
 * to a base arc -- a physical button in perspective -- because it is then
 * unmistakable against everything else on the grid: the dial is an arc with a
 * pointer, the enum a rectangle, the fader a column.
 *
 * The cap TRAVELS when it fires. That is the whole reason to prefer it to a
 * flash: a trigger has no value, so nothing else on screen changes when you
 * click it, and a button that visibly moves is a stronger confirmation than a
 * widget that merely inverts.
 *
 * Sized to the cell rather than to taste: BTN_RX 7 makes it 15 wide inside a
 * KW of 17, and cap + depth + base is 12 rows inside a BOX_H of 15, which
 * leaves room for the 2px travel without touching the label below.
 */
/*
 * Sized by the row budget, not by taste. The widget band is BOX_H = 15 rows
 * and everything must stay inside it (see the note above drawKnobWidget):
 *
 *     1  air
 *    13  the button: BTN_RY (cap top half) + BTN_DEPTH + BTN_RY (base arc)
 *     1  air                              ... + 1, because a span from a to b
 *    ---                                      is b - a + 1 rows, not b - a
 *    15
 *
 * That +1 is not pedantry: it is the bug this shape shipped with first. The
 * button overflowed one row into the label beneath it, and clipped() cannot
 * catch that because the pixels are still on the screen.
 *
 * There is no CLK cue on the widget. It was tried, cost 7 of these 15 rows,
 * and squeezed the cap to a 15x5 lozenge -- and the footer already says CLK
 * FIRE the moment the knob is held, which is the same promise in a place that
 * costs nothing.
 */
const BTN_RX = 7;
const BTN_RY = 3;
const BTN_DEPTH = 6;
const BTN_TRAVEL = 2;

/*
 * Timing, and why the two halves come apart.
 *
 * A real button pops back long before the flash of the thing it did has
 * faded, so the cap returns at BTN_PRESS_MS while the burst keeps expanding
 * to BTN_FLASH_MS. Held together they read as one state change; apart they
 * read as an event with a consequence.
 *
 * The burst geometry is the startup animation's impact lines
 * (src/modules/tools/splash-test/ui.js): IMPACT_GAP 2 and a very SHORT line.
 * Unlike the splash, which holds three static lines for 20 ticks, these
 * EXPAND -- the gap grows as the burst travels outward and the stubs go with
 * it, which is what makes it read as radiating rather than as a decoration
 * switched on.
 */
const BTN_PRESS_MS = 120;
const BTN_FLASH_MS = 300;
const BTN_RAYS = 8;
const BTN_RAY_GAP = 2;
const BTN_RAY_LEN = 2;
const BTN_RAY_TRAVEL = 4;   /* how far the burst moves out over its life */

/*
 * A clean 1px ellipse outline.
 *
 * Thresholding on distance (|hypot(dx/rx, dy/ry) - 1| <= k) is the obvious
 * way and it looks JAGGED at this size: the tolerance band is wider where the
 * curve runs shallow, so the outline goes two and three pixels thick along the
 * top and bottom and thin at the sides. Walking each column for its exact y
 * and each row for its exact x, and taking the union, gives an even, connected
 * hairline instead.
 */
function ellipseOutline(ctx, cx, cy, rx, ry, color, bottomOnly) {
    const put = (x, y) => {
        if (bottomOnly && y < cy) return;
        ctx.fillRect(x, y, 1, 1, color);
    };
    for (let dx = -rx; dx <= rx; dx++) {
        const dy = Math.round(ry * Math.sqrt(Math.max(0, 1 - (dx / rx) ** 2)));
        put(cx + dx, cy + dy);
        put(cx + dx, cy - dy);
    }
    for (let dy = -ry; dy <= ry; dy++) {
        const dx = Math.round(rx * Math.sqrt(Math.max(0, 1 - (dy / ry) ** 2)));
        put(cx + dx, cy + dy);
        put(cx - dx, cy + dy);
    }
}

function ellipseFill(ctx, cx, cy, rx, ry, color) {
    for (let dy = -ry; dy <= ry; dy++) {
        const w = Math.round(rx * Math.sqrt(Math.max(0, 1 - (dy / ry) ** 2)));
        if (w > 0) ctx.fillRect(cx - w, cy + dy, w * 2 + 1, 1, color);
    }
}

/* Impact stubs, following the cap's ellipse so they sit an even gap off the
 * rim rather than bunching at the flat top and bottom. */
function buttonRays(ctx, cx, cy, progress) {
    const out = BTN_RAY_GAP + Math.round(progress * BTN_RAY_TRAVEL);
    for (let i = 0; i < BTN_RAYS; i++) {
        const a = (Math.PI * 2 * i) / BTN_RAYS;
        const ux = Math.cos(a), uy = Math.sin(a);
        const x0 = cx + ux * (BTN_RX + out);
        const y0 = cy + uy * (BTN_RY + out);
        const x1 = cx + ux * (BTN_RX + out + BTN_RAY_LEN);
        const y1 = cy + uy * (BTN_RY + out + BTN_RAY_LEN);
        line(ctx, Math.round(x0), Math.round(y0), Math.round(x1), Math.round(y1), 1);
    }
}

/*
 * Three states:
 *   idle      raised outline
 *   selected  cap filled with CLK knocked out -- the affordance has to be ON
 *             the control, not only in the footer, because a button has no
 *             value to read and nothing else says it is pressable
 *   fired     cap pressed down BTN_TRAVEL, sides shortened, stubs radiating
 */
function drawButton(ctx, cx, rowY, phase) {
    const pressed = phase.pressed;
    const travel = pressed ? BTN_TRAVEL : 0;
    /* One row of air top and bottom; see the budget above. */
    const capY = rowY + 1 + BTN_RY + travel;
    const baseY = capY + BTN_DEPTH - travel;

    ellipseOutline(ctx, cx, baseY, BTN_RX, BTN_RY, 1, true);   /* base arc */
    line(ctx, cx - BTN_RX, capY, cx - BTN_RX, baseY, 1);        /* sides */
    line(ctx, cx + BTN_RX, capY, cx + BTN_RX, baseY, 1);

    /* Highlighted and pressed both FILL the cap -- the filled disk is the
     * highlight. The outline goes on top of the fill, not instead of it:
     * filling alone left the rim a pixel short at the shallow top and bottom,
     * so the disk looked like it was missing a line. */
    if (phase.filled) ellipseFill(ctx, cx, capY, BTN_RX, BTN_RY, 1);
    ellipseOutline(ctx, cx, capY, BTN_RX, BTN_RY, 1, false);

    /* One burst per press still in flight, so a fast double-tap throws two
     * expanding rings rather than cancelling the first. */
    for (const b of phase.bursts) buttonRays(ctx, cx, capY, b);
}

/*
 * Idle / highlighted / pressed, resolved from the press timestamps alone.
 *
 * A press does NOT clear the bursts already travelling. Overwriting a single
 * timestamp made a rapid second press swallow the first ring and restart from
 * the centre, which reads as an animation glitch rather than as two events.
 * Keeping every press still inside BTN_FLASH_MS lets them overlap, and the
 * cap is pressed if ANY of them is recent.
 *
 * Accepts a bare number as well as a list: a caller that has only ever stamped
 * one time still works.
 */
function buttonPhase(fired, now, held) {
    const stamps = Array.isArray(fired) ? fired : (fired > 0 ? [fired] : []);
    const bursts = [];
    let pressed = false;
    if (typeof now === "number") {
        for (const t of stamps) {
            const age = now - t;
            if (age < 0 || age >= BTN_FLASH_MS) continue;
            bursts.push(age / BTN_FLASH_MS);
            if (age < BTN_PRESS_MS) pressed = true;
        }
    }
    return { pressed, filled: held || bursts.length > 0, bursts };
}

function drawEnumSquare(ctx, kx, ky, text) {
    const w = ENUM_W, h = BOX_H;
    ctx.fillRect(kx, ky, w, 1, 1);
    ctx.fillRect(kx, ky + h - 1, w, 1, 1);
    ctx.fillRect(kx, ky, 1, h, 1);
    ctx.fillRect(kx + w - 1, ky, 1, h, 1);

    const [raw1, raw2] = enumSquareLines(text);
    const line1 = fitLine(raw1, ENUM_TEXT_W);
    const line2 = fitLine(raw2, ENUM_TEXT_W);
    const totalH = line2.length > 0 ? 11 : 5;
    const startY = ky + 1 + Math.floor((h - 2 - totalH) / 2);
    /* Centre within the TEXT area (inside the margins), not the interior, so
     * an odd remainder can never round back onto the frame. */
    const tx = (lw) => centreX(kx + 2, ENUM_TEXT_W, lw);
    fontPrint4x5(ctx, tx(fontWidth4x5(line1)), startY, line1, 1);
    if (line2.length > 0) fontPrint4x5(ctx, tx(fontWidth4x5(line2)), startY + 6, line2, 1);
}

/** A knob that cannot be turned (filepath/canvas/wav_position/string), showing
 * the value's tail. Not part of Movy's vocabulary (its modules do not carry
 * this param type in the fleet this was built against) but needed so an opaque
 * param on a Movy-style page is a door to its existing editor.
 *
 * It draws NO frame: the divable brackets (drawDivableMark) are its frame. It
 * used to draw the same solid box as an enum square, which meant a door and a
 * turnable enum were pixel-identical — you found out which was which by
 * turning one and having nothing happen. */
/*
 * A FRAMED NUMBER, for octave offsets, transposes and voice counts.
 *
 * An arc answers "how far along its range is this", which is right for a cutoff
 * and wrong for an octave: -1 and +1 are two detents apart on a -24..24
 * transpose and read as two nearly identical arcs. For a small integer the
 * number IS the value, so draw the number.
 *
 * THE BOUND IS THE WHOLE DESIGN, and it has to be tight.
 *
 * Measured against the fleet, a span limit of 128 frames 1392 parameters across
 * 60 modules — including obxd volume [0..100], 9w9 tune [0..127] and minijv
 * macro_cutoff [0..127]. Those are continuous AMOUNTS that happen to be typed
 * int, and an arc is the right picture of them. Framing everything is the same
 * mistake divable_mark already records: a mark on almost every cell is a mark
 * on none, and it would destroy the contrast that makes a framed number
 * readable in the first place.
 *
 * The discriminator is span. A small range means the number IS the reading
 * ("octave +2", "8 voices", "channel 10"); a wide one is a sweep, and its
 * position on an arc is what you actually want.
 *
 * A BIPOLAR range earns more room, because the SIGN is information an arc
 * physically cannot show: -1 and +1 sit two detents apart on a -24..24
 * transpose and draw as two nearly identical arcs. That is the case this cell
 * exists for, so it is worth a wider bound than a one-sided count.
 *
 * At 24/48 this frames 273 parameters and refuses every 0..127 amount.
 *
 * An int with no declared range is refused too: nothing bounds how many digits
 * could arrive, and the cell has room for about three.
 */
export const FRAME_MAX_SPAN = 24;
export const FRAME_BIPOLAR_MAX_SPAN = 48;

export function shouldFrameNumber(meta) {
    if (!meta) return false;
    if (meta.kind === KIND_ENUM || meta.kind === KIND_OPAQUE) return false;
    if (meta.type !== "int") return false;
    if (typeof meta.min !== "number" || typeof meta.max !== "number") return false;
    if (!isFinite(meta.min) || !isFinite(meta.max)) return false;
    const span = meta.max - meta.min;
    return span <= (meta.min < 0 ? FRAME_BIPOLAR_MAX_SPAN : FRAME_MAX_SPAN);
}

/*
 * THE SIGN IS THE POINT, and only on a range that HAS a negative side. A bare
 * "2" on a -24..24 transpose is ambiguous in exactly the way this cell exists
 * to fix; a "+4" on a 1..8 voice count is noise, because there is nothing for
 * it to contrast with.
 *
 * An unread value is "--", never 0: a parameter that was never answered
 * reporting a confident zero is the collapse the tri-state read contract exists
 * to prevent, and Number("") is 0 and finite.
 */
export function framedNumberText(meta, raw) {
    if (raw === null || raw === undefined || raw === "") return "--";
    const n = Math.round(Number(raw));
    if (!isFinite(n)) return "--";
    const bipolar = !!meta && typeof meta.min === "number" && meta.min < 0;
    return (n > 0 && bipolar) ? "+" + n : String(n);
}

/*
 * The SAME box the enum square draws — same width, same height, same frame, one
 * centred line of 4x5. Sharing it is the point: these are the two "a value, in
 * a box" cells on the grid, sitting side by side, and two frames drawn by two
 * functions is how a pair like that comes to differ by a pixel that nobody
 * meant. Their pixels are covered by the movy geometry baseline together.
 */
export const FRAME_W = ENUM_W;
export const FRAME_H = BOX_H;

export function drawFramedNumber(ctx, kx, ky, text) {
    drawEnumSquare(ctx, kx, ky, text);
}

function drawOpaqueBox(ctx, kx, ky, value, override) {
    const h = BOX_H;
    const shown = (override === null || override === undefined)
        ? displayValue(value === undefined ? null : value, { kind: KIND_OPAQUE })
        : String(override);
    centeredText(ctx, kx + 2, KW - 4,
        ky + 1 + Math.floor((h - 2 - FONT_H) / 2), fitDev(ctx, shown, KW - 4), 1);
}

/*
 * "You can go into this."
 *
 * Corner brackets around the CELL, drawn after the widget, so the mark is
 * independent of what the widget is: it reads the same over a box, over an arc
 * knob, and over a viz graphic that spans and covers the cell. That is what
 * makes it a grammar rather than a decoration on one widget type — the
 * alternatives tried (dashed frame, dog-ear, chevron) all attach to a frame,
 * and a divable param drawn as a waveform hasn't got one.
 *
 * Rejected: a "..." mark on the label. It collides with truncation, and does so
 * worst exactly here, where the value shown IS truncated ("kick_01.wav" -> "KI")
 * so "SMP.." reads as a cut-off label. Rejected: a box-with-arrow icon; at the
 * ~8px of clear corner a 32px cell has, it does not resolve into a box and an
 * arrow, it resolves into a smudge.
 *
 * Keyed on meta.divable_mark, which is NOT kind === OPAQUE and is no longer
 * meta.divable either. Three different questions:
 *
 *   kind === OPAQUE   a knob cannot turn it. granny's wav_position is a ranged
 *                     number a knob turns perfectly well AND has a waveform
 *                     editor worth opening, so it draws as a KNOB wearing
 *                     brackets; keying the mark on opaqueness made it dead.
 *   divable           a click opens something. Every enum does now.
 *   divable_mark      it wears these brackets. Opaque types only — bracketing
 *                     135 enums would erase the mark's meaning, and the footer
 *                     already says CLK OPEN while such a knob is held.
 *
 * MUST stay inside rowY..rowY+BOX_H-1. One row of overflow lands on LBL0_Y and
 * the brackets merge into the label below.
 */
const BRACKET_LEN = 4;
/**
 * Corner brackets around an arbitrary rect. Exported because the mark works at
 * two scales: around a CELL it says "this parameter opens something", and
 * around a whole menu page body it says the same thing about the page. One
 * grammar, one drawing, so the two can never drift apart.
 *
 * `len` is the arm length. A 32px cell wants the default 4; a 124px page frame
 * wants more, or the corners read as specks rather than as a frame.
 */
export function drawBrackets(ctx, x, y, w, h, len = BRACKET_LEN) {
    for (let i = 0; i < len; i++) {
        ctx.fillRect(x + i, y, 1, 1, 1);
        ctx.fillRect(x + w - 1 - i, y, 1, 1, 1);
        ctx.fillRect(x + i, y + h - 1, 1, 1, 1);
        ctx.fillRect(x + w - 1 - i, y + h - 1, 1, 1, 1);
    }
    for (let i = 0; i < len - 1; i++) {
        ctx.fillRect(x, y + i, 1, 1, 1);
        ctx.fillRect(x + w - 1, y + i, 1, 1, 1);
        ctx.fillRect(x, y + h - 1 - i, 1, 1, 1);
        ctx.fillRect(x + w - 1, y + h - 1 - i, 1, 1, 1);
    }
}

function drawDivableMark(ctx, g, col, rowY) {
    drawBrackets(ctx, cellLeft(g, col) + 1, rowY, g.cellW - 2, BOX_H);
}

function drawKnobWidget(ctx, g, col, rowY, meta, raw, modRaw, liveRaw, cellText, btnPhase) {
    const kx = cellLeft(g, col) + Math.floor((g.cellW - KW) / 2), ky = rowY;
    /* Anything that cannot show two values at once shows the live one, so it
     * animates under modulation instead of freezing on the base. */
    const shown = (liveRaw === null || liveRaw === undefined) ? raw : liveRaw;
    if (meta.kind === KIND_OPAQUE) { drawOpaqueBox(ctx, kx, ky, shown, cellText); return; }
    /*
     * A TRIGGER is a button, so the cell shows the ACTION, not the value.
     *
     * Drawing its "current option" is meaningless — the module reports a
     * constant idle spelling, and for euclidrum that constant is an em-dash,
     * which the 5x7 atlas cannot draw at all. The result was a blank square:
     * a control that looked broken while working perfectly.
     *
     * So the square shows an ACTION mark and the cell's own label underneath
     * says which action it is — "Clear", "Rnd Preset", "Recover Loop".
     *
     * Deliberately NOT options[1]. That is the value that FIRES it, not a verb,
     * and the fleet proves it is not reliably readable: euclidrum's is "Rnd!"
     * (fine) but magneto's is "Cleared" and "Saved" — past participles that
     * read as STATUS under a label that already says "Clear". A module that
     * wants its own wording can supply short_options[1], which is the existing
     * hook for "what this widget should say in three characters".
     */
    if (meta.writeOnly) {
        /* The cell's own label underneath names the action ("Clear", "Rnd
         * Preset"), so the bang carries no text — which also sidesteps the
         * module's idle spelling entirely. euclidrum's is an em-dash the 5x7
         * atlas cannot draw, and drawing it was the original blank square. */
        /* Clock injected, never read here: this renderer is pure with respect
         * to the device, which is what lets the harness draw what the device
         * draws. `now` absent simply means "never lit". */
        drawButton(ctx, cellLeft(g, col) + Math.floor(g.cellW / 2), ky,
                   btnPhase || { pressed: false, filled: false, burst: -1 });
        return;
    }
    if (meta.kind === KIND_ENUM) {
        /* A plugin may report an enum as its option NAME rather than as an
         * index (see learnEnumWireFormat) — resolve either to the index, or
         * `short_options` is skipped for exactly the modules whose values are
         * words and therefore need shortening most. */
        const idx = enumIndexOf(meta, shown);
        /*
         * `short_options` is for the SQUARE only.
         *
         * The square is two lines of the 5x3 font — three characters a line —
         * so a declared option of "Bipolar" or "THRU" has to be cut down. The
         * wrong way to solve that is to declare the short form as the option
         * itself, which is what happened first: the held-knob HEADER then also
         * read "BI" and "THR", and the header exists precisely to show the full
         * value. So options stay full and readable everywhere, and a module may
         * supply a parallel short_options that only this widget consults.
         */
        const shortOpts = Array.isArray(meta.short_options) ? meta.short_options : null;
        const text = (shortOpts && shortOpts[idx] !== undefined)
            ? String(shortOpts[idx])
            : ((Array.isArray(meta.options) && meta.options[idx] !== undefined)
                ? String(meta.options[idx]) : String(shown ?? ""));
        /* Its own centring — it is ENUM_W wide, not KW. */
        drawEnumSquare(ctx, cellLeft(g, col) + Math.floor((g.cellW - ENUM_W) / 2), ky, text);
        return;
    }
    /*
     * A SMALL INT IS A NUMBER, not a position on a range — see
     * shouldFrameNumber. Checked here, after the opaque/writeOnly/enum branches
     * that own their cells outright, and before the arc it replaces.
     */
    if (shouldFrameNumber(meta)) {
        drawFramedNumber(ctx, cellLeft(g, col) + Math.floor((g.cellW - FRAME_W) / 2), ky,
                         framedNumberText(meta, raw));
        return;
    }
    /* `?? 0` because the ARC has to point somewhere: an unread value draws its
     * "--" in the label cell below, and a knob with no pointer at all would
     * read as a missing widget rather than a missing value. The LED, which
     * shares this normaliser, treats null differently — see knob_leds.mjs. */
    drawArcKnob(ctx, kx, ky, normalizedOf(meta, raw) ?? 0);
    /*
     * The dot is drawn whenever a source is driving this parameter, INCLUDING
     * when the live value has landed exactly on the base.
     *
     * It used to be suppressed within 0.02 of the base, on the reasoning that
     * "a dot sitting under the pointer says nothing and just thickens it".
     * That is true about the pixels and wrong about the meaning: absent, the
     * knob is pixel-identical to an unmodulated one, so the reading is not
     * "the LFO is at its base" but "there is no LFO". Reported from the
     * device -- a bipolar LFO on a knob at 0 spends half its cycle clamped
     * there, and the indicator vanished for that half.
     *
     * The label keeps its own tilde either way, but that is four pixels on
     * the row beneath; the dot is the mark you actually read on the knob.
     */
    if (modRaw !== null && modRaw !== undefined) {
        /* The SAME normaliser as the pointer above. The dot and the pointer are
         * read against each other -- "how far the LFO has pulled it from where
         * you set it" -- so two mappings would make that distance meaningless.
         * null (unread, or a range that cannot be normalised) draws no dot,
         * which is right: there is nothing to compare. */
        const modNorm = normalizedOf(meta, modRaw);
        if (modNorm !== null) drawModDot(ctx, kx, ky, modNorm);
    }
}

/* schwung-movy renderer/label.ts drawWaveMark (the modulation tilde), ported. */
function drawWaveMark(ctx, x, y, on) {
    ctx.fillRect(x, y, 1, 1, on);
    ctx.fillRect(x + 2, y, 1, 1, on);
    ctx.fillRect(x + 1, y + 1, 1, 1, on);
    ctx.fillRect(x + 3, y + 1, 1, 1, on);
}

/**
 * schwung-movy renderer/label.ts drawLabelCell, ported. Shows the short name
 * normally, and the value while touched (inverted strip) — Movy's automation
 * dot has no equivalent in this library's data model (that is a
 * per-lane-automation concept the grid does not have) so only the touched
 * inversion and the modulation mark are ported.
 */
/*
 * `showValue` and `inverted` are separate arguments although every caller
 * currently passes the same value for both: a held cell shows its value in an
 * inverted strip. They came apart for a gesture that has since been removed,
 * and are kept apart because "which text" and "which polarity" are genuinely
 * two questions — collapsing them is what made the removed gesture hard to
 * express in the first place.
 */
function drawLabelCell(ctx, g, col, lblY, label, displayValue, showValue, inverted, modulated) {
    const cellX = cellLeft(g, col);
    const text = showValue ? displayValue : label;
    const tw = fontWidth4x5(text);
    /* Same rule as every other centred run — `floor(CELL_W/2) - floor(tw/2)`
     * biases the other way on odd widths, which made a label and the box above
     * it disagree by a pixel. */
    const tx = centreX(cellX, g.cellW, tw);
    /* One clear row above and below the glyphs inside the band — see LBL_H. */
    const ty = lblY + Math.floor((LBL_H - LBL_FONT_H) / 2);
    if (inverted) {
        ctx.fillRect(cellX, lblY, g.cellW, LBL_H, 1);
        fontPrint4x5(ctx, tx, ty, text, 0);
    } else {
        fontPrint4x5(ctx, tx, ty, text, 1);
    }
    if (modulated) {
        const wx = Math.max(cellX, tx - 6);
        drawWaveMark(ctx, wx, lblY + 1, inverted ? 0 : 1);
    }
}

/* --------------------------------------------------------------- one row */

/*
 * A partial geometry is a caller bug, not a shorthand. `{cellW: 29}` leaves
 * `x0` undefined, every `cellLeft` result NaN, and the knob pointer reaches
 * render_page.mjs's `line()` -- a `for (;;)` loop whose exit condition is
 * `x0 === x1 && y0 === y1` -- which NaN never satisfies, so it spins the UI
 * tick forever. Confirmed: the call did not return in 8 seconds. Fail loudly
 * at the boundary instead of drawing a plausibly-wrong screen or hanging.
 *
 * Deliberately NOT `{ ...GRID_GEOM, ...geom }`: that would make a typo like
 * `{x: 6, cellW: 30}` silently fall back to x0=0 and draw a screen that looks
 * plausible and is wrong, instead of throwing.
 */
function resolveGeom(geom) {
    if (geom === undefined || geom === null) return GRID_GEOM;
    if (!Number.isFinite(geom.x0) || !Number.isFinite(geom.cellW))
        throw new TypeError("drawKnobRow: geom needs both x0 and cellW");
    return geom;
}

/**
 * One row of the knob grid: four cells, each a widget above a label band.
 *
 * A public entry point for knob_card.mjs, which does not exist yet, so its
 * non-obvious constraints are recorded here rather than left to be
 * rediscovered by that caller:
 *
 *   - always exactly 4 columns (`slotBase = row * 4`) -- there is no
 *     narrower strip;
 *   - `rowY` and `lblY` are ABSOLUTE screen rows, not offsets from `geom`;
 *   - `geom` is all-or-nothing -- see `resolveGeom` above. Omit it entirely
 *     for the grid's own {x0: 0, cellW: CELL_W}, or pass both fields.
 */
export function drawKnobRow(ctx, o, row, rowY, lblY, geom) {
    const g = resolveGeom(geom);
    const { page, metaIndex, values, touched, modulated, viz, modValues } = o;
    /*
     * EVERY held knob inverts, not just the one the header follows. A single
     * index could not express two fingers: touching a second knob overwrote it
     * and releasing that one cleared it, so a knob still under a finger stopped
     * being highlighted. Falls back to the single `touched` when a caller does
     * not supply the set.
     */
    const held = Array.isArray(o.touchedSlots) && o.touchedSlots.length
        ? o.touchedSlots
        : (typeof touched === "number" && touched >= 0 ? [touched] : []);
    /*
     * What each widget animates.
     *
     * A knob can legibly show TWO values — pointer on the base, dot on the arc
     * — so it does. Nothing else can: a filter curve cannot be drawn twice
     * without becoming unreadable, and an enum square has one line of text.
     * Those widgets therefore show the LIVE value and no baseline, which is
     * also what makes them animate under modulation.
     *
     * This matters because `values` now holds the BASE for a modulated key
     * (the pointer needs it). Handing that straight to viz/enum would freeze
     * a modulated filter curve at the value you dialled in, which is a
     * regression against showing the effective value.
     *
     * Only cloned when something is ACTUALLY modulated. `modValues` is always
     * an object, so a truthiness test cloned `values` twice per frame (once
     * per row) on every page whether or not a source was running — and
     * `values` is not page-sized, it accumulates every key the read cursor has
     * ever reached, which on a 76-page module is hundreds. Allocating and
     * copying that at 55fps is pure garbage for the overwhelmingly common case
     * of nothing modulated at all. `hasMod` makes the empty case free.
     */
    let hasMod = false;
    if (modValues) { for (const _k in modValues) { hasMod = true; break; } }
    const liveValues = hasMod ? Object.assign({}, values, modValues) : values;
    const slotBase = row * 4;

    const covered = new Array(4).fill(false);
    for (const group of (viz || [])) {
        if (!group || typeof group.slotStart !== "number") continue;
        if (Math.floor(group.slotStart / 4) !== row) continue;
        const localStart = group.slotStart - slotBase;
        for (let s = localStart; s < localStart + group.slotSpan && s < 4; s++) covered[s] = true;
        /* The widget band's height is the gap between the row and its label,
         * not a constant -- LBL0_Y - ROW0_Y is only correct for the grid's
         * own two rows because both of the grid's gaps happen to be 15px. */
        drawVizGroup(ctx, {
            x: cellLeft(g, localStart), y: rowY,
            w: group.slotSpan * g.cellW, h: lblY - rowY,
        }, group, liveValues, metaIndex);
    }

    for (let col = 0; col < 4; col++) {
        const slot = slotBase + col;
        const cellX = cellLeft(g, col);
        const key = page.keys[slot];
        if (!key) continue;
        const meta = metaIndex.getOrGuess(key);
        const raw = values ? values[key] : null;
        const isTouched = held.indexOf(slot) >= 0;

        /*
         * A value the HOST resolves, per surface. `displayFor` is the same
         * separate-short-and-long-labels idea short_options solves for enums,
         * for a value whose reading cannot be declared statically: an LFO's
         * target is stored as "fx1" and reads "FX 1: Room Size", and only the
         * host knows what is loaded in fx1. Optional everywhere; the ordinary
         * displayValue path is untouched when no formatter is supplied.
         */
        const cellText = o.displayFor ? o.displayFor(key, raw, "cell") : null;

        if (!covered[col]) {
            /* modValues holds the live modulated value for keys a source is
             * driving; `values` stays the base the user dialled in. */
            /* Knob: base + dot. Enum/opaque: the live value, no baseline —
             * drawKnobWidget picks per kind. */
            /* Trigger flash: injected, never clocked here — this renderer is
             * pure with respect to the device, which is what lets the harness
             * draw what the device draws. Absent means "never lit". */
            const firedAt = (o.triggerFiredAt && o.triggerFiredAt[key]) || 0;
            const btnPhase = buttonPhase(firedAt, o.nowMs, isTouched);
            drawKnobWidget(ctx, g, col, rowY, meta, raw,
                           modValues ? modValues[key] : undefined,
                           liveValues ? liveValues[key] : undefined,
                           cellText, btnPhase);
        }

        /* Budget in CHARACTERS, not pixels: Elektron's labels are 3-4 glyphs
         * whether or not more would technically fit, which is what keeps a row
         * of them scannable. `M` is the widest glyph, so measuring that many
         * of it gives a width no LABEL_CHARS-long label can exceed. */
        /* AFTER the widget and OUTSIDE the `covered` test: a viz group that
         * spans the cell (mrsample's SMP waveform) is still divable, and the
         * mark is the only thing that says so. */
        /* `divable_mark`, NOT `divable` — an enum opens a picker but is not
         * bracketed. See param_meta.mjs normalize() for why the mark cannot
         * simply follow divability. */
        if (meta.divable_mark) drawDivableMark(ctx, g, col, rowY);


        const label = labelForCell(meta.label || meta.key, g.cellW);
        const display = fitDev(ctx,
            (cellText === null || cellText === undefined) ? displayValue(raw, meta) : String(cellText),
            g.cellW - 2);
        drawLabelCell(ctx, g, col, lblY, label, display, isTouched, isTouched,
                      modulated ? !!modulated(key) : false);
    }
}

/*
 * The hint footer.
 *
 * Hints come from the CALLER, never from here: which gesture does what is the
 * input mapping's business, and this library does not own input (same rule that
 * puts the first-run hint panel's text in the caller's hands). A tool embedding
 * the grid has its own gestures and must be able to say so.
 *
 * FIT-AWARE, in priority order. Measured across the real vocabulary, a pair
 * costs 24-38px, so THREE pairs only fit when every word is <= 4 characters:
 *
 *     JOG PAGE / SHFT SECT / CLK MENU     134px   does NOT fit
 *     JOG SCRUB / CLK OPEN / MUTE DFLT    139px   does NOT fit
 *     JOG SEL / CLK LOAD / BACK EXIT      126px   fits
 *     any two pairs                       84-98px always fits
 *
 * So the caller passes hints most-important-first and the tail is dropped, not
 * squeezed: a half-drawn pill running off the right edge is worse than an
 * absent one.
 *
 * BACK IS THE EXCEPTION, AND IT IS A RULE: a back hint is PINNED TO THE RIGHT
 * EDGE, and it is the last hint to be dropped, not the first.
 *
 * Being last in the array is not the same as being on the right. With two
 * pairs, packing left-to-right leaves BACK sitting mid-screen with ~30px of
 * space beside it — which is what "JOG MOVE / BACK EXIT" looked like on the
 * chain editor under Shift. "Back is bottom-right" is a fixed place the user
 * can learn, so it must not move with the number of hints beside it.
 *
 * This also removes the old advice to "put BACK first if it matters", which
 * traded the rule away to survive the tail-drop. Pinning gets both: BACK holds
 * the right edge AND is protected, so the hints that lose to a narrow screen
 * are the middle ones.
 */
export const HINT_PAD = 2;
export const HINT_GAP = 4;

/** Width one [KEY] ACTION pair occupies, so a caller can budget before drawing. */
export function hintPairWidth(key, action) {
    return fontWidth4x5(caps(key)) + HINT_PAD + HINT_GAP
         + fontWidth4x5(caps(action)) + HINT_GAP;
}

/**
 * @param {Array<[string,string]>} hints  [key, action] pairs, most important first
 * @returns {number} how many pairs were drawn
 */
/** Is this hint the BACK affordance? The one hint with a fixed home. */
export function isBackHint(h) {
    return !!h && /^back$/i.test(String(h[0]).trim());
}

/*
 * The footer canon, for the NEW footer only.
 *
 * menu_layout.mjs has FOOTER_VERBS for the old text footer, pinned by
 * tests/shadow/test_footer_verb_consistency.sh — that canon exists because the
 * old footer drifted into verb soup ("Push:" vs "Click:", "Exit" vs "exit").
 * The new footer never got the same treatment, so this is it.
 *
 * KEYS name the physical control, so they are fixed by the hardware, not by
 * taste. ACTIONS are free except after BACK.
 *
 * BACK's action word says WHERE BACK GOES, and the two are not synonyms:
 *
 *   EXIT  leaves this view entirely   (module picker, preset browser list)
 *   OUT   rises one level, staying in the view
 *         (an entered menu page, a confirm modal)
 *
 * Keeping both is deliberate. Collapsing them would tell the user "back" does
 * one thing when it does two, and the difference is the one thing they cannot
 * see before pressing it.
 */
export const FOOTER_CANON = Object.freeze({
    keys: Object.freeze(["JOG", "CLK", "BACK", "SHFT", "MUTE", "KNB"]),
    backActions: Object.freeze(["EXIT", "OUT"]),
});

export function drawFooter(ctx, hints) {
    ctx.fillRect(0, RULE_Y, W, 1, 1);
    if (!hints || !hints.length) return 0;
    const ty = FOOTER_Y + Math.floor((FOOTER_H - FONT4_HEIGHT) / 2);

    const list = hints.filter(Boolean);
    /* Exactly one back hint is pinned. If a caller passes two (nobody does),
     * the extra stays an ordinary hint rather than fighting for the same x. */
    const backIdx = list.findIndex(isBackHint);
    const back = backIdx >= 0 ? list[backIdx] : null;
    const flow = backIdx >= 0 ? list.filter((_, i) => i !== backIdx) : list;

    /* One pair, drawn with its KEY inverted into a pill and its ACTION plain,
     * so the pair reads as one thing. Without the pill a row of hints is an
     * unparseable run: "JOG PAGE CLK MENU BACK EXIT". */
    const drawPair = (x, h) => {
        const key = caps(h[0]), action = caps(h[1]);
        const kw = fontWidth4x5(key);
        ctx.fillRect(x, ty - 1, kw + HINT_PAD * 2, FONT4_HEIGHT + 2, 1);
        fontPrint4x5(ctx, x + HINT_PAD, ty, key, 0);
        fontPrint4x5(ctx, x + kw + HINT_PAD + HINT_GAP, ty, action, 1);
    };

    let drawn = 0;
    /* Reserve the back hint's room BEFORE laying anything else out — that is
     * what makes the middle hints lose the fight for a narrow screen instead
     * of BACK losing it. */
    const backW = back ? hintPairWidth(caps(back[0]), caps(back[1])) : 0;
    const backX = back ? W - backW : W;
    const limit = back ? backX : W;

    let x = 1;
    for (const h of flow) {
        if (x + hintPairWidth(caps(h[0]), caps(h[1])) > limit) break;
        drawPair(x, h);
        x += hintPairWidth(caps(h[0]), caps(h[1]));
        drawn++;
    }
    if (back) { drawPair(Math.max(x, backX), back); drawn++; }
    return drawn;
}

/* ------------------------------------------------------------------ page */

/**
 * @param {object} ctx  draw context { fillRect, print, textWidth }
 * @param {object} o
 * @param {object} o.page        a PAGE_KNOBS page from page_plan.planPages
 * @param {object} o.metaIndex   from param_meta.buildMetaIndex
 * @param {object} o.values      key -> raw value
 * @param {string} o.title       left side of the header
 * @param {number} o.pageIndex
 * @param {number} o.pageCount
 * @param {number} [o.touched]   physical knob 0-7 currently held, or -1
 * @param {Array}  [o.pageGroups] one bank id per page, for the bank bar
 * @param {Function} [o.modulated] (key) => boolean
 * @param {Array}  [o.viz]       resolved graphic groups (viz.mjs resolveViz)
 * @param {Array}  [o.footer]    [key, action] hint pairs, most important first
 */
/**
 * The preset browser's body, in the page chrome.
 *
 * A preset level publishes a count, an index and the name of whichever preset
 * is selected — there is no list to show, so this shows the one you are on, as
 * large as the space allows, with its position underneath. Same header, same
 * bank bar, same footer as every other page; only the body differs.
 *
 * @param {object} o  { name, index, count, entered }
 */
export function drawPresetBody(ctx, rect, o) {
    const name = o && o.name ? String(o.name) : "--";
    const has = o && typeof o.count === "number" && o.count > 0;
    const pos = has ? `${(o.index | 0) + 1} of ${o.count}` : "";

    /* The NAME in the big face, centred, truncated to the full width. Preset
     * names run long ("SQ Fat Analog Brass 3") and there is nothing else on
     * this page competing for the room. */
    const nameY = rect.y + Math.floor((rect.h - FONT_H - FONT4_HEIGHT - 3) / 2);
    const shown = fitText({ textWidth: tzWidth }, caps(name), rect.w - 8);
    tzPrint(ctx, centreX(rect.x, rect.w, tzWidth(shown)), nameY, shown, 1);

    /* Position underneath in the small face: it is reference, not the thing
     * you are reading. */
    if (pos) {
        const py = nameY + FONT_H + 3;
        fontPrint4x5(ctx, centreX(rect.x, rect.w, fontWidth4x5(caps(pos))), py, caps(pos), 1);
    }
}

export function renderPageMovy(ctx, o) {
    const page = o.page;
    const touched = typeof o.touched === "number" ? o.touched : -1;

    if (touched >= 0 && page && page.keys && page.keys[touched] && o.metaIndex) {
        const hk = page.keys[touched];
        const m = o.metaIndex.getOrGuess(hk);
        const v = o.values ? o.values[hk] : null;
        /* "header", not "cell": this is the surface with room, and the whole
         * reason a host-resolved value has two forms at all. */
        const hv = o.displayFor ? o.displayFor(hk, v, "header") : null;
        drawHeader(ctx, m.label || m.key,
                   (hv === null || hv === undefined) ? displayValue(v, m) : String(hv), true);
    } else {
        /* pageLabel, not page.name: a page belonging to a CHILD level is
         * named after WHICH CHILD it is showing, which the planned name
         * cannot know. Falls back to the name for every other page. */
        drawHeader(ctx, o.title || "",
                   (o.pageLabel !== undefined && o.pageLabel !== null)
                       ? o.pageLabel
                       : (page ? page.name : null),
                   false);
    }
    drawBankBar(ctx, o.pageIndex | 0, Math.max(1, o.pageCount | 0), o.pageGroups);

    if (!page || !page.keys) return;
    const hasParams = page.keys.some(Boolean);
    if (!hasParams) {
        tzPrint(ctx, 2, ROW0_Y + 4, caps("No params"), 1);
        if (o.footer) drawFooter(ctx, o.footer);
        return;
    }

    drawKnobRow(ctx, o, 0, ROW0_Y, LBL0_Y);
    drawKnobRow(ctx, o, 1, ROW1_Y, LBL1_Y);
    if (o.footer) drawFooter(ctx, o.footer);
}
