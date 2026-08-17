/**
 * fake_device.mjs — a Move made of the fleet fixture.
 *
 * Answers `getParam` / `setParam` the way a real chain slot does, so the whole
 * interaction model can be driven headlessly: turn knobs, page around, watch a
 * module finish loading and republish a bigger tree, and assert on what got
 * written, read and announced.
 *
 * It deliberately reproduces the awkward parts of the real thing, because those
 * are what the controller has to survive:
 *
 *   - reads and writes are counted, so a test can prove the staggered cursor
 *     issues ONE read per tick rather than eight
 *   - `is_loading` can be held high and then dropped, republishing a different
 *     hierarchy — the Virus / minijv-expansion case that shifts every page index
 *   - a param can be made to lag, returning its previous value for N reads, which
 *     is the race that write-back suppression exists to survive
 *   - unknown keys return null, as the device does
 *
 * Node-only; nothing here ships.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const FIXTURE = path.join(ROOT, "tests", "fixtures", "module-contracts.json");

let FLEET = null;
export function fleet() {
    if (!FLEET) FLEET = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
    return FLEET;
}

export function moduleById(id) {
    const m = fleet().modules.find((x) => x.id === id);
    if (!m) throw new Error(`fixture has no module "${id}"`);
    return m;
}

/**
 * @param {object} o
 * @param {string} o.id          module to load into the slot
 * @param {string} [o.prefix]    param prefix, default "synth"
 * @param {object} [o.initial]   key -> initial raw value
 */
export function createFakeDevice({ id, prefix = "synth", initial = {} } = {}) {
    let mod = moduleById(id);
    const store = Object.create(null);
    const reads = [];
    const writes = [];
    const announcements = [];
    let loading = false;
    const lag = Object.create(null);     /* key -> { remaining, value } */

    const seed = () => {
        for (const p of (mod.chain_params || [])) {
            if (!p || !p.key) continue;
            const min = typeof p.min === "number" ? p.min : 0;
            store[p.key] = String(initial[p.key] !== undefined ? initial[p.key] : min);
        }
        for (const [k, v] of Object.entries(initial)) store[k] = String(v);
    };
    seed();

    const getParam = (key) => {
        reads.push(key);
        if (!key.startsWith(prefix + ":")) return null;
        const bare = key.slice(prefix.length + 1);

        if (bare === "ui_hierarchy") return mod.ui_hierarchy ? JSON.stringify(mod.ui_hierarchy) : null;
        if (bare === "chain_params") return mod.chain_params ? JSON.stringify(mod.chain_params) : null;
        if (bare === "is_loading") return loading ? "1" : "0";

        /* A lagging param serves its stale value for a few reads — this is the
         * race that makes write-back suppression necessary rather than tidy. */
        const l = lag[bare];
        if (l && l.remaining > 0) { l.remaining--; return l.value; }

        return store[bare] !== undefined ? store[bare] : null;
    };

    const setParam = (key, value) => {
        writes.push([key, value]);
        if (!key.startsWith(prefix + ":")) return;
        store[key.slice(prefix.length + 1)] = String(value);
    };

    return {
        getParam, setParam,
        announce: (t) => { if (t) announcements.push(t); },

        /* inspection */
        reads, writes, announcements,
        store,
        readsFor: (bare) => reads.filter((k) => k === `${prefix}:${bare}`).length,
        lastWrite: () => writes[writes.length - 1] || null,
        resetCounters: () => { reads.length = 0; writes.length = 0; announcements.length = 0; },

        /* device behaviours a test can provoke */
        setLoading: (on) => { loading = !!on; },
        /** Swap the loaded module, as an async load finishing with a bigger tree. */
        becomeModule: (newId) => { mod = moduleById(newId); seed(); },
        /** Rewrite the loaded module's chain_params, as a DSP does when it
         *  finishes loading and republishes real enum options. */
        patchChainParams: (fn) => { mod = { ...mod, chain_params: fn(JSON.parse(JSON.stringify(mod.chain_params || []))) }; },
        /** Make a key serve a stale value for the next `n` reads. */
        lagParam: (bare, staleValue, n) => { lag[bare] = { remaining: n, value: String(staleValue) }; },
        get moduleId() { return mod.id; },
    };
}
