/**
 * cases.mjs — the representative page renders that the snapshot test pins.
 *
 * Shared by the test and by gen_snapshots.mjs so the two can never disagree
 * about what is being rendered. Each case is chosen because it is the hardest
 * example of something in the fleet, not because it looks nice:
 *
 *   obxd      the ordinary case — 8 declared knobs, both layouts, plus the
 *             held-knob and reveal-values states
 *   arp       enums and a half-empty page
 *   sf2       2 authored knobs, the rest carried by overflow pages
 *   mrdrums   opaque cells (filepath / wav_position) that a knob cannot turn
 *   forge     labels containing characters the device font cannot draw
 *   minijv    a deep page, prefixed to stay distinct from three siblings
 *   touched   a held knob showing its full name and value
 *   locked    a sequencer's parameter locks decorating the grid
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createFramebuffer, drawContext } from "./harness.mjs";
import { planPages, PAGE_KNOBS } from "../../src/shared/param_pages/page_plan.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { renderPage, renderPicker, renderHint, LAYOUT_BAR, LAYOUT_DIAL } from "../../src/shared/param_pages/render_page.mjs";
import { groupIndex } from "../../src/shared/param_pages/page_nav.mjs";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
export const FIXTURE = path.join(ROOT, "tests", "fixtures", "module-contracts.json");
export const SNAPSHOT = path.join(ROOT, "tests", "fixtures", "snapshots", "param_pages.txt");

/** Deterministic stand-in values — there is no device to read from. */
export function fakeValue(key, meta) {
    let h = 0;
    for (let i = 0; i < key.length; i++) h = (h * 31 + key.charCodeAt(i)) >>> 0;
    const t = (h % 1000) / 1000;
    if (!meta) return t.toFixed(3);
    if (meta.kind === "opaque") return "/data/UserData/Samples/kick_01.wav";
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    const v = min + (max - min) * t;
    return (meta.type === "int" || meta.type === "enum") ? String(Math.round(v)) : v.toFixed(3);
}

const CASES = [
    { name: "obxd / dial — the default: the ordinary 8-knob page", id: "obxd", nth: 0 },
    { name: "obxd / dial — knob 2 held: name and value in the header strip, cell keeps its label", id: "obxd", nth: 0, touched: 1 },
    { name: "obxd / dial — reveal held: every label swapped for its value", id: "obxd", nth: 0, revealValues: true },
    { name: "obxd / bar  — the alternative: every value visible at once", id: "obxd", nth: 0, layout: LAYOUT_BAR },
    { name: "obxd / bar  — knob 2 held", id: "obxd", nth: 0, layout: LAYOUT_BAR, touched: 1 },
    { name: "arp         — enums, and a page with only four params", id: "arp", nth: 0, layout: LAYOUT_BAR },
    { name: "sf2         — 2 authored knobs plus 4 params[]-only keys on one page", layout: LAYOUT_BAR, id: "sf2", nth: 0 },
    { name: "hush1       — a pure overflow continuation page", layout: LAYOUT_BAR, id: "hush1", nth: 1 },
    { name: "mrdrums     — opaque cells: a knob cannot turn a filepath", layout: LAYOUT_BAR, id: "mrdrums", nth: 0 },
    { name: "forge       — labels folded to ASCII (Copy A>B, Swap A<>B)", layout: LAYOUT_BAR, id: "forge", nth: 3 },
    { name: "minijv      — a deep page, prefixed apart from its three siblings", layout: LAYOUT_BAR, id: "minijv", nth: 6 },
    {
        name: "obxd / bar  — parameter locks: slots 0 and 3 held by a sequencer step",
        id: "obxd", nth: 0, layout: LAYOUT_BAR,
        decorations: [{ value: "12", locked: true }, null, null, { value: "99", locked: true }],
    },
    {
        name: "obxd / bar  — drawn into a tool's region: value line drops, nothing escapes the rect",
        id: "obxd", nth: 0, layout: LAYOUT_BAR, rect: { x: 0, y: 14, w: 128, h: 50 },
    },
    { name: "euclidrum   — eight cells the module names identically, disambiguated by key", id: "euclidrum", nth: 0 },
    { name: "branchage   — a sparse page: unused knob positions marked, not blank", id: "branchage", nth: 3, layout: LAYOUT_BAR },
    { name: "obxd / dial — knob 1 modulated (the list editor's \"~\")", id: "obxd", nth: 0, modulated: 0 },
    { name: "minijv      — the section picker: 70 patch pages folded to 12 sections", id: "minijv", nth: 0, picker: 6 },
    { name: "obxd        — the first-run gesture hint, cleared by any input", id: "obxd", nth: 0, hint: true },
];

/** Render every case; returns the full snapshot text. */
export function renderCases() {
    const fx = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
    const out = [];
    const missing = new Set();

    for (const c of CASES) {
        const mod = fx.modules.find((m) => m.id === c.id);
        if (!mod) throw new Error(`fixture has no module "${c.id}"`);
        const { pages } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
        const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
        const grid = pages.map((p, i) => ({ p, i })).filter(({ p }) => p.kind === PAGE_KNOBS);
        const picked = grid[c.nth];
        if (!picked) throw new Error(`${c.id} has no grid page #${c.nth}`);

        const values = {};
        for (const k of picked.p.keys) values[k] = fakeValue(k, metaIndex.getOrGuess(k));

        const fb = createFramebuffer();
        if (c.hint) {
            renderPage(drawContext(fb), {
                page: picked.p, metaIndex, values,
                title: `T1 > ${String(mod.name || mod.id).toUpperCase()}`,
                pageIndex: picked.i, pageCount: pages.length, touched: -1,
            });
            renderHint(drawContext(fb), {
                lines: ["Jog: page", "Shift+Jog: section", "Click: section list",
                        "Hold knob: name", "Shift: show values"],
                title: "Param Pages",
            });
            if (fb.clipped() > 0) throw new Error(`${c.name}: hint drew outside the display`);
            for (const g of fb.missingGlyphs) missing.add(g);
            out.push(`### ${c.name}`);
            out.push(`# shown once per session`);
            out.push(fb.toBlocks());
            out.push("");
            continue;
        }
        if (c.picker !== undefined) {
            renderPicker(drawContext(fb), { entries: groupIndex(pages), index: c.picker, title: "Sections" });
            if (fb.clipped() > 0) throw new Error(`${c.name}: picker drew ${fb.clipped()} px outside the display`);
            for (const g of fb.missingGlyphs) missing.add(g);
            out.push(`### ${c.name}`);
            out.push(`# ${groupIndex(pages).length} sections from ${pages.length} pages`);
            out.push(fb.toBlocks());
            out.push("");
            continue;
        }
        renderPage(drawContext(fb), {
            page: picked.p, metaIndex, values,
            title: `T1 > ${String(mod.name || mod.id).toUpperCase()}`,
            pageIndex: picked.i, pageCount: pages.length,
            touched: c.touched === undefined ? -1 : c.touched,
            decorations: c.decorations || null,
            layout: c.layout || LAYOUT_DIAL,
            revealValues: !!c.revealValues,
            rect: c.rect || null,
            /* Real section ids, so the page rule in a snapshot is the one the
             * device draws rather than an ungrouped approximation. */
            pageGroups: pages.map((p) => (p.level == null ? p.kind : p.level)),
            modulated: c.modulated === undefined ? null : ((k) => k === picked.p.keys[c.modulated]),
        });
        if (fb.clipped() > 0) throw new Error(`${c.name}: drew ${fb.clipped()} px outside the display`);
        for (const g of fb.missingGlyphs) missing.add(g);

        out.push(`### ${c.name}`);
        out.push(`# page "${picked.p.name}" (${picked.i + 1}/${pages.length}), keys: ${picked.p.keys.join(", ")}`);
        out.push(fb.toBlocks());
        out.push("");
    }

    return { text: out.join("\n"), missing: [...missing] };
}
