/**
 * dump_contracts.mjs — capture every installed module's declared UI contract.
 *
 * The fleet fixture the param-page tests run against
 * (tests/fixtures/module-contracts.json) is currently derived from a capture by
 * megadake. This regenerates it from a real device so it is ours to refresh as
 * the fleet moves.
 *
 * The capture logic is pure with respect to the device: everything it needs
 * arrives as injected functions, so it runs against a real Move, against a
 * simulated one built from an existing fixture (see the round-trip test), or
 * against anything else that can answer the same three questions.
 *
 *   listModules()            -> [{ id, category, componentKey }]
 *   loadModule(m)            -> boolean   (bring it up in the probe slot)
 *   getParam(m, key)         -> string|null
 *
 * `ui_hierarchy` and `chain_params` are served by the *loaded DSP*, not by files
 * on disk, which is why this has to run on a device with each module actually
 * instantiated. That also makes it destructive to whatever is in the probe slot
 * — the device entry point (dump_contracts_device.js) saves and restores it.
 */

/** Keys a module serves that never appear in its own chain_params. */
export const OUT_OF_BAND_KEYS = ["is_loading", "load_error", "preset_name", "bank_name", "preset_names"];

function parseJson(raw) {
    if (raw === null || raw === undefined || raw === "") return null;
    try { return JSON.parse(raw); } catch { return null; }
}

/**
 * @param {object} io
 * @param {Function} io.listModules
 * @param {Function} io.loadModule
 * @param {Function} io.getParam
 * @param {Function} [io.log]
 * @param {Function} [io.now]  injectable clock so a dump is reproducible in tests
 * @returns {{generated_at:string, module_count:number, modules:Array, failures:Array}}
 */
export function dumpContracts({ listModules, loadModule, getParam, log, now } = {}) {
    const say = log || (() => {});
    const modules = [];
    const failures = [];

    for (const m of listModules()) {
        let ok = false;
        try { ok = !!loadModule(m); } catch (e) { ok = false; }
        if (!ok) {
            failures.push({ id: m.id, reason: "load failed" });
            modules.push({ id: m.id, category: m.category, component_key: m.componentKey, status: "load-failed" });
            say(`  ${m.id}: load failed`);
            continue;
        }

        const hierarchy = parseJson(getParam(m, "ui_hierarchy"));
        const chainParams = parseJson(getParam(m, "chain_params"));

        /* Preset metadata lives across several keys and is not a param. Capture
         * the count but never the names: minijv has 2427 and reports them one
         * at a time, so materialising the list would take minutes and produce a
         * fixture nobody wants to read. */
        let presets = null;
        const rootLevel = hierarchy && hierarchy.levels && hierarchy.levels.root;
        const presetLevel = rootLevel && rootLevel.list_param
            ? rootLevel
            : Object.values((hierarchy && hierarchy.levels) || {}).find((l) => l && l.list_param);
        if (presetLevel) {
            const countRaw = getParam(m, presetLevel.count_param);
            const count = countRaw === null ? 0 : parseInt(countRaw, 10) || 0;
            presets = {
                list_param: presetLevel.list_param,
                count_param: presetLevel.count_param,
                name_param: presetLevel.name_param || null,
                count,
                names: null,
            };
        }

        modules.push({
            id: m.id,
            category: m.category,
            component_key: m.componentKey,
            status: "ok",
            name: m.name || null,
            version: m.version || null,
            ui_hierarchy: hierarchy,
            chain_params: chainParams,
            presets,
        });

        const nLevels = hierarchy && hierarchy.levels ? Object.keys(hierarchy.levels).length : 0;
        say(`  ${m.id}: ${nLevels} levels, ${(chainParams || []).length} params` +
            (presets ? `, ${presets.count} presets` : ""));
    }

    return {
        _source: "Captured from a device by tools/param-pages/dump_contracts.mjs.",
        generated_at: (now ? now() : new Date()).toISOString(),
        module_count: modules.length,
        modules,
        failures,
    };
}

/**
 * Compare a fresh dump against the checked-in fixture and describe what moved.
 * Run after regenerating so a fixture refresh is a reviewed change rather than a
 * silent one — a module that regressed its declaration looks exactly like a
 * module that improved it, in a diff of 563 KB of JSON.
 */
export function diffDumps(oldDump, newDump) {
    const byId = (d) => new Map((d.modules || []).map((m) => [m.id, m]));
    const a = byId(oldDump), b = byId(newDump);
    const out = { added: [], removed: [], changed: [] };

    for (const id of b.keys()) if (!a.has(id)) out.added.push(id);
    for (const id of a.keys()) if (!b.has(id)) out.removed.push(id);

    for (const [id, oldMod] of a) {
        const newMod = b.get(id);
        if (!newMod) continue;
        const levels = (m) => Object.keys((m.ui_hierarchy && m.ui_hierarchy.levels) || {}).length;
        const params = (m) => (m.chain_params || []).length;
        const dl = levels(newMod) - levels(oldMod);
        const dp = params(newMod) - params(oldMod);
        if (dl || dp) {
            out.changed.push({
                id,
                levels: `${levels(oldMod)} -> ${levels(newMod)}`,
                params: `${params(oldMod)} -> ${params(newMod)}`,
            });
        }
    }
    return out;
}
