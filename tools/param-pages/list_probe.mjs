/*
 * Records WHAT a list draw said, not where it put it.
 *
 * The one-list work deliberately changes every list's chrome, so a probe that
 * captured x/y would pin the thing being changed. What must survive is the
 * behaviour: which items are visible, in what order, which is selected, what
 * values they carry, whether edit mode was signalled.
 */
export function probe(drawFn) {
    const rows = [];
    const prev = {
        print: globalThis.print,
        fill_rect: globalThis.fill_rect,
        set_pixel: globalThis.set_pixel,
        clear_screen: globalThis.clear_screen,
        text_width: globalThis.text_width,
    };
    let fills = [];
    globalThis.clear_screen = () => { rows.length = 0; fills = []; };
    globalThis.set_pixel = () => {};
    globalThis.text_width = (t) => String(t).length * 6;
    globalThis.fill_rect = (x, y, w, h) => { fills.push({ x, y, w, h }); };
    globalThis.print = (x, y, text, color) => {
        rows.push({ y, x, text: String(text), inverted: color === 0 });
    };
    try { drawFn(); } finally { Object.assign(globalThis, prev); }

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
            inverted: cells.some((c) => c.inverted),
        };
    });
    return {
        rows: ordered,
        labels: ordered.map((r) => r.label.replace(/^[>*]?\s*/, "").trim()),
        values: ordered.map((r) => r.value),
        selectedIndex: ordered.findIndex((r) => r.inverted),
        editBrackets: ordered.some((r) => /^\[.*\]$/.test(r.value)),
        fillCount: fills.length,
    };
}
