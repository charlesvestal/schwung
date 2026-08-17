/**
 * announce_page.mjs — what a screen reader should say about a knob page.
 *
 * PURE: these build strings. The caller passes them to
 * `shared/screen_reader.mjs`'s `announce()`, which owns the "is the shadow UI
 * even visible" gate and the TTS plumbing.
 *
 * Why this is a module rather than a few template literals: a grid is *worse*
 * than a list for a screen-reader user unless it is deliberately handled. A list
 * announces the selected row as you scroll — the reading order is the navigation
 * order, and one utterance describes where you are. A grid has eight cells that
 * nothing selects; without the strings below it announces nothing at all, which
 * is why the list stays the default surface under TTS until this is wired.
 *
 * The design principle: say where you are, then what changed, and never repeat
 * what the user just did. Turning a knob should say the value, not re-read the
 * name every detent.
 */

import { formatParamValue } from "../param_format.mjs";
import { KIND_ENUM, KIND_OPAQUE } from "./param_meta.mjs";

/**
 * Landing on a page. Position first, because "3 of 12" is the thing a sighted
 * user gets free from the segmented rule and a screen-reader user does not.
 *
 * Continuation pages read as "page 2 of Filter" rather than "Filter - 2", which
 * is punctuation read aloud.
 */
export function announcePage(page, pageIndex, pageCount) {
    if (!page) return "";
    const pos = `${pageIndex + 1} of ${pageCount}`;
    const m = String(page.name || "").match(/^(.*?) - (\d+)$/);
    const name = m ? `page ${m[2]} of ${m[1]}` : (page.name || "");

    if (page.kind === "preset") return `${name}, preset browser, ${pos}`;
    if (page.kind === "items") return `${name}, list, ${pos}`;
    if (page.kind === "modes") return `mode select, ${pos}`;
    if (page.kind === "child") return `${name}, selector, ${pos}`;

    const n = (page.keys || []).length;
    return `${name}, ${pos}, ${n} ${n === 1 ? "control" : "controls"}`;
}

/**
 * Touching a knob: the full name and current value. This is the moment the
 * five-character label stops mattering — say the whole thing.
 */
export function announceTouch(meta, value, slot, decoration) {
    if (!meta) return `knob ${slot + 1}, unassigned`;
    const name = meta.label || meta.key;
    const locked = decoration && decoration.locked;
    const shown = decoration && decoration.value !== undefined ? decoration.value : value;

    if (meta.kind === KIND_OPAQUE) {
        const tail = String(shown == null ? "" : shown).split("/").pop();
        return `${name}, ${tail || "not set"}, click to open`;
    }
    const spoken = spokenValue(meta, shown);
    return locked ? `${name}, ${spoken}, locked` : `${name}, ${spoken}`;
}

/**
 * Turning a knob. Value only — the user is holding the control and already knows
 * which one; re-reading the name every detent makes the readout unusable.
 */
export function announceTurn(meta, value) {
    if (!meta) return "";
    return spokenValue(meta, value);
}

/**
 * Reading the whole page out — the gesture that replaces "glance at the screen".
 * Names and values in knob order, so the spoken order matches the physical one.
 */
export function announcePageContents(page, metaIndex, values, decorations) {
    if (!page || !page.keys || !metaIndex) return "";
    const parts = [];
    for (let i = 0; i < page.keys.length; i++) {
        const key = page.keys[i];
        if (!key) continue;
        const meta = metaIndex.getOrGuess(key);
        const dec = decorations ? decorations[i] : null;
        const raw = dec && dec.value !== undefined ? dec.value : (values ? values[key] : null);
        const name = meta.label || key;
        parts.push(dec && dec.locked
            ? `${name} ${spokenValue(meta, raw)} locked`
            : `${name} ${spokenValue(meta, raw)}`);
    }
    return parts.join(", ");
}

/**
 * A value as speech rather than as pixels. Two differences from the on-screen
 * form: an enum says its option name (never an index), and an unread value says
 * so instead of speaking the "--" placeholder as punctuation.
 */
export function spokenValue(meta, raw) {
    if (raw === null || raw === undefined || raw === "") return "not read yet";
    if (!meta) return String(raw);
    if (meta.kind === KIND_ENUM && Array.isArray(meta.options)) {
        const idx = Math.round(Number(raw));
        if (idx >= 0 && idx < meta.options.length) return String(meta.options[idx]);
        return String(raw);
    }
    /* formatParamValue already expands units the way a reader should hear them
     * ("-6.0 dB", "1.50 kHz", "+7 st"). */
    return formatParamValue(raw, meta);
}
