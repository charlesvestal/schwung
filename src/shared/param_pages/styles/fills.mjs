/**
 * styles/fills.mjs — SET 3: ten treatments of the footer band.
 *
 * The shipping footer (`drawFooter`, render_page_movy.mjs) is a 1px solid rule
 * across the whole screen at RULE_Y (55) with a row of key/action hint pairs
 * below it, each key inverted into a pill. The pill is ours and is good; the
 * FULL-WIDTH HAIRLINE separating a body from a status strip is the single most
 * generic piece of chrome on the screen — it is what every 1-bit device does,
 * Elektron included, and it is the cheapest thing in the whole UI to change
 * because nothing reads it, it only reads as a boundary.
 *
 * The axis is minimal -> radical:
 *
 *   1-5   the boundary is still a RULE. Only its texture changes: solid,
 *         absent, dotted, dashed, doubled. Nothing about the hints moves.
 *   6-7   the boundary becomes a GROUND. The footer is a filled band and the
 *         hints sit in or on it, which changes the figure/ground relationship
 *         of the whole screen rather than the width of one line.
 *   8-10  the footer stops being uniform and starts POINTING — a tab, a
 *         tether, a terrain, each of which says which cell the hints are about.
 *
 * BUDGET, and it is tight. Everything lives in rows RULE_Y (55) through 63,
 * nine rows, and the hint text is five of them. Two rows of that budget are
 * spent by the pill (one above the glyphs, one below), which is why several
 * options here shift `ty` or drop the pill padding rather than growing the
 * band upward.
 *
 * NOTHING may reach row 7. That is the bank bar's row, and it is the reason
 * the enum picker's list starts at y=9 rather than at MENU_LIST_Y (10) — this
 * codebase has already lost a menu row to something drawn over that band. No
 * option here goes above 55, which is 48 rows clear, but the rule is written
 * down so the next author of a "grow the footer upward" option knows where the
 * wall is.
 *
 * ACTIVE CELL. Options 8, 9 and 10 point at one of the four columns. Which one
 * is not available from `(ctx, hints)` — the shipping signature carries no
 * focus — so the catalog draws them pointing at column 2 and the note says so.
 * Adopting any of the three means widening the signature, which is a real cost
 * and is stated in each note rather than hidden.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import {
    W, RULE_Y, FOOTER_Y, FOOTER_H, CELL_W,
    HINT_PAD, HINT_GAP, hintPairWidth, isBackHint, drawFooter,
} from "../render_page_movy.mjs";
import { fontWidth4x5, fontPrint4x5, FONT4_HEIGHT } from "../font4x5.mjs";
import {
    DIAG_LIGHT, DIAG_THIRD,
    fillDithered, fillTerrain, dottedRule, notchCorners,
} from "./dither.mjs";

const up = (s) => String(s == null ? "" : s).toUpperCase();

/**
 * Which column the hints are about. Not derivable from `(ctx, hints)`; see the
 * header note. Column 2 of 4, so the mark lands where a mark is hardest to
 * confuse with the screen edge.
 */
const ACTIVE_COL = 2;
const ACTIVE_X = ACTIVE_COL * CELL_W;
const ACTIVE_CX = ACTIVE_X + (CELL_W >> 1);

/**
 * drawFooter's own layout, extracted so an option can reuse the positions
 * while drawing the pairs differently.
 *
 * Reserving the back hint's room BEFORE laying anything else out is what makes
 * the middle hints lose the fight for a narrow screen instead of BACK losing
 * it — copied rather than changed, because a set that quietly relaid the
 * footer would be comparing two things at once.
 */
function layout(hints) {
    const list = (hints || []).filter(Boolean);
    const backIdx = list.findIndex(isBackHint);
    const back = backIdx >= 0 ? list[backIdx] : null;
    const flow = backIdx >= 0 ? list.filter((_, i) => i !== backIdx) : list;

    const backW = back ? hintPairWidth(back[0], back[1]) : 0;
    const backX = back ? W - backW : W;
    const limit = back ? backX : W;

    const out = [];
    let x = 1;
    for (const h of flow) {
        const w = hintPairWidth(h[0], h[1]);
        if (x + w > limit) break;
        out.push({ x, h });
        x += w;
    }
    if (back) out.push({ x: Math.max(x, backX), h: back });
    return out;
}

/**
 * One key/action pair with every colour a variable.
 *
 * `padY` 1 gives the shipping pill (a row of ground above and below the
 * glyphs). Options 5, 9 and 10 spend a row of the nine on a second rule, a
 * tether or a ridge, so they pass `padTop: 0` — the pill loses its top row and
 * keeps its bottom one.
 *
 * ASYMMETRIC, not padY: 0, on purpose. A pill exactly as tall as its glyphs
 * puts white against the top and bottom of every stroke at once, and an
 * inverted 4x5 face with no ground at all around it is the worst-legibility
 * thing this catalog can draw. Keeping one row of the pair is the difference
 * between cramped and unreadable, and it is free — the row below the glyphs is
 * inside the budget either way.
 */
function pair(ctx, x, ty, h, o) {
    const key = up(h[0]), action = up(h[1]);
    const kw = fontWidth4x5(key), aw = fontWidth4x5(action);
    const padY = o.padY === undefined ? 1 : o.padY;
    const padTop = o.padTop === undefined ? padY : o.padTop;
    const padBot = o.padBot === undefined ? padY : o.padBot;
    const pillC = o.pill === undefined ? 1 : o.pill;
    const keyC = o.keyText === undefined ? 0 : o.keyText;
    const actC = o.actionText === undefined ? 1 : o.actionText;

    const pillY = ty - padTop, pillW = kw + HINT_PAD * 2, pillH = FONT4_HEIGHT + padTop + padBot;
    ctx.fillRect(x, pillY, pillW, pillH, pillC);

    /*
     * ALL FOUR corners notched.
     *
     * The 1px notch is the convergent idiom this catalog keeps (see the spec):
     * at one pixel and two colours there is no second way to soften a corner.
     *
     * The top two were done first on the reasoning that the pill sits on the
     * last rows of the screen and its bottom corners have no ground to read
     * against. That was wrong, and measuring settles it: HINT_Y is 57, padTop
     * is 1, and the pill is FONT4_HEIGHT + 2 = 7 rows, so it occupies 56..62
     * and **row 63 is black beneath it**. There is a full row of ground under
     * every pill, so the bottom notches read exactly as the top ones do.
     *
     * Guarded on height so a padding-squeezed pill (the double-rule option
     * drops to padY 0) does not have its corners eaten into illegibility.
     *
     * Skipped when the pill is drawn in the ground colour (the inverted-band
     * option, where `pill` is 0): knocking a corner out of a hole fills it in,
     * which is the opposite of the intent.
     */
    if (pillC && pillH >= 3 && pillW >= 3) {
        ctx.fillRect(x, pillY, 1, 1, 0);
        ctx.fillRect(x + pillW - 1, pillY, 1, 1, 0);
        ctx.fillRect(x, pillY + pillH - 1, 1, 1, 0);
        ctx.fillRect(x + pillW - 1, pillY + pillH - 1, 1, 1, 0);
    }

    fontPrint4x5(ctx, x + HINT_PAD, ty, key, keyC);

    const ax = x + kw + HINT_PAD + HINT_GAP;
    /* A cleared plate behind the action text. Only for the dithered and
     * terrain grounds: a 1px 4x5 glyph against a 25%-or-denser fill loses its
     * silhouette, which is the same failure the arc-knob set's numeric slab
     * fixed with a two-pixel margin. */
    if (o.plate) ctx.fillRect(ax - 1, ty - 1, aw + 2, FONT4_HEIGHT + 2, 0);
    fontPrint4x5(ctx, ax, ty, action, actC);
}

const HINT_Y = FOOTER_Y + 1;   /* 57 — where drawFooter puts the glyphs */

/* --------------------------------------------------------------- 1..5 --
 * The boundary is still a rule. */

/** 1. thin-rule — the shipping footer, unmodified. */
function drawThinRule(ctx, hints) {
    drawFooter(ctx, hints);
}

/**
 * 2. no-rule — the boundary carried by SPACE alone.
 *
 * The rule is removed and the hints drop one row, so the gap between the last
 * label row (LBL1_Y..54) and the pills is the only separator. On a screen this
 * dense that is a real question rather than a lazy one: the pills are already
 * inverted blocks, so the row is unmistakably a different kind of thing even
 * with nothing drawn between it and the body.
 */
function drawNoRule(ctx, hints) {
    for (const { x, h } of layout(hints)) pair(ctx, x, HINT_Y + 1, h, {});
}

/**
 * 3. dotted-rule — a 50% rule.
 *
 * vimana's separator idiom, and the same `dottedRule` the arc-knob set's
 * terrain cell stands on. Half the weight of a solid hairline while still
 * being a line, which is exactly the register a boundary between a body and a
 * status strip should be in.
 */
function drawDottedRule(ctx, hints) {
    dottedRule(ctx, 0, RULE_Y, W, 0);
    for (const { x, h } of layout(hints)) pair(ctx, x, HINT_Y, h, {});
}

/**
 * 4. dashed-rule — 2 on, 1 off.
 *
 * Denser than the dotted rule and coarser than the solid one, and unlike
 * either it has a visible PERIOD — at 3px the eye reads it as a deliberate
 * pattern rather than as a line that failed to render. That is the whole
 * difference between this and option 3, which at 1-on-1-off can be mistaken
 * for a dithered solid on a display with any bleed.
 */
function drawDashedRule(ctx, hints) {
    for (let i = 0; i < W; i++) if ((i % 3) !== 2) ctx.fillRect(i, RULE_Y, 1, 1, 1);
    for (const { x, h } of layout(hints)) pair(ctx, x, HINT_Y, h, {});
}

/**
 * 5. double-rule — two hairlines with a row between them.
 *
 * The typographic answer: a double rule is heavier than a single one WITHOUT
 * being thicker, because the eye reads the gap as part of the mark. It also
 * costs two of the nine rows, which is why the pills lose their vertical
 * padding here — the pill becomes exactly as tall as the glyphs. That is a
 * visible change to the hints and it is the honest price of the construction,
 * not a drafting slip.
 */
function drawDoubleRule(ctx, hints) {
    ctx.fillRect(0, RULE_Y, W, 1, 1);
    ctx.fillRect(0, RULE_Y + 2, W, 1, 1);
    for (const { x, h } of layout(hints)) pair(ctx, x, RULE_Y + 3, h, { padTop: 0 });
}

/* --------------------------------------------------------------- 6..7 --
 * The boundary becomes a ground. */

/**
 * 6. inverted-band — the footer is the dark thing on the screen.
 *
 * The whole strip below the rule row is filled, corners notched, and the
 * figure/ground relationship of every hint flips: the key pill becomes a
 * KNOCKED-OUT block with white glyphs, the action text becomes black on white.
 * No rule is needed because the band's own top edge is the boundary.
 *
 * This is the biggest single change to the screen's overall value that any
 * option in any set makes, and that cuts both ways — it anchors the layout,
 * and it makes the footer the first thing the eye lands on when the footer is
 * the least important thing on the page.
 */
function drawInvertedBand(ctx, hints) {
    ctx.fillRect(0, FOOTER_Y, W, FOOTER_H, 1);
    notchCorners(ctx, 0, FOOTER_Y, W, FOOTER_H);
    for (const { x, h } of layout(hints))
        pair(ctx, x, HINT_Y + 1, h, { pill: 0, keyText: 1, actionText: 0 });
}

/**
 * 7. dither-band — a 25% ground under a solid rule.
 *
 * The middle term between a hairline and a filled band: the strip reads as a
 * distinct surface without becoming the darkest thing on screen. The hints are
 * knocked out on cleared plates, which is what keeps 4x5 glyphs legible over a
 * dither — the arc-knob set's numeric slab learned the same lesson at the same
 * scale.
 *
 * Cheaper on the eye than option 6 and, for the same reason, less decisive: at
 * true size DIAG_LIGHT under a rule can read as a display artefact rather than
 * as a design.
 */
function drawDitherBand(ctx, hints) {
    ctx.fillRect(0, RULE_Y, W, 1, 1);
    fillDithered(ctx, 0, FOOTER_Y, W, FOOTER_H, DIAG_LIGHT);
    for (const { x, h } of layout(hints))
        pair(ctx, x, HINT_Y + 1, h, { plate: true });
}

/* -------------------------------------------------------------- 8..10 --
 * The footer points. */

/**
 * 8. tab-band — the band extrudes upward under the active cell.
 *
 * An inverted footer with a two-row tab rising under one column, so the strip
 * says WHICH cell the hints are about. This is the file-tab idiom, and on a
 * knob grid it answers a question the current footer cannot: with eight cells
 * and one set of hints, nothing on screen connects the two.
 *
 * Requires a focus argument the shipping signature does not carry.
 */
function drawTabBand(ctx, hints) {
    ctx.fillRect(0, FOOTER_Y + 1, W, FOOTER_H - 1, 1);
    notchCorners(ctx, 0, FOOTER_Y + 1, W, FOOTER_H - 1);
    ctx.fillRect(ACTIVE_X, RULE_Y, CELL_W, 2, 1);
    /* Only the tab's TOP corners are notched. Notching the bottom pair would
     * cut two holes where the tab meets the band, which reads as a rendering
     * fault rather than as a softened corner. */
    ctx.fillRect(ACTIVE_X, RULE_Y, 1, 1, 0);
    ctx.fillRect(ACTIVE_X + CELL_W - 1, RULE_Y, 1, 1, 0);
    for (const { x, h } of layout(hints))
        pair(ctx, x, HINT_Y + 1, h, { pill: 0, keyText: 1, actionText: 0 });
}

/**
 * 9. tethered — a dotted rule with one 5-3-1 spike.
 *
 * The lightest possible way to say the same thing option 8 says: the boundary
 * stays a 50% rule and a three-row triangle drops out of it under the active
 * cell. Nothing is filled, nothing inverts, and the connection between the
 * hints and the cell is made by a mark the width of a character.
 *
 * The triangle costs three of the nine rows, so the pills lose their padding
 * and the hints sit on the last five rows of the screen. Requires the same
 * focus argument option 8 does.
 */
function drawTethered(ctx, hints) {
    dottedRule(ctx, 0, RULE_Y, W, 0);
    /* 5-3-1, centred. Drawn solid over the dotted rule so the spike reads as
     * attached to it rather than as floating under it. */
    ctx.fillRect(ACTIVE_CX - 2, RULE_Y, 5, 1, 1);
    ctx.fillRect(ACTIVE_CX - 1, RULE_Y + 1, 3, 1, 1);
    ctx.fillRect(ACTIVE_CX, RULE_Y + 2, 1, 1, 1);
    for (const { x, h } of layout(hints)) pair(ctx, x, RULE_Y + 3, h, { padTop: 0 });
}

/**
 * 10. terrain-band — the boundary is a RIDGE that rises toward the active cell.
 *
 * `fillTerrain` in the three rows above the hints: a solid crest over a
 * DIAG_THIRD hatch, sloping up to a peak under the active column. Same
 * construction the arc-knob and fader sets use for a value, carrying no value
 * at all — it is pure direction, a rule that leans toward the thing the hints
 * are about.
 *
 * A FULL-BAND landscape was the first drawing of this and it did not survive
 * being looked at: the hint pills own rows 57..63, so a hill drawn across all
 * eight rows had its crest covered everywhere except the gaps between pairs,
 * and what showed was disconnected specks of hatch. Nine rows minus seven of
 * hints leaves three, so three is what the ridge gets — and even then the pills
 * lose their top row to it.
 *
 * The most distinctive option in the set and the least defensible: it spends
 * three of the footer's nine rows on a graphic encoding a single bit, and
 * re-using the value language for something that is not a value is exactly the
 * overloading that makes a UI feel decorated. In the set to mark the end of the
 * axis honestly.
 */
const RIDGE_H = 3;
function drawTerrainBand(ctx, hints) {
    const heights = new Array(W);
    for (let i = 0; i < W; i++) {
        const d = Math.abs(i - ACTIVE_CX) / 46;
        heights[i] = 0.34 + 0.66 * Math.max(0, 1 - d * d);
    }
    fillTerrain(ctx, 0, RULE_Y, W, RIDGE_H, heights, DIAG_THIRD, true);
    for (const { x, h } of layout(hints))
        pair(ctx, x, RULE_Y + RIDGE_H, h, { padTop: 0 });
}

/* --------------------------------------------------------------- probe --
 *
 * The footer draws at absolute screen rows, so a swatch of just the strip
 * needs the coordinates moved rather than the drawing changed. `shiftY` wraps
 * the context AFTER the dither predicates have been evaluated — `fillDithered`
 * asks the pattern about the real screen coordinate and only then calls
 * `fillRect`, so the swatch has the same phase the device does.
 */
function shiftY(ctx, dy) {
    return {
        fillRect: (x, y, w, h, c) => ctx.fillRect(x, y + dy, w, h, c),
        print: (x, y, t, c) => ctx.print(x, y + dy, t, c),
        textWidth: (t) => ctx.textWidth(t),
        line: typeof ctx.line === "function"
            ? (x0, y0, x1, y1, c) => ctx.line(x0, y0 + dy, x1, y1 + dy, c) : undefined,
    };
}

const PROBE_HINTS = [["JOG", "PG"], ["SHFT", "SECT"], ["CLK", "MENU"], ["BACK", "EXIT"]];

export function register() {
    return registerSet({
    id: "fills",
    title: "Footer band — the boundary and the hint strip",
    kind: KIND_DRAW,
    replaces: "drawFooter",
    /* The nine rows the footer owns, RULE_Y..63. A surface exactly that tall is
     * also the clipping assertion: an option that reaches row 54 or row 64
     * fails the test rather than quietly overprinting the labels or falling off
     * the screen. */
    probeSize: { w: W, h: FOOTER_Y + FOOTER_H - RULE_Y },
    probe: (ctx, draw) => draw(shiftY(ctx, -RULE_Y), PROBE_HINTS),
    baseline: drawFooter,
    context: (ctx, draw, info) => {
        const RM = info.RM;
        ctx.fillRect(0, RM.RULE_Y, RM.W, RM.FOOTER_Y + RM.FOOTER_H - RM.RULE_Y, 0);
        draw(ctx, (info.pageCase && info.pageCase.footer) || PROBE_HINTS);
    },
    options: [
        {
            position: 1, id: "thin-rule", name: "Thin rule", draw: drawThinRule,
            note: "The shipping footer: a 1px solid rule across all 128 columns with the hint pairs below it. It is here as the honest baseline and it is not bad — the pill is ours and it works. What it is, is generic: a full-width hairline under a body is what every 1-bit device draws, which makes it the cheapest thing on the screen to change because nothing reads it, it only reads as a boundary.",
        },
        {
            position: 2, id: "no-rule", name: "No rule", draw: drawNoRule,
            note: "The rule deleted and the hints dropped one row, so the separation is carried by white space alone. Less lazy than it sounds: the pills are already inverted blocks, so the strip is unmistakably a different kind of thing without a line telling you so. The risk is on a busy page where the second label row runs long — with nothing between them, a descender-height gap is all that separates a label from a hint.",
        },
        {
            position: 3, id: "dotted-rule", name: "Dotted rule", draw: drawDottedRule,
            note: "A 50% dotted rule, which is vimana's separator idiom and the same primitive the terrain options stand on. Half the weight of the hairline while still being a line, which is the register a boundary between a body and a status strip probably wants. Cheapest differentiating change in the whole catalog: one call, no layout consequences.",
        },
        {
            position: 4, id: "dashed-rule", name: "Dashed rule", draw: drawDashedRule,
            note: "2 on, 1 off. Denser than the dotted rule and, unlike it, visibly PERIODIC — at a 3px period the eye reads a deliberate pattern rather than a line that failed to render, which is a real risk with 1-on-1-off on any display with bleed. The middle setting between options 1 and 3, and the one to pick if the dotted rule turns out to look broken on hardware.",
        },
        {
            position: 5, id: "double-rule", name: "Double rule", draw: drawDoubleRule,
            note: "Two hairlines with a clear row between them — heavier than a single rule without being thicker, because the eye reads the gap as part of the mark. It costs two of the footer's nine rows, so the pills lose their vertical padding and become exactly as tall as their glyphs. That change to the hints is the honest price of the construction and it should be judged as part of the option, not overlooked.",
        },
        {
            position: 6, id: "inverted-band", name: "Inverted band", draw: drawInvertedBand,
            note: "The whole footer filled, corners notched, and every hint flipped: the key pill becomes a knocked-out block with white glyphs, the action text goes black. No rule is needed because the band's top edge is the boundary. This is the largest change to the screen's overall value in the catalog, which anchors the layout and also makes the footer the first thing the eye lands on when the footer is the least important thing on the page.",
        },
        {
            position: 7, id: "dither-band", name: "Dither band", draw: drawDitherBand,
            note: "A DIAG_LIGHT ground under a solid rule, with the hint actions knocked out on cleared plates so 4x5 glyphs keep their silhouette over the dither. The middle term between a hairline and a filled band: the strip becomes a distinct surface without becoming the darkest thing on screen. Correspondingly less decisive — at true size a 25% ground under a rule can read as a display artefact rather than as a design.",
        },
        {
            position: 8, id: "tab-band", name: "Tab band", draw: drawTabBand,
            note: "An inverted band with a two-row tab rising under one column, so the strip says which cell the hints are about — a question the current footer cannot answer at all, since eight cells share one set of hints. The file-tab idiom, and the strongest connection between chrome and content available at this size. Requires a focus argument drawFooter does not take; the catalog draws it pointing at column 2.",
        },
        {
            position: 9, id: "tethered", name: "Tethered", draw: drawTethered,
            note: "The lightest way to say what option 8 says: the boundary stays a 50% dotted rule and a solid 5-3-1 spike drops out of it under the active cell. Nothing fills, nothing inverts, and the link between hints and cell costs one character-width of ink. The spike takes three of the nine rows, so the pills lose their padding and the hints sit on the screen's last five rows. Same focus-argument cost as option 8.",
        },
        {
            position: 10, id: "terrain-band", name: "Terrain band", draw: drawTerrainBand,
            note: "The footer as a landscape: a solid crest over a DIAG_THIRD hatch, sloping up toward the active column. The most distinctive option in the set and the least defensible — it spends the screen's whole bottom strip on a graphic encoding a single bit, it forces the hint plates from optional to mandatory, and it re-uses the language the knob and fader sets use for VALUES on something that is not a value. It is here to mark the end of the axis honestly, not because it should win.",
        },
    ],
    });
}
