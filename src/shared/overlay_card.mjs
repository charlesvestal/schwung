/*
 * overlay_card.mjs — ONE card chrome, for every overlay on the device.
 *
 * The chain editor's knob card (knob_card.mjs) grew a frame worth having: a
 * 2px border, a cleared gutter so the card lifts off whatever is behind it, a
 * white header band with the name knocked out of it, and a black gap keeping
 * the two apart. Everything else on the device drew its own box — six of them,
 * all slightly different:
 *
 *   drawOverlay          1px border, name + "Value: 0.62"   (menu_layout)
 *   drawMessageOverlay   1px border, title, lines, [OK]     (menu_layout)
 *   drawConfirmOverlay   TWO 1px borders, title, footer     (menu_layout)
 *   drawSkipbackToast    1px border, one centred line       (sampler_overlay)
 *   drawSetPageToast     1px border, one centred line       (sampler_overlay)
 *   drawShiftKnobOverlay 1px border, three left lines       (sampler_overlay)
 *
 * knob_card.mjs's own header note says it exists because turning a knob "used
 * to answer with a name and `Value: 0.62` in a centred box, on a screen that
 * already owns renderers for labelled cells" — that centred box is
 * drawOverlay, and it was still what every other overlay looked like. This
 * module is the frame from that card, extracted so both it and the six above
 * are the same object.
 *
 * THE GAP IS LOAD-BEARING, and it is the reason this is worth sharing rather
 * than copying. The border is white and so is the header band. Where they
 * touch, the border stops existing: the card reads as one fat stripe with no
 * left, right or top. One black row between them is the whole fix, it is
 * invisible in review, and it is asserted on the pixel buffer in
 * tests/host/test_overlay_card.sh. Any frame here has to keep a black row
 * between any white border and any white fill inside it.
 *
 * Pure, like the rest of this family: takes a draw context, draws, and touches
 * no parameter, no device global and no state — which is what lets the whole
 * thing be rendered into tools/param-pages/harness.mjs and inspected pixel by
 * pixel. `deviceCtx()` is the one concession, for callers that have globals
 * rather than a ctx; it resolves them at CALL time so a probe that swaps the
 * globals still sees every draw.
 */

export const SCREEN_WIDTH = 128;
export const SCREEN_HEIGHT = 64;

/** Inset from the screen edges. A card is a modal, not a band. */
export const CARD_X = 3;
export const CARD_W = 122;
/** 2px reads as a frame at this size where 1px reads as a hairline. */
export const BORDER_W = 2;
/** The black row that keeps a white band from eating the border. See above. */
export const GAP_W = 1;
/** Cleared outside the border, so the card lifts off what is behind it. */
export const GUTTER = 2;
/** The 5x7 device font plus one clear row above and below. */
export const BAND_H = 9;
/** Body text: the 7px glyph plus breathing room. */
export const LINE_H = 10;
/** Inside the border and the gap — where content may start. */
export const INSET = BORDER_W + GAP_W;

/* Resolved at CALL time, never at module load — see the module note. */
export function deviceCtx() {
    return {
        fillRect: (x, y, w, h, c) => fill_rect(x, y, w, h, c),
        print: (x, y, t, c) => print(x, y, t, c),
        textWidth: (t) => (typeof text_width === "function"
            ? text_width(String(t)) : String(t).length * 6),
    };
}

/** Content width inside the border and the gap, for a card of width `w`. */
export function contentW(w = CARD_W) { return w - INSET * 2; }

/**
 * How tall a card holding this much is.
 *
 * Callers size themselves through this rather than adding up constants, so a
 * change to BORDER_W or BAND_H moves every overlay together instead of leaving
 * one of them a pixel out.
 */
export function cardHeight({ band = false, lines = 0, footer = false } = {}) {
    let h = INSET * 2;
    if (band) h += BAND_H;
    if (lines > 0) h += (band ? GAP_W : 0) + lines * LINE_H;
    if (footer) h += GAP_W + LINE_H;
    return h;
}

/** Centred on screen, at the height its content needs. */
export function cardRect(content, x = CARD_X, w = CARD_W) {
    const h = cardHeight(content);
    return { x, y: Math.floor((SCREEN_HEIGHT - h) / 2), w, h };
}

/**
 * The frame alone: gutter cleared, 2px border, interior cleared.
 *
 * Clearing the interior is both "the card is opaque" and "the gap is cut" —
 * the black row the module note is about is this fill, not a separate step.
 */
export function drawCardFrame(ctx, r) {
    ctx.fillRect(r.x - GUTTER, r.y - GUTTER, r.w + GUTTER * 2, r.h + GUTTER * 2, 0);
    ctx.fillRect(r.x, r.y, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y + r.h - BORDER_W, r.w, BORDER_W, 1);
    ctx.fillRect(r.x, r.y, BORDER_W, r.h, 1);
    ctx.fillRect(r.x + r.w - BORDER_W, r.y, BORDER_W, r.h, 1);
    ctx.fillRect(r.x + BORDER_W, r.y + BORDER_W,
                 r.w - BORDER_W * 2, r.h - BORDER_W * 2, 0);
}

/**
 * The white band, with `left` knocked out of it and `right` set flush right.
 *
 * THE NAME LOSES A COLLISION. The right-hand value is the thing being read — a
 * truncated value is a wrong reading, where a truncated name is still
 * recognisable. (Carried over from drawKnobCard's header, which is where this
 * rule was worked out.)
 */
export function drawCardBand(ctx, x, y, w, left, right) {
    ctx.fillRect(x, y, w, BAND_H, 1);
    const val = String(right === null || right === undefined ? "" : right);
    const vw = val ? ctx.textWidth(val) : 0;
    let nm = String(left === null || left === undefined ? "" : left);
    const nameMax = w - 6 - vw;
    while (nm.length > 1 && ctx.textWidth(nm) > nameMax) nm = nm.slice(0, -1);
    ctx.print(x + 2, y + 1, nm, 0);
    if (val) ctx.print(x + w - 2 - vw, y + 1, val, 0);
}

/**
 * The generic overlay: frame, optional band, centred body lines, optional
 * footer. Returns the rect it drew, so a caller that has to hand a blit rect
 * to the shim passes on the geometry rather than recomputing it — computing it
 * twice is how a blit rect and its contents drift apart.
 *
 * `clipped` counts lines wider than the card. Nothing on screen reports that:
 * an over-long line is drawn straight through the border, so the count is the
 * only way a test or a log can see it.
 */
export function drawOverlayCard(ctx, o = {}) {
    const c = ctx || deviceCtx();
    const lines = (o.lines || []).filter(l => l !== null && l !== undefined && l !== "");
    const hasBand = o.title !== null && o.title !== undefined && o.title !== "";
    const footer = o.footer || "";
    const w = o.w || CARD_W;
    const x = o.x !== undefined ? o.x : Math.floor((SCREEN_WIDTH - w) / 2);

    const r = cardRect({ band: hasBand, lines: lines.length, footer: !!footer }, x, w);
    drawCardFrame(c, r);

    const cx = r.x + INSET;
    const cw = contentW(w);
    let y = r.y + INSET;
    const rows = [];

    if (hasBand) {
        drawCardBand(c, cx, y, cw, o.title, o.titleRight);
        y += BAND_H + GAP_W;
    }
    for (const line of lines) {
        /* Body text is CENTRED; the band is not. A band is a label for the
         * card and reads as a heading flush left; a body line is the message
         * and reads as prose. Left-aligning both made the three-line shift
         * overlay look like a truncated list. */
        const tw = c.textWidth(line);
        const tx = (o.alignLeft) ? cx : cx + Math.floor((cw - tw) / 2);
        c.print(tx, y, line, 1);
        rows.push({ y, text: line });
        y += LINE_H;
    }
    if (footer) {
        y += GAP_W;
        const fw = c.textWidth(footer);
        c.print(cx + Math.floor((cw - fw) / 2), y, footer, 1);
        rows.push({ y, text: footer });
    }

    /*
     * The rect to hand the shim when this card is drawn over MOVE's picture,
     * gutter included and clamped to the screen.
     *
     * The gutter is not decoration: drawCardFrame clears it so the card lifts
     * off what is behind it, and a blit of the bare card leaves Move's pixels
     * hard against the border. Clamped because a card at CARD_X = 3 has only
     * 3px of room for a 2px gutter — an unclamped rect would ask the shim to
     * blit from x = 1 with a width running one pixel past the display.
     */
    const gx = Math.max(0, r.x - GUTTER);
    const gy = Math.max(0, r.y - GUTTER);
    const blit = {
        x: gx, y: gy,
        w: Math.min(SCREEN_WIDTH - gx, r.w + (r.x - gx) + GUTTER),
        h: Math.min(SCREEN_HEIGHT - gy, r.h + (r.y - gy) + GUTTER),
    };

    const all = lines.concat(footer ? [footer] : []);
    return {
        ...r,
        blit,
        /* Where each text row was actually drawn, top-aligned (print takes y as
         * the glyph TOP). Returned so a test can assert placement without a
         * framebuffer — that every row is inside the card, and that no row
         * overlaps the band. Both are wrong invisibly: a row past the bottom
         * border is simply drawn over it, and a row on the band is white on
         * white. */
        textRows: rows,
        bandBottom: hasBand ? r.y + INSET + BAND_H : r.y + INSET,
        clipped: all.filter(l => c.textWidth(l) > cw).length,
    };
}
