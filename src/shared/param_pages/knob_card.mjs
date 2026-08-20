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
 * THE GAP IS LOAD-BEARING. The border is white and so is the header band. Where
 * they touch, the border stops existing: a short card with no gap reads as one
 * fat stripe laid across sliced-off diagram boxes, with no left, right or top.
 * One black row between them is the whole fix, and it is asserted on the pixel
 * buffer in tests/host/test_knob_card.sh because it is invisible in review.
 * Any future change to this frame has to keep a black row between any white
 * border and any white fill inside it.
 */

import { drawKnobRow, ROW0_Y, LBL0_Y, LBL_H, RULE_Y, HEADER_H }
    from "./render_page_movy.mjs";

/** Inset from the screen edges. The card is a modal, not a band. */
export const CARD_X = 3;
export const CARD_W = 122;
/** 2px reads as a frame at this size where 1px reads as a hairline. */
export const BORDER_W = 2;
/** The black row that keeps the band from eating the border. See above. */
export const GAP_W = 1;
/** Cleared outside the border, so the card lifts off the diagram. */
export const GUTTER = 2;
/** The 5x7 device font plus one clear row above and below. */
export const HEADER_BAND_H = 9;

const INSET = BORDER_W + GAP_W;
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
export function knobCardContentW() { return CARD_W - INSET * 2; }

/**
 * Name left, value right, both knocked out of a white band.
 *
 * The NAME loses a collision. The value is the thing being read — a truncated
 * value is a wrong reading, where a truncated name is still recognisable.
 */
function drawCardHeader(ctx, x, y, w, name, value) {
    ctx.fillRect(x, y, w, HEADER_BAND_H, 1);
    const val = String(value === null || value === undefined ? "" : value);
    const vw = val ? ctx.textWidth(val) : 0;
    let nm = String(name === null || name === undefined ? "" : name);
    const nameMax = w - 6 - vw;
    while (nm.length > 1 && ctx.textWidth(nm) > nameMax) nm = nm.slice(0, -1);
    ctx.print(x + 2, y + 1, nm, 0);
    if (val) ctx.print(x + w - 2 - vw, y + 1, val, 0);
}

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

    ctx.fillRect(r.x - GUTTER, r.y - GUTTER, r.w + GUTTER * 2, r.h + GUTTER * 2, 0);

    ctx.fillRect(r.x, r.y, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y + r.h - BORDER_W, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y, BORDER_W, r.h, 1);
    ctx.fillRect(r.x + r.w - BORDER_W, r.y, BORDER_W, r.h, 1);

    /* The interior, cleared. This is both the card being opaque and the gap
     * being cut — see the module doc. */
    ctx.fillRect(r.x + BORDER_W, r.y + BORDER_W,
                 r.w - BORDER_W * 2, r.h - BORDER_W * 2, 0);

    const cx = r.x + INSET;
    const cw = knobCardContentW();
    drawCardHeader(ctx, cx, r.y + INSET, cw, o.name, o.value);
    if (!hasStrip) return r;

    const rowY = r.y + INSET + HEADER_BAND_H + GAP_W;
    drawKnobRow(ctx, o, (o.row | 0), rowY, rowY + LBL_DY,
                  { x0: cx, cellW: Math.floor(cw / 4) });
    return r;
}
