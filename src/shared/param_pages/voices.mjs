/**
 * voices.mjs — what a module says about its performance surface.
 *
 * Two questions a sequencer has to answer before it can draw anything, and
 * until now could not ask: is this a drum rack or a keyboard, and what does
 * each pad address? Movy answers both from a private table —
 * `movy_config.json`, 14 bundled configs and a 4-module override list — and
 * `padScoping.concreteKeyTemplate` in it is a verbatim re-spelling of
 * `child_key_template`. Every sequencer that ever ships would rebuild that.
 *
 * LAYOUT IS DECLARED, NEVER INFERRED. The obvious shortcut — "it has notes on
 * its pages, so it is drums" — is wrong: a sampler with key zones, a
 * multitimbral synth and a chord module all legitimately carry notes on
 * per-zone pages, and inference would seat them as racks. So `layout` is its
 * own statement, and ABSENT IS A THIRD STATE meaning the module has not said.
 * All 100 captured fleet modules are in that state; answering "chromatic" for
 * them would put words in their mouth and make "declared melodic"
 * indistinguishable from "never asked".
 *
 * VOICES ARE ORDERED HERE AND NOWHERE ELSE. The order is a fact with several
 * consumers, and a fact with several consumers written down in none of them is
 * how the metronome and recall_quantize both got the same off-by-one. It is
 * why the chain host reports a NOTE and not a voice index: a second ordering
 * implementation in C would fail silently as "the grid follows the wrong pad".
 *
 * PURE. It never reads a param. Callers pass a hierarchy object in and get
 * plain data out.
 */

import { hasChildren, childCount, childLabel } from "./child_key.mjs";

const LAYOUTS = ["drums", "chromatic"];

/**
 * The declared layout, or null for "the module has not said".
 *
 * Null for absent, for a non-string, and for a string we do not recognise —
 * an unknown value is an unanswered question, not a licence to pick a default.
 */
export function layoutOf(hierarchy) {
    const v = hierarchy && hierarchy.layout;
    return (typeof v === "string" && LAYOUTS.indexOf(v) >= 0) ? v : null;
}

/** The module-owned focus param for the sibling shape, or null. Its value is a
 *  LEVEL NAME; the template shape uses `child_index_param` instead. */
export function focusParamOf(hierarchy) {
    const k = hierarchy && hierarchy.focus_param;
    return (typeof k === "string" && k.length) ? k : null;
}

function levelNote(level) {
    const n = level && level.note;
    return Number.isFinite(n) ? (n | 0) : null;
}

/* The note instance `i` of a child level plays, or null when the level
 * declares no note map at all — which is every multitimbral synth in the
 * fleet, and must NOT read as a rack of voices. */
function childNote(level, i) {
    const sparse = level && level.child_notes;
    if (Array.isArray(sparse)) {
        const n = sparse[i];
        return Number.isFinite(n) ? (n | 0) : null;
    }
    const base = level && level.child_note_base;
    return Number.isFinite(base) ? ((base | 0) + i) : null;
}

/* A voice's name is the INSTANCE LABEL, resolved by child_key.mjs and nowhere
 * else. This function once repeated childLabel's `child_names` lookup verbatim
 * — two implementations of one rule, agreeing only for as long as nobody
 * touched either. The picker and the voice list must never be able to disagree
 * about what pad 3 is called. */
function childVoiceName(level, i) {
    return childLabel(level, i);
}

function childVoiceRole(level, i) {
    const roles = level && level.child_roles;
    return (Array.isArray(roles) && typeof roles[i] === "string" && roles[i].length)
        ? roles[i] : null;
}

/* Every voice one level contributes, in instance order. A level is either a
 * single voice (it declares `note`) or a rack of them (it declares a note
 * map), never both. */
function voicesForLevel(name, level, out) {
    if (!level) return;
    if (hasChildren(level)) {
        const n = childCount(level);
        for (let i = 0; i < n; i++) {
            const note = childNote(level, i);
            if (note === null) continue;
            out.push({
                index: out.length, level: name, childIndex: i,
                name: childVoiceName(level, i), note,
                role: childVoiceRole(level, i),
            });
        }
        return;
    }
    const note = levelNote(level);
    if (note === null) return;   /* a page, not a voice — 9W9's Reverb/Delay */
    out.push({
        index: out.length, level: name, childIndex: null,
        name: (typeof level.name === "string" && level.name) ? level.name : name,
        note,
        role: (typeof level.role === "string" && level.role) ? level.role : null,
    });
}

/**
 * The canonical, ordered voice list.
 *
 * Order: `root`'s nav links first, in declared order — that is the order the
 * user sees and the order a rack should be seated in — then any voice level
 * `root` does not link, in `levels` declaration order. The second half is not
 * cosmetic: a voice reachable only from a sub-level still needs a stable
 * index, and dropping it would make two consumers disagree about the same
 * list while both looked correct.
 */
export function voicesOf(hierarchy) {
    const levels = (hierarchy && hierarchy.levels) || {};
    const out = [];
    const seen = new Set();

    const root = levels.root;
    for (const p of (root && root.params) || []) {
        const name = p && typeof p === "object" && p.level;
        if (!name || seen.has(name)) continue;
        seen.add(name);
        voicesForLevel(name, levels[name], out);
    }
    for (const name of Object.keys(levels)) {
        if (name === "root" || seen.has(name)) continue;
        voicesForLevel(name, levels[name], out);
    }
    return out;
}

/** The voice a MIDI note plays, or null. First match wins: two voices on one
 *  note is a module bug, and picking the first is stable rather than clever. */
export function voiceIndexFromNote(voices, note) {
    if (!Array.isArray(voices) || !Number.isFinite(note)) return null;
    for (const v of voices) if (v.note === (note | 0)) return v.index;
    return null;
}

/** The voice a level name addresses, or null when that level is a page. */
export function voiceIndexFromLevel(voices, levelName) {
    if (!Array.isArray(voices) || !levelName) return null;
    for (const v of voices) {
        if (v.level === levelName && v.childIndex === null) return v.index;
    }
    return null;
}

/** The voice a child level's zero-based instance addresses, or null. */
export function voiceIndexFromChild(voices, levelName, childIndex) {
    if (!Array.isArray(voices) || !levelName) return null;
    for (const v of voices) {
        if (v.level === levelName && v.childIndex === childIndex) return v.index;
    }
    return null;
}

/**
 * The voice a raw wire value names, or NULL if it does not name one.
 *
 * Null for a failed read, an empty answer, whitespace, a non-number, or an
 * index outside the list — never a fallback to 0. The caller uses this to
 * decide whether to MOVE the user's focus, and moving it to the first voice
 * because a read timed out re-keys every page on screen. Same tri-state rule
 * childIndexFromWire follows, for the same reason.
 */
export function voiceIndexFromWire(voices, raw) {
    if (!Array.isArray(voices) || !voices.length) return null;
    if (raw === null || raw === undefined) return null;
    const s = String(raw).trim();
    if (!s.length) return null;
    const n = Number(s);
    if (!Number.isFinite(n)) return null;
    const i = Math.round(n);
    return (i >= 0 && i < voices.length) ? i : null;
}
