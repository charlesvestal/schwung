/**
 * param_card.mjs — the floating card a MODULE draws while its knob is turned.
 *
 * Some values only mean something as a picture. A crossfade sitting between two
 * named waveform anchors is a position, not a number; a delay feedback that
 * computes to eleven audible repeats and then to HI CUT ONLY is a reading no
 * unit can spell. The grid can draw an envelope, a filter and six other named
 * graphics, but those are a fixed vocabulary — a module whose value falls
 * outside it has had nowhere to put the picture, and has had to ship its own
 * whole-screen editor to get one.
 *
 * So a parameter may name a drawer, and while its knob is held or has just been
 * turned the drawer paints a card over the page:
 *
 *   { "key": "blend", "type": "float", "min": 0, "max": 1, "step": 0.01,
 *     "card_script": "cards.js#blend_card", "card_w": 96, "card_h": 44 }
 *
 * ⭑ IT FLOATS, AND THAT IS WHY IT NEEDS NO CLEAR. The enum peek beside it is
 * full-screen on purpose and therefore cannot draw without the frame owner's
 * clearScreen — see renderOverlays. A card is the opposite: the page must stay
 * visible around it, so it blanks its OWN rect with fillRect and never asks for
 * the frame. That makes it drawable by an embedded consumer that owns no frame
 * at all, and keeps the library's "no file here clears the screen" contract
 * without an exception.
 *
 * THE SPLIT: THE MODULE DECLARES THE SIZE, THIS FILE OWNS THE FRAME.
 * The card is positioned, clamped, cleared and bordered here; the drawer is
 * handed the content rect and paints inside it. A module that positioned itself
 * would eventually leave debris — the size is per parameter, so it changes as
 * the focus moves between knobs, and a shrinking card must not leave the last
 * one's edges behind.
 *
 * Frame grammar is the knob card's, deliberately: 2px border, one black row
 * inside it, a cleared gutter outside. Two cards that differ only in who filled
 * them should not read as two different objects.
 */

import { SCREEN_WIDTH, SCREEN_HEIGHT } from "../list_geometry.mjs";

/** Border thickness. 2px reads as a frame where 1px reads as a hairline. */
export const BORDER_W = 2;
/** The black row inside the border. See knob_card.mjs — the gap is load-bearing. */
export const GAP_W = 1;
/** Cleared outside the border, so the card lifts off the page under it. */
export const GUTTER = 2;

const INSET = BORDER_W + GAP_W;

/** What a card costs before the drawer gets any room. */
export const CARD_CHROME = INSET * 2;

/**
 * The default when a module declares a drawer but no size. Wide enough for a
 * short label and a bar, short enough to leave the page readable around it.
 */
export const DEFAULT_CARD_W = 96;
export const DEFAULT_CARD_H = 34;

/** Smallest card that can hold a border, its gap and one pixel of content. */
const MIN_SIDE = CARD_CHROME + 1;

function clampSide(v, dflt, max) {
    const n = Math.round(Number(v));
    /*
     * A NONSENSE SIZE FALLS BACK, IT DOES NOT BREAK THE FRAME.
     *
     * Zero, negative, NaN and a string all arrive here from a hand-written
     * module.json, and every one of them would otherwise draw a border with no
     * inside — or, negative, a fillRect walking backwards across the page.
     */
    if (!isFinite(n) || n < MIN_SIDE) return Math.min(dflt, max);
    return Math.min(n, max);
}

/**
 * Where a card of this declared size sits: centred, clamped to the panel.
 *
 * Centred rather than anchored to the touched cell. The cell is 30px wide and a
 * card is not, so anchoring would put most cards off the edge and the rest in a
 * different place per knob — a picture that moves while you read it.
 *
 * @param {object} meta  the param's metadata; card_w / card_h are optional
 * @returns {{x:number,y:number,w:number,h:number}} the OUTER rect
 */
export function paramCardRect(meta) {
    const w = clampSide(meta && meta.card_w, DEFAULT_CARD_W, SCREEN_WIDTH - GUTTER * 2);
    const h = clampSide(meta && meta.card_h, DEFAULT_CARD_H, SCREEN_HEIGHT - GUTTER * 2);
    return {
        x: Math.floor((SCREEN_WIDTH - w) / 2),
        y: Math.floor((SCREEN_HEIGHT - h) / 2),
        w,
        h,
    };
}

/** The rect the DRAWER gets: inside the border and its gap. */
export function paramCardContentRect(outer) {
    return {
        x: outer.x + INSET,
        y: outer.y + INSET,
        w: outer.w - INSET * 2,
        h: outer.h - INSET * 2,
    };
}

/**
 * Draw the frame and hand the inside to the module.
 *
 * ⚠ A DRAWER THAT THROWS MUST COST NOTHING AND SAY SO ONCE.
 *
 * This runs on the draw path, up to every frame while a knob is held, and the
 * module's code is not ours. An exception escaping here would take the whole
 * page down with it — and the failure this tree has already shipped once is
 * worse than a crash: a hook whose thrower was caught but whose caller reported
 * success, leaving whatever drew before the throw, then nothing, with no error
 * anywhere. So the throw is caught HERE, `onError` is called once so the caller
 * can retire the drawer for the session rather than re-entering it 60 times a
 * second, and the frame is left as a plain empty card rather than a hole.
 *
 * @param {object} ctx    fillRect / print / textWidth, as render() takes
 * @param {object} o      { meta, draw, name, value, raw, onError }
 * @returns {boolean}     true when a card was drawn
 */
export function drawParamCard(ctx, o) {
    if (!ctx || !o || typeof o.draw !== "function") return false;

    const r = paramCardRect(o.meta);

    /* Lift it off the page: clear the gutter, then the border, then the inside.
     * Clipped at the panel edge by the caller`s own fillRect, which is why the
     * gutter may safely be asked for outside it. */
    ctx.fillRect(r.x - GUTTER, r.y - GUTTER, r.w + GUTTER * 2, r.h + GUTTER * 2, 0);
    ctx.fillRect(r.x, r.y, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y + r.h - BORDER_W, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y, BORDER_W, r.h, 1);
    ctx.fillRect(r.x + r.w - BORDER_W, r.y, BORDER_W, r.h, 1);
    ctx.fillRect(r.x + BORDER_W, r.y + BORDER_W,
                 r.w - BORDER_W * 2, r.h - BORDER_W * 2, 0);

    const content = paramCardContentRect(r);
    try {
        o.draw(ctx, {
            x: content.x, y: content.y, w: content.w, h: content.h,
            name: o.name === undefined ? "" : o.name,
            /* The FORMATTED reading, the same string the header would show. */
            value: o.value === undefined ? "" : o.value,
            /*
             * The RAW wire value, for a drawer that has to compute rather than
             * print — a repeat count, a position between anchors.
             *
             * ⚠ May be null: a key the module does not serve reads as "no
             * answer", and the contract is that a drawer draws NOTHING
             * interpretive then. A read that did not answer must never become a
             * picture; that rule is why the grid reads on touch and turn and
             * never on the draw path, and it does not stop applying because the
             * pixels are somebody else`s.
             */
            raw: o.raw === undefined ? null : o.raw,
        });
    } catch (e) {
        if (typeof o.onError === "function") o.onError(e);
        return true;
    }
    return true;
}
