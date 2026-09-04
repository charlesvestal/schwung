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

/**
 * Human label for instance `i` — "Pad 3", "Tone 1", or a declared name.
 *
 * `child_names` lets a drum module say "Kick" where the generated label can
 * only say "Pad 1". It falls back PER ITEM rather than wholesale: a module
 * that names its first four pads and leaves the rest keeps "Pad 5" for the
 * others, so a partial declaration is an improvement rather than a trade.
 */
export function childLabel(level, i) {
    return childName(level, i) || `${(level && level.child_label) || "Item"} ${i + indexBase(level)}`;
}

/**
 * The name instance `i` DECLARES, or null if it declares none.
 *
 * Split out because two callers need "what is this instance called" and they
 * disagree about the fallback NUMBER, which is a separate and older question:
 * `childLabel` counts from `child_index_base`, while the grid's Selected-Pad
 * page has always counted from 1 regardless. minijv's `part_selector` declares
 * no base, so unifying the numbering would silently renumber its picker from
 * "Part 1-8" to "Part 0-7" — a visible change nobody asked for.
 *
 * So the NAME is shared and the numbering fallbacks stay as they are. Sharing
 * only the half that is genuinely one fact is what stops a module's declared
 * "Kick" appearing in one list and "Pad 1" in another.
 */
export function childName(level, i) {
    const names = level && level.child_names;
    return (Array.isArray(names) && typeof names[i] === "string" && names[i].length)
        ? names[i] : null;
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
 * The param through which the MODULE owns which instance is focused, or null.
 *
 *   "pads": { …, "child_index_param": "focused_pad" }
 *
 * Without this the index is UI-only state, written in exactly one place: a
 * pick from the instance list. That is fine for a synth whose "Part 2" is a
 * deliberate navigation choice, and wrong for a drum module, where hitting a
 * pad is how you choose the thing you are editing. mrdrums is the case --
 * `ui_auto_select_pad` follows the pad you play, so with the index owned by
 * the UI the grid would sit on Pad 1 while the module moved on, and declaring
 * a child level at all would COST that behaviour. This is what makes the
 * declaration additive for such a module instead of a trade.
 *
 * The param is the single source of truth in BOTH directions: a pick writes
 * it, and reading it back is what moves the UI. So there is no third copy of
 * the index to disagree, and a module that moves the focus itself and a user
 * who picks from the list cannot fight -- they are the same write.
 *
 * OPTIONAL, and its absence is the existing behaviour exactly: a level with no
 * `child_index_param` keeps a purely local index, so nothing that works today
 * changes.
 */
export function childIndexParam(level) {
    if (!hasChildren(level)) return null;
    const k = level && level.child_index_param;
    return (typeof k === "string" && k.length) ? k : null;
}

/**
 * The wire value naming instance `i` — in the MODULE's numbering.
 *
 * `i` is zero-based everywhere inside the page engine; the module counts from
 * `child_index_base` (pads are 1..16). Converting in one place, next to
 * formatIndex which already does it for key templates, is what stops the two
 * spellings drifting.
 */
export function childIndexToWire(level, i) {
    return String((i | 0) + indexBase(level));
}

/**
 * The zero-based instance a wire value names, or NULL if it does not name one.
 *
 * Null for a failed read, an empty answer, a non-number, or an index outside
 * the declared count — never a fallback to 0. The caller uses this to decide
 * whether to MOVE the user's focus, and moving it to the first instance
 * because a read timed out would re-key every page on screen and drop the
 * cached values with it. Same tri-state rule the read cursor follows.
 */
export function childIndexFromWire(level, raw) {
    if (!hasChildren(level)) return null;
    if (raw === null || raw === undefined || raw === "") return null;
    const n = Number(raw);
    if (!Number.isFinite(n)) return null;
    const i = Math.round(n) - indexBase(level);
    if (i < 0 || i >= childCount(level)) return null;
    return i;
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
