/*
 * widget-test/canvas.js — the reference custom widget.
 *
 * ONE FILE, TWO SCALES. `drawCell` paints a single knob box inside the grid;
 * `draw` paints the fullscreen view you dive into from the `detail` cell. Both
 * hang off the same overlay object, so an author writes one thing.
 *
 * This module is source-only (like tools/{ui,seq,config,splash}-test) and is
 * not in the release tarball. It exists so the custom-widget contract has a
 * worked example that is exercised by tests/host/test_widget_module_poc.sh
 * rather than only described in prose -- writing it is what found that widgets
 * were being registered from the canvas-open path, which meant they never
 * appeared until the fullscreen view had been opened once.
 *
 * THE RULES A WIDGET AUTHOR MUST HOLD ON TO
 *
 *   The frame is not the screen. (0,0) is this knob box's top-left and
 *   ctx.width/ctx.height are the box's. There is no way to name an absolute
 *   coordinate, and anything outside the frame is clipped. Size everything
 *   against ctx.width/ctx.height: the same widget is handed at least sixteen
 *   different frame sizes across the two renderers.
 *
 *   You are given values; you cannot read them. There is no ctx.getParam on the
 *   draw path. A read is ~2.8ms against a 1.68ms whole-page render, so one read
 *   costs more than redrawing the entire screen.
 *
 *   The label is not yours. Schwung draws every cell's label.
 *
 *   One strike. If drawCell throws, this widget is disabled for the session and
 *   the built-in widget draws instead -- the page stays correct.
 */

globalThis.canvas_overlay = {
    /* Must match the chain_params declaration:
     *   "viz": { "kind": "custom:wtmeter" } */
    widgetKind: "custom:wtmeter",

    /*
     * A SECOND WIDGET, because one module may declare several.
     *
     * This used to be impossible, and impossible in the quiet way: the host
     * read a single `widgetKind` string, so a module naming two kinds got the
     * first registered and the second ignored -- and an ignored kind is not an
     * error, it just falls through to a built-in dial. A correct-looking page
     * and nothing to search for.
     *
     * The object form gives each kind its own drawer, which is what you want
     * when the two widgets have nothing to do with each other. (An ARRAY of
     * names is the other form: several kinds sharing one drawCell, told apart
     * by group.keys -- right when they really are one drawing at two crops.)
     */
    widgetKinds: {
        "custom:wtmode": {
            nominal: null,
            /* Three states, drawn as a filled box that grows. Deliberately not
             * a meter: it is here to look like a DIFFERENT widget, so that a
             * page carrying both shows two kinds rather than one twice. */
            draw(ctx, { values, group }) {
                const key = group && group.keys && group.keys[0];
                const v = key && values ? Number(values[key]) : NaN;
                const w = ctx.width, h = ctx.height;
                if (w < 5 || h < 5) return;
                /* Frame, then a bar whose height reads the state. */
                ctx.fillRect(0, 0, w, 1, 1);
                ctx.fillRect(0, h - 1, w, 1, 1);
                ctx.fillRect(0, 0, 1, h, 1);
                ctx.fillRect(w - 1, 0, 1, h, 1);
                if (!Number.isFinite(v)) return;   /* no answer, no picture */
                const steps = 3;
                const lit = Math.max(1, Math.min(steps, Math.round(v) + 1));
                const barH = Math.max(1, Math.round((h - 4) * (lit / steps)));
                ctx.fillRect(2, h - 2 - barH, w - 4, barH, 1);
            },
        },
    },

    /* Only sprite-based widgets need a nominal frame. This one draws
     * proportionally, so it has none and works at every size. */

    /* ---------------------------------------------------------- in-grid --
     *
     * A segmented meter: ticks across the box, filled up to the value, with a
     * baseline so an empty meter still reads as a control rather than a blank
     * cell.
     */
    drawCell(ctx, { values, group }) {
        const key = group && group.keys && group.keys[0];
        const raw = key && values ? Number(values[key]) : NaN;
        const v = Number.isFinite(raw) ? Math.max(0, Math.min(1, raw)) : 0;

        const w = ctx.width, h = ctx.height;
        if (w < 3 || h < 3) return;          /* too small to say anything */

        ctx.fillRect(0, h - 1, w, 1, 1);      /* baseline */

        /* Segments sized from the frame, never from a constant. */
        const seg = 2;
        const gap = 1;
        const n = Math.max(1, Math.floor((w + gap) / (seg + gap)));
        const lit = Math.round(v * n);
        const top = 1;
        const barH = Math.max(1, h - 3);

        for (let i = 0; i < lit; i++) {
            const x = i * (seg + gap);
            /* A rising staircase, so the shape says "more" even in one colour. */
            const grow = Math.max(1, Math.round(barH * ((i + 1) / n)));
            ctx.fillRect(x, top + (barH - grow), seg, grow, 1);
        }
    },

    /* ------------------------------------------------------- fullscreen --
     *
     * Reached by clicking the `detail` cell. Same overlay object, whole screen.
     * ctx here is the canvas runtime context -- also without getParam, because
     * draw is on the draw path too.
     */
    draw(ctx) {
        ctx.clear();
        ctx.print(2, 0, "WIDGET TEST", 1);
        ctx.drawRect(2, 12, ctx.width - 4, 20, 1);
        ctx.print(2, 36, "grid: custom:wtmeter", 1);
        ctx.print(2, 46, "one file, two scales", 1);
    },

    onOpen() { /* values may be fetched here -- onOpen is an event, not a frame */ },
};
