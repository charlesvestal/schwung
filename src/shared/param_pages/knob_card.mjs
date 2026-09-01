/**
 * knob_card.mjs — the chain editor's knob feedback card.
 *
 * Turning a knob in the chain editor used to answer with a name and
 * `Value: 0.62` in a centred box, on a screen that already owns renderers for
 * labelled cells, arc knobs, enum squares and viz groups. This draws a
 * bordered card over the diagram instead, carrying the parameter name and
 * value in an inverted band and, beneath it, the four cells of that knob's row
 * — the SAME `drawKnobRow` the knob grid uses, at a narrower cell.
 *
 * Pure, like everything else in this directory: takes a draw context, draws,
 * and touches no parameter, no device global and no state. That is what lets
 * the whole card be rendered into tools/param-pages/harness.mjs and inspected
 * pixel by pixel — which is the only way to catch the failure it actually has.
 *
 * THE GAP IS LOAD-BEARING — see overlay_card.mjs, which now owns the frame,
 * the band and that rule. This file keeps only the LAYOUT that is specific to
 * a knob card: the height it needs for a row of four cells, and the
 * drawKnobRow call itself.
 *
 * The constants below are re-exported rather than deleted. They are part of
 * this module's published surface (tests and the harness import CARD_W and
 * HEADER_BAND_H from here), and re-exporting keeps that working while leaving
 * exactly one definition of each.
 */

import { drawKnobRow, ROW0_Y, LBL0_Y, LBL_H, RULE_Y, HEADER_H }
    from "./render_page_movy.mjs";
import { drawCardFrame, drawCardBand, contentW,
         CARD_X, CARD_W, BORDER_W, GAP_W, GUTTER, BAND_H, INSET }
    from "../overlay_card.mjs";

export { CARD_X, CARD_W, BORDER_W, GAP_W, GUTTER };
/** The 5x7 device font plus one clear row above and below. */
export const HEADER_BAND_H = BAND_H;
const ROW_H = (LBL0_Y + LBL_H) - ROW0_Y;
const LBL_DY = LBL0_Y - ROW0_Y;
/** The band between the screen header and the footer rule. */
const BODY_TOP = HEADER_H + 1;
const BODY_BOT = RULE_Y;

/**
 * Where the card sits. Centred in the body band so both heights look
 * deliberate rather than anchored to whichever edge was convenient.
 */
export function knobCardRect(hasStrip) {
    const h = hasStrip
        ? INSET * 2 + HEADER_BAND_H + GAP_W + ROW_H
        : INSET * 2 + HEADER_BAND_H;
    const y = BODY_TOP + Math.floor(((BODY_BOT - BODY_TOP) - h) / 2);
    return { x: CARD_X, y, w: CARD_W, h };
}

/** Content width inside the border and the gap. */
export function knobCardContentW() { return contentW(CARD_W); }

/**
 * @param {object} ctx  fillRect/print/textWidth, plus the native line/arc
 *                      primitives when the caller has them
 * @param {object} o    { name, value } always; for the full card also
 *                      { page, metaIndex, values, touched, row, viz, modulated }
 *                      — the same shapes renderPageMovy takes
 * @returns {object}    the rect it drew into
 */
export function drawKnobCard(ctx, o) {
    const keys = o && o.page && o.page.keys;
    const hasStrip = !!(keys && keys.some(Boolean));
    const r = knobCardRect(hasStrip);

    drawCardFrame(ctx, r);

    const cx = r.x + INSET;
    const cw = knobCardContentW();
    drawCardBand(ctx, cx, r.y + INSET, cw, o.name, o.value);
    if (!hasStrip) return r;

    const rowY = r.y + INSET + HEADER_BAND_H + GAP_W;
    drawKnobRow(ctx, o, (o.row | 0), rowY, rowY + LBL_DY,
                  { x0: cx, cellW: Math.floor(cw / 4) });
    return r;
}
