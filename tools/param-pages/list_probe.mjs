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
 * behaviour fact, so a row counts as selected if EITHER is present. That
 * specifically survives a re-skin dropping ONE of those two mechanisms (e.g.
 * a highlight rect with no color inversion, or vice versa). It does NOT make
 * every chrome decision invisible to this probe -- assertion 7 pins the
 * `[value]` bracket convention for edit mode literally, because that
 * convention (not merely "edit mode is signalled somehow") is what the task
 * asked this file to protect.
 *
 * Two more limitations worth naming rather than discovering by surprise:
 *
 * - The label marker prefix ("> " selected / "  " unselected today) is
 *   stripped by treating any leading run of non-alphanumeric characters as
 *   chrome, not by matching today's literal characters -- so a re-skin that
 *   swaps the marker glyph (e.g. "> " -> "» ") does not require a probe
 *   change. A label that legitimately starts with punctuation would be
 *   mis-stripped; none of the fixtures here do.
 *
 * - `selectedIndex` and the entries in `labels`/`values` are ROW indices,
 *   not ITEM indices, whenever `getSubLabel` or a `type: 'divider'` entry is
 *   in play -- both draw an extra print() at a distinct y, which this probe
 *   has no way to distinguish from a second list row. Both are live in real
 *   menus (the file browser uses sub-labels; several Tools screens use
 *   dividers), so a caller of this probe against such a list must not assume
 *   `labels[i]` lines up with `items[i]`. None of the fixtures in
 *   test_list_behavior.sh use either, so this file's own assertions are not
 *   affected -- this is a note for whoever reuses the probe next.
 *
 * `announce: false` is passed by every fixture as a deliberate choice, not
 * because the real code would crash without it: `announceMenuItem` /
 * `announce()` guard on `typeof shadow_get_display_mode` and similar host
 * globals and no-op harmlessly under plain node. It is disabled anyway so a
 * probe run never depends on (or masks a change to) screen-reader behaviour,
 * which is a separate contract from what is drawn.
 */
import { LIST_LINE_HEIGHT } from '../../src/shared/menu_layout.mjs';

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
    globalThis.fill_rect = (x, y, w, h, color) => { fills.push({ x, y, w, h, color }); };
    globalThis.print = (x, y, text, color) => {
        rows.push({ y, x, text: String(text), inverted: color === 0 });
    };
    try { drawFn(); } finally { Object.assign(globalThis, prev); }

    /* A row's y is highlighted if a NON-CLEARING fill_rect's vertical span
     * covers it. color !== 0 excludes a background clear (drawOverlay /
     * drawStatusOverlay elsewhere in menu_layout.mjs already fill_rect with
     * color 0 to blank an area) -- without that check, any re-skin that
     * clears the list area before drawing would make every row read as
     * selected.
     *
     * The span checked is capped to LIST_LINE_HEIGHT rather than the fill's
     * full height: with a sub-label present, the selected row's highlight
     * rect is taller than one row (it also covers the sub-label line), and
     * its bottom edge can reach into the NEXT item's row y. Capping to one
     * line height keeps the highlight test from flagging that neighbour too.
     * `LIST_LINE_HEIGHT` is imported from the real module rather than
     * hardcoded so this stays anchored to whatever line height the re-skin
     * actually uses. */
    const rowFilled = (y) => fills.some((f) => {
        if (f.color === 0) return false;
        const span = Math.min(f.h, LIST_LINE_HEIGHT);
        return y >= f.y && y < f.y + span;
    });

    /* Group by row (same y), left-to-right: label first, value second. */
    const byY = new Map();
    for (const r of rows) {
        if (!byY.has(r.y)) byY.set(r.y, []);
        byY.get(r.y).push(r);
    }
    const ordered = [...byY.keys()].sort((a, b) => a - b).map((y) => {
        let cells = byY.get(y).sort((a, b) => a.x - b.x);
        /* The selection caret is its OWN print now (menu_layout draws it at
         * labelX and starts the label at labelX + CARET_W, so a row does not
         * shift when you select it). Drop it before reading the label, or the
         * label column would read ">" on every selected row. */
        const isCaret = (t) => /^[>*]$/.test(String(t).trim());
        if (cells.length > 1 && isCaret(cells[0].t !== undefined ? cells[0].t : cells[0].text)) {
            cells = cells.slice(1);
        }
        return {
            y,
            label: (cells[0] && cells[0].text) || "",
            value: cells.length > 1 ? cells[cells.length - 1].text : "",
            selected: cells.some((c) => c.inverted) || rowFilled(y),
        };
    });
    return {
        rows: ordered,
        /* Strip a leading run of non-alphanumeric characters (marker glyph
         * and/or padding spaces) rather than today's literal "> "/"  " --
         * see the file header. */
        labels: ordered.map((r) => r.label.replace(/^[^A-Za-z0-9]+/, "").trim()),
        values: ordered.map((r) => r.value),
        selectedIndex: ordered.findIndex((r) => r.selected),
        editBrackets: ordered.some((r) => /^\[.*\]$/.test(r.value)),
    };
}
