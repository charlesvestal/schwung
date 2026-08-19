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

import { KIND_ENUM, KIND_OPAQUE } from "./param_meta.mjs";
import { formatParamValue } from "../param_format.mjs";
import { asciiFold, fitText, shortenLabel, line } from "./render_page.mjs";
import { drawVizGroup } from "./viz_draw.mjs";
import { enumSquareLines } from "./font5x3.mjs";
import { fontPrint as tzPrint, fontWidth as tzWidth, HEIGHT as TZ_H } from "./font_tamzen6x12.mjs";
import { fontWidth4x5, fontPrint4x5, FONT4_HEIGHT, FONT4_MEASURE } from "./font4x5.mjs";

/**
 * Text is set in the DEVICE 5x7 font, in caps, with labels abbreviated to a
 * mnemonic. font4x5 is kept for the enum square alone.
 *
 * On hardware the 5-row font read as too small — legibility mattered more
 * than knob size, and the Move panel is physically smaller than the Elektron
 * one this grid was measured against, so matching its pixel geometry does not
 * reproduce its apparent size. The device font is 40% taller (7 rows vs 5).
 *
 * It costs almost nothing horizontally, because the labels are already
 * mnemonics: at a 6px monospaced advance a 4-character label is 24px in a
 * 30px cell, against 23px for the same label in font4x5, whose M is 6 wide
 * too. Vertically it costs 2 rows per label band (LBL_H 7 -> 9), paid for by
 * the bottom pad and one row off each widget row.
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
const LABEL_CHARS = 4;

/** Uppercase, ascii-folded — the Elektron register. font4x5 has no lowercase. */
function caps(s) { return asciiFold(String(s == null ? "" : s)).toUpperCase(); }
/** fitText/shortenLabel measure through ctx.textWidth; hand them Tamzen. */
const TZ_MEASURE = { textWidth: tzWidth };
function fitDev(ctx, s, maxWidth) { return caps(fitText(TZ_MEASURE, caps(s), maxWidth)); }

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
};

/** Word-level pass before shortenLabel. Numbers and short words pass through. */
function preAbbreviate(label) {
    const s = asciiFold(String(label == null ? "" : label)).trim();
    if (!s) return "";
    return s.split(/[\s_]+/).filter(Boolean)
        .map((w) => WORD_ABBREV[w.toLowerCase()] || w)
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
 * for a 2px gutter on both sides of every label:
 *
 *      0..6    header      (7)   text at y=1, 5 tall
 *      7..8    bank bar    (2)
 *      9..10   gutter      (2)
 *     11..26   knob row 0  (16)  16 because viz bodies need rect.y+1..+14
 *     27..28   gutter      (2)
 *     29..34   label row 0 (6)
 *     35..36   gutter      (2)
 *     37..52   knob row 1  (16)
 *     53..54   gutter      (2)
 *     55..60   label row 1 (6)
 *     61..63   bottom pad  (3)
 *
 * The knob itself is only ~12 rows tall now that the arc is open at the
 * bottom, so a knob row carries more air below it than a graphic row does.
 * The 16 is held for the graphics, which still draw a full 15-row body.
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
export const ROW0_Y = 12;
export const LBL0_Y = 28;
export const ROW1_Y = 39;
export const LBL1_Y = 55;
export const CELL_W = 32;
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
 */
export const LBL_H = 9;
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
    const l = fit5(left, right ? Math.floor(W * 0.55) : W - 4);
    fontPrint4x5(ctx, 2, 1, l, color);
    if (right) {
        const r = fit5(right, Math.floor(W * 0.6));
        fontPrint4x5(ctx, W - fontWidth4x5(r) - 2, 1, r, color);
    }
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
 * track between them. A 2x2 centred here spans roughly r-1 .. r+0.7: far
 * enough out to read as riding the arc rather than floating in the middle of
 * the knob, close enough in that it does not overwrite the ring and break the
 * circle's silhouette.
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
 * A filled 2x2 square, not a circle. At r=7 a rasterised disc of any useful
 * size either disappears into the 1px arc or swamps it, and a square is one
 * fill_rect (487ns) against a circle's scan. Drawn ON the arc radius so it
 * reads as travelling along the track rather than floating.
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

/** A knob that cannot be turned (filepath/canvas/wav_position/string): same
 * framed box as an enum square, showing the value's tail. Not part of Movy's
 * vocabulary (its modules do not carry this param type in the fleet this was
 * built against) but needed so an opaque param on a Movy-style page is a door
 * to its existing editor rather than a blank cell. */
function drawOpaqueBox(ctx, kx, ky, value) {
    const h = BOX_H;
    ctx.fillRect(kx, ky, KW, 1, 1);
    ctx.fillRect(kx, ky + h - 1, KW, 1, 1);
    ctx.fillRect(kx, ky, 1, h, 1);
    ctx.fillRect(kx + KW - 1, ky, 1, h, 1);
    const shown = String(value == null ? "" : value).split("/").pop() || "--";
    /* Centre inside the text area (frame + 1px margin on each side) both ways,
     * the same spans the enum square uses. */
    centeredText(ctx, kx + 2, KW - 4,
        ky + 1 + Math.floor((h - 2 - FONT_H) / 2), fitDev(ctx, shown, KW - 4), 1);
}

function drawKnobWidget(ctx, col, rowY, meta, raw, modRaw, liveRaw) {
    const kx = col * CELL_W + Math.floor((CELL_W - KW) / 2), ky = rowY;
    /* Anything that cannot show two values at once shows the live one, so it
     * animates under modulation instead of freezing on the base. */
    const shown = (liveRaw === null || liveRaw === undefined) ? raw : liveRaw;
    if (meta.kind === KIND_OPAQUE) { drawOpaqueBox(ctx, kx, ky, shown); return; }
    if (meta.kind === KIND_ENUM) {
        const idx = Math.round(Number(shown));
        const text = (Array.isArray(meta.options) && meta.options[idx] !== undefined) ? String(meta.options[idx]) : String(shown ?? "");
        /* Its own centring — it is ENUM_W wide, not KW. */
        drawEnumSquare(ctx, col * CELL_W + Math.floor((CELL_W - ENUM_W) / 2), ky, text);
        return;
    }
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    const num = Number(raw);
    const normVal = (max > min && isFinite(num)) ? Math.max(0, Math.min(1, (num - min) / (max - min))) : 0;
    drawArcKnob(ctx, kx, ky, normVal);
    /* Only when modulation is actually driving this param somewhere OTHER
     * than the base — a dot sitting under the pointer says nothing and just
     * thickens it. */
    if (modRaw !== null && modRaw !== undefined) {
        const mnum = Number(modRaw);
        if (isFinite(mnum) && max > min) {
            const modNorm = Math.max(0, Math.min(1, (mnum - min) / (max - min)));
            if (Math.abs(modNorm - normVal) > 0.02) drawModDot(ctx, kx, ky, modNorm);
        }
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
function drawLabelCell(ctx, col, lblY, label, displayValue, touched, modulated) {
    const cellX = col * CELL_W;
    const text = touched ? displayValue : label;
    const tw = tzWidth(text);
    /* Same rule as every other centred run — `floor(CELL_W/2) - floor(tw/2)`
     * biases the other way on odd widths, which made a label and the box above
     * it disagree by a pixel. */
    const tx = centreX(cellX, CELL_W, tw);
    /* One clear row above and below the glyphs inside the band — see LBL_H. */
    const ty = lblY + Math.floor((LBL_H - FONT_H) / 2);
    if (touched) {
        ctx.fillRect(cellX, lblY, CELL_W, LBL_H, 1);
        tzPrint(ctx, tx, ty, text, 0);
    } else {
        tzPrint(ctx, tx, ty, text, 1);
    }
    if (modulated) {
        const wx = Math.max(cellX, tx - 6);
        drawWaveMark(ctx, wx, lblY + 1, touched ? 0 : 1);
    }
}

/* --------------------------------------------------------------- one row */

function drawKnobRow(ctx, o, row, rowY, lblY) {
    const { page, metaIndex, values, touched, modulated, viz, modValues } = o;
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
     */
    const liveValues = modValues ? Object.assign({}, values, modValues) : values;
    const slotBase = row * 4;

    const covered = new Array(4).fill(false);
    for (const g of (viz || [])) {
        if (!g || typeof g.slotStart !== "number") continue;
        if (Math.floor(g.slotStart / 4) !== row) continue;
        const localStart = g.slotStart - slotBase;
        for (let s = localStart; s < localStart + g.slotSpan && s < 4; s++) covered[s] = true;
        drawVizGroup(ctx, {
            x: localStart * CELL_W, y: rowY, w: g.slotSpan * CELL_W, h: LBL0_Y - ROW0_Y,
        }, g, liveValues, metaIndex);
    }

    for (let col = 0; col < 4; col++) {
        const slot = slotBase + col;
        const key = page.keys[slot];
        if (!key) continue;
        const meta = metaIndex.getOrGuess(key);
        const raw = values ? values[key] : null;
        const isTouched = touched === slot;

        if (!covered[col]) {
            /* modValues holds the live modulated value for keys a source is
             * driving; `values` stays the base the user dialled in. */
            /* Knob: base + dot. Enum/opaque: the live value, no baseline —
             * drawKnobWidget picks per kind. */
            drawKnobWidget(ctx, col, rowY, meta, raw,
                           modValues ? modValues[key] : undefined,
                           liveValues[key]);
        }

        /* Budget in CHARACTERS, not pixels: Elektron's labels are 3-4 glyphs
         * whether or not more would technically fit, which is what keeps a row
         * of them scannable. `M` is the widest glyph, so measuring that many
         * of it gives a width no LABEL_CHARS-long label can exceed. */
        const labelWidth = Math.min(CELL_W - 2, tzWidth("M".repeat(LABEL_CHARS)));
        const label = caps(shortenLabel(TZ_MEASURE, preAbbreviate(meta.label || meta.key), labelWidth));
        const display = (raw === null || raw === undefined)
            ? "--" : fitDev(ctx, formatParamValue(raw, meta), CELL_W - 2);
        drawLabelCell(ctx, col, lblY, label, display, isTouched, modulated ? !!modulated(key) : false);
    }
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
 */
export function renderPageMovy(ctx, o) {
    const page = o.page;
    const touched = typeof o.touched === "number" ? o.touched : -1;

    if (touched >= 0 && page && page.keys && page.keys[touched] && o.metaIndex) {
        const m = o.metaIndex.getOrGuess(page.keys[touched]);
        const v = o.values ? o.values[page.keys[touched]] : null;
        const val = (v === null || v === undefined) ? "--" : formatParamValue(v, m);
        drawHeader(ctx, m.label || m.key, val, true);
    } else {
        drawHeader(ctx, o.title || "", page ? page.name : null, false);
    }
    drawBankBar(ctx, o.pageIndex | 0, Math.max(1, o.pageCount | 0), o.pageGroups);

    if (!page || !page.keys) return;
    const hasParams = page.keys.some(Boolean);
    if (!hasParams) {
        tzPrint(ctx, 2, ROW0_Y + 4, caps("No params"), 1);
        return;
    }

    drawKnobRow(ctx, o, 0, ROW0_Y, LBL0_Y);
    drawKnobRow(ctx, o, 1, ROW1_Y, LBL1_Y);
}
