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
 * @param {boolean} [o.serveLists] answer the RUNTIME list params (see below)
 */
export function createFakeDevice({ id, prefix = "synth", initial = {}, serveLists = false } = {}) {
    let mod = moduleById(id);
    const store = Object.create(null);
    const reads = [];
    const writes = [];
    const announcements = [];
    let loading = false;
    const lag = Object.create(null);      /* key -> { remaining, value } */
    const failing = Object.create(null);  /* key -> reads still to fail */
    let staged = null;                    /* see stagePlugin */
    /*
     * A clock the test drives.
     *
     * A debounce is measured in MILLISECONDS, not in reads — the whole defect
     * is that the UI re-reads too SOON, and a read-counted debounce cannot
     * express "too soon" because it is the reads themselves that move it along.
     * Pass this as `io.now` and advance it explicitly.
     */
    let clock = 0;
    /** Whether this device answers `is_loading` at all. Almost no module does,
     *  and a fix that depends on it would be a fix for a fleet of one. */
    let servesIsLoading = true;

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

        /* A read the param channel could not SERVE answers null, and it is not
         * the same answer as "" — see failParam. Checked first, because the
         * contract keys below are exactly the ones worth failing. */
        const f = failing[bare];
        if (f && f.remaining > 0) { f.remaining--; return null; }

        /* Genuine absence is "": the shim replies with an error and a zeroed
         * buffer, and the JS binding hands that back as an empty string. Only
         * an unservable request is null. */
        if (bare === "ui_hierarchy") return mod.ui_hierarchy ? JSON.stringify(mod.ui_hierarchy) : "";
        if (bare === "chain_params") {
            /*
             * A staged contract is not visible until its debounce has run out
             * — see stagePlugin. Until then this answers with what is still
             * LOADED, which is the previous selection's contract, and that is
             * the whole of the airwindows one-behind bug.
             */
            if (staged) {
                const pending = staged.reads > 0 || clock < staged.untilMs;
                if (pending) { if (staged.reads > 0) staged.reads--; return JSON.stringify(staged.during); }
                mod = { ...mod, chain_params: staged.after };
                staged = null;
            }
            return mod.chain_params ? JSON.stringify(mod.chain_params) : "";
        }
        /* An unserved key answers "", not null — see the header. Almost nothing
         * in the fleet implements is_loading, so a test can turn it off and
         * prove the behaviour under test does not secretly depend on it. */
        if (bare === "is_loading") return servesIsLoading ? (loading ? "1" : "0") : "";

        /*
         * RUNTIME LISTS, opt-in.
         *
         * A preset browser's contents never come from the contract — they come
         * from the loaded plugin at runtime, so an unserved one draws a page
         * with a "--" where the name goes and nothing else. That empty frame is
         * indistinguishable from a module whose preset page is genuinely
         * broken, which is worse than not drawing it: an audit that invents a
         * defect costs more than one that omits a page.
         *
         * The capture knows the param NAMES and the COUNT (dump_contracts
         * records both, and deliberately not the names — minijv has 2427 and
         * serves them one at a time). That is enough to draw the real chrome:
         * the count drives the scrollbar and the index, and a synthetic name
         * fills the row. The names are as fake as every value here; the
         * LAYOUT is the device's.
         *
         * Off by default so no existing test's read counts move.
         */
        if (serveLists && mod.presets) {
            const ps = mod.presets;
            if (bare === ps.count_param) return String(ps.count || 0);
            if (bare === ps.list_param && store[bare] === undefined) return "0";
            if (ps.name_param && bare === ps.name_param) {
                const idx = parseInt(store[ps.list_param] ?? "0", 10) || 0;
                const names = ps.names;
                return Array.isArray(names) && names[idx] !== undefined
                    ? String(names[idx])
                    : `Preset ${String(idx + 1).padStart(3, "0")}`;
            }
        }
        if (serveLists && bare.endsWith("_items")) {
            /* An items page reads a JSON array of {index,label}. Nothing in the
             * capture records one, so this is shape-only: enough rows to show
             * the list's scrolling and truncation behaviour. */
            return JSON.stringify(Array.from({ length: 12 },
                (_, i) => ({ index: i, label: `Item ${i + 1}` })));
        }

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
        /** The injectable clock. Pass as `io.now`; move it with `advance`. */
        now: () => clock,
        advance: (ms) => { clock += ms; return clock; },
        /** Stop answering `is_loading` at all — what the fleet actually does. */
        setServesIsLoading: (on) => { servesIsLoading = !!on; },
        /** Swap the loaded module, as an async load finishing with a bigger tree. */
        becomeModule: (newId) => { mod = moduleById(newId); seed(); },
        /** Rewrite the loaded module's chain_params, as a DSP does when it
         *  finishes loading and republishes real enum options. */
        patchChainParams: (fn) => { mod = { ...mod, chain_params: fn(JSON.parse(JSON.stringify(mod.chain_params || []))) }; },
        /** Make a key serve a stale value for the next `n` reads. */
        lagParam: (bare, staleValue, n) => { lag[bare] = { remaining: n, value: String(staleValue) }; },
        /**
         * Make a key FAIL for the next `n` reads — null, the way the binding
         * answers when it cannot claim the param channel within its 100 ms
         * timeout. Distinct from an absent key, which answers "".
         *
         * This is the granny sequence: picking a sample makes the module load a
         * WAV synchronously on the thread that serves param requests, and the
         * contract read the UI issues milliseconds later gets nothing back.
         */
        failParam: (bare, n) => { failing[bare] = { remaining: n }; },
        /**
         * Select something whose contract only arrives after a debounce.
         *
         * schwung-airwindows is the case this exists for. Writing
         * `plugin_index` updates `selected_plugin_index` synchronously — so
         * `plugin_name` is immediately right — but only SCHEDULES the plugin
         * load on a worker thread, 300 ms later
         * (clap_fx.cpp:806-822, PLUGIN_LOAD_DEBOUNCE_MS). `chain_params` is
         * generated from `cached_param_names`, which is not rewritten until
         * that load finishes, so for the whole debounce it reports the
         * PREVIOUS effect.
         *
         * @param {Array}  after         the contract once the load lands
         * @param {object} [o]
         * @param {number} [o.debounceReads]  reads served stale first
         * @param {number} [o.debounceMs]     ms of CLOCK the load takes. The
         *                                    honest unit: 300 ms of debounce
         *                                    plus the dlopen. Reads do not
         *                                    move it along, which is the point.
         * @param {Array}  [o.during]         what is served meanwhile;
         *                                    defaults to the current contract,
         *                                    which is what the module does.
         *                                    Pass [] for the window in which
         *                                    the plugin pointer is NULL.
         */
        stagePlugin: (after, { debounceReads = 0, debounceMs = 0, during } = {}) => {
            staged = {
                reads: debounceReads,
                untilMs: clock + debounceMs,
                during: during !== undefined ? during : (mod.chain_params || []),
                after,
            };
        },
        get moduleId() { return mod.id; },
    };
}
