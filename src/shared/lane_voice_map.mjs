/*
 * lane_voice_map.mjs -- which of a drum module's parameters belong to which
 * voice, for automation that follows Move's per-voice paste.
 *
 * On a drum track Move's step/page paste copies only the SELECTED voice's
 * notes (measured: a step holding 36 and 37, voice 36 selected, pasted 36
 * alone). Schwung automation belongs to the slot, not a voice, so without this
 * a paste would copy every lock on the step -- including a snare's onto a step
 * that got only a kick. With it, the chain replaces only the pasted voice's
 * parameters and leaves everything else where it was.
 *
 * The map is built from what the module DECLARES, through voices.mjs and
 * child_key.mjs, so there is one definition of "which pad is which" and "what
 * a pad's keys are" -- never a second copy in C.
 *
 * Wire format, parsed by the chain (chain_lanes.c, "lanes:voice_map"):
 *
 *     "<note>:<key>,<key>,...;<note>:..."
 *
 * "" means NOT A RACK (or nothing usable): pastes stay whole-step, which is
 * exactly right on a melodic track, where Move copies every note of the step.
 * PURE.
 */
import { voicesOf, padLayoutOf } from "./param_pages/voices.mjs";
import { hasChildren, childKeysFor, childCount } from "./param_pages/child_key.mjs";

export const LANE_VOICE_MAP_MAX = 16000;   /* the chain's buffer, less a margin */

function levelKeys(level) {
    const out = [];
    const push = (k) => { if (typeof k === "string" && k.length && out.indexOf(k) < 0) out.push(k); };
    for (const k of (level && level.knobs) || []) push(typeof k === "string" ? k : (k && k.key));
    for (const p of (level && level.params) || []) {
        if (p && typeof p === "object" && p.level) continue;      /* a nav link, not a parameter */
        push(typeof p === "string" ? p : (p && p.key));
    }
    return out;
}

/* The wire string for a parsed ui_hierarchy, or "" for "no voices".
 *
 * A drum module may spread one pad over SEVERAL child levels, and only one of
 * them declares the notes: dr32's Sample level carries `child_note_base`,
 * while Shape and Mix are 32-pad levels with no note map, which voicesOf()
 * rightly does not count as voices of their own. They are the same pads --
 * they share the rack's `child_index_param` -- so instance i of each is the
 * voice instance i of the note-carrying level plays, and its keys join that
 * voice. Without this a pad's volume and pan would be nobody's. */
export function laneVoiceMap(hierarchy) {
    if (!hierarchy || padLayoutOf(hierarchy) !== "drums") return "";
    const levels = hierarchy.levels || {};
    const byNote = new Map();
    const add = (note, keys) => {
        if (!byNote.has(note)) byNote.set(note, []);
        const acc = byNote.get(note);
        for (const k of keys)
            if (/^[A-Za-z0-9_.\-]+$/.test(k) && acc.indexOf(k) < 0) acc.push(k);   /* no separators */
    };
    const noteOf = new Map();          /* child_index_param -> [note by child index] */
    const voiced = new Set();
    for (const v of voicesOf(hierarchy)) {
        const level = levels[v.level];
        voiced.add(v.level);
        if (v.childIndex !== null && hasChildren(level)) {
            add(v.note, childKeysFor(level, v.childIndex));
            const ip = level.child_index_param;
            if (typeof ip === "string" && ip.length) {
                if (!noteOf.has(ip)) noteOf.set(ip, []);
                if (noteOf.get(ip)[v.childIndex] === undefined) noteOf.get(ip)[v.childIndex] = v.note;
            }
        } else {
            add(v.note, levelKeys(level));
        }
    }
    for (const name of Object.keys(levels)) {
        const level = levels[name];
        if (voiced.has(name) || !hasChildren(level)) continue;
        const notes = noteOf.get(level.child_index_param);
        if (!notes) continue;          /* a child level of something else entirely */
        for (let i = 0; i < childCount(level); i++)
            if (notes[i] !== undefined) add(notes[i], childKeysFor(level, i));
    }
    /* A key more than one voice lists is not a VOICE's (dr32's ui_current_pad
     * sits on every pad level): it belongs to the track, and a voice paste
     * leaves it alone. */
    const owners = new Map();
    for (const keys of byNote.values()) for (const k of keys) owners.set(k, (owners.get(k) || 0) + 1);
    const parts = [];
    for (const [note, keys] of byNote) {
        const own = keys.filter((k) => owners.get(k) === 1);
        if (own.length) parts.push(`${note}:${own.join(",")}`);
    }
    const s = parts.join(";");
    return s.length <= LANE_VOICE_MAP_MAX ? s : "";   /* too big to carry: whole-step, never partial */
}
