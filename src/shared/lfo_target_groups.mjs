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
import {
    hasChildren, childCount, childLabel, childKeysFor, childIndexParam,
} from "./param_pages/child_key.mjs";

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

/** Group key prefix for one instance of a child level ("Pad 3"). */
export const CHILD_GROUP_PREFIX = "__child__:";

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

    /*
     * A CHILD LEVEL LISTS TEMPLATES, NOT KEYS — and matching them raw is how
     * the module that needs grouping most ended up with none of it.
     *
     * mrdrums declares 16 pads on `root` and again on `pad_settings`
     * (`child_key_template: "p{index}_{key}"`), so those levels list `vol`,
     * `pan`, `tune`, `start`… while `chain_params` publishes `p01_vol` …
     * `p16_mode`. The bare key is in nobody's chain_params, so both levels
     * collected NOTHING, were dropped as empty, and all 200+ concrete keys
     * fell to the orphan sweep. Reported from the device as "mrdrums has
     * everything under Other" — which was true, and was this.
     *
     * `page_plan.mjs` resolves the same templates through `child_key.mjs`;
     * this simply has to as well.
     *
     * ONE GROUP PER INSTANCE, not one per level. A level's 16×13 expansion in
     * a single row is 208 params behind one click — the flat list again,
     * wearing a name. "Pad 3" is also how the routing is actually thought
     * about.
     *
     * Levels SHARING a `child_index_param` merge into the same instance
     * groups, for the reason `childPickerNeeded` gives: two levels naming one
     * index are two views of one focus, not two sets of pads. mrdrums is
     * exactly that — root contributes vol/pan/tune/…, pad_settings adds
     * sample_path, rand_pan_amt and the rest, — and keying the groups by level instead
     * would split one pad across two lists.
     */
    /*
     * The FOCUSED-INSTANCE ALIAS, which is the target you actually want.
     *
     * A child module publishes both `p03_start` (pad 3's sample start) and
     * `pad_start` (the focused pad's). The alias is the one the knob grid
     * draws and the one an LFO should be aimed at: routing to the concrete key
     * instead is precisely what left `<alias>:modulated` answering 0 and took
     * the whole modulation-indicator chain down with it (see
     * MODULATABLE_TYPES above). But the alias appears in NO level — only the
     * template does — so it was landing in "Other" while the concrete keys got
     * sixteen tidy groups. That steers the user at the wrong key, which is
     * worse than the flat list this feature replaced.
     *
     * The alias prefix is not declared anywhere, so it is INFERRED, with a
     * guard: an unclaimed `chain_param` of the form `<prefix>_<k>` for a key
     * `k` the level declares, where one prefix accounts for at least two such
     * keys. One coincidental match is not a family. Everything else stays in
     * "Other", and losslessness is unaffected either way — this only decides
     * which heading a param appears under, never whether it appears.
     */
    const aliasKeysFor = (level) => {
        if (!hasChildren(level)) return [];
        const declared = knobKeys(level).concat(paramKeys(level));
        const byPrefix = new Map();
        const seenHere = new Set();
        for (const k of declared) {
            for (const p of flat) {
                if (claimed.has(p.key) || seenHere.has(p.key)) continue;
                if (!p.key.endsWith("_" + k)) continue;
                const prefix = p.key.slice(0, p.key.length - k.length - 1);
                if (!prefix || prefix.includes("_")) continue;
                seenHere.add(p.key);
                if (!byPrefix.has(prefix)) byPrefix.set(prefix, []);
                byPrefix.get(prefix).push(p);
            }
        }
        let best = [];
        for (const list of byPrefix.values()) if (list.length > best.length) best = list;
        return best.length >= 2 ? best : [];
    };

    const instanceGroups = new Map();   /* focus key -> [{label, params}] */
    const collectChildren = (levelKey, level) => {
        if (!hasChildren(level)) return;
        const focus = childIndexParam(level) || levelKey;
        let bucket = instanceGroups.get(focus);
        if (!bucket) {
            bucket = [];
            instanceGroups.set(focus, bucket);
            groups.push({ __instances: bucket });   /* holds walk-order position */
        }
        for (let i = 0; i < childCount(level); i++) {
            if (!bucket[i]) bucket[i] = { label: childLabel(level, i), params: [] };
            for (const k of childKeysFor(level, i)) {
                if (claimed.has(k) || !byKey.has(k)) continue;
                claimed.add(k);
                bucket[i].params.push(byKey.get(k));
            }
        }
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
                /* ORDER IS LOAD-BEARING: the concrete expansion claims p01_vol
                 * … p16_mode BEFORE the alias sweep runs, so the only thing
                 * left ending in `_vol` is the alias. Run the other way round
                 * and "largest family wins" picks `p01_` over `pad_` — measured,
                 * it put pad 1's twelve keys on the root page and left "Pad 1"
                 * holding four. */
                const at = groups.length;
                collectChildren(levelKey, level);
                for (const p of aliasKeysFor(level)) {
                    claimed.add(p.key);
                    params.push(p);
                }
                /* A level that contributed nothing of its OWN is still worth
                 * walking for its instances — mrdrums' root declares nothing
                 * but templates. Spliced at the position reserved above so the
                 * level reads before the instances it owns. */
                if (params.length) {
                    /* The walker calls every root "Main", which is right for the
                     * grid (one root, one place you land) and wrong here as soon
                     * as `modes` gives us two of them: the second would be claimed
                     * as "Main - 2" when the module already calls it "Performance".
                     * Use the mode's own name whenever there is more than one. */
                    const label = (isRoot && roots.length > 1) ? nameOf(levelKey, level) : title;
                    groups.splice(at, 0, { key: levelKey, label: claimName(label), params });
                }
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
        const at = groups.length;
        collectChildren(levelKey, levels[levelKey]);
        for (const p of aliasKeysFor(levels[levelKey])) { claimed.add(p.key); params.push(p); }
        if (params.length) {
            groups.splice(at, 0, { key: levelKey, label: claimName(nameOf(levelKey, levels[levelKey])), params });
        }
    }

    /* Expand the instance placeholders in the walk position their level held,
     * dropping instances that claimed nothing (a level whose templates the
     * module does not actually publish). Named only now, so claimName sees
     * them in the order they will be shown. */
    for (let i = groups.length - 1; i >= 0; i--) {
        const g = groups[i];
        if (!g.__instances) continue;
        const live = g.__instances.filter((inst) => inst && inst.params.length);
        groups.splice(i, 1, ...live.map((inst, n) => ({
            key: `${CHILD_GROUP_PREFIX}${i}:${n}`,
            label: claimName(inst.label),
            params: inst.params,
        })));
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
