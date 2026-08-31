/**
 * fake_values.mjs — deterministic stand-in readings for a page nobody has read.
 *
 * A preview has no device, so every widget still needs *something* to point at.
 * These are synthesised: enough to judge a LAYOUT, never enough to judge a
 * patch. Deterministic per key, so the same module renders identically on every
 * run and a diff between two runs means the drawing changed rather than the
 * dice.
 *
 * Lives here rather than in preview.mjs because the audit sheet renders the
 * same pages and would otherwise have its own copy — and two value generators
 * means the audit's picture of a cell is not the picture preview.mjs shows for
 * the same cell, which is exactly the drift that makes a reviewer distrust
 * both. One generator, one picture.
 *
 * Node-only. Nothing here ships to the device.
 */

/**
 * @param {string} key   the param key, the only entropy source
 * @param {object} meta  from metaIndex.getOrGuess(key), or null
 * @returns {string} a wire-format value, as a plugin would report it
 */
export function fakeValue(key, meta) {
    let h = 0;
    for (let i = 0; i < key.length; i++) h = (h * 31 + key.charCodeAt(i)) >>> 0;
    const t = (h % 1000) / 1000;
    if (!meta) return String(t.toFixed(3));
    if (meta.kind === "opaque") return "/data/UserData/Samples/kick_01.wav";
    const min = typeof meta.min === "number" ? meta.min : 0;
    const max = typeof meta.max === "number" ? meta.max : 1;
    const v = min + (max - min) * t;
    return meta.type === "int" || meta.type === "enum" ? String(Math.round(v)) : v.toFixed(3);
}
