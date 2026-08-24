/*
 * Render the ONE LIST's real states to PNG, driving the actual menu_layout
 * functions -- not a reimplementation. If this looks wrong, the device looks
 * wrong.
 *
 *   node tools/param-pages/preview_list.mjs <out-dir> [scale]
 *
 * This exists because text-art review missed the scroll-arrow/value collision
 * three times running. Render it and LOOK at it.
 */
import * as H from "./harness.mjs";
import * as ML from "../../src/shared/menu_layout.mjs";
import fs from "node:fs";
import path from "node:path";

const OUT = process.argv[2] || ".";
const SCALE = parseInt(process.argv[3] || "5", 10);
fs.mkdirSync(OUT, { recursive: true });

function shot(name, draw) {
    const fb = H.createFramebuffer();
    const ctx = H.drawContext(fb);
    globalThis.clear_screen = () => fb.fillRect(0, 0, 128, 64, 0);
    globalThis.fill_rect = fb.fillRect;
    globalThis.print = fb.print;
    globalThis.text_width = fb.textWidth;
    globalThis.set_pixel = fb.setPixel;
    globalThis.line = ctx.line;
    clear_screen();
    draw();
    const file = path.join(OUT, name + ".png");
    fs.writeFileSync(file, fb.toPng(SCALE));
    console.log(`${name}  clipped=${fb.clipped()}  missing=${[...fb.missingGlyphs].join("") || "-"}`);
    return file;
}

const listArea = { topY: ML.LIST_TOP_Y, bottomY: ML.FOOTER_RULE_Y };

/* 1. Slot settings, mid-list, with a value column and a footer. */
const SLOT = [
    { label: "Patch:",      value: "Braids" },
    { label: "Edit Chain:", value: "" },
    { label: "Volume:",     value: "100%" },
    { label: "Muted:",      value: "No" },
    { label: "Soloed:",     value: "No" },
];
shot("01-slot-settings", () => {
    ML.drawMenuHeader("Slot 1");
    ML.drawMenuList({
        items: SLOT, selectedIndex: 0,
        getLabel: (i) => i.label, getValue: (i) => i.value,
        listArea, valueAlignRight: true, prioritizeSelectedValue: true,
        announce: false,
    });
    ML.drawMenuFooter(["Back: slots", "Click: edit"]);
});

/* 2. The same list in edit mode -- the affordance all four spellings converged on. */
shot("02-edit-mode", () => {
    ML.drawMenuHeader("Slot 1");
    ML.drawMenuList({
        items: SLOT, selectedIndex: 2,
        getLabel: (i) => i.label, getValue: (i) => i.value,
        listArea, valueAlignRight: true, prioritizeSelectedValue: true,
        editMode: true, announce: false,
    });
    ML.drawMenuFooter(["Click: done", "Jog: adjust"]);
});

/* 3. A long list, scrolled to the end -- scroll arrow, selection off the last row. */
const LONG = Array.from({ length: 12 }, (_, i) => ({
    label: "Item " + (i + 1), value: "v" + (i + 1),
}));
shot("03-scrolled", () => {
    ML.drawMenuHeader("Global Settings");
    ML.drawMenuList({
        items: LONG, selectedIndex: 11,
        getLabel: (i) => i.label, getValue: (i) => i.value,
        listArea, valueAlignRight: true, announce: false,
    });
    ML.drawMenuFooter(["Back: return", "Jog: scroll"]);
});

/* 4. The file picker -- labels only, no value column. */
const FILES = [
    { label: "Samples/" }, { label: "Recordings/" }, { label: "kick_01.wav" },
    { label: "snare_hi.wav" }, { label: "pad_long_name_here.wav" },
];
shot("04-file-picker", () => {
    ML.drawMenuHeader("File Browser");
    ML.drawMenuList({
        items: FILES, selectedIndex: 2,
        getLabel: (i) => i.label, getValue: () => "",
        listArea, announce: false,
    });
    ML.drawMenuFooter(["Click: select", "Jog: scroll"]);
});

/* 5. Save As -- the widget that was copy-pasted between slots and Master FX. */
shot("05-name-preview", () => {
    ML.drawNamePreview({ name: "My Patch 01", selectedIndex: 1 });
});

/* 6. Confirm modal -- rode along with the geometry move. */
shot("06-confirm", () => {
    ML.drawConfirmModal({ title: "Overwrite?", name: "My Patch 01", selectedIndex: 0 });
});
