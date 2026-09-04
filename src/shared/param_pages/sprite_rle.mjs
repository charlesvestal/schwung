/**
 * sprite_rle.mjs — 1-bit sprite runs, and how one is placed in a frame.
 *
 * THE FORMAT IS RUNS BECAUSE OF THE FULL-PAGE COST, NOT THE PER-SPRITE COST.
 *
 * A QuickJS binding is ~490ns. A 17x15 knob box blitted per pixel is 255 calls
 * ~= 125us, which against a 1.68ms page render sounds survivable -- until you
 * notice a page holds EIGHT knob boxes, and a module that ships one custom
 * widget ships eight. That is ~1ms of the render gone before anything else is
 * drawn. Row runs (a 1-bit sprite is typically 2-4 per row) are ~45 calls
 * ~= 22us, so a full page is ~180us.
 *
 * Check the full-page figure, not the per-sprite one. It is what makes runs
 * non-optional rather than a nicety.
 *
 * NEVER FRACTIONALLY SCALED. 1-bit art on a 128x64 mono display cannot be
 * resampled -- it dithers into mush. So a sprite carries the NOMINAL frame it
 * was drawn for, is anchored 1:1 (integer scale only on an exact multiple), and
 * is REFUSED when it does not fit. A refusal is not a failure: the caller falls
 * back to the built-in widget, which is a correct picture rather than a smeared
 * one.
 *
 * The generator (tools/param-pages/widget_gen.mjs) enforces the nominal frame at
 * BUILD time, on the author's machine, so this runtime refusal is a backstop
 * rather than the normal path.
 */

/**
 * Build a sprite from row strings, "#" set and anything else clear.
 *
 * The generator emits this same shape, so a hand-written test sprite and a
 * generated one are the same thing -- which is what lets the round-trip test
 * compare them by content.
 */
export function spriteFromRows(rows) {
    const h = rows.length;
    const w = h ? rows[0].length : 0;
    const runs = [];
    for (let y = 0; y < h; y++) {
        const row = rows[y];
        let x = 0;
        while (x < w) {
            if (row[x] !== "#") { x++; continue; }
            let end = x;
            while (end < w && row[end] === "#") end++;
            runs.push([x, y, end - x]);
            x = end;
        }
    }
    return { w, h, runs };
}

/**
 * Where a sprite sits inside a frame, or null if it does not fit.
 *
 * @param {object} nominal  { w, h } the sprite was drawn for
 * @param {object} frame    { width, height } of the frame ctx
 * @returns {{x: number, y: number, scale: number}|null}
 */
export function anchorSprite(nominal, frame) {
    const { w, h } = nominal;
    const fw = frame.width, fh = frame.height;
    if (!(w > 0 && h > 0) || w > fw || h > fh) return null;

    /* Integer scale only, and only on an EXACT multiple in both axes -- a
     * sprite that would "fit" at 1.5x is drawn at 1x, never resampled. */
    let scale = 1;
    for (let s = 2; w * s <= fw && h * s <= fh; s++) {
        if (fw % w === 0 && fh % h === 0 && fw / w >= s && fh / h >= s) scale = s;
    }

    return {
        x: Math.floor((fw - w * scale) / 2),
        y: Math.floor((fh - h * scale) / 2),
        scale,
    };
}

/**
 * Draw a sprite at frame-local (ox, oy).
 *
 * @param {object} ctx     a frame ctx from frame_ctx.mjs
 * @param {object} sprite  from spriteFromRows or the generator
 * @param {number} ox
 * @param {number} oy
 * @param {number} color
 * @param {object} [fit]   { width, height } to check against; omitted = no check
 * @returns {boolean} false if the sprite was refused for not fitting
 */
export function drawSprite(ctx, sprite, ox, oy, color = 1, fit = null) {
    if (fit && !anchorSprite({ w: sprite.w, h: sprite.h }, fit)) return false;
    for (const [x, y, len] of sprite.runs) {
        ctx.fillRect(ox + x, oy + y, len, 1, color);
    }
    return true;
}

/** Anchor and draw in one call, against the ctx's own dimensions. */
export function drawSpriteAnchored(ctx, sprite, color = 1) {
    const a = anchorSprite({ w: sprite.w, h: sprite.h }, ctx);
    if (!a) return false;
    if (a.scale === 1) return drawSprite(ctx, sprite, a.x, a.y, color);
    for (const [x, y, len] of sprite.runs) {
        ctx.fillRect(a.x + x * a.scale, a.y + y * a.scale, len * a.scale, a.scale, color);
    }
    return true;
}
