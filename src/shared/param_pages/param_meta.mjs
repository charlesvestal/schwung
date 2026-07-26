/**
 * param_meta.mjs — resolve a param key to its declared metadata, and classify
 * how a knob page should treat it.
 *
 * PURE, like page_plan.mjs: inputs are the parsed `ui_hierarchy` and
 * `chain_params`; nothing here reads or writes a param.
 *
 * Precedence deliberately matches the list editor's `getParamMetadata`
 * (shadow_ui.js): `{ ...inlineHierarchyMeta, ...chainParamsMeta }` — chain_params
 * wins on any field both declare, and the inline entry supplies what
 * chain_params omits (most usefully `label`, which chain_params spells `name`).
 * Matching it matters: the same param must not read differently depending on
 * whether you are looking at the list or the grid.
 *
 * Fleet reality this is built against (76 modules, see
 * docs/plans/2026-07-26-param-pages-audit.md):
 *   chain_params   float 1685, int 1125, enum 774, filepath 22, wav_position 2,
 *                  string 1, canvas 1; ui_type:"wav_position" a further 19
 *   inline params  float 212, enum 118, int 56, filepath 4, toggle 2
 *   inline extras  default 330, min/max 268, step 247, unit 124, options 116,
 *                  display_format 16 — all of which must survive resolution
 *   metadata gaps  only 3 modules declare knob keys with no chain_params entry
 *                  (impressive-chords 15, sfz 8, clap 2), so inference is a
 *                  narrow fallback and never infers structure.
 */

/** A knob turns it continuously — float/int. */
export const KIND_NUMBER = "number";
/** A knob steps through discrete options — enum/toggle. */
export const KIND_ENUM = "enum";
/** A knob cannot drive it; the cell opens a fullscreen editor on click —
 *  filepath/file/canvas/wav_position/string and the dynamic pickers. */
export const KIND_OPAQUE = "opaque";

const OPAQUE_TYPES = new Set([
    "filepath", "file", "canvas", "wav_position", "string",
    "module_picker", "parameter_picker",
]);

const lower = (v) => String(v == null ? "" : v).toLowerCase();

function keyOf(entry) {
    if (typeof entry === "string") return entry;
    if (entry && typeof entry === "object" && entry.key) return entry.key;
    return null;
}

/**
 * Collect inline `params[]` metadata from every level, plus `chain_params`,
 * into one lookup.
 *
 * Inline entries are indexed across ALL levels because a page can draw keys
 * from any level. Where two levels declare inline metadata for the same key,
 * the first occurrence wins and the conflict is reported — silently picking one
 * is how a param ends up with a different range on two pages.
 *
 * @returns {{get: (key:string)=>object|null, keys: string[], conflicts: string[]}}
 */
export function buildMetaIndex({ hierarchy, chainParams } = {}) {
    const inline = new Map();
    const conflicts = [];

    for (const lvl of Object.values((hierarchy && hierarchy.levels) || {})) {
        if (!lvl || typeof lvl !== "object") continue;
        for (const p of (lvl.params || [])) {
            if (!p || typeof p !== "object" || p.level) continue;   /* nav entry */
            const k = keyOf(p);
            if (!k) continue;
            if (inline.has(k)) {
                const prev = inline.get(k);
                if (JSON.stringify(prev) !== JSON.stringify(p)) conflicts.push(k);
                continue;
            }
            inline.set(k, p);
        }
        /* knobs[] entries may themselves be objects carrying metadata. */
        for (const k of (lvl.knobs || [])) {
            if (!k || typeof k !== "object" || !k.key) continue;
            if (!inline.has(k.key)) inline.set(k.key, k);
        }
    }

    const chain = new Map();
    for (const p of (chainParams || [])) {
        if (p && p.key && !chain.has(p.key)) chain.set(p.key, p);
    }

    const cache = new Map();
    const get = (key) => {
        if (cache.has(key)) return cache.get(key);
        const i = inline.get(key) || null;
        const c = chain.get(key) || null;
        const meta = (i || c) ? normalize(key, { ...(i || {}), ...(c || {}) }) : null;
        cache.set(key, meta);
        return meta;
    };

    /* A key a module puts on a knob but declares nowhere. Rare and real: sfz
     * (8 keys) and clap (2) in the fleet. Returning null would leave the cell
     * blank, so synthesise the same 0..1 float Movy assumes and mark it for
     * repair by inferFromValue on the first successful read. This is the only
     * place the library guesses, and it guesses a range — never a structure. */
    const getOrGuess = (key) => get(key) || {
        key, label: key.replace(/_/g, " "),
        type: "float", min: 0, max: 1, step: 0.01,
        kind: KIND_NUMBER, guessed: true,
    };

    const keys = [...new Set([...inline.keys(), ...chain.keys()])];
    return { get, getOrGuess, keys, conflicts: [...new Set(conflicts)] };
}

/**
 * Fill in the fields a renderer needs, without inventing structure.
 * Never widens a declared range and never fabricates options.
 */
function normalize(key, raw) {
    const meta = { ...raw, key };

    /* `wav_position` arrives as either type or ui_type in the fleet (2 vs 19
     * occurrences); normalise to type so one check covers both. */
    let type = lower(meta.type);
    const uiType = lower(meta.ui_type);
    if (!type && uiType) type = uiType;
    if (uiType === "wav_position") type = "wav_position";

    /* `toggle` is used inline but is absent from the documented type list in
     * docs/MODULES.md — treat it as a two-option enum rather than dropping it.
     * See audit §2; the contract should adopt it or the modules should stop. */
    if (type === "toggle" && !Array.isArray(meta.options)) {
        meta.options = ["Off", "On"];
        type = "enum";
    }
    if (!type) type = Array.isArray(meta.options) ? "enum" : "float";
    meta.type = type;

    /* Display label. chain_params spells it `name`, inline entries `label`;
     * fall back to a de-underscored key the way the list editor does. */
    meta.label = meta.label || meta.name || key.replace(/_/g, " ");

    const num = (v) => (typeof v === "number" && isFinite(v) ? v : null);
    let min = num(meta.min);
    let max = num(meta.max);
    let step = num(meta.step);

    if (type === "enum") {
        const n = Array.isArray(meta.options) ? meta.options.length : 0;
        /* An enum with no declared options is index-addressed; leave the range
         * open rather than guessing a count. */
        meta.min = min !== null ? min : 0;
        meta.max = max !== null ? max : (n > 0 ? n - 1 : null);
        meta.step = 1;
    } else if (type === "int") {
        meta.min = min;
        meta.max = max;
        meta.step = step !== null && step > 0 ? step : 1;
    } else if (type === "float") {
        /* A declared range is never overridden; an under-specified float gets
         * the conventional 0..1/0.01. Params declared nowhere at all are the
         * only ones marked `guessed` — see getOrGuess. */
        meta.min = min !== null ? min : 0;
        meta.max = max !== null ? max : 1;
        meta.step = step !== null && step > 0 ? step : 0.01;
    }

    meta.kind = OPAQUE_TYPES.has(type) ? KIND_OPAQUE
              : type === "enum" ? KIND_ENUM
              : KIND_NUMBER;
    return meta;
}

/** True when a knob can drive this param at all. */
export function isTurnable(meta) {
    return !!meta && meta.kind !== KIND_OPAQUE;
}

/**
 * One-shot repair for a param whose type/range we had to guess (`meta.guessed`).
 * Mirrors how the enum layer learns its exchange format: on the first successful
 * read, an obviously-integral value outside the guessed 0..1 range switches the
 * param to int and widens the range to contain it.
 *
 * Returns the fields to overwrite, or null to keep the guess. Caller applies the
 * patch and clears `guessed` so this runs once.
 *
 * Ported from schwung-movy's meta inference (c) 2026 megadake, MIT.
 */
export function inferFromValue(meta, raw) {
    if (!meta || !meta.guessed || raw === null || raw === undefined) return null;
    const s = String(raw).trim();
    if (s === "") return null;
    const v = Number(s);
    if (!isFinite(v)) return null;

    if (Number.isInteger(v) && Math.abs(v) > 1) {
        /* Negatives are almost always symmetric bipolar controls (transpose,
         * detune) — mirror the magnitude so zero stays centred. Positives get
         * the smallest power of two that contains the value: enough to hold it
         * without over-claiming a 0..127 range we cannot confirm. */
        const min = v < 0 ? v : 0;
        const max = v < 0 ? -v : pow2AtLeast(v);
        return { type: "int", kind: KIND_NUMBER, min, max, step: 1 };
    }
    return null;
}

function pow2AtLeast(n) {
    let p = 1;
    while (p < n) p *= 2;
    return p;
}
