/**
 * child_key.mjs — addressing repeated elements (drum pads, tones, parts, voices)
 * from declared metadata alone.
 *
 * The problem, quantified: a drum module has 16 pads sharing ~14 params, so it
 * declares 200+ concrete keys (`p01_vol`, `p02_vol`, …). Listing them all in a
 * menu is unusable, so modules list an *alias* (`pad_vol`) meaning "the focused
 * pad" and keep the concrete keys undeclared in any level. Fleet-wide that is
 * the single largest source of unreachable params: mrdrums 209, weird-dreams
 * 187, forge 106.
 *
 * Schwung already documents `child_prefix` / `child_count` / `child_label` for
 * exactly this (docs/MODULES.md), and shadow_ui.js implements it — but it
 * assumes one fixed key shape, `${prefix}${index}_${key}`, which none of those
 * six modules use. So they each solved it again in a third-party config file.
 *
 * This is the smallest extension that lets them stop: an explicit key template,
 * an index base, zero-padding, and per-key overrides. `child_prefix` stays as
 * sugar for the shape it already means, so nothing that works today changes.
 *
 *   "pads": {
 *     "child_count": 16,
 *     "child_label": "Pad",
 *     "child_key_template": "p{index}_{key}",
 *     "child_index_base": 1,          // pads are 1..16, not 0..15
 *     "child_index_digits": 2,        // p01_, not p1_
 *     "child_key_overrides": { "fx1": "v{index}_{key}" },
 *     "knobs": ["vol", "pan", "tune"]
 *   }
 *
 * PURE. Given a level and an index it produces a key; it never reads a param.
 */

/** True when a level declares repeated instances in any supported form. */
export function hasChildren(level) {
    return !!(level && level.child_count > 0 && (level.child_prefix || level.child_key_template));
}

/** How many instances the level declares. */
export function childCount(level) {
    return hasChildren(level) ? Math.max(0, level.child_count | 0) : 0;
}

/** Human label for instance `i` — "Pad 3", "Tone 1". */
export function childLabel(level, i) {
    const base = (level && level.child_label) || "Item";
    return `${base} ${i + indexBase(level)}`;
}

function indexBase(level) {
    const b = level && level.child_index_base;
    return typeof b === "number" ? b : 0;
}

function formatIndex(level, i) {
    const n = i + indexBase(level);
    const digits = (level && level.child_index_digits) | 0;
    return digits > 0 ? String(n).padStart(digits, "0") : String(n);
}

/**
 * The concrete param key for instance `i` of `key` in `level`.
 *
 * Returns null when the level declares no children, so callers can treat "not
 * a child level" and "child level" through one code path.
 *
 * @param {object} level  the ui_hierarchy level
 * @param {number} i      zero-based instance, regardless of child_index_base
 * @param {string} key    the generic key the level lists
 */
export function resolveChildKey(level, i, key) {
    if (!hasChildren(level) || !key) return null;
    const idx = formatIndex(level, i);
    const template = (level.child_key_overrides && level.child_key_overrides[key])
        || level.child_key_template
        /* Legacy form: child_prefix means "<prefix><index>_<key>", which is what
         * shadow_ui.js has always built and what docs/MODULES.md describes. */
        || `${level.child_prefix}{index}_{key}`;
    return template.replace(/\{index\}/g, idx).replace(/\{key\}/g, key);
}

/** Every concrete key instance `i` addresses, in the level's declared order. */
export function childKeysFor(level, i) {
    if (!hasChildren(level)) return [];
    const out = [];
    const seen = new Set();
    const push = (k) => { const c = resolveChildKey(level, i, k); if (c && !seen.has(c)) { seen.add(c); out.push(c); } };
    for (const k of (level.knobs || [])) push(typeof k === "string" ? k : (k && k.key));
    for (const p of (level.params || [])) {
        if (p && typeof p === "object" && p.level) continue;
        push(typeof p === "string" ? p : (p && p.key));
    }
    return out;
}

/**
 * Every concrete key a child level can address across all its instances.
 * Used by the contract validator to stop reporting per-instance keys as
 * unreachable once a template covers them.
 */
export function allChildKeys(level) {
    const out = new Set();
    for (let i = 0; i < childCount(level); i++) {
        for (const k of childKeysFor(level, i)) out.add(k);
    }
    return out;
}
