/**
 * lfo_target_flat.mjs — every LFO target as ONE list, with the categories as
 * dividers.
 *
 * The target picker is a hierarchy (component > section > param) and the jog
 * walks it as one: click in, Back out. Turning the grid's Target knob shows
 * the same targets FLAT instead -- one scrolling list, "None" first, then each
 * component's params in the picker's own order, a divider row naming each
 * section ("Mini-JV / Filter", "Freeverb", "LFO 2", "Sends"). The cursor
 * skips dividers, so the knob runs straight through from one category into
 * the next while the dividers say where you are.
 *
 * PURE: the host owns the reads (sectionsOf) and the drawing.
 *
 *   comps       the picker's component list, [{key, label}]; "__clear__" is
 *               left out -- "None" heads the list instead
 *   sectionsOf  (compIndex) -> [{ label|null, params: [{key, label}] }],
 *               non-empty sections only; [] and the component is skipped
 *
 * A row is { type: "divider", label } or { label, route: {target, param} }.
 */

export const CLEAR_KEY = "__clear__";

/* "Synth: Mini-JV" -> "Mini-JV". The kind prefix is what the picker's first
 * step needs; here the divider is naming the module. */
function moduleName(label) {
    const s = String(label || "");
    const at = s.indexOf(": ");
    return at < 0 ? s : s.slice(at + 2);
}

export function buildFlatTargetRows(comps, sectionsOf) {
    const rows = [{ label: "None", route: { target: "", param: "" } }];
    (comps || []).forEach((c, i) => {
        if (!c || !c.key || c.key === CLEAR_KEY) return;
        const name = moduleName(c.label || c.key);
        for (const sec of (sectionsOf(i) || [])) {
            if (!sec || !sec.params || !sec.params.length) continue;
            rows.push({ type: "divider", label: sec.label ? name + " / " + sec.label : name });
            for (const p of sec.params) {
                if (p && p.key) rows.push({ label: p.label || p.key, route: { target: c.key, param: p.key } });
            }
        }
    });
    return rows;
}

const selectable = (r) => r && r.type !== "divider";

/** Move `n` selectable rows (signed) from `index`, stopping at either end. */
export function moveFlatCursor(rows, index, n) {
    let at = index;
    const d = n > 0 ? 1 : -1;
    for (let k = Math.abs(n); k > 0; k--) {
        let j = at + d;
        while (j >= 0 && j < rows.length && !selectable(rows[j])) j += d;
        if (j < 0 || j >= rows.length) break;
        at = j;
    }
    return at;
}

/** The row holding a routing; 0 (None) for none or one not listed. */
export function indexOfFlatRoute(rows, target, param) {
    if (!target || !param) return 0;
    const i = rows.findIndex((r) => r.route && r.route.target === target && r.route.param === param);
    return i >= 0 ? i : 0;
}
