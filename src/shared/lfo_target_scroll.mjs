/**
 * lfo_target_scroll.mjs — the LFO target picker walked as ONE list.
 *
 * The picker is a hierarchy: component > section > param, walked by the jog
 * (click in, Back out). A knob walks the SAME menus as if they were flat:
 * every param of every section of every component in the picker's own order,
 * crossing from one section's screen into the next when a list runs out, with
 * [Clear Target] (the "__clear__" component) a stop of its own.
 *
 * PURE: positions in, positions out. The host owns the reads (sectionsOf) and
 * turns a position back into its picker state and screen.
 *
 *   pos = { comp, sec, param }   sec/param are -1 on a component with no
 *                                params of its own (__clear__)
 *   sectionsOf(compIndex) -> [{ label, params: [{key, label}] }]
 *                                non-empty sections only; [] = nothing to
 *                                offer, and the component is skipped
 */

export const CLEAR_KEY = "__clear__";

/** A leaf of component `c`: "first", "last", or the param keyed `stored`. */
export function landOn(comps, sectionsOf, c, where, stored) {
    const comp = comps && comps[c];
    if (!comp) return null;
    if (comp.key === CLEAR_KEY) return { comp: c, sec: -1, param: -1 };
    const sections = sectionsOf(c) || [];
    if (!sections.length) return null;
    if (where === "stored" && stored) {
        for (let s = 0; s < sections.length; s++) {
            const p = sections[s].params.findIndex((x) => x && x.key === stored);
            if (p >= 0) return { comp: c, sec: s, param: p };
        }
    }
    if (where === "last") {
        const s = sections.length - 1;
        return { comp: c, sec: s, param: sections[s].params.length - 1 };
    }
    return { comp: c, sec: 0, param: 0 };
}

/** One item forward (dir > 0) or back. Null at either end. */
export function stepOne(comps, sectionsOf, pos, dir) {
    if (!pos) return null;
    const d = dir > 0 ? 1 : -1;
    if (pos.sec >= 0) {
        const sections = sectionsOf(pos.comp) || [];
        const sec = sections[pos.sec];
        if (sec) {
            const p = pos.param + d;
            if (p >= 0 && p < sec.params.length) return { comp: pos.comp, sec: pos.sec, param: p };
            const s = pos.sec + d;
            if (s >= 0 && s < sections.length) {
                return { comp: pos.comp, sec: s, param: d > 0 ? 0 : sections[s].params.length - 1 };
            }
        }
    }
    for (let c = pos.comp + d; c >= 0 && c < (comps || []).length; c += d) {
        const at = landOn(comps, sectionsOf, c, d > 0 ? "first" : "last");
        if (at) return at;
    }
    return null;
}

/** `n` items (signed), stopping at an end. Returns the last reachable pos. */
export function step(comps, sectionsOf, pos, n) {
    let at = pos;
    const d = n > 0 ? 1 : -1;
    for (let i = Math.abs(n); i > 0; i--) {
        const next = stepOne(comps, sectionsOf, at, d);
        if (!next) break;
        at = next;
    }
    return at;
}
