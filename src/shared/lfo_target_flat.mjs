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

/*
 * A divider names the POSITION as well as the module: two Freeverbs in FX 1
 * and FX 2 must not read the same, and "Mini-JV" alone does not say it is the
 * synth. The position is the short tag the Target cell's square already
 * uses (SYN, FX1, MF1); LFO 2 and Sends are positions with no module.
 */
function positionTag(key) {
    const k = String(key || "");
    let m;
    if (k === "synth") return "SYN";
    if ((m = /^fx(\d+)$/.exec(k))) return "FX" + m[1];
    if ((m = /^midi_fx(\d+)$/.exec(k))) return "MF" + m[1];
    return "";
}
function moduleName(label) {
    const s = String(label || "");
    const at = s.indexOf(": ");
    return at < 0 ? s : s.slice(at + 2);
}

/** A caption that cannot fit is cut, measured, with a trailing dot. */
export function fitDividerLabel(label, measure, maxW) {
    let t = String(label || "");
    if (typeof measure !== "function" || !(maxW > 0) || measure(t) <= maxW) return t;
    while (t.length > 1 && measure(t + ".") > maxW) t = t.slice(0, -1);
    return t.replace(/[\s\-_/.:]+$/, "") + ".";
}

/*
 * TWO levels of divider in one flat list: the MODULE, with its position
 * ("SYN Mini-JV", "FX1 Freeverb", "LFO 2"), then each of its SECTIONS
 * ("Filter", "Envelope") when it has them. One line for both did not fit --
 * the device font holds ~16 characters, and a long section name cut every
 * module to "Min." -- and the position is the one fact a flat list needs
 * most: two Freeverbs must not read the same.
 */
export function buildFlatTargetRows(comps, sectionsOf, { measure, maxW } = {}) {
    const rows = [{ label: "None", route: { target: "", param: "" } }];
    (comps || []).forEach((c, i) => {
        if (!c || !c.key || c.key === CLEAR_KEY) return;
        const secs = (sectionsOf(i) || []).filter((x) => x && x.params && x.params.length);
        if (!secs.length) return;
        const tag = positionTag(c.key);
        const head = tag ? tag + " " + moduleName(c.label || c.key) : String(c.label || c.key);
        rows.push({ type: "divider", label: fitDividerLabel(head, measure, maxW), level: 0 });
        for (const sec of secs) {
            /* drawMenuList indents a level-1 caption 10 px, so it has 10 px less room. */
            if (sec.label) rows.push({ type: "divider", label: fitDividerLabel(sec.label, measure, maxW > 10 ? maxW - 10 : maxW), level: 1 });
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

/*
 * OPENING THE LIST MUST NOT BECOME A DECISION.
 *
 * The list opens on the stored routing and a release commits the row under
 * the cursor, so wherever the cursor LANDS is what a one-detent brush writes.
 * Three ways it used to land on "None" for a live LFO: the target read
 * failed (null), the component's section read timed out (its rows are
 * missing), or the route is simply not offered. The release then wrote
 * target="" and enabled=0 -- a working LFO switched off by touching a knob.
 *
 *   null read          -> null: refuse to open; nothing is known to show
 *   no routing ("")    -> stored on None
 *   listed             -> stored on its row
 *   NOT listed         -> a "Current" row is added right after None, holding
 *                         the stored route, and the cursor sits on it
 */
export function planFlatTargetOpen(rows, target, param, { measure, maxW } = {}) {
    if (target === null || target === undefined || param === null || param === undefined) return null;
    const base = rows || [];
    if (!target || !param) return { rows: base, stored: 0 };
    const i = base.findIndex((r) => r && r.route && r.route.target === target && r.route.param === param);
    if (i >= 0) return { rows: base, stored: i };
    const tag = positionTag(target) || String(target);
    const out = base.slice(0, 1).concat([
        { type: "divider", label: fitDividerLabel("Current", measure, maxW), level: 0 },
        { label: tag + " " + param, route: { target: String(target), param: String(param) }, unlisted: true },
    ], base.slice(1));
    return { rows: out, stored: 2 };
}

/*
 * The row a release/click should commit, or null for "write nothing".
 * A cursor still on the stored row is not a choice: re-writing the same
 * routing resets the chain's modulation base (and forces enabled=1 on an LFO
 * the list had switched off), so it is skipped -- as the file list does.
 */
export function flatTargetCommitRow(rows, index, stored) {
    if (index === stored) return null;
    const r = (rows || [])[index];
    return r && r.route ? r : null;
}
