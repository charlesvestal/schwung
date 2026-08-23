/*
 * DOES EVERY LIST ROW FIT? — a full sweep of labels against their own values.
 *
 * A list row is one line: cursor prefix, label, gap, value, right edge. The
 * label's room is therefore whatever the WIDEST value of that parameter leaves
 * behind, which means a name cannot be judged on its own — "Overlay Knobs" is
 * a fine name next to "Off" and an impossible one next to "+Jog Touch".
 *
 * This measures that, for every parameter we declare (Global Settings, a chain
 * slot's settings, Master FX's) and for every parameter the fleet declares
 * (tests/fixtures/module-contracts.json), using the real device font through
 * the harness framebuffer — not a 6px-per-glyph estimate, which over-reserves
 * by up to 4px a character and would call rows broken that are fine.
 *
 *   node tools/param-pages/measure_labels.mjs            # ours only
 *   node tools/param-pages/measure_labels.mjs --fleet    # ours + all modules
 *   node tools/param-pages/measure_labels.mjs --json     # machine-readable
 */
import { createFramebuffer } from "./harness.mjs";
import { MENU_LIST_X } from "../../src/shared/param_pages/page_controller.mjs";
import { SCREEN_WIDTH } from "../../src/shared/list_geometry.mjs";
import { DEFAULT_LABEL_GAP } from "../../src/shared/menu_layout.mjs";
import * as fs from "node:fs";

const fb = createFramebuffer();
const w = (s) => fb.textWidth(String(s == null ? "" : s));

/* The page-chrome row, which is the narrowest list we draw and so the one that
 * decides. valuePaddingRight is 10 there — the frame's right column is x=123.
 * The scroll arrow no longer pulls this in: it is charged to the two rows it
 * actually touches (see menu_layout.mjs). */
export const VALUE_RIGHT_EDGE = SCREEN_WIDTH - 10;
/* No cursor prefix: the selected row is inverted, so the caret is gone and the
 * label starts at MENU_LIST_X itself. */
export const ROW_BUDGET = VALUE_RIGHT_EDGE - MENU_LIST_X - DEFAULT_LABEL_GAP;

/** The widest string this param can ever show in the value column. */
export function widestValue(p) {
    if (Array.isArray(p.options) && p.options.length) {
        let best = "", bw = -1;
        for (const o of p.options) { const x = w(o); if (x > bw) { bw = x; best = String(o); } }
        return best;
    }
    /* Non-enums: the widest thing formatParamValue can produce is bounded by
     * the range and the unit, and "-0.00 ms" is the worst realistic shape. */
    const unit = p.unit ? " " + p.unit : "";
    const n = (p.type === "int") ? String(p.min !== undefined ? p.min : -999)
                                 : (p.min !== undefined && p.min < 0 ? "-0.00" : "0.00");
    return n + unit;
}

/** { fits, over, room } for one declared param on a page-chrome row. */
export function measureRow(p) {
    const value = widestValue(p);
    const room = ROW_BUDGET - w(value);
    const need = w(p.name || p.key);
    /* preferFullName: the declaration says this row may exceed its width because
     * the whole name matters more than fitting. Reported as `exempt` rather
     * than silently as `fits`, so a sweep still shows it and nobody discovers
     * the truncation on a device wondering whether it is a bug. */
    const exempt = !!p.preferFullName;
    return { name: p.name || p.key, value, room, need,
             fits: need <= room || exempt, exempt,
             over: need - room };
}

/* ---------------------------------------------------------------- our own */
async function ours() {
    const G = await import("../../src/shadow/shadow_ui_global_grid.mjs");
    const S = await import("../../src/shadow/shadow_ui_slot_grid.mjs");
    const out = [];
    const io = { readParam: () => "0", writeParam: () => {} };
    for (const p of G.buildGlobalSettingsContract(io).chainParams) out.push(["global", p]);
    for (const p of (S.SLOT_GRID_PARAMS || [])) out.push(["slot", p]);
    for (const p of (S.MASTER_GRID_PARAMS || [])) out.push(["master", p]);
    return out;
}

/* ------------------------------------------------------------------ fleet */
function fleet() {
    const { FIXTURE } = { FIXTURE: "tests/fixtures/module-contracts.json" };
    if (!fs.existsSync(FIXTURE)) return [];
    const fx = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
    const out = [];
    for (const mod of fx.modules || []) {
        for (const p of (mod.chain_params || [])) out.push([mod.id, p]);
    }
    return out;
}

/* The CLI half runs only when this file IS the entry point. It is imported by
 * tests/host/test_global_settings_contract.sh for measureRow, and a module body
 * that printed its report on import leaked the whole table into that test's
 * output -- which it did, once. */
const isMain = !!(process.argv[1] && process.argv[1].endsWith("measure_labels.mjs"));
if (isMain) {
const argv = process.argv.slice(2);
const rows = [...(await ours()), ...(argv.includes("--fleet") ? fleet() : [])]
    .map(([src, p]) => ({ src, key: p.key, ...measureRow(p) }));

if (argv.includes("--json")) {
    console.log(JSON.stringify({ budget: ROW_BUDGET, rows }, null, 2));
} else {
    const bad = rows.filter((r) => !r.fits).sort((a, b) => b.over - a.over);
    console.log(`row budget ${ROW_BUDGET}px  (label + value, page-chrome list)\n`);
    const exempt = rows.filter((r) => r.exempt && r.over > 0);
    console.log(`${rows.length} rows measured, ${bad.length} do not fit` +
        (exempt.length ? `, ${exempt.length} deliberately over (preferFullName)` : "") + "\n");
    for (const r of exempt) {
        console.log(`  over by ${r.over}px BY CHOICE: ${JSON.stringify(r.name)} ` +
                    `beside ${JSON.stringify(r.value)}`);
    }
    if (exempt.length) console.log("");
    console.log("over  where     name                 value");
    for (const r of bad) {
        console.log(`${String(r.over).padStart(4)}  ${r.src.padEnd(9)} ` +
                    `${String(r.name).padEnd(20)} ${JSON.stringify(r.value)}`);
    }
}
}
