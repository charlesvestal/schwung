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

node --input-type=module -e '
const R = process.cwd();
const { drawMenuList } = await import(R + "/src/shared/menu_layout.mjs");
const { probe } = await import(R + "/tools/param-pages/list_probe.mjs");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const items = (n) => Array.from({ length: n }, (_, i) => ({ label: "Item " + (i + 1) }));

const run = (opts) => probe(() => drawMenuList({
    items: opts.items,
    selectedIndex: opts.selectedIndex,
    getLabel: (item) => item.label,
    getValue: opts.getValue || ((item) => item.value || ""),
    announce: false,
    editMode: opts.editMode || false,
}));

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

/* ---- 3. 12-item list at index 0: window starts at Item 1; record window size N ---- */
let N;
{
    const r = run({ items: items(12), selectedIndex: 0 });
    if (r.labels[0] !== "Item 1")
        fail("12-item list at index 0: window should start at Item 1, got " + r.labels[0]);
    N = r.labels.length;
    if (r.selectedIndex !== 0)
        fail("12-item list at index 0: expected row 0 selected, got row " + r.selectedIndex);
}

/* ---- 4. 12-item list at index 11: Item 12 is visible AND selected ----
 *
 * keepOffLastRow (default true) reserves the bottom visual row so the
 * selection is never drawn on it. Near the end of a list that is one item
 * short of a full window, that reservation is satisfied by the ABSENCE of a
 * 13th item rather than by leaving a populated row blank -- so the window at
 * this boundary is genuinely N-1 items, not N. That is real behaviour of
 * todays drawMenuList, not a probe artifact: startIdx = selectedIndex -
 * (effectiveMaxVisible - 2) = 11 - 3 = 8, and endIdx = min(8+5, 12) = 12,
 * which spans only items 8..11 (4 items when N=5). Item 12 is visible and is
 * the selected row, just not the last row of the nominal window. */
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

    /* Action item: no getValue at all (the default in run() returns ""). */
    const r2 = run({ items: [{ label: "Do Thing" }], selectedIndex: 0, getValue: undefined });
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
console.log("PASS: list behaviour contract -- items, order, selection, scroll boundary (N=" + N + "), value text and edit mode all hold against todays drawMenuList");
'
