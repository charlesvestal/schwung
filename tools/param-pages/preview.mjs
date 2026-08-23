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
 * --layout list renders the SAME page as five rows (page_controller.mjs,
 * LAYOUT_LIST). It drives a real controller against a fake device serving the
 * fixture's contract, so the values, labels and chrome are the ones the engine
 * produces rather than a re-implementation. Add --enter to see the entered
 * state (a row highlighted) and --edit to see a row opened for editing
 * (`[value]`); without either it draws the inert, bracketed state you arrive on.
 *
 * --trailing appends the "User Presets" and "Module" pages every REAL
 * component gets at the end of its jog sequence (shadow_ui.js
 * componentTrailingMenus), with a representative loaded preset so the `*`
 * and the Save/Delete rows are visible. Being PAGE_MENU pages (not
 * PAGE_KNOBS), they are always driven through a real controller — same as
 * --layout list — even under --layout movy, because a menu has no grid
 * representation to hand-render.
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
import { createController, LAYOUT_LIST } from "../../src/shared/param_pages/page_controller.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";
import { createFakeDevice } from "./fake_device.mjs";

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
    console.error("usage: preview.mjs <module-id> [--page N | --all] [--layout dial|bar|movy|list] [--trailing] [--reveal] [--touch N] [--png DIR] [--scale N]");
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

/*
 * A representative "User Presets" / "Module" trailing set — same shape
 * shadow_ui.js's componentTrailingMenus builds, with a loaded preset that has
 * drifted (the `*`) so every row (including the Save/Delete pair that only
 * appear with a record) is visible in one render.
 */
const TRAILING_MENUS_FIXTURE = () => ([
    { name: "User Presets", entries: [
        { label: "Preset", value: "Fat Brass *" },
        { label: "Load…", action: "up_load" },
        { label: "Save", action: "up_save" },
        { label: "Save As", action: "up_save_as" },
        { label: "Delete", action: "up_delete" },
    ] },
    { name: "Module", entries: [
        { label: "Swap Module", action: "swap_module" },
        { label: "Remove Module", action: "remove_module" },
    ] },
]);
const trailingFlag = !!flag("trailing");

const { pages, warnings } = planPages({
    hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params,
    trailingMenus: trailingFlag ? TRAILING_MENUS_FIXTURE() : undefined,
});
const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
const gridPages = pages.map((p, i) => ({ p, i })).filter(({ p }) => p.kind === PAGE_KNOBS);

const LAYOUTS = { dial: LAYOUT_DIAL, bar: LAYOUT_BAR, movy: LAYOUT_MOVY, list: LAYOUT_LIST };
const layout = LAYOUTS[flag("layout", "dial")] || LAYOUT_DIAL;
const touched = flag("touch") !== null ? parseInt(flag("touch"), 10) : -1;
const revealValues = !!flag("reveal");
const pngDir = flag("png");
const scale = parseInt(flag("scale", "4"), 10);

let chosen;
if (flag("all")) {
    /* Same selection as before when there is nothing trailing to add — the
     * filter is `p.trailing` (set by page_plan.mjs's buildTrailingPages), not
     * a kind test, so PAGE_PRESET/PAGE_ITEMS pages elsewhere in a fixture
     * stay excluded exactly as --all always excluded them. */
    chosen = pages.map((p, i) => ({ p, i })).filter(({ p }) => p.kind === PAGE_KNOBS || p.trailing);
} else if (flag("page") !== null) {
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

    if (layout === LAYOUT_LIST || p.kind !== PAGE_KNOBS) {
        /* Drive the REAL controller against a fake device serving this module's
         * contract — the list layout, and every non-grid page kind (menu,
         * preset, items), live in page_controller.mjs, so anything short of
         * driving the real thing would be previewing a re-implementation. A
         * trailing "User Presets"/"Module" page is PAGE_MENU, so it always
         * takes this path, even under --layout movy — there is no grid
         * representation of a menu for renderPageMovy to draw below. */
        const dev = createFakeDevice({ id: mod.id, prefix: "synth", initial: values });
        const ctrl = createController({
            getParam: dev.getParam, setParam: dev.setParam,
            announce: () => {}, now: dev.now,
            trailingMenus: trailingFlag ? TRAILING_MENUS_FIXTURE : undefined,
        });
        ctrl.load({ prefix: "synth" });
        const ctrlLayout = layout === LAYOUT_LIST ? LAYOUT_LIST : LAYOUT_MOVY;
        ctrl.setLayout(ctrlLayout);
        ctrl.goToPage(i, { remember: false });
        /* One read per tick is the whole point of the cursor; give it enough
         * ticks for the page's values to land, exactly as the device does. */
        for (let t = 0; t < (p.keys || []).length + 3; t++) ctrl.tick();
        if (flag("enter") || flag("edit")) ctrl.enterMenu();
        if (flag("edit")) ctrl.onClick(0);
        ctrl.render(drawContext(fb), {
            title,
            footer: [["JOG", "PG"], ["SHFT", "SECT"], ["CLK", "MENU"]],
        });
    } else if (layout === LAYOUT_MOVY) {
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
