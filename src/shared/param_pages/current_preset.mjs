/*
 * current_preset.mjs — which user preset a component is currently on, and
 * whether it has been changed since.
 *
 * PURE. No param I/O, no drawing, no globals — the caller does every read.
 *
 * A user preset is the component opaque `<prefix>:state` blob, and a blob
 * carries no name, so "you are on Fat Brass" is bookkeeping we keep rather
 * than something we can ask for. The `*` is the answer to "does the live blob
 * still hash to what we stored when it was loaded".
 *
 * The one rule worth stating: a read has THREE answers, not two.
 *
 *   text   the component answered
 *   ""     it declares no state
 *   null   the read did not complete
 *
 * `null` is not news about the module, so nothing here may turn it into a
 * verdict. An unknown hash reports NOT modified — a `*` that appears because a
 * read timed out is worse than no `*` at all, because the fix for it is to
 * press Save, which would overwrite a good preset with whatever is live.
 */

/* FNV-1a over the blob. Not cryptographic — this only has to separate two
 * states of the same module, and it must be identical across runs so the
 * value can be persisted into slot_N.json and compared after a reboot.
 * Length is mixed into the returned string (not just folded into the hash)
 * so two different-length blobs cannot collide on the digit string alone. */
export function hashState(blob) {
    if (blob === null || blob === undefined) return null;
    const s = String(blob);
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i);
        h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
    }
    return `${h.toString(16)}:${s.length}`;
}

/** The record stored per slot+prefix, and persisted with the slot. */
export function makeRecord(name, blob) {
    return { name: String(name || ""), hash: hashState(blob) };
}

/**
 * Has the live state moved away from the loaded preset?
 *
 * False whenever we cannot tell: no record, no name, no stored hash, or a
 * live read that failed. See the header — an unknown must never become a `*`.
 */
export function isModified(record, liveBlob) {
    if (!record || !record.name || !record.hash) return false;
    const live = hashState(liveBlob);
    if (live === null) return false;
    return live !== record.hash;
}

/** The right-aligned value on the User Presets page first row. */
export function presetRowValue(record, liveBlob) {
    if (!record || !record.name) return "(none)";
    return isModified(record, liveBlob) ? `${record.name} *` : record.name;
}
