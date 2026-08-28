/**
 * lfo_target_groups.mjs — the LFO target picker's second step.
 *
 * An LFO's target param was chosen from ONE flat list: every modulatable key
 * the component declares, in `chain_params` order. Measured against the
 * 95-module fleet capture, that is 418 rows for minijv, 303 for surge, 250 for
 * forge, 231 for mrdrums — one unbroken jog-scroll, with the module author's
 * own section names ("Patch", "Perf", "Voice", "Mix", "FX") sitting unused in
 * the same `ui_hierarchy` the knob grid pages from. 84 of the 95 publish them.
 *
 * So group by level. Two properties make that safe rather than merely nicer:
 *
 *   NAMED THE SAME. The rows come out of `level_walk.mjs`, which is where the
 *   grid gets its page titles. A group called something other than the page it
 *   corresponds to would defeat the point of grouping at all, and no screen
 *   shows the two side by side, so nothing would ever report the drift.
 *
 *   LOSSLESS. The union of the groups is exactly the flat list — same keys,
 *   same labels, no duplicates. Every modulatable param no level claimed lands
 *   in a trailing "Other". Grouping must never cost you a target that the flat
 *   list offered, because the routing it would have made is still a routing the
 *   DSP would have honoured.
 *
 * PURE: no IPC, no drawing. The caller does both reads and decides when.
 */

import {
    knobKeys, paramKeys, makeLevelWalker, levelNamer,
} from "./param_pages/level_walk.mjs";
import { makeClaimer } from "./param_pages/page_plan.mjs";

/**
 * Param types an LFO can drive. A string or an action has no range for a depth
 * to mean anything against, so it is not offered.
 *
 * `wav_position` is in the list and its absence was expensive: it is a ranged
 * number a knob turns, `chain_mod_emit_value` scales it by the declared range
 * like any other, and leaving it out silently excluded every sample-position
 * param in the fleet (mrdrums Sample Start, granny, mrsample) — which forced
 * those routings onto the concrete per-pad key instead of the alias, and took
 * the whole modulation-indicator chain down with it. See the history in
 * `lfoTargetParamsFor`, which this replaces.
 */
export const MODULATABLE_TYPES = new Set(["float", "int", "enum", "wav_position"]);

/**
 * Below this, grouping is a regression: an extra menu level over six rows
 * costs a click and saves no scrolling. 8 is one knob page.
 */
export const MIN_PARAMS_TO_GROUP = 8;

/** Where params no level claimed go. Always last. */
export const OTHER_GROUP_KEY = "__other__";

/**
 * The flat list, unchanged: every modulatable param a component declares, in
 * `chain_params` order, labelled the way it has always been labelled.
 *
 * Exported because it is the losslessness reference — a test asserts the
 * groups union to exactly this — and because the caller still needs it
 * whenever `grouped` comes back false.
 */
export function flatLfoTargetParams(chainParams) {
    const out = [];
    const seen = new Set();
    for (const p of (Array.isArray(chainParams) ? chainParams : [])) {
        if (!p || !p.key || seen.has(p.key)) continue;
        if (!MODULATABLE_TYPES.has(p.type)) continue;
        seen.add(p.key);
        out.push({ key: p.key, label: p.name || p.label || p.key });
    }
    return out;
}

/**
 * Which level keys the walk should start from.
 *
 * `root` normally. With `modes`, EVERY mode — unlike the grid, this does not
 * gate on the active one. A routing at a mode-inactive param is still a valid
 * routing the DSP will honour, and minijv (which has no `root` at all, and 55
 * levels across two modes) would otherwise reach only half its own tree. The
 * orphan sweep would then relocate those params to "Other", which is a worse
 * answer than naming the level they came from.
 *
 * The picker and the grid therefore disagree about minijv's level list. That
 * is deliberate: the grid is a performance surface and this is a routing menu.
 */
function walkRoots(hierarchy, levels) {
    const modes = Array.isArray(hierarchy && hierarchy.modes) ? hierarchy.modes : null;
    if (modes && modes.length) {
        const roots = modes.filter((m) => levels[m]);
        if (roots.length) return roots;
    }
    if (levels.root) return ["root"];
    const first = Object.keys(levels)[0];
    return first ? [first] : [];
}

/**
 * Group a component's modulatable params by the levels its hierarchy declares.
 *
 * @param {object} o
 * @param {object|null} o.hierarchy    parsed ui_hierarchy, or null when the
 *                                     module declares none. NOT for a read that
 *                                     FAILED — see the caller's tri-state note;
 *                                     a failed read must not become a menu.
 * @param {Array}  o.chainParams       parsed chain_params
 * @param {number} [o.minToGroup]
 * @returns {{grouped: boolean, flat: Array, groups: Array}}
 *          `groups` is [{key, label, params: [{key,label}]}]; when `grouped` is
 *          false the caller skips the group step and offers `flat` directly.
 */
export function groupLfoTargetParams({ hierarchy, chainParams, minToGroup } = {}) {
    const flat = flatLfoTargetParams(chainParams);
    const limit = (typeof minToGroup === "number") ? minToGroup : MIN_PARAMS_TO_GROUP;
    const ungrouped = { grouped: false, flat, groups: [] };

    if (flat.length <= limit) return ungrouped;

    const levels = (hierarchy && hierarchy.levels && typeof hierarchy.levels === "object")
        ? hierarchy.levels : null;
    if (!levels) return ungrouped;

    /* Label by key, so a level naming a param the module also declares gets the
     * declared label rather than the level's spelling of it. The flat list is
     * the single source of both membership and naming. */
    const byKey = new Map(flat.map((p) => [p.key, p]));

    const { nameOf } = levelNamer(levels);
    const claimed = new Set();
    const claimName = makeClaimer(new Set());
    const groups = [];

    /* A level's own params, knobs first — the order the grid puts them in.
     * First group to list a key keeps it, so the groups are disjoint and the
     * union is the flat list exactly once. */
    const collect = (level) => {
        const out = [];
        for (const k of knobKeys(level).concat(paramKeys(level))) {
            if (claimed.has(k) || !byKey.has(k)) continue;
            claimed.add(k);
            out.push(byKey.get(k));
        }
        return out;
    };

    const roots = walkRoots(hierarchy, levels);
    const seenLevels = new Set();
    for (const rootKey of roots) {
        const { visit, visited } = makeLevelWalker({
            levels, rootKey,
            onLevel: ({ levelKey, level, title, isRoot }) => {
                if (seenLevels.has(levelKey)) return;
                seenLevels.add(levelKey);
                const params = collect(level);
                /* A level that contributed nothing is a nav node, not a
                 * category the user can pick anything out of. */
                if (!params.length) return;
                /* The walker calls every root "Main", which is right for the
                 * grid (one root, one place you land) and wrong here as soon
                 * as `modes` gives us two of them: the second would be claimed
                 * as "Main - 2" when the module already calls it "Performance".
                 * Use the mode's own name whenever there is more than one. */
                const label = (isRoot && roots.length > 1) ? nameOf(levelKey, level) : title;
                groups.push({ key: levelKey, label: claimName(label), params });
            },
        });
        visit(rootKey, null, false);
        for (const k of visited) seenLevels.add(k);
    }

    /* Levels no edge reached. Not the same sweep the planner runs — that one
     * excludes another mode's tree, and here there is no active mode to be
     * other than. */
    for (const levelKey of Object.keys(levels)) {
        if (seenLevels.has(levelKey)) continue;
        seenLevels.add(levelKey);
        const params = collect(levels[levelKey]);
        if (params.length) {
            groups.push({ key: levelKey, label: claimName(nameOf(levelKey, levels[levelKey])), params });
        }
    }

    /* The losslessness guarantee. Anything still unclaimed is a param the
     * hierarchy never mentions — 195 of po32-drum's, all of them — and it stays
     * offerable. */
    const rest = flat.filter((p) => !claimed.has(p.key));
    if (rest.length) {
        groups.push({ key: OTHER_GROUP_KEY, label: claimName("Other"), params: rest });
    }

    /* One group is the flat list with a click in front of it. */
    if (groups.length <= 1) return ungrouped;
    return { grouped: true, flat, groups };
}

/**
 * Where a stored routing sits in a grouped list: `{groupIndex, paramIndex}`,
 * both 0 when the key is not offered.
 *
 * The picker used to reset to index 0 on every entry, so re-aiming an LFO
 * already pointed at param #143 cost the same 143 jog steps as the first time
 * — with the answer sitting in `target_param` the whole while. A key that is
 * no longer offered (the module was swapped) falls back to the top rather than
 * to nothing: the routing is still stored, but there is no row to land on.
 */
export function locateLfoTargetParam(groups, targetParam) {
    if (!targetParam || !Array.isArray(groups)) return { groupIndex: 0, paramIndex: 0 };
    for (let g = 0; g < groups.length; g++) {
        const params = (groups[g] && groups[g].params) || [];
        for (let i = 0; i < params.length; i++) {
            if (params[i] && params[i].key === targetParam) return { groupIndex: g, paramIndex: i };
        }
    }
    return { groupIndex: 0, paramIndex: 0 };
}

/** Index of `key` in a [{key}] list, or 0 — the same fallback, for step 1. */
export function indexOfKey(list, key) {
    if (!key || !Array.isArray(list)) return 0;
    const at = list.findIndex((e) => e && e.key === key);
    return at >= 0 ? at : 0;
}
