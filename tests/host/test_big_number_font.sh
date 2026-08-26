#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE BIG NUMBER HAS ITS OWN FONT, CUT FOR DIGITS AND NOTHING ELSE.
#
# Unframed at the body font's 7 rows the cell read as bare. It now uses the
# Tamzen 8x16 cut (9 rows) rather than a 2x scale of the 6x12 -- an
# integer-scaled bitmap is the same letterform with every flaw doubled, and it
# caps at two glyphs.
#
# TWO BOUNDS, AND THE FLEET IS SWEPT AGAINST BOTH, because the span rule bounds
# the RANGE and not the digit count: an int 100..120 is span 20 and three digits
# wide, so "no value overflows" cannot be reasoned about from the rule and has
# to be measured over every value of every cell.
#
# THE CHARSET IS THE OTHER HALF, and it is the one that fails silently. The
# generator's OVERRIDES are hand-drawn at SEVEN rows for glyphs taller than that
# window; generated at nine they would be wrong, in nine characters nobody would
# look at. So the number font is generated for "0123456789+-" alone and this
# asserts that bigNumberText can emit nothing outside it -- including the "--"
# an unread value draws, which is the case a digits-only charset most easily
# forgets.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the big number font test" >&2
  exit 1
fi

node --input-type=module -e '
import fs from "node:fs";
import { shouldDrawBigNumber, bigNumberText, CELL_W, BOX_H }
    from "./src/shared/param_pages/render_page_movy.mjs";
import { fontWidth, HEIGHT, missingGlyphs }
    from "./src/shared/param_pages/font_tamzen8x16_num.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { planPages } from "./src/shared/param_pages/page_plan.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const BUDGET = CELL_W - 2;
if (HEIGHT > BOX_H)
    bad("the number font is " + HEIGHT + " rows and the widget box is " + BOX_H);
/* It must actually be BIGGER than the body font, or none of this was worth it. */
if (HEIGHT <= 7)
    bad("the number font is " + HEIGHT + " rows, no taller than the 7-row body font -- " +
        "the cell is still bare and this change bought nothing");

const fleet = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8")).modules;
let cells = 0, widest = 0, widestAt = "";
const missing = new Set();
const over = [];
for (const c of fleet) {
    let cp = c.chain_params;
    if (typeof cp === "string") { try { cp = JSON.parse(cp); } catch { continue; } }
    if (!Array.isArray(cp)) continue;
    let plan;
    try { plan = planPages({ hierarchy: c.ui_hierarchy, chainParams: cp }); } catch { continue; }
    const metaIndex = buildMetaIndex({ hierarchy: c.ui_hierarchy, chainParams: cp });
    for (const page of plan.pages || []) {
        for (const k of page.keys || []) {
            if (!k) continue;
            const m = metaIndex.getOrGuess(k);
            if (!shouldDrawBigNumber(m)) continue;
            cells++;
            const texts = [];
            for (let v = m.min; v <= m.max; v++) texts.push(bigNumberText(m, String(v)));
            texts.push(bigNumberText(m, null));      /* the unread form */
            texts.push(bigNumberText(m, ""));        /* and the unserved one */
            for (const t of texts) {
                for (const ch of missingGlyphs(t)) missing.add(ch);
                const w = fontWidth(t);
                if (w > widest) { widest = w; widestAt = c.id + ":" + k + " = " + t; }
                if (w > BUDGET) over.push(c.id + ":" + k + " = " + t + " (" + w + "px)");
            }
        }
    }
}
if (cells < 100)
    bad("only " + cells + " big-number cells found -- the sweep is not reaching the fleet");
if (over.length)
    bad(over.length + " value(s) overflow the " + BUDGET + "px cell, e.g. " +
        over.slice(0, 4).join(", ") + " -- the span rule bounds the RANGE, not the digit count");
if (missing.size)
    bad("bigNumberText can emit glyph(s) the number font does not have: " +
        [...missing].map((c) => JSON.stringify(c)).join(" ") +
        " -- the charset is restricted on purpose (the generator OVERRIDES are cut " +
        "at 7 rows), so either the text changed or the font must be regenerated");

if (fail === 0) {
    console.log("PASS: " + cells + " big-number cells, widest " + widest + "/" + BUDGET +
        "px (" + widestAt + "), " + HEIGHT + "-row font, every glyph present");
}
process.exit(fail ? 1 : 0);
'
