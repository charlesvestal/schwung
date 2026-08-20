/**
 * lfo_target_label.mjs — what an LFO's target is CALLED.
 *
 * An LFO stores its routing as two internal keys: `target` ("fx1") and
 * `target_param` ("room_size"). Every surface that showed it showed those keys
 * — the list row read "fx1:room_size" and the knob grid, with 30px to spend,
 * read "FX" — while the picker you set it with had already resolved the same
 * routing to "FX 1: Freeverb" and "Room Size". The names existed; nothing
 * carried them back out.
 *
 * This is that mapping, pure: it takes the two arrays the picker already
 * builds (`getTargetComponents()` / `getTargetParams(key)`, both {key,label})
 * and returns the three renderings the surfaces actually need. It performs no
 * lookups of its own — resolving a component costs several IPC round trips at
 * ~2.8ms each, so WHEN to do that is the caller's decision, not this module's.
 *
 * Two forms, because the space differs by an order of magnitude and cutting
 * one string both ways loses the wrong end of it:
 *
 *   short   "Room Size"              a 30px grid cell — the param is what you
 *                                    are looking at; which module is implied
 *                                    by the cell you clicked to get here
 *   long    "Airwindows: Room Size"  anywhere with room — the held-knob header
 *                                    and a full list row
 *
 * The long form names the MODULE, not the slot. "FX 1: Regen" was the first
 * cut and it spends the scarce half of the line on the half you already know:
 * you are looking at that slot, you navigated to it. Which module is loaded
 * there is the thing you cannot see from where you are standing.
 */

/** Shown by every form when the LFO is not routed anywhere. */
export const NO_TARGET = "None";

/**
 * A key with no declared label, made readable: `room_size` -> `Room Size`.
 *
 * Only reached when a component is loaded but its contract does not declare
 * the param (a module swapped since the routing was made, or a param that
 * chain_params omits). Better than showing the token, and honest — it is
 * derived from the key, so it cannot claim a name the module did not give.
 */
export function prettifyKey(key) {
    const s = String(key || "").replace(/[_:]+/g, " ").trim();
    if (!s) return "";
    return s.split(/\s+/).map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(" ");
}

/**
 * Split a picker label into the part that names the slot and the part that
 * names the module: "FX 1: Freeverb" -> ["FX 1", "Freeverb"].
 *
 * A label with no separator is both — "LFO 2" is the slot AND the thing.
 */
function splitComponentLabel(label) {
    const s = String(label || "");
    const at = s.indexOf(": ");
    if (at < 0) return [s, s];
    return [s.slice(0, at), s.slice(at + 2)];
}

/**
 * @param {object}  o
 * @param {string}  o.target        stored component key, e.g. "fx1"
 * @param {string}  o.targetParam   stored param key, e.g. "room_size"
 * @param {Array}   [o.components]  [{key,label}] from getTargetComponents()
 * @param {Array}   [o.params]      [{key,label}] from getTargetParams(target)
 * @returns {{short: string, long: string, empty: boolean}}
 */
export function describeLfoTarget({ target, targetParam, components, params } = {}) {
    const t = String(target || "");
    const p = String(targetParam || "");
    if (!t || !p) return { short: NO_TARGET, long: NO_TARGET, empty: true };

    const comp = (components || []).find((c) => c && c.key === t);
    const [, moduleName] = comp
        ? splitComponentLabel(comp.label)
        /* Routed at something no longer on offer — a module swapped out from
         * under the routing. Say which key it was rather than "None": the
         * routing is still stored and is still what the LFO will drive if the
         * module comes back. */
        : [prettifyKey(t), prettifyKey(t)];

    const pm = (params || []).find((x) => x && x.key === p);
    const paramLabel = pm && pm.label ? pm.label : prettifyKey(p);

    return {
        short: paramLabel,
        long: `${moduleName}: ${paramLabel}`,
        empty: false,
    };
}
