/*
 * scene_doc.mjs -- the SCENE BANK as a document, and where each part of it
 * lives. Pure: no host calls. The host reads and writes through the param
 * channel; this only says what to ask and what the answers mean.
 *
 * THE DSP HOLDS THE LIVE BANK. Locks are made below the UI (the edit arm is
 * decided in the chain host and the shim), so the UI is never the authority --
 * it SAVES what the seven scopes report and LOADS a saved document back into
 * them. A second model of the bank here would be a copy that drifts.
 *
 * A bank spans SEVEN SCOPES, each with its own table and verbs:
 *   slot0..slot3   chain slot N           key "scenes:<verb>"            slot N
 *   mfx            Master FX              key "master_fx:scenes:<verb>"  slot 0
 *   send1, send2   the send buses         key "send<N>:scenes:<verb>"    slot 0
 *
 * The wire line is "<n> <target> <param> <value> <module>" (scene_morph.h).
 *
 * Design: docs/superpowers/specs/2026-09-27-scene-morphing-design.md.
 */

export const SCENE_COUNT = 16;
export const DOC_VERSION = 1;

export const SCOPES = [
    { id: "slot0", slot: 0, prefix: "" },
    { id: "slot1", slot: 1, prefix: "" },
    { id: "slot2", slot: 2, prefix: "" },
    { id: "slot3", slot: 3, prefix: "" },
    { id: "mfx", slot: 0, prefix: "master_fx:" },
    { id: "send1", slot: 0, prefix: "send1:" },
    { id: "send2", slot: 0, prefix: "send2:" },
];

/** The param-channel address of a scope's verb. */
export function scopeKey(scope, verb) {
    return { slot: scope.slot, key: scope.prefix + "scenes:" + verb };
}

/**
 * One scope's dump as locks, or null when the text is not a dump. `null` in
 * means null out: a read that did not complete is not an empty bank.
 */
export function parseDump(text) {
    if (text === null || text === undefined) return null;
    const out = [];
    for (const raw of String(text).split("\n")) {
        const line = raw.trim();
        if (!line) continue;
        const f = line.split(/\s+/);
        if (f.length !== 5) return null;
        const n = Number(f[0]);
        const value = Number(f[3]);
        if (!Number.isInteger(n) || n < 0 || n >= SCENE_COUNT || !Number.isFinite(value)) return null;
        out.push({ n, target: f[1], param: f[2], value, module: f[4] });
    }
    return out;
}

/** A lock as the wire line the DSP loads. */
export function lockLine(l) {
    return l.n + " " + l.target + " " + l.param + " " + l.value + " " + l.module;
}

const clampScene = (v) => (Number.isInteger(v) && v >= 0 && v < SCENE_COUNT) ? v : -1;

/**
 * The document for a set: A, B, and every scope's locks grouped by scene.
 * `dumps` maps scope id -> dump text. ANY missing or unparsable scope makes
 * the whole document null -- a save that silently dropped a scope would write
 * a bank without it, and the next load would erase that scope's scenes.
 */
export function buildDoc({ a, b, dumps }) {
    const scenes = [];
    for (let n = 0; n < SCENE_COUNT; n++) scenes.push({ n, locks: [] });
    for (const scope of SCOPES) {
        const locks = parseDump(dumps ? dumps[scope.id] : null);
        if (!locks) return null;
        for (const l of locks) {
            scenes[l.n].locks.push({ scope: scope.id, target: l.target, param: l.param,
                                      module: l.module, value: l.value });
        }
    }
    return {
        v: DOC_VERSION,
        a: clampScene(a),
        b: clampScene(b),
        scenes: scenes.filter((s) => s.locks.length > 0),
    };
}

/**
 * A saved document, or null. An UNKNOWN version is null and must be LEFT ON
 * DISK by the caller: a later build's bank is not ours to overwrite.
 */
export function parseDoc(text) {
    if (typeof text !== "string" || !text.trim()) return null;
    let d;
    try { d = JSON.parse(text); } catch (e) { return null; }
    if (!d || typeof d !== "object" || d.v !== DOC_VERSION || !Array.isArray(d.scenes)) return null;
    return d;
}

/**
 * What to load into each scope: scope id -> text (one lock per line). EVERY
 * scope gets an entry, empty when it holds nothing, so loading a set also
 * clears what the previous set left in a scope this one does not use.
 */
export function docToLoads(doc) {
    const out = {};
    for (const s of SCOPES) out[s.id] = [];
    const ids = new Set(SCOPES.map((s) => s.id));
    for (const sc of (doc && Array.isArray(doc.scenes)) ? doc.scenes : []) {
        const n = Number(sc && sc.n);
        if (!Number.isInteger(n) || n < 0 || n >= SCENE_COUNT || !Array.isArray(sc.locks)) continue;
        for (const l of sc.locks) {
            if (!l || !ids.has(l.scope)) continue;
            if (typeof l.target !== "string" || typeof l.param !== "string" ||
                typeof l.module !== "string" || !Number.isFinite(Number(l.value))) continue;
            if (/\s/.test(l.target + l.param + l.module) || !l.target || !l.param || !l.module) continue;
            out[l.scope].push(lockLine({ n, target: l.target, param: l.param,
                                         module: l.module, value: Number(l.value) }));
        }
    }
    const loads = {};
    for (const s of SCOPES) loads[s.id] = out[s.id].length ? out[s.id].join("\n") + "\n" : "";
    return loads;
}

/** How many pairs a scope's load holds -- what `scenes:count` must read back. */
export function expectedPairCount(loadText) {
    const seen = new Set();
    for (const line of String(loadText || "").split("\n")) {
        const f = line.trim().split(/\s+/);
        if (f.length === 5) seen.add(f[1] + " " + f[2]);
    }
    return seen.size;
}

/** Sum per-scope "n0,...,n15" answers into one per-scene count. null in -> null. */
export function sumLockCounts(answers) {
    const total = new Array(SCENE_COUNT).fill(0);
    for (const a of answers) {
        if (a === null || a === undefined) return null;
        const parts = String(a).split(",");
        if (parts.length !== SCENE_COUNT) return null;
        for (let i = 0; i < SCENE_COUNT; i++) total[i] += Number(parts[i]) || 0;
    }
    return total;
}
