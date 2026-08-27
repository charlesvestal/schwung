/**
 * composite.mjs — render a whole page with the CHOSEN options in place.
 *
 * The catalog answers "which of these ten", one control at a time. It cannot
 * answer the question that actually matters, which is what the picks look like
 * TOGETHER: a fader that reads well beside the current enum square may not read
 * well beside the chosen one, and nothing in a per-set contact sheet can show
 * that. This renders the whole page from the decision list.
 *
 * Picks are read from PICKS below, which mirrors
 * docs/superpowers/specs/2026-08-26-ui-differentiation-decisions.md. Keep the
 * two in step; this file is the visual half of that document.
 *
 * Node-only. Nothing here ships to the device.
 */

import fs from "node:fs";
import path from "node:path";
import { createFramebuffer, drawContext } from "./harness.mjs";
import * as S from "../../src/shared/param_pages/styles/index.mjs";
import * as RM from "../../src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";

/* The decisions, by set id -> option id. `null` = keep what ships. */
export const PICKS = {
    knob: "arc-short",
    fader: "outline-fill",
    fills: "no-rule",
    enum_square: "thin-frame",
    label_cell: "half-strip",
    opaque_box: "door-open",
    viz_envelope: "ghost-fill",
    viz_filter: "ghost-fill",
    viz_lfo: "ghost-fill",
    viz_sample: "ghost-fill",
    viz_switch: "pill-inverted",
    font: "metric-matched",
    anim: null,                    /* deferred to hardware */
};

function optionFor(setId) {
    const want = PICKS[setId];
    if (!want) return null;
    const set = S.setById(setId);
    if (!set) return null;
    return set.options.find((o) => o.id === want) || null;
}

/**
 * A page with every pick applied.
 *
 * Built by rendering the shipping page and then overdrawing the parts that
 * changed, rather than by reimplementing renderPageMovy: the point is to see
 * the picks against REAL chrome, and a reimplementation would drift from what
 * the device draws, which is the one thing this render exists to show.
 */
export function renderComposite(pageCase) {
    const fb = createFramebuffer();
    const ctx = drawContext(fb);
    RM.renderPageMovy(ctx, pageCase);

    const g = RM.GRID_GEOM;
    const knobOpt = optionFor("knob");
    const enumOpt = optionFor("enum_square");
    const labelOpt = optionFor("label_cell");
    const fillsOpt = optionFor("fills");

    /* Widgets, row by row. Cells carrying a viz or a switch are left alone --
     * those are drawn by the viz layer and substituting a knob into one would
     * be showing a page that cannot exist. */
    /* Slots a viz group covers, so a knob is never painted over a curve or a
     * switch. Mirrors drawKnobRow own `covered` computation rather than
     * guessing at it. */
    const covered = new Set();
    for (const grp of (pageCase.viz || []))
        for (let i = 0; i < (grp.slotCount || 1); i++) covered.add(grp.slotStart + i);

    /* Viz groups get their chosen treatment. Each group knows its own kind, so
     * a waveform gets the LFO pick and a switch gets the switch pick -- drawing
     * one treatment over all of them would show a page that cannot exist. */
    const VIZ_SET_FOR = { envelope: "viz_envelope", filter: "viz_filter", lfo: "viz_lfo",
                          sample: "viz_sample", waveform: "viz_lfo", switch: "viz_switch" };
    for (const grp of (pageCase.viz || [])) {
        const setId = VIZ_SET_FOR[grp.kind];
        const opt = setId ? optionFor(setId) : null;
        if (!opt) continue;
        const row = grp.slotStart < 4 ? 0 : 1;
        const rowY = row === 0 ? RM.ROW0_Y : RM.ROW1_Y;
        const col0 = grp.slotStart % 4;
        const rect = { x: col0 * RM.CELL_W + 1, y: rowY + 1,
                       w: (grp.slotCount || 1) * RM.CELL_W - 2, h: 13 };
        ctx.fillRect(col0 * RM.CELL_W, rowY, (grp.slotCount || 1) * RM.CELL_W, RM.BOX_H, 0);

        /* The two families take DIFFERENT arguments and there is no calling
         * convention that satisfies both. A curve treatment is
         * (ctx, rect, heights, opts) -- it is handed the shape rather than
         * deriving it, which is what stops a treatment from misrepresenting a
         * filter. The switch is (ctx, rect, key, values, metaIndex) like the
         * widget it replaces. One call site for both silently drew nothing. */
        if (setId === "viz_switch") {
            opt.draw(ctx, rect, grp.key || (grp.roles && grp.roles.value), pageCase.values, pageCase.metaIndex);
        } else {
            const n = rect.w;
            const heights = Array.from({ length: n }, (_, i) =>
                0.5 + 0.42 * Math.sin((i / (n - 1)) * Math.PI * 2));
            opt.draw(ctx, rect, heights, { baseFrac: 0.5, mirror: false });
        }
    }

    const keys = (pageCase.page && pageCase.page.keys) || [];
    const rows = [[0, RM.ROW0_Y, RM.LBL0_Y], [1, RM.ROW1_Y, RM.LBL1_Y]];
    for (const [row, rowY, lblY] of rows) {
        for (let col = 0; col < 4; col++) {
            const idx = row * 4 + col;
            const key = keys[idx];
            if (!key) continue;
            const meta = pageCase.metaIndex && pageCase.metaIndex.get
                ? pageCase.metaIndex.get(key) : (pageCase.metaIndex || {})[key];
            if (covered.has(idx)) continue;

            const cellX = col * RM.CELL_W;
            const kx = cellX + Math.floor((RM.CELL_W - RM.KW) / 2);
            const v = (idx + 1) / 9;

            if (meta && meta.kind === "enum" && enumOpt) {
                ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
                enumOpt.draw(ctx, cellX + Math.floor((RM.CELL_W - RM.ENUM_W) / 2), rowY, "POLY");
            } else if (meta && meta.kind !== "enum" && knobOpt) {
                ctx.fillRect(cellX, rowY, RM.CELL_W, RM.BOX_H, 0);
                knobOpt.draw(ctx, kx, rowY, v);
            }

            if (labelOpt) {
                ctx.fillRect(cellX, lblY, RM.CELL_W, RM.LBL_H, 0);
                /* The same label the shipping row draws: meta.label shortened
                 * by labelForCell. Using the raw param KEY instead produced
                 * "LFO1_" across every cell -- a page that looks plausible and
                 * is not the one the device draws, which is the one thing a
                 * composite must not do. */
                const label = RM.labelForCell((meta && (meta.label || meta.key)) || key, g.cellW);
                const touched = idx === 2 || idx === 6;
                labelOpt.draw(ctx, g, col, lblY, label,
                              touched ? "0.62" : "", touched, touched, false);
            }
        }
    }

    if (fillsOpt) {
        /* The whole band the footer owns, RULE_Y..63 — derived, not
         * RULE_Y + FOOTER_H + 1, which stopped clearing the last row the
         * moment the bands were re-cut and FOOTER_H stopped being 8. */
        ctx.fillRect(0, RM.RULE_Y, RM.W, RM.FOOTER_Y + RM.FOOTER_H - RM.RULE_Y, 0);
        /* pageCase.footer, in the shape drawFooter expects -- [[key, action],
         * ...]. Flattening it to a list of words made the footer render as
         * single letters. */
        fillsOpt.draw(ctx, pageCase.footer);
    }

    return fb;
}

function main() {
    const problems = S.validateAll();
    if (problems.length) {
        console.error("composite: registry has problems:");
        for (const p of problems) console.error("  " + p);
        process.exit(1);
    }

    /* catalog-page.json stores the CONTRACT; metaIndex and viz are live objects
     * with methods and cannot survive JSON, so they are rebuilt. Identical to
     * catalog.mjs loadPageCase -- deliberately copied rather than approximated,
     * because guessing the shape here produced a page that threw. */
    const j = JSON.parse(fs.readFileSync("tools/param-pages/catalog-page.json", "utf8"));
    const metaIndex = buildMetaIndex({ hierarchy: j.hierarchy, chainParams: j.chain_params });
    const { groups } = resolveViz({ keys: j.page.keys, metaIndex });
    const pageCase = {
        page: j.page, metaIndex, values: j.values, title: j.title,
        pageIndex: j.pageIndex, pageCount: j.pageCount,
        touched: typeof j.touched === "number" ? j.touched : -1,
        viz: groups, footer: j.footer,
    };

    const outDir = "catalog-out/_composite";
    fs.mkdirSync(outDir, { recursive: true });

    const before = createFramebuffer();
    RM.renderPageMovy(drawContext(before), pageCase);
    fs.writeFileSync(path.join(outDir, "before.png"), before.toPng(4));

    const after = renderComposite(pageCase);
    fs.writeFileSync(path.join(outDir, "after.png"), after.toPng(4));

    /* Stacked, because side by side at 4x is 1024px and the difference between
     * two 1-bit pages is easier to see one above the other. */
    const both = createFramebuffer(128, 64 * 2 + 3);
    for (const [src, yOff] of [[before, 0], [after, 67]])
        for (let y = 0; y < 64; y++)
            for (let x = 0; x < 128; x++)
                both.setPixel(x, y + yOff, src.pixels[y * 128 + x]);
    fs.writeFileSync(path.join(outDir, "before-after.png"), both.toPng(4));

    const chosen = Object.entries(PICKS).filter(([, v]) => v);
    console.log(`composite: ${chosen.length} pick(s) applied -> ${outDir}/`);
    for (const [k, v] of chosen) console.log(`  ${k.padEnd(14)} ${v}`);
    const open = Object.entries(PICKS).filter(([, v]) => !v).map(([k]) => k);
    if (open.length) console.log(`  still open: ${open.join(", ")}`);
    if (after.clipped()) console.log(`  WARNING: ${after.clipped()} pixel(s) drawn outside the screen`);
}

main();
