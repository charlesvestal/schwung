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

import { enumWiresNames } from "../param_format.mjs";

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

    /*
     * DIVABLE and OPAQUE are different questions, and conflating them was a bug.
     *
     *   opaque  = a knob cannot drive this at all      (filepath, canvas, string)
     *   divable = clicking it opens an editor          (all of the above, AND
     *                                                   wav_position)
     *
     * granny declares
     *   {"key":"position","type":"wav_position","min":0,"max":1,"step":0.01}
     * — a number with a full range, which a knob can obviously turn, that ALSO
     * has a waveform scrubber worth opening. Marking it opaque made the knob
     * dead and forced a trip through the editor to move a value the encoder was
     * sitting on. Both wav_position params in the fleet are ranged like this.
     *
     * So a ranged wav_position is a NUMBER that happens to be divable. Every
     * other opaque type stays opaque, and is divable too — there is nothing else
     * to do with it.
     */
    const opaqueType = OPAQUE_TYPES.has(type);
    const ranged = typeof meta.min === "number" && typeof meta.max === "number"
                   && meta.max > meta.min;
    /*
     * An ENUM is a third case, and it is why `divable` and the MARK had to come
     * apart as well.
     *
     * Every enum opens a list of its options on a click, and the knob keeps
     * stepping it — a Recv Ch with seventeen options or a Braids algorithm with
     * forty-seven is not something you want to hunt for one detent at a time.
     * So an enum is divable AND KIND_ENUM, exactly the pairing wav_position
     * already needed.
     *
     * But it must NOT wear the corner brackets. There are ~135 enum params in
     * the fleet against ~5 filepath/string/canvas ones: bracket them all and
     * nearly every cell on nearly every page is marked, which is the same as
     * marking none.
     *
     * THE CELL MARKS DO NOT MEAN "DIVABLE", and this is the thing to get right
     * before touching either of them. Measured over the fleet: 967 divable
     * cells on knob pages, of which 953 (99%) wear NO mark at all. Divability
     * is a FOOTER fact — hold the knob and it says CLK OPEN. What the two
     * marks distinguish is something narrower, and the split is clean with
     * zero overlap:
     *
     *   corner brackets   7 cells, ALL turnable    "the knob works, AND it opens"
     *   chevron box       7 cells, NONE turnable   "there is no knob; only a door"
     *
     * The chevron is therefore NOT a divable mark and must not be described as
     * one. It is the WIDGET: an opaque cell has no value-shape to draw, so
     * drawOpaqueBox's notched frame with the chevron in its broken edge is
     * simply what that cell looks like. The brackets are an annotation on top
     * of a working widget. Reported as exactly this confusion — "is it
     * confusing we have brackets and carats that both mean divable" — and the
     * answer is that they never meant the same thing, but the old name said
     * they did.
     *
     * So this flag is now `opaque_type`, a fact about the DECLARATION and
     * nothing more, and the bracket rule is `alsoOpens()` below.
     *
     * An enum that declares NO options is index-addressed and has no list to
     * show, so it is not a door.
     */
    /*
     * ACCESS: which direction this parameter actually means something in.
     *
     *   "readwrite"  (default) an ordinary control
     *   "read"       a READOUT. The value means something, writing means
     *                nothing. keydetect's `detected_key` is 25 key names with
     *                no set_param branch at all — deliberately, and documented
     *                as such when an enum could only be nudged. Enums became
     *                divable in 1.0, so the picker opened on it and silently
     *                discarded the choice. That is a design gap, not a bug in
     *                keydetect: a display-only item was never expressible.
     *   "write"      a TRIGGER. Writing does something, the value means
     *                nothing — the module reports a constant. This is the more
     *                dangerous end: euclidrum's `rnd_preset` declares
     *                ["—","Rnd!"] and fires on anything that is not the
     *                em-dash, so an INDEX write of "0" — which MEANS the
     *                em-dash, "do nothing" — randomises all eight lanes and
     *                destroys the kit.
     *
     * One axis rather than two flags, because they are the same question asked
     * in opposite directions, and a param cannot be both.
     */
    const access = lower(meta.access);
    meta.readOnly = access === "read";
    meta.writeOnly = access === "write";

    const listableEnum = type === "enum"
                       && Array.isArray(meta.options) && meta.options.length > 0;
    meta.opaque_type = opaqueType;
    /* Neither end of the axis opens a list. A readout has nothing to choose;
     * a trigger's options are a spelling of "do it", not a menu. */
    meta.divable = (opaqueType || listableEnum) && !meta.readOnly && !meta.writeOnly;
    meta.kind = (opaqueType && !(type === "wav_position" && ranged)) ? KIND_OPAQUE
              : type === "enum" ? KIND_ENUM
              : KIND_NUMBER;
    return meta;
}

/**
 * An enum's raw value as an INDEX, whichever convention it arrived in.
 *
 * The grid holds enums as numbers end to end, but a plugin is equally entitled
 * to report `"major"` as `"1"` (see learnEnumWireFormat in param_format.mjs),
 * and `Number("major")` is NaN — which seeds a knob at option 0 instead of
 * where the value actually is, and makes a widget fall back to printing the
 * raw string instead of consulting `short_options`.
 *
 * @returns {number} the index, or -1 when the value resolves to neither
 */
export function enumIndexOf(meta, raw) {
    if (!meta || raw === null || raw === undefined) return -1;
    const s = String(raw);
    if (s.trim() === "") return -1;
    const byName = Array.isArray(meta.options)
        ? (meta.options.indexOf(s) >= 0 ? meta.options.indexOf(s) : meta.options.indexOf(s.trim()))
        : -1;
    const num = Number(s.trim());
    const byNumber = isFinite(num) ? Math.round(num) : -1;
    /*
     * An enum whose OPTIONS are themselves numerals ("1", "2", "4", "8") makes
     * the two readings disagree — "4" is the name of option 2 — and nothing in
     * the value can settle it. The known convention can: ask the name first
     * for a plugin that speaks names, and the number first for one that does
     * not. Same precedence as shadow_ui.js's `pluginUsesIndex`, which checks
     * `options.indexOf(currentVal)` before parsing an int.
     */
    if (enumWiresNames(meta)) return byName >= 0 ? byName : byNumber;
    return byNumber >= 0 ? byNumber : byName;
}

/** True when clicking this param should open an editor the grid does not own. */
export function isDivable(meta) {
    return !!meta && !!meta.divable;
}

/**
 * True when the cell should WEAR THE CORNER BRACKETS: "this knob works, and it
 * also opens something."
 *
 * The bracket rule, single-sourced. It used to be open-coded as
 * `meta.opaque_type && meta.kind !== KIND_OPAQUE` at each draw site, which is
 * three terms of subtlety repeated per caller — and one of the sites is the
 * per-cell mark while the other is the mark for a whole viz group, so they
 * drifted the moment either was touched.
 *
 * Reads as exactly one thing in the fleet: a RANGED wav_position, which is a
 * number a knob turns perfectly well AND has a waveform editor worth opening.
 * That pairing is the entire reason `opaque_type`, `kind` and `divable` are
 * three separate fields.
 *
 * NOT the chevron. An opaque cell (KIND_OPAQUE) is excluded because it has no
 * knob behaviour to annotate — drawOpaqueBox's notched frame IS its widget,
 * and brackets on the same rect read as a doubled border. See normalize() for
 * why "both marks mean divable" is the wrong mental model.
 *
 * `divable` is required as well, so a read-only or write-only declaration
 * cannot wear a mark promising a door that onClick will refuse to open. Zero
 * fleet params diverge on that term today (21 either way); it is there so the
 * mark cannot start lying.
 */
export function alsoOpens(meta) {
    return opensOnClick(meta) && meta.kind !== KIND_OPAQUE;
}

/**
 * True when clicking this cell opens an EDITOR — as opposed to an option list.
 *
 * The broader of the two: it is `alsoOpens` without the "and a knob can turn
 * it" term, so it covers both opaque kinds. Used where the cell's own widget
 * is not drawn and the question is only whether the thing on screen is a door:
 * a viz group that spans and covers its cells draws ONE mark across the span,
 * and it must appear for mrdrums, whose `pad_start` is a non-ranged
 * wav_position and therefore KIND_OPAQUE.
 *
 * `opaque_type` rather than `divable` is what keeps ENUMS out. Every enum with
 * options is divable, so the broader test framed mrsample's Loop switch —
 * the "bracket them all and every cell is marked" failure. An enum opens a
 * LIST, which the footer announces; only these open an editor.
 */
export function opensOnClick(meta) {
    return !!meta && !!meta.opaque_type && !!meta.divable;
}

/**
 * True when clicking this cell FLIPS it instead of opening anything.
 *
 * An enum with exactly two options has nothing to browse: the list would show
 * the value already in the cell and the only other value there is. So the
 * click writes the other one, and the picker is never raised.
 *
 * ONE definition because it answers two questions that must not disagree —
 * what the click DOES (page_controller.onClick) and what the footer PROMISES
 * while the knob is held (paramPagesFooterHints). The divable/opaque pair one
 * function up is written up as exactly that failure, three times over: a cell
 * became a door and the footer had to be told separately, so it advertised
 * CLK MENU over a click that opened an editor.
 *
 * `divable` is required, which is what keeps triggers and readouts out: both
 * are excluded from `divable` by construction, and a trigger is a two-option
 * enum in the wire format, so a test on the option count alone would turn
 * every momentary in the fleet into a latch.
 */
export function flipsOnClick(meta) {
    return !!meta && !!meta.divable && meta.kind === KIND_ENUM
        && Array.isArray(meta.options) && meta.options.length === 2;
}

/** True when a knob can drive this param at all. */
export function isTurnable(meta) {
    /* A readout has nothing to set; a trigger is fired, not scrubbed — turning
     * one would walk it through "do nothing" and "do it" as if they were
     * values, which for euclidrum's rnd_preset means randomising a kit on the
     * way past. */
    return !!meta && meta.kind !== KIND_OPAQUE && !meta.readOnly && !meta.writeOnly;
}

/** A readout: the value means something, writing to it does not. */
export function isReadOnly(meta) { return !!meta && !!meta.readOnly; }

/**
 * A trigger: writing does something, the value means nothing.
 *
 * The wire value that FIRES it is the module's business — ["idle","trigger"],
 * ["—","Rnd!"], ["Play","Save"] are all in the fleet — so the host sends
 * option 1 through the ordinary enum wire (enumWireValue), which respects
 * whichever convention that module speaks.
 */
export function isTrigger(meta) { return !!meta && !!meta.writeOnly; }

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
