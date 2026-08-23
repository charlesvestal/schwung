#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE LABEL FLOOR — every row gets one, not just the selected row.
#
# drawMenuList used to lay the VALUE out first, at its full measured width, and
# give the label whatever was left. On a full-width list that is invisible (the
# value column has a floor at x=92 anyway); on a page-chrome list, which hands
# the floor to its own left edge so the label budget does the work, it produced
# rows that convey nothing. Rendered from breakbeat's real contract:
#
#     A...        kick_01.wav
#     A Le...       1/2 bar
#     B...        kick_01.wav
#     B Le...        4 bars
#
# Two rows reading `kick_01.wav`, labelled `A...` and `B...`. The user cannot
# tell which sample is which — the one thing the screen exists to say.
#
# `selectedMinLabelChars` already existed but rescued only the SELECTED row.
# `minLabelChars` (default 8) is the general floor: the value may not push the
# label below the width of its own first N characters, MEASURED, and when the
# two compete the value truncates instead.
#
# WHY THIS FILE IS NOT VACUOUS. Assertion 2 draws the same fixture with the
# floor explicitly switched OFF and requires the label to be crushed. So the two
# halves fail in opposite directions: mutate the DEFAULT to 0 and assertion 1
# fails; make the floor unconditional (ignore the option) and assertion 2 fails.
# Neither can be satisfied by a renderer that simply does nothing.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
const { drawMenuList } = await import(R + "/src/shared/menu_layout.mjs");
const { drawPageChromeList } = await import(R + "/src/shared/param_pages/page_controller.mjs");
const { createFramebuffer } = await import(R + "/tools/param-pages/harness.mjs");

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* breakbeats real first knobs page: two samples that differ only in their
 * LABEL, which is exactly the case the old budget destroyed. */
const ROWS = [
    { name: "A Sample", value: "kick_01_long.wav" },
    { name: "A Length", value: "1/2 bar triplet" },
    { name: "B Sample", value: "kick_01_long.wav" },
    { name: "B Length", value: "4 bars looped" },
    { name: "B Chance", value: "63 percent" },
    /* A sixth entry in a five-row rect, so a scroll arrow IS on screen. It no
     * longer widens the defect for every row — the clearance is charged per
     * row now, and only the first and last visible rows sit under an arrow —
     * but it keeps this fixture rendering the same five-of-six window the
     * screenshot above was taken in. */
    { name: "Complexity", value: "63" },
];
/* The filenames are long ON PURPOSE. With the arrow clearance charged per row
 * (see menu_layout.mjs) a middle row gets the full 118px, and at the old
 * `kick_01.wav` the floor stopped biting there at all — assertion 5, the
 * vacuity guard in this file, caught that immediately and refused to let
 * assertion 1 pass on a fixture that no longer reproduces anything. Long
 * enough to crush an eight-character label at 118px is the property this
 * fixture has to keep, not any particular filename. */
const VISIBLE = 5;
/* The page-chrome list rect, i.e. the geometry drawPageChromeList renders in. */
const RECT = { x: 8, y: 10, w: 112, h: 45 };

/* Rows as DRAWN: the strings that reached print(), left to right, per row. */
function render(draw) {
    const fb = createFramebuffer();
    const cells = [];
    const ctx = {
        fillRect: fb.fillRect,
        print: (x, y, t, c) => { cells.push({ x, y, t: String(t) }); fb.print(x, y, t, c); },
        setPixel: fb.setPixel,
        textWidth: fb.textWidth,
    };
    draw(ctx);
    const byRow = new Map();
    for (const c of cells) {
        if (!byRow.has(c.y)) byRow.set(c.y, []);
        byRow.get(c.y).push(c);
    }
    const rows = [...byRow.entries()].sort((a, b) => a[0] - b[0]).map(([, cs]) => {
        cs.sort((a, b) => a.x - b.x);
        /* The cursor prefix is chrome, not label. */
        const last = cs[cs.length - 1];
        return {
            label: (cs[0] ? cs[0].t : "").replace(/^[^A-Za-z0-9]+/, ""),
            value: cs.length > 1 ? last.t : "",
            /* Where the value ENDS. valueAlignRight promises one edge for every
             * row; assertion 6 checks a truncated value still reaches it. */
            valueRight: cs.length > 1 ? last.x + fb.textWidth(last.t) : -1,
        };
    });
    return { rows, clipped: fb.clipped(), width: fb.textWidth };
}

/* The real call site, with NO options of its own — so assertion 1 is measuring
 * the shipped default and not a value this file chose. */
const chrome = (index = 0) =>
    render((ctx) => drawPageChromeList(ctx, RECT, ROWS, index));

/* The same geometry with the floor switched off: drawPageChromeLists own
 * arguments, restated here only because it exposes no way to pass one through.
 * Kept in sync by assertion 5, which requires the two to agree wherever the
 * floor does not bite. */
const withFloor = (minLabelChars, index = 0) => render((ctx) => drawMenuList({
    ctx, items: ROWS, selectedIndex: index,
    listArea: { topY: RECT.y, bottomY: RECT.y + RECT.h },
    labelX: RECT.x, indicatorX: 110, indicatorBottomY: RECT.y + RECT.h,
    getLabel: (e) => e.name, getValue: (e) => e.value,
    valueAlignRight: true, valueX: RECT.x, valuePaddingRight: 10,
    announce: false, minLabelChars,
}));

/* ---- 1. EVERY ROW keeps a readable label, through the real call site ----- */
{
    const { rows, clipped } = chrome();
    if (rows.length !== VISIBLE) fail(`${rows.length} rows drawn, want ${VISIBLE}`);
    for (let i = 0; i < VISIBLE; i++) {
        if (!rows[i]) { fail(`row ${i} was not drawn at all`); continue; }
        if (rows[i].label !== ROWS[i].name) {
            fail(`row ${i} label is ${JSON.stringify(rows[i].label)}, `
                 + `want the whole ${JSON.stringify(ROWS[i].name)} — `
                 + `an 8-character name fits the floor`);
        }
    }
    /* THE ACCEPTANCE CASE: the two sample rows must be told apart. */
    if (rows[0] && rows[2] && rows[0].label === rows[2].label) {
        fail(`the two sample rows are indistinguishable: both read `
             + `${JSON.stringify(rows[0].label)}`);
    }
    /* And the value that had to give way is still recognisable, still on the
     * screen, and not printed over the label. */
    if (!rows[0] || !/^kick/.test(rows[0].value)) {
        fail(`row 0 value is ${JSON.stringify(rows[0] && rows[0].value)}, `
             + `want a truncation still beginning "kick"`);
    }
    if (rows[0] && rows[0].value === "kick_01.wav") {
        fail("row 0 value was NOT truncated — the label cannot have been given "
             + "its floor, or the geometry under test is not the page-chrome one");
    }
    if (clipped !== 0) fail(`${clipped} pixels fell outside the display`);
}

/* ---- 2. NON-VACUITY: with the floor off, the label IS crushed ------------
 * Also the tunability pin — an option that cannot be switched off is not one. */
{
    const { rows } = withFloor(0);
    const crushed = rows.filter((r, i) => ROWS[i] && r.label !== ROWS[i].name).length;
    if (crushed < 3) {
        fail(`minLabelChars: 0 kept ${VISIBLE - crushed} of ${VISIBLE} labels intact `
             + `— the floor is not switchable, so assertion 1 proves nothing`);
    }
    /* The reported row, verbatim: `B Sample` against `kick_01.wav` came out
     * `B...`, which names nothing. */
    if (!rows[2] || rows[2].label.length > 4) {
        fail(`with the floor off row 2 reads ${JSON.stringify(rows[2] && rows[2].label)}; `
             + `this fixture no longer reproduces the defect and assertion 1 is vacuous`);
    }
}

/* ---- 3. The floor is MEASURED, not 6px per glyph ------------------------- */
{
    /* Eight narrow glyphs measure far less than eight wide ones. If the
     * reservation were `8 * 6`, the narrow label would take room it cannot use
     * and the value would pay for it — so the value must come out WIDER here. */
    const narrow = [{ name: "lililili filter", value: "Sample & Hold" }];
    const wide   = [{ name: "MWMWMWMW filter", value: "Sample & Hold" }];
    const draw = (items) => render((ctx) => drawMenuList({
        ctx, items, selectedIndex: -1,
        listArea: { topY: RECT.y, bottomY: RECT.y + RECT.h },
        labelX: RECT.x, indicatorX: 110, indicatorBottomY: RECT.y + RECT.h,
        getLabel: (e) => e.name, getValue: (e) => e.value,
        valueAlignRight: true, valueX: RECT.x, valuePaddingRight: 10,
        announce: false,
    }));
    const n = draw(narrow).rows[0], w = draw(wide).rows[0];
    if (!(n.value.length > w.value.length)) {
        fail(`a narrow 8-glyph label left ${JSON.stringify(n.value)} and a wide one `
             + `${JSON.stringify(w.value)} — the floor is reserving a fixed `
             + `characters * ${6} rather than measuring the glyphs`);
    }
}

/* ---- 4. A short label costs the value NOTHING ---------------------------- */
{
    const SHORT = [{ name: "Cutoff", value: "60" },
                   { name: "Decay",  value: "-29.45" }];
    const draw = (opts) => render((ctx) => drawMenuList({
        ctx, items: SHORT, selectedIndex: -1,
        listArea: { topY: RECT.y, bottomY: RECT.y + RECT.h },
        labelX: RECT.x, indicatorX: 110, indicatorBottomY: RECT.y + RECT.h,
        getLabel: (e) => e.name, getValue: (e) => e.value,
        valueAlignRight: true, valueX: RECT.x, valuePaddingRight: 10,
        announce: false, ...opts,
    }));
    const on = JSON.stringify(draw({}).rows);
    const off = JSON.stringify(draw({ minLabelChars: 0 }).rows);
    if (on !== off) {
        fail(`the floor perturbed a row whose label already fits:\n  on  ${on}\n  off ${off}`);
    }
}

/* ---- 5. The marquee still engages for a label that does not fit its floor -
 * More labels are now truncated-but-readable rather than scrolled, so the risk
 * is the opposite one: that nothing scrolls any more. */
{
    const LONG = [{ name: "Filter 1 Cutoff Frequency", value: "-29.45" }];
    const seen = new Set();
    for (let f = 0; f < 120; f++) {
        const { rows } = render((ctx) => drawMenuList({
            ctx, items: LONG, selectedIndex: 0,
            listArea: { topY: RECT.y, bottomY: RECT.y + RECT.h },
            labelX: RECT.x, indicatorX: 110, indicatorBottomY: RECT.y + RECT.h,
            getLabel: (e) => e.name, getValue: (e) => e.value,
            valueAlignRight: true, valueX: RECT.x, valuePaddingRight: 10,
            announce: false,
        }));
        seen.add(rows[0].label);
    }
    if (seen.size < 2) {
        fail(`the selected label never scrolled over 120 draws (always `
             + `${JSON.stringify([...seen][0])}) — the marquee stopped engaging`);
    }
}

/* ---- 6. A TRUNCATED VALUE IS STILL RIGHT-ALIGNED -------------------------
 *
 * valueAlignRight promises one right edge for every row. The value used to be
 * POSITIONED at its full measured width, clamped up to the label floor, and
 * only then cut down to what fit from there -- while still being drawn at the
 * original x. So a cut value stopped short of the edge and left a hole, and the
 * label was budgeted against a value that no longer reached that far. Both
 * columns paid for space neither used:
 *
 *     "  Move..."  45px  +  "Schwu..."  38px  =  83px of 118
 *
 * Neither existing assertion could see it: the label floor was honoured (the
 * label just did not spend its whole reservation) and the value did fit. The
 * 99 chain-editor pixel cases missed it too -- none of them has a row where
 * BOTH columns truncate.
 */
{
    const LONG = [
        { name: "Move Resample", value: "Schwung Mix" },   /* both truncate */
        { name: "Skipback Key",  value: "Sh+Cap" },
        { name: "Latency Comp",  value: "On" },            /* neither does */
    ];
    const { rows } = render((ctx) => drawMenuList({
        ctx, items: LONG, selectedIndex: -1,
        listArea: { topY: RECT.y, bottomY: RECT.y + RECT.h },
        labelX: RECT.x, indicatorX: 110, indicatorBottomY: RECT.y + RECT.h,
        getLabel: (e) => e.name, getValue: (e) => e.value,
        valueAlignRight: true, valueX: RECT.x, valuePaddingRight: 10,
        announce: false,
    }));

    const edges = rows.map((r) => r.valueRight).filter((x) => x >= 0);
    const want = Math.max(...edges);
    for (let i = 0; i < rows.length; i++) {
        if (rows[i].valueRight < 0) continue;
        if (rows[i].valueRight !== want) {
            fail(`row ${i} (${JSON.stringify(rows[i].label)} / `
                 + `${JSON.stringify(rows[i].value)}) ends its value at x=`
                 + `${rows[i].valueRight}, but the column edge is x=${want} -- a `
                 + `truncated value must still be right-aligned, or it leaves a `
                 + `hole and the label is budgeted against space the value gave up`);
        }
    }

}

if (failures) { console.error(`${failures} failure(s)`); process.exit(1); }
console.log("PASS: every row keeps a label floor; the value truncates when the two compete");
'
