/*
 * Where Back from a hierarchy level lands in the LIST editor.
 *
 * A level is reached one of two ways: as a level's `children` (the preset
 * browser drills straight into it), or through a NAVIGATION PARAM — a
 * `{level: ...}` row in some level's `params`. On a preset level those rows
 * are only visible in preset EDIT mode, so a level reached through one must
 * return to edit mode, not to the browser. The list editor used to forget
 * that: loadHierarchyLevel resets a preset level to its browser, and OB-Xd,
 * Dexed, Hera, Braids, Osirus and nine more (14 of 100 fleet modules) put
 * their categories on a preset root — so going Osc 1 -> Back -> Osc 2 landed
 * in the preset list instead.
 */

/* Parent of `level`, and whether it was reached through a navigation param. */
export function hierBackParent(levels, level) {
    for (const [name, def] of Object.entries(levels || {})) {
        if (!def) continue;
        if (def.children === level) return { parent: name, viaNavParam: false };
        if (Array.isArray(def.params)) {
            for (const p of def.params) {
                if (p && typeof p === "object" && p.level === level) {
                    return { parent: name, viaNavParam: true };
                }
            }
        }
    }
    return { parent: "root", viaNavParam: false };
}

/* Row in the parent's (visible) params that leads back to `level`, or -1. */
export function hierBackRow(params, level) {
    if (!Array.isArray(params)) return -1;
    return params.findIndex(p => p && typeof p === "object" && p.level === level);
}

/* True when the parent is a preset level that must reopen in EDIT mode. */
export function hierBackReopensPresetEdit(parentDef, viaNavParam) {
    return !!(viaNavParam && parentDef && parentDef.list_param && parentDef.count_param);
}
