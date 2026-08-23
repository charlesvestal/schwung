/*
 * Records WHAT a list draw said, not where it put it.
 *
 * The one-list work deliberately changes every list's chrome, so a probe that
 * captured x/y would pin the thing being changed. What must survive is the
 * behaviour: which items are visible, in what order, which is selected, what
 * values they carry, whether edit mode was signalled.
 *
 * Selection detection is deliberately redundant, for the same reason.
 * `drawMenuList` marks the selected row with BOTH inverted text (color === 0)
 * AND a fill_rect spanning the row -- two independent chrome signals for one
 * behaviour fact. A re-skin is free to change either mechanism on its own
 * (e.g. drop the inverted-text convention in favour of a marker glyph, or
 * stop filling the full row), so a row counts as selected if EITHER signal
 * is present. Detecting on only one would make this test couple to a chrome
 * decision it has no business caring about, and fail on a correct re-skin --
 * exactly the failure mode this file exists to avoid.
 *
 * The value column is NOT given the same treatment: "the value is the last
 * print() cell on the row" is still a real assumption about today's chrome
 * (one label print + one value print per row, same y). That is a known,
 * accepted limitation rather than a solved one -- an extra print() on the
 * row (e.g. a right-hand glyph) or a value baseline that no longer shares
 * the label's exact y would break this grouping. If a Task 2 assertion about
 * `values` fails, check this grouping assumption before assuming a real
 * regression in the re-skinned list.
 */
export function probe(drawFn) {
    const rows = [];
    const fills = [];
    const prev = {
        print: globalThis.print,
        fill_rect: globalThis.fill_rect,
        set_pixel: globalThis.set_pixel,
        clear_screen: globalThis.clear_screen,
        text_width: globalThis.text_width,
    };
    globalThis.clear_screen = () => { rows.length = 0; fills.length = 0; };
    globalThis.set_pixel = () => {};
    globalThis.text_width = (t) => String(t).length * 6;
    globalThis.fill_rect = (x, y, w, h) => { fills.push({ x, y, w, h }); };
    globalThis.print = (x, y, text, color) => {
        rows.push({ y, x, text: String(text), inverted: color === 0 });
    };
    try { drawFn(); } finally { Object.assign(globalThis, prev); }

    /* A row's y is highlighted if any fill_rect's vertical span covers it --
     * catches a highlight drawn as a filled band even if the re-skin stops
     * inverting the text color. */
    const rowFilled = (y) => fills.some((f) => y >= f.y && y < f.y + f.h);

    /* Group by row (same y), left-to-right: label first, value second. */
    const byY = new Map();
    for (const r of rows) {
        if (!byY.has(r.y)) byY.set(r.y, []);
        byY.get(r.y).push(r);
    }
    const ordered = [...byY.keys()].sort((a, b) => a - b).map((y) => {
        const cells = byY.get(y).sort((a, b) => a.x - b.x);
        return {
            y,
            label: (cells[0] && cells[0].text) || "",
            value: cells.length > 1 ? cells[cells.length - 1].text : "",
            selected: cells.some((c) => c.inverted) || rowFilled(y),
        };
    });
    return {
        rows: ordered,
        labels: ordered.map((r) => r.label.replace(/^[>*]?\s*/, "").trim()),
        values: ordered.map((r) => r.value),
        selectedIndex: ordered.findIndex((r) => r.selected),
        editBrackets: ordered.some((r) => /^\[.*\]$/.test(r.value)),
    };
}
