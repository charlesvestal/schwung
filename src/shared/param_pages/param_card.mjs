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
 * without an exception. Such a consumer passes its own rect (see
 * paramCardRect); needing no clear is not the same as knowing where to draw.
 *
 * ⭑ A DRAWER CANNOT EXPRESS A SCREEN COORDINATE, exactly as a cell widget
 * cannot. The drawer is handed a frameCtx scoped to the inside of the card, so
 * (0,0) is its own top-left, ctx.width/height are the card's, and there is no
 * accessor that reaches absolute space. The reasoning is frame_ctx.mjs's and
 * applies here for the same reason it applies there: the rect is not stable.
 * A card's size is per PARAMETER and the panel clamps it, so coordinates
 * authored against one card are wrong on the next one -- and a floating card
 * that could paint outside its own border would eat the page it is meant to
 * float over. Two module draw hooks, one coordinate contract.
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
import { frameCtx } from "./frame_ctx.mjs";

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
 * Where a card of this declared size sits: centred, clamped to its frame.
 *
 * Centred rather than anchored to the touched cell. The cell is 30px wide and a
 * card is not, so anchoring would put most cards off the edge and the rest in a
 * different place per knob — a picture that moves while you read it.
 *
 * ⚠ CENTRED IN THE CALLER'S FRAME, NOT THE SCREEN.
 *
 * This took SCREEN_WIDTH/SCREEN_HEIGHT unconditionally, which is right for the
 * full-screen host and wrong for the one case this file claims to support: a
 * tool that embeds the grid in its own chrome (render() takes a `rect` for
 * exactly that). A card centred on the panel while the page it belongs to sits
 * in a corner is not floating over that page, it is painting over the host's
 * screen — and "drawable by an embedded consumer" was the claim being made two
 * paragraphs up. A frame is now passed in, defaulting to the whole panel so the
 * full-screen caller is unchanged.
 *
 * @param {object} meta   the param's metadata; card_w / card_h are optional
 * @param {object} [frame] {x,y,w,h} to centre within; defaults to the panel
 * @returns {{x:number,y:number,w:number,h:number}} the OUTER rect
 */
export function paramCardRect(meta, frame) {
    const fx = frame && Number.isFinite(frame.x) ? frame.x : 0;
    const fy = frame && Number.isFinite(frame.y) ? frame.y : 0;
    const fw = frame && frame.w > 0 ? frame.w : SCREEN_WIDTH;
    const fh = frame && frame.h > 0 ? frame.h : SCREEN_HEIGHT;

    const w = clampSide(meta && meta.card_w, DEFAULT_CARD_W, Math.max(MIN_SIDE, fw - GUTTER * 2));
    const h = clampSide(meta && meta.card_h, DEFAULT_CARD_H, Math.max(MIN_SIDE, fh - GUTTER * 2));
    return {
        x: fx + Math.floor((fw - w) / 2),
        y: fy + Math.floor((fh - h) / 2),
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
 *                        draw is called as draw(frameCtx, { w, h, name, value, raw })
 * @returns {boolean}     true when a card was drawn
 */
export function drawParamCard(ctx, o) {
    if (!ctx || !o || typeof o.draw !== "function") return false;

    const r = paramCardRect(o.meta, o.frame);

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
    /*
     * THE DRAWER GETS THE INSIDE OF THE CARD AND NOTHING ELSE. No x/y is passed
     * because there is nothing for one to mean: the context's origin already IS
     * the content rect's top-left, and a drawer that received both would be
     * invited to add them.
     */
    const inner = frameCtx(ctx, content);
    try {
        o.draw(inner, {
            w: content.w, h: content.h,
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
        /*
         * RE-CLEAR WHAT IT MANAGED TO PAINT.
         *
         * The comment above promised "a plain empty card rather than a hole",
         * and without this it was not one: a drawer that paints and THEN throws
         * leaves its half-finished output sitting inside the frame, which is a
         * picture built from an answer that did not arrive. Measured at 400 lit
         * pixels for a drawer that filled a rect before throwing.
         */
        ctx.fillRect(content.x, content.y, content.w, content.h, 0);
        if (typeof o.onError === "function") o.onError(e);
        return true;
    }
    return true;
}
