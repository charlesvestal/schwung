#!/usr/bin/env node
/**
 * preview.mjs — render param pages for a module from the fleet fixture and
 * print them as half-block art (or write PNGs).
 *
 * No hardware required: text goes through the device's own font atlas, so what
 * you see here is what the OLED shows.
 *
 *   node tools/param-pages/preview.mjs obxd
 *   node tools/param-pages/preview.mjs minijv --page 5 --layout dial
 *   node tools/param-pages/preview.mjs sf2 --all
 *   node tools/param-pages/preview.mjs obxd --png /tmp/out --scale 4
 *   node tools/param-pages/preview.mjs braids --layout movy
 *   node tools/param-pages/preview.mjs --list
 *
 * --layout movy renders schwung-movy's own knob-grid layout
 * (render_page_movy.mjs) instead of the dial/bar grid — graphics (envelope,
 * filter, lfo, eq) resolve the same way the real controller does
 * (viz.mjs resolveViz), so this is what the "Knobs" setting on device draws.
 *
 * Values are synthesised (mid-range, deterministic per key) since there is no
 * device to read them from — enough to judge layout, not to judge a patch.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createFramebuffer, drawContext } from "./harness.mjs";
import { planPages, PAGE_KNOBS } from "../../src/shared/param_pages/page_plan.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { renderPage, LAYOUT_DIAL, LAYOUT_BAR } from "../../src/shared/param_pages/render_page.mjs";
import { renderPageMovy, LAYOUT_MOVY } from "../../src/shared/param_pages/render_page_movy.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const FIXTURE = path.join(ROOT, "tests", "fixtures", "module-contracts.json");

const argv = process.argv.slice(2);
const flag = (name, dflt = null) => {
    const i = argv.indexOf("--" + name);
    return i >= 0 ? (argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[i + 1] : true) : dflt;
};
const positional = argv.filter((a, i) => !a.startsWith("--") && (i === 0 || !argv[i - 1].startsWith("--") || argv[i - 1] === "--all"));

const fx = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));

if (flag("list")) {
    const rows = fx.modules.map((m) => {
        const r = planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params });
        return [m.id, m.category, r.pages.length];
    }).sort((a, b) => b[2] - a[2]);
    for (const [id, cat, n] of rows) console.log(String(n).padStart(4), id.padEnd(20), cat);
    process.exit(0);
}

const id = positional[0];
if (!id) {
    console.error("usage: preview.mjs <module-id> [--page N | --all] [--layout dial|bar|movy] [--reveal] [--touch N] [--png DIR] [--scale N]");
    process.exit(2);
}
const mod = fx.modules.find((m) => m.id === id);
if (!mod) {
    console.error(`no module "${id}" in the fixture (try --list)`);
    process.exit(2);
}

/* Deterministic pseudo-values so a page looks alive and reruns identically. */
function fakeValue(key, meta) {
    let h = 0;
    for (let i = 0; i < key.length; i++) h = (h * 31 + key.charCodeAt(i)) >>> 0;
    const t = (h % 1000) / 1000;
    if (!meta) return String(t.toFixed(3));
    if (meta.kind === "opaque") return "/data/UserData/Samples/kick_01.wav";
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    const v = min + (max - min) * t;
    return meta.type === "int" || meta.type === "enum" ? String(Math.round(v)) : v.toFixed(3);
}

const { pages, warnings } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
const gridPages = pages.map((p, i) => ({ p, i })).filter(({ p }) => p.kind === PAGE_KNOBS);

const LAYOUTS = { dial: LAYOUT_DIAL, bar: LAYOUT_BAR, movy: LAYOUT_MOVY };
const layout = LAYOUTS[flag("layout", "dial")] || LAYOUT_DIAL;
const touched = flag("touch") !== null ? parseInt(flag("touch"), 10) : -1;
const revealValues = !!flag("reveal");
const pngDir = flag("png");
const scale = parseInt(flag("scale", "4"), 10);

let chosen;
if (flag("all")) chosen = gridPages;
else if (flag("page") !== null) {
    const n = parseInt(flag("page"), 10);
    chosen = [pages[n] ? { p: pages[n], i: n } : gridPages[0]];
} else chosen = gridPages.slice(0, 1);

const title = `T1 > ${String(mod.name || mod.id).toUpperCase()}`;
console.log(`${mod.id} — ${pages.length} pages (${gridPages.length} grid), layout=${layout}` +
            (warnings.length ? `  [${warnings.join("; ")}]` : ""));

for (const { p, i } of chosen) {
    const fb = createFramebuffer();
    const values = {};
    for (const k of (p.keys || [])) values[k] = fakeValue(k, metaIndex.getOrGuess(k));

    if (layout === LAYOUT_MOVY) {
        const { groups } = resolveViz({ keys: p.keys || [], metaIndex });
        renderPageMovy(drawContext(fb), {
            page: p, metaIndex, values, title,
            pageIndex: i, pageCount: pages.length, touched, viz: groups,
            /* Representative hints so previews show the real vertical budget.
             * The device's own hints come from shadow_ui_param_pages.mjs. */
            footer: touched >= 0
                ? [["MUTE", "DFLT"], ["SHFT", "FINE"]]
                : [["JOG", "PG"], ["SHFT", "SECT"], ["CLK", "MENU"]],
        });
    } else {
        renderPage(drawContext(fb), {
            page: p, metaIndex, values, title,
            pageIndex: i, pageCount: pages.length, touched, layout, revealValues,
        });
    }

    console.log(`\n── page ${i}: ${p.kind} "${p.name}"  ${(p.keys || []).length} keys` +
                (p.authored === false ? "  (overflow)" : ""));
    if (pngDir && pngDir !== true) {
        fs.mkdirSync(pngDir, { recursive: true });
        const file = path.join(pngDir, `${mod.id}-${String(i).padStart(2, "0")}.png`);
        fs.writeFileSync(file, fb.toPng(scale));
        console.log("   " + file);
    } else {
        console.log(fb.toBlocks());
    }
}
