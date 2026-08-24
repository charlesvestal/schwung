#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE BEHAVIOUR CONTRACT OF THE ONE LIST.
#
# Phase 1 of docs/superpowers/specs/2026-08-23-one-list-engine-design.md
# re-skins drawMenuList to the movy chrome, so EVERY list in the shadow UI
# changes appearance -- that is the deliverable, not a regression. A pixel
# identity gate would fail on success and pass only if the work were skipped.
#
# So this pins BEHAVIOUR and nothing else: which items are visible, in what
# order, which is selected, what values they carry, whether edit mode was
# signalled. Deliberately NO x/y assertions -- pinning coordinates here would
# re-create the gate this file exists to replace.
#
# Written against the list as it behaves TODAY, so it describes the contract
# rather than whatever the re-skin happens to produce.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

# There must be exactly one definition of LIST_TOP_Y in src/. A second copy is
# how the movy re-skin came to be inert on the device while this test stayed
# green: every view module imported the stale duplicate and passed it back in
# as listArea.topY, so the value under test was one nothing rendered with.
#
# The whole chrome is pinned, not just LIST_TOP_Y -- the duplicate was a block
# of eleven constants, and pinning one of them would have let the other ten
# fork again. `export const` is the shape that matters: a re-export
# (`export { LIST_TOP_Y } from ...`) is how the same names reach ~50 call sites
# from the one leaf and must NOT count as a definition.
geom_failures=0
for name in LIST_TOP_Y LIST_LABEL_X LIST_LINE_HEIGHT LIST_HIGHLIGHT_HEIGHT \
            TITLE_Y TITLE_RULE_Y LIST_VALUE_X FOOTER_RULE_Y FOOTER_TEXT_Y \
            HEADER_H RULE_Y MENU_LIST_X; do
    n=$(grep -rn "export const ${name}\b" src | wc -l | tr -d ' ')
    if [ "$n" != "1" ]; then
        echo "FAIL: ${name} has ${n} definitions in src/, expected exactly 1"
        grep -rn "export const ${name}\b" src || true
        geom_failures=$((geom_failures + 1))
    fi
done
if [ "$geom_failures" != "0" ]; then exit 1; fi

node --input-type=module -e '
const R = process.cwd();
const {
    drawMenuList, LIST_TOP_Y, FOOTER_RULE_Y, LIST_INDICATOR_BOTTOM_Y,
    LIST_LINE_HEIGHT,
} = await import(R + "/src/shared/menu_layout.mjs");
const { probe } = await import(R + "/tools/param-pages/list_probe.mjs");
const CV = await import(R + "/src/shared/chain_ui_views.mjs");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* ---- 0. The value under test is the value the SCREENS use ----------------
 *
 * Every shadow view module (shadow_ui.js, shadow_ui_slots / _settings /
 * _master_fx / _patches / _tools / _store) imports these from
 * chain_ui_views.mjs and passes them straight back to drawMenuList as
 * `listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y }`. When
 * chain_ui_views carried its own stale copy, this file measured menu_layout s
 * default and the device rendered something else -- so both doors are checked
 * here, by identity, before any window arithmetic is believed. */
for (const [name, mine, theirs] of [
    ["LIST_TOP_Y", LIST_TOP_Y, CV.LIST_TOP_Y],
    ["FOOTER_RULE_Y", FOOTER_RULE_Y, CV.FOOTER_RULE_Y],
    ["LIST_LINE_HEIGHT", LIST_LINE_HEIGHT, CV.LIST_LINE_HEIGHT],
]) {
    if (mine !== theirs)
        fail(name + ": menu_layout says " + mine + " but chain_ui_views -- which is " +
             "what every shadow view module actually imports -- says " + theirs +
             "; this test would be measuring a rect no screen renders with");
}
if (LIST_TOP_Y !== 10)
    fail("LIST_TOP_Y must be the movy value 10 (list rect 10..55, header band " +
         "ends at row 6); got " + LIST_TOP_Y + ". 15 is the pre-re-skin value, " +
         "and it leaves an 8-row dead band under the header and fits only 4 rows");

const items = (n) => Array.from({ length: n }, (_, i) => ({ label: "Item " + (i + 1) }));

const run = (opts) => {
    const call = {
        items: opts.items,
        selectedIndex: opts.selectedIndex,
        getLabel: (item) => item.label,
        announce: false,
        editMode: opts.editMode || false,
    };
    /* omitGetValue exercises drawMenuList own `getValue ? getValue(item, i)
     * : ""` fallback (menu_layout.mjs:227) -- a genuinely absent key, not a
     * no-op function supplied by this harness. */
    if (!opts.omitGetValue) call.getValue = opts.getValue || ((item) => item.value || "");
    if (opts.listArea) call.listArea = opts.listArea;
    return probe(() => drawMenuList(call));
};

/* ---- 1. 3-item list at index 0: all three visible, in order, row 0 selected ---- */
{
    const r = run({ items: items(3), selectedIndex: 0 });
    if (r.labels.join(",") !== "Item 1,Item 2,Item 3")
        fail("3-item list at index 0: expected all 3 labels in order, got " + r.labels.join(","));
    if (r.selectedIndex !== 0)
        fail("3-item list at index 0: expected row 0 selected, got row " + r.selectedIndex);
}

/* ---- 2. 3-item list at index 2: selection follows, still no scroll ---- */
{
    const r = run({ items: items(3), selectedIndex: 2 });
    if (r.labels.join(",") !== "Item 1,Item 2,Item 3")
        fail("3-item list at index 2: expected all 3 labels still visible, got " + r.labels.join(","));
    if (r.selectedIndex !== 2)
        fail("3-item list at index 2: expected row 2 selected, got row " + r.selectedIndex);
}

/* ---- 3. 12-item list at index 0: window starts at Item 1; record window size N ----
 *
 * Two variants matter here, and they genuinely differ TODAY: a caller who
 * lets `listArea` default (no footer on screen) gets the full run down to
 * `LIST_INDICATOR_BOTTOM_Y`; a caller who reserves footer space (the common
 * real case -- `listArea: {bottomY: FOOTER_RULE_Y}`) gets one fewer row.
 * Both window sizes are DERIVED from the same constants drawMenuList itself
 * divides by (`floor((bottomY - topY) / lineHeight)`), not hardcoded here,
 * so this reads real behaviour rather than asserting a magic number.
 *
 * There used to be a `LIST_MAX_VISIBLE = 5` exported at the top of
 * menu_layout.mjs. It was DEAD -- drawMenuList never referenced it -- and the
 * window size is 5 only because LIST_INDICATOR_BOTTOM_Y/LIST_TOP_Y/
 * LIST_LINE_HEIGHT divide out to 5. It has been deleted rather than left
 * around to be read as documenting behaviour it never controlled.
 *
 * The movy re-skin HAS NOW LANDED, and it moved the WITH-FOOTER case from 4
 * to 5: one shared rect, MENU_LIST_Y=10..RULE_Y=55, regardless of footer. The
 * derived checks above self-adjust (they divide the same constants
 * drawMenuList does), so on their own they would have accepted the change
 * silently -- and would equally accept a future change that LOST a row. The
 * two literal pins below exist for that: both windows must be 5, which is the
 * bar the design set (no-footer holds at 5, with-footer gains a row back). */
let N, footerN;
{
    const r = run({ items: items(12), selectedIndex: 0 });
    if (r.labels[0] !== "Item 1")
        fail("12-item list at index 0: window should start at Item 1, got " + r.labels[0]);
    N = r.labels.length;
    const expectedN = Math.floor((LIST_INDICATOR_BOTTOM_Y - LIST_TOP_Y) / LIST_LINE_HEIGHT);
    if (N !== expectedN)
        fail("no-footer window size: expected the derived " + expectedN + ", got " + N);
    if (r.selectedIndex !== 0)
        fail("12-item list at index 0: expected row 0 selected, got row " + r.selectedIndex);
}
{
    const r = run({ items: items(12), selectedIndex: 0, listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y } });
    if (r.labels[0] !== "Item 1")
        fail("with-footer window at index 0: window should start at Item 1, got " + r.labels[0]);
    footerN = r.labels.length;
    const expectedFooterN = Math.floor((FOOTER_RULE_Y - LIST_TOP_Y) / LIST_LINE_HEIGHT);
    if (footerN !== expectedFooterN)
        fail("with-footer window size: expected the derived " + expectedFooterN + ", got " + footerN);
    if (r.selectedIndex !== 0)
        fail("with-footer window at index 0: expected row 0 selected, got row " + r.selectedIndex);
}
/* The literal bar, stated once for both windows. ONE rect means one number. */
const REQUIRED_ROWS = 5;
if (N !== REQUIRED_ROWS)
    fail("no-footer window must hold " + REQUIRED_ROWS + " rows, got " + N);
if (footerN !== REQUIRED_ROWS)
    fail("with-footer window must hold " + REQUIRED_ROWS + " rows, got " + footerN +
         " -- the one rect (10..55) fits five; a smaller number means the footer " +
         "is reserving space again, which is the row the re-skin bought back");

/* ---- 4. 12-item list at index 11: Item 12 is visible AND selected ----
 *
 * keepOffLastRow (default true) only comes into play once a list is longer
 * than the window: maxSelectedRow = effectiveMaxVisible - 2 pins the
 * selection to the second-to-last row instead of letting it reach the final
 * one, and startIdx shifts so the selection lands there. The consequence:
 * whenever selectedIndex is the LAST item of ANY list longer than the
 * window, there is nothing left to fill the row after it, so the window
 * drawn is N-1 items, not N -- true here (n=12, sel=11) and equally true for,
 * say, n=13, sel=12. This is NOT a property of keepOffLastRow in general: a
 * list that already fits inside the window (assertion 2 above, n=3, sel=2)
 * never triggers this shift, and its last item is drawn on the actual last
 * row, selected -- "the selection is never drawn on the bottom row" is only
 * true of the scrolled case.
 *
 * Arithmetic for this case: startIdx = selectedIndex -
 * (effectiveMaxVisible - 2) = 11 - 3 = 8, endIdx = min(8+5, 12) = 12, which
 * spans only items 8..11 (4 items when N=5). Item 12 is visible and is the
 * selected row, just not the last row of the nominal 5-row window.
 *
 * (The labels.length === N-1 check below is an additional pin beyond what
 * the task strictly required -- kept because it is real, verified behaviour,
 * not because the task asked for a window-size assertion at this index.) */
{
    const r = run({ items: items(12), selectedIndex: 11 });
    if (r.labels[r.labels.length - 1] !== "Item 12")
        fail("12-item list at index 11: Item 12 should be the last visible label, got " + r.labels[r.labels.length - 1]);
    if (r.labels[r.selectedIndex] !== "Item 12")
        fail("12-item list at index 11: the selected row should be Item 12, got " + r.labels[r.selectedIndex]);
    if (r.labels.length !== N - 1)
        fail("12-item list at index 11: expected the keepOffLastRow boundary window of " + (N - 1) + " items, got " + r.labels.length);
}

/* ---- 5. 12-item list at index 6: window is still N rows (size stable mid-scroll) ---- */
{
    const r = run({ items: items(12), selectedIndex: 6 });
    if (r.labels.length !== N)
        fail("12-item list at index 6: expected window size " + N + ", got " + r.labels.length);
    if (r.labels[r.selectedIndex] !== "Item 7")
        fail("12-item list at index 6: expected Item 7 selected, got " + r.labels[r.selectedIndex]);
}

/* ---- 6. empty value renders no value text; action item renders no value ---- */
{
    const withEmpty = [{ label: "Has Value", value: "42" }, { label: "Empty Value", value: "" }];
    const r = run({ items: withEmpty, selectedIndex: 0, getValue: (it) => it.value });
    if (r.values[0] !== "42")
        fail("item with a value: expected value text 42, got " + JSON.stringify(r.values[0]));
    if (r.values[1] !== "")
        fail("item with an empty value: expected no value text, got " + JSON.stringify(r.values[1]));

    /* Action item: genuinely no getValue key passed to drawMenuList, not a
     * no-op function -- exercises the getValue-absent branch in
     * menu_layout.mjs: `getValue ? getValue(item, i) : ""`. */
    const r2 = run({ items: [{ label: "Do Thing" }], selectedIndex: 0, omitGetValue: true });
    if (r2.values[0] !== "")
        fail("action item: expected no value text, got " + JSON.stringify(r2.values[0]));
}

/* ---- 7. editMode signals brackets on the selected row only, without
 *          changing which items are visible ---- */
{
    const withValue = [{ label: "A", value: "on" }, { label: "B", value: "off" }];
    const getValue = (it) => it.value;

    const rOff = run({ items: withValue, selectedIndex: 0, getValue, editMode: false });
    if (rOff.editBrackets)
        fail("editMode false: expected no [bracketed] value, got " + JSON.stringify(rOff.values));
    if (rOff.labels.join(",") !== "A,B")
        fail("editMode false: visible items changed unexpectedly: " + rOff.labels.join(","));

    const rOn = run({ items: withValue, selectedIndex: 0, getValue, editMode: true });
    if (rOn.values[0] !== "[on]")
        fail("editMode true: expected [on] on the selected row, got " + JSON.stringify(rOn.values[0]));
    if (rOn.values[1] !== "off")
        fail("editMode true: the non-selected row should not gain brackets, got " + JSON.stringify(rOn.values[1]));
    if (rOn.labels.join(",") !== "A,B")
        fail("editMode true: visible items changed unexpectedly: " + rOn.labels.join(","));
}

if (failures) process.exit(1);
console.log("PASS: list behaviour contract -- items, order, selection, scroll boundary (no-footer N=" + N + ", with-footer N=" + footerN + "), value text and edit mode all hold against todays drawMenuList");
'

# ---------------------------------------------------------------------------
# THE BUDGET IS MEASURED, NOT chars * 6.
#
# The block above runs through list_probe.mjs, which installs a MONOSPACE
# text_width stand-in (length * 6) on purpose -- it pins behaviour and must not
# see chrome. That is exactly why it could not catch this: drawMenuList budgeted
# label and value width at a flat 6px per glyph while the device font is
# proportional ('!' is 2px, 'i'/'l' are 4, only the widest 78 glyphs are 6), so
# it reserved the WIDEST glyph for every character and truncated text that fits.
#
# This block therefore uses the real font atlas from the harness. It is still
# not an x/y assertion: it asserts WHAT TEXT comes out.
node --input-type=module -e '
const R = process.cwd();
const H = await import(R + "/tools/param-pages/harness.mjs");
const { drawMenuList } = await import(R + "/src/shared/menu_layout.mjs");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

function draw(opts) {
    const fb = H.createFramebuffer();
    const printed = [];
    globalThis.fill_rect = fb.fillRect;
    globalThis.set_pixel = fb.setPixel;
    globalThis.text_width = fb.textWidth;
    globalThis.print = (x, y, t, c) => { printed.push(String(t)); return fb.print(x, y, t, c); };
    drawMenuList(Object.assign({ announce: false }, opts));
    return { printed, clipped: fb.clipped(), width: fb.textWidth };
}

/* "Braids" is 34px in the device font and 36px under the 6px assumption. The
 * unselected value column here is exactly 34px wide (valueX 92 .. 126), so the
 * assumption ellipsised it to "Br..." and the measurement does not. This is the
 * literal regression, from tools/param-pages/preview_list.mjs 02-edit-mode. */
{
    const r = draw({
        items: [{ l: "Patch:", v: "Braids" }, { l: "Volume:", v: "100%" }],
        selectedIndex: 1,
        getLabel: (i) => i.l, getValue: (i) => i.v,
        valueAlignRight: true, editMode: true,
    });
    if (!r.printed.includes("Braids"))
        fail("a value that MEASURES inside its column was truncated: " + JSON.stringify(r.printed));
    if (r.clipped !== 0)
        fail("edit-mode brackets ran off the display: clipped=" + r.clipped);
    if (!r.printed.includes("[100%]"))
        fail("the selected row lost its edit brackets: " + JSON.stringify(r.printed));
}

/* And the label column. A name of narrow glyphs must not be cut at the count a
 * flat 6px would allow; a name of wide glyphs still must be. */
{
    const narrow = "Illlillilli Filter";      /* 18 chars, mostly 4px */
    const r = draw({
        items: [{ l: narrow }], selectedIndex: -1,
        getLabel: (i) => i.l,
        indicatorX: 120, labelX: 8,
    });
    const shown = r.printed[0].replace(/^\s+/, "");
    if (shown !== narrow)
        fail("a narrow-glyph label that fits was truncated to " + JSON.stringify(shown));
    if (r.clipped !== 0)
        fail("the label overran the display: clipped=" + r.clipped);
}

if (failures) process.exit(1);
console.log("PASS: label and value budgets measure the proportional font, brackets included");
'
