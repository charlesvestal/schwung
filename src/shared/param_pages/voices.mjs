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
 * per-zone pages, and inference would seat them as racks. So `pad_layout` is its
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
/* One implementation of "what is this level called". See the note on
 * navLabelsOf in page_plan.mjs. The import direction is safe: page_plan.mjs
 * imports child_key/viz/param_meta and never voices.mjs, so there is no cycle,
 * and page_controller.mjs (which imports both) loads page_plan first anyway. */
import { navLabelsOf, declaredLevelName } from "./page_plan.mjs";

const LAYOUTS = ["drums", "chromatic"];

/**
 * The declared layout, or null for "the module has not said".
 *
 * Null for absent, for a non-string, and for a string we do not recognise —
 * an unknown value is an unanswered question, not a licence to pick a default.
 */
export function padLayoutOf(hierarchy) {
    const v = hierarchy && hierarchy.pad_layout;
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
function voicesForLevel(name, level, out, navLabel) {
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
        /* Resolved by page_plan.mjs's declaredLevelName, NOT by a second
         * spelling here. This line used to read `level.name || name`, which
         * dropped the nav-link label and the level's own `label` — so
         * `root: {params: [{level: "bd", label: "Bass Drum"}]}, bd: {note: 36}`
         * had the page header say "Bass Drum" and the voice list say "bd".
         * The fallback is the raw key, not the page title's prettify(): a voice
         * name is an identity a sequencer matches on, not chrome. */
        name: declaredLevelName(name, level, navLabel) || name,
        note,
        role: (typeof level.role === "string" && level.role) ? level.role : null,
    });
}

/**
 * The canonical, ordered voice list.
 *
 * Order: `root` itself, then `root`'s nav links in declared order — that is the
 * order the user sees and the order a rack should be seated in — then any voice
 * level `root` does not link, in `Object.keys(levels)` order. The last part is
 * not cosmetic: a voice reachable only from a sub-level still needs a stable
 * index, and dropping it would make two consumers disagree about the same
 * list while both looked correct.
 *
 * `Object.keys` IS THE ORDER, and it is not quite declaration order: JavaScript
 * enumerates INTEGER-LIKE keys first, ascending, before the rest in insertion
 * order. So `{root, "10": …, "2": …, "1": …}` walks 1, 2, 10 — the module's
 * declared order silently rewritten, in the one file that exists to own that
 * order. No fleet module names a level "1".."16" today, so this is the promise
 * rather than a bug to route around: a module wanting a specific order for
 * numeric-looking levels must LINK them from root, where declared order is
 * honoured verbatim because nav entries are an array. Pinned by
 * tests/host/test_voices.sh so the comment cannot drift back to claiming
 * "declaration order".
 */
export function voicesOf(hierarchy) {
    const levels = (hierarchy && hierarchy.levels) || {};
    const out = [];
    const seen = new Set();
    const navLabel = navLabelsOf(levels);

    const root = levels.root;

    /* ROOT ITSELF CAN BE THE RACK, and skipping it made this whole feature a
     * no-op for the one module it was written for.
     *
     * Every fixture in the tests declares `root: { params: [{level: "pads"}] }`
     * and puts the rack in a sibling level -- so the walk started at root's nav
     * links, and root was only ever a signpost. mrdrums does not do that. Its
     * root IS the 16-pad child level (`child_count: 16`, `child_key_template`
     * "p{index}_{key}", `child_index_param` "ui_current_pad"), and it is the
     * flagship template-shape drum module in the fleet.
     *
     * Measured against tests/fixtures/module-contracts.json: `voicesOf` on the
     * real mrdrums hierarchy returned ZERO voices, and still returned zero
     * after adding the layout and note declarations a fleet PR would add. Every
     * unit test passed throughout, because every fixture agreed with the code
     * rather than with the fleet.
     *
     * Root goes FIRST because it is where the user lands. */
    if (root) voicesForLevel("root", root, out, navLabel);
    /* MARK ROOT SEEN AS PART OF EXPANDING IT. A "Home" nav entry pointing back
     * at root is an ordinary thing for a rack to declare, and without this it
     * expanded root a SECOND time: sixteen pads became thirty-two, same levels,
     * same notes, at two sets of indices. The voice index is the identity this
     * whole file exists to own, so a doubled list is two consumers disagreeing
     * about which pad is which — not a cosmetic duplicate. */
    seen.add("root");

    for (const p of (root && root.params) || []) {
        const name = p && typeof p === "object" && p.level;
        if (!name || seen.has(name)) continue;
        seen.add(name);
        voicesForLevel(name, levels[name], out, navLabel);
    }
    for (const name of Object.keys(levels)) {
        if (seen.has(name)) continue;
        voicesForLevel(name, levels[name], out, navLabel);
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

/**
 * The voice a child level's zero-based instance addresses, or null.
 *
 * NO CALLER IN THIS REPO, DELIBERATELY. It is the consumer-side half of the
 * contract — an external sequencer (movy is the live case) reads
 * `<prefix>:<child_index_param>`, turns it into an instance with
 * `childIndexFromWire`, and lands here. The grid does not: for the template
 * shape the focus priority rule stops at `child_index_param`, which it already
 * follows through page_controller's rotation stop, so the grid never has an
 * instance in hand that it did not put there itself.
 *
 * Kept rather than dropped because the ORDERING it reads is the fact this file
 * owns, and an external consumer re-deriving "instance i of level L is voice n"
 * is exactly the second implementation the design forbids. Exercised by
 * tests/host/test_voices.sh.
 */
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
