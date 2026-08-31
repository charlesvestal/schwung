/**
 * level_walk.mjs — how a ui_hierarchy's levels are TRAVERSED and NAMED.
 *
 * Extracted from `page_plan.mjs`, which is still its only reason for being
 * shaped this way. It moved out because a second consumer arrived: the LFO
 * target picker groups a component's modulatable params by the same levels,
 * and a group row that is not called what the corresponding grid page is
 * called defeats the point of grouping it at all.
 *
 * Two copies of these rules would drift in silence. There is no surface
 * anywhere that shows a grid page title next to the picker's row for the same
 * level, so nothing would ever report the disagreement — the user would just
 * find "Oper1/Env" in one place and "Env" in the other and have no way to know
 * they were the same thing.
 *
 * Nothing here knows about pages, knobs, widgets or IPC. It is the tree and
 * the names, and that is all.
 */

/* -------------------------------------------------------------- primitives */

export function keyOf(entry) {
    if (typeof entry === "string") return entry;
    if (entry && typeof entry === "object" && entry.key) return entry.key;
    return null;
}

/* A nav entry points at another level and carries no param of its own. */
export function levelOf(entry) {
    return (entry && typeof entry === "object" && entry.level) ? entry.level : null;
}

/* `children` is absent as null, missing, or the literal string "None" — dexed
 * and every minijv level serialise it that way. */
export function childOf(level) {
    const c = level && level.children;
    return (c && c !== "None") ? c : null;
}

export function knobKeys(level) {
    return ((level && level.knobs) || []).map(keyOf).filter((k) => k !== null);
}

/* Editable (non-nav) keys a level lists in `params`, in declaration order. */
export function paramKeys(level) {
    return ((level && level.params) || [])
        .filter((p) => !levelOf(p))
        .map(keyOf)
        .filter((k) => k !== null);
}

/* 6-char parent tag: one word → first 6 chars, multi-word → first 4 of word one
 * plus the other words' initials ("Operator 1" → "Oper1"). The 128 px header
 * also carries the module name, so this has to stay short. */
export function levelNameToPrefix(name) {
    const words = String(name || "").split(/\s+/).filter(Boolean);
    if (words.length === 0) return "";
    if (words.length === 1) return words[0].slice(0, 6);
    return (words[0].slice(0, 4) + words.slice(1).map((w) => w[0].toUpperCase()).join("")).slice(0, 6);
}

/* ------------------------------------------------------------------- names */

/**
 * What each level is CALLED.
 *
 * `declaredName` answers null when the module named nothing, which is the
 * distinction the walk root needs: it overrides a declared name with "Main"
 * only when there IS one to override. `nameOf` always answers something.
 */
export function levelNamer(levels) {
    /* A level's display name usually lives on the nav entry that points at it,
     * not on the level itself, so collect labels from every level's nav
     * entries. Nav label beats the level's own `label`: 24 levels across
     * dexed/linein/minijv/obxd/sf2/sfz/nam disagree, and the nav label is the
     * one users already see. */
    const navLabel = Object.create(null);
    for (const lvl of Object.values(levels || {})) {
        for (const p of ((lvl && lvl.params) || [])) {
            const target = levelOf(p);
            if (target && p.label) navLabel[target] = p.label;
        }
    }
    /* A level key is an internal identifier ("root", "patch_main", "osc1"); it
     * is only a last resort for a page title, and never raw — "osc1" reads as
     * "Osc1", not as a variable name. */
    const prettify = (key) => String(key)
        .replace(/[_-]+/g, " ")
        .replace(/\b[a-z]/g, (c) => c.toUpperCase());
    const declaredName = (key, lvl) => (lvl && lvl.name) || navLabel[key] || (lvl && lvl.label) || null;
    const nameOf = (key, lvl) => declaredName(key, lvl) || prettify(key);
    return { declaredName, nameOf, prettify };
}

/* -------------------------------------------------------------------- walk */

/**
 * Depth-first over both edges a level can carry — a `params` entry naming a
 * `level`, and `children` — visiting each level at most once.
 *
 * `onLevel({ levelKey, level, prefix, base, title, isRoot })` is called in
 * walk order. `base` is the level's own name with the root's "Main" override
 * applied; `title` is `prefix/base` when a prefix is in force.
 *
 * Returns `{ visit, visited }`. The caller keeps `visit` because the orphan
 * sweep re-enters the walk at levels no edge reached, and keeps `visited`
 * because that is how it knows which those are.
 *
 * @param {object}   o
 * @param {object}   o.levels     hierarchy.levels
 * @param {string}   o.rootKey    where the walk starts
 * @param {function} o.onLevel
 * @param {function} [o.isVisible]  (cond, level) -> bool; defaults to visible
 */
export function makeLevelWalker({ levels, rootKey, onLevel, isVisible } = {}) {
    const lv = levels || {};
    const { nameOf } = levelNamer(lv);
    const vis = isVisible || (() => true);
    const visited = new Set();

    /* `transparent` marks a level reached through a `children` edge: it stands
     * in for its parent's menu rather than being a category, so it neither takes
     * nor contributes a name prefix. Without this every moog page would read
     * "main/Oscillator 1". A `params` nav edge DOES contribute one — that is
     * what keeps minijv's four "Filter" pages apart as Tone 1/Filter etc. */
    function visit(levelKey, prefix, transparent) {
        if (visited.has(levelKey)) return;
        visited.add(levelKey);
        const lvl = lv[levelKey];
        if (!lvl) return;
        if (lvl.visible_if && !vis(lvl.visible_if, lvl)) return;

        /* The walk root's page is always "Main", even when the level declares a
         * label. 16 modules would otherwise open on a page called "Patch" /
         * "Console" / "BOOM"; one consistent name for "where you land" beats
         * each module's own word for it. */
        const isRoot = levelKey === rootKey;
        const base = isRoot ? "Main" : nameOf(levelKey, lvl);
        onLevel({
            levelKey, level: lvl, prefix, base, isRoot,
            title: prefix ? `${prefix}/${base}` : base,
        });

        /* Root's children carry no prefix: they are the module's top-level
         * categories, so "Filter" beats "Root/Filter" — and the header is only
         * 128 px wide, shared with the module name. Prefixes start one level
         * down, which is exactly where they earn their keep (minijv would
         * otherwise show four pages called "Filter").
         *
         * A level declaring no knobs of its own is a MENU, not a category — it
         * exists to point at other levels. It contributes no prefix either, so
         * minijv's tone_selector ("Edit Tones") gives "Tone 1" rather than
         * "EditT/Tone 1", while tone1 itself still prefixes its children as
         * "Tone1/Wave". */
        const isMenuLevel = knobKeys(lvl).length === 0 && !lvl.items_param;
        const childPrefix = (isRoot || isMenuLevel) ? prefix
            : transparent ? prefix
            : levelNameToPrefix(nameOf(levelKey, lvl));
        /* Both edges, always: a level with knobs can still own sub-levels
         * (dexed's Operators, forge's Voice, minijv's tone1). */
        for (const p of ((lvl.params) || [])) {
            const target = levelOf(p);
            if (target) visit(target, childPrefix, false);
        }
        const kid = childOf(lvl);
        if (kid) visit(kid, prefix, true);
    }

    return { visit, visited };
}

/**
 * Every level reachable from `root` by the same two edges the walk follows.
 * Used to work out which levels belong to a mode OTHER than the active one;
 * the walk itself stays recursive-with-prefixes and cannot answer that
 * question on its own.
 */
export function reachableFrom(levels, root) {
    const seen = new Set();
    const stack = [root];
    while (stack.length) {
        const key = stack.pop();
        if (seen.has(key)) continue;
        const lvl = levels[key];
        if (!lvl) continue;
        seen.add(key);
        for (const p of (lvl.params || [])) {
            const t = levelOf(p);
            if (t) stack.push(t);
        }
        const kid = childOf(lvl);
        if (kid) stack.push(kid);
    }
    return seen;
}
