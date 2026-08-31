/*
 * Render the module-lists screens to PNG, driving the real drawListScreen and
 * drawConfirmOverlay. If this looks wrong, the device looks wrong.
 *
 *   node tools/param-pages/preview_module_lists.mjs <out-dir> [scale]
 *
 * The VALUE COLUMN is the thing to look at. A checkbox, a member count and a
 * filter name all want the same pixels the scroll arrow does, and the longest
 * of them is a user-typed list name with nothing capping its length.
 */
import * as H from "./harness.mjs";
import * as ML from "../../src/shared/menu_layout.mjs";
import { drawListScreen, drawChainPicker } from "../../src/shared/chain_editor_chrome.mjs";
import fs from "node:fs";
import path from "node:path";

const OUT = process.argv[2] || ".";
const SCALE = parseInt(process.argv[3] || "5", 10);
fs.mkdirSync(OUT, { recursive: true });

let bad = 0;
function shot(name, draw) {
    const fb = H.createFramebuffer();
    const hctx = H.drawContext(fb);
    globalThis.clear_screen = () => fb.fillRect(0, 0, 128, 64, 0);
    globalThis.fill_rect = fb.fillRect;
    globalThis.print = fb.print;
    globalThis.text_width = fb.textWidth;
    globalThis.set_pixel = fb.setPixel;
    globalThis.line = hctx.line;
    clear_screen();
    draw({ fillRect: fb.fillRect, print: fb.print, textWidth: fb.textWidth });
    fs.writeFileSync(path.join(OUT, name + ".png"), fb.toPng(SCALE));
    const miss = [...fb.missingGlyphs].join("") || "-";
    if (fb.clipped() || miss !== "-") bad++;
    console.log(`${name}  clipped=${fb.clipped()}  missing=${miss}`);
}

/* 1. Membership. Checked and unchecked, plus a long name against the checkbox. */
shot("lists_membership", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Braids", headerRight: "LISTS", index: 0,
        entries: [
            { name: "Favorites", value: "*" },
            { name: "Live Set", value: "" },
            { name: "Monday Night Drone", value: "*" },
            { name: "New List...", value: "" },
            { name: "Edit Lists...", value: "" },
        ],
        footer: [["CLK", "TOGGLE"], ["BACK", "MODULE"]],
    });
});

/* 2. Edit Lists, with counts — three digits is the widest this column carries. */
shot("lists_edit", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Edit Lists", index: 1,
        entries: [
            { name: "Favorites", value: "3" },
            { name: "Live Set", value: "17" },
            { name: "Monday Night Drone", value: "128" },
        ],
        footer: [["JOG", "SEL"], ["CLK", "EDIT"], ["BACK", "LISTS"]],
    });
});

/* 3. Favorites offers Clear ALONE — Rename and Delete absent, not greyed. */
shot("lists_actions_favorites", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Favorites", index: 0,
        entries: [{ name: "Clear" }],
        footer: [["CLK", "DO"], ["BACK", "EDIT"]],
    });
});

/* 4. An ordinary list, all three verbs. */
shot("lists_actions_ordinary", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Live Set", index: 1,
        entries: [{ name: "Rename" }, { name: "Delete" }, { name: "Clear" }],
        footer: [["JOG", "SEL"], ["CLK", "DO"], ["BACK", "EDIT"]],
    });
});

/* 5. The confirm, with the footer swapped to agree with the overlay. */
shot("lists_actions_confirm", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Live Set", index: 1,
        entries: [{ name: "Rename" }, { name: "Delete" }, { name: "Clear" }],
        footer: [["CLK", "YES"], ["BACK", "NO"]],
    });
    ML.drawConfirmOverlay("Delete List", ["Live Set?"], "CLK=Yes  BACK=No");
});

/* 6. The confirm with a LONG name — the case that ran off both sides of the
 *    box before it was fitted. */
shot("lists_actions_confirm_long", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Monday Night Drone", index: 1,
        entries: [{ name: "Rename" }, { name: "Delete" }, { name: "Clear" }],
        footer: [["CLK", "YES"], ["BACK", "NO"]],
    });
    ML.drawConfirmOverlay("Delete List", ["Monday Night Drone?"], "CLK=Yes  BACK=No");
});

/* 7. The picker with the filter row, drawn through the REAL drawChainPicker
 *    so the entry-value rule is exercised, not reimplemented. */
shot("lists_picker_filtered", (ctx) => {
    drawChainPicker(ctx, {
        headerLeft: "S1 > Synth",
        entries: [
            { id: "__list_filter__", name: "List", value: "Monday Night Drone", clickVerb: "LIST" },
            { id: "", name: "None" },
            { id: "braids", name: "Braids" },
            { id: "hera", name: "Hera" },
            { id: "__get_more__", name: "[Get more...]" },
        ],
        index: 0,
        currentId: "braids",
        emptyMessage: "No modules available",
    });
});

/* 8. Same picker on All, cursor on the loaded module, so the `*` and the
 *    filter row are visible together — the two rules that share the column. */
shot("lists_picker_all", (ctx) => {
    drawChainPicker(ctx, {
        headerLeft: "S1 > Synth",
        entries: [
            { id: "__list_filter__", name: "List", value: "All", clickVerb: "LIST" },
            { id: "", name: "None" },
            { id: "braids", name: "Braids" },
            { id: "__move_left__", name: "  Move Left" },
            { id: "__move_right__", name: "  Move Right" },
            { id: "hera", name: "Hera" },
        ],
        index: 2,
        currentId: "braids",
        emptyMessage: "No modules available",
    });
});

if (bad) { console.error(bad + " shot(s) clipped or missing glyphs"); process.exit(1); }
console.log("all shots clean");
