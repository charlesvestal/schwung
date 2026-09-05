/*
 * widget-test/cards.js — the card that floats over the page while Level turns.
 *
 * The THIRD module-supplied surface, and the last one this fixture was missing:
 *
 *   drawCell (canvas.js)  one knob box, always, while the page is up
 *   drawCard (here)       over the page, only while the knob is being turned
 *   draw    (canvas.js)   the whole screen, when you dive into Detail
 *
 * A card answers a different question from a cell. The cell says "this knob is
 * at 0.6"; the card says what 0.6 MEANS -- here, a bar with its numeric reading
 * and the name, which is the smallest thing that justifies the surface at all.
 *
 * SAME COORDINATE CONTRACT AS A CELL WIDGET. The drawer is handed a frameCtx
 * scoped to the INSIDE of the card, so (0,0) is the content area's top-left and
 * ctx.width/ctx.height are its size. There is no accessor that reaches a screen
 * pixel, and the card is centred in the page's frame -- which is the whole panel
 * for the shadow UI and the embedded region for a tool that supplies a rect. So
 * size everything against ctx.width/ctx.height; an absolute coordinate is wrong
 * somewhere by construction.
 *
 * NO READS. `raw` and `value` are handed in. `raw` MAY BE NULL -- a key the
 * module does not serve reads as "no answer" -- and the contract is that a
 * drawer draws nothing interpretive then, rather than drawing zero. A read that
 * did not answer must never become a picture.
 *
 * THE REST OF THE PAGE IS HANDED IN TOO, as `values` -- the same map a cell
 * widget gets. That matters for a card whose MEANING depends on a sibling: this
 * one names what the blend is between, which is a fact about `mode`, not about
 * `level`. Before it existed, such a card had no route to that fact at all --
 * no getParam on this path, and the card script is its own closure, so it
 * cannot see a variable the module drawCell set either. The first module to
 * need it went through globalThis, which worked and was a side channel.
 *
 * Same null rules apply: a sibling may be missing or null, and an absent one
 * must not become a picture either -- here the label is simply omitted.
 *
 * ONE STRIKE. If this throws it is retired for the session and the card is left
 * empty; the page keeps working.
 *
 * Declared in the DSP's chain_params as:
 *     "card_script": "cards.js#blend_card", "card_w": 96, "card_h": 34
 */

const MODE_NAMES = ["Dry/Wet", "In/Out", "A/B"];

globalThis.blend_card = function (ctx, { name, value, raw, values }) {
    const w = ctx.width, h = ctx.height;
    if (w < 8 || h < 8) return;

    /* The name, top-left. print truncates to the frame, so a long name cannot
     * push the layout around. */
    ctx.print(0, 0, String(name || ""), 1);

    /*
     * NOTHING INTERPRETIVE WITHOUT AN ANSWER.
     *
     * raw === null means the read did not answer. Drawing a bar at zero would
     * be indistinguishable from a genuine zero, which is the picture this rule
     * exists to prevent. The name and the frame still draw: the card is present
     * and honest about knowing nothing yet.
     */
    const n = raw === null || raw === undefined ? NaN : Number(raw);
    if (!Number.isFinite(n)) {
        ctx.print(0, h - 7, "--", 1);
        return;
    }

    const v = Math.max(0, Math.min(1, n));

    /* The formatted reading, right-aligned against the frame's own width. */
    const s = String(value || "");
    const tw = ctx.textWidth(s);
    if (tw <= w) ctx.print(w - tw, 0, s, 1);

    /*
     * WHAT THE BLEND IS BETWEEN -- a fact that lives on a DIFFERENT parameter.
     * Omitted rather than guessed when `mode` is absent or unanswered.
     */
    const mode = values ? Number(values.mode) : NaN;
    if (Number.isFinite(mode) && MODE_NAMES[mode | 0]) {
        const label = MODE_NAMES[mode | 0];
        if (ctx.textWidth(label) <= w) ctx.print(0, 8, label, 1);
    }

    /* A bar, sized from the frame rather than from a constant. */
    const barY = Math.max(9, h - 10);
    const barH = Math.max(3, h - barY - 1);
    ctx.fillRect(0, barY, w, barH, 1);                       /* outline */
    ctx.fillRect(1, barY + 1, w - 2, barH - 2, 0);           /* hollow */
    const fill = Math.round((w - 4) * v);
    if (fill > 0) ctx.fillRect(2, barY + 2, fill, barH - 4, 1);
};
