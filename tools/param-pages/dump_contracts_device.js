/*
 * dump_contracts_device.js — device half of the fleet capture.
 *
 * ⚠ UNVERIFIED ON HARDWARE. Written without a Move available; the capture logic
 * it drives (dump_contracts.mjs) is covered by a round-trip test against a
 * simulated device, but this wiring is not. Read it before running it.
 *
 * Runs in the shadow_ui QuickJS context, where shadow_get_param /
 * shadow_set_param and the os module are globals.
 *
 * What it does, and why that is intrusive: `ui_hierarchy` and `chain_params`
 * are served by the *loaded DSP*, not by files on disk, so every module has to
 * be instantiated in a real chain slot to be captured. This loads each one into
 * PROBE_SLOT in turn. It saves that slot's current module first and restores it
 * at the end, but a crash mid-run leaves the wrong module loaded there — use an
 * empty slot, and expect audio from the slot while it runs.
 *
 * Usage (over SSH, with Schwung running):
 *   1. copy this file to /data/UserData/schwung/
 *   2. from the shadow UI's script hook, or any context that can evaluate it:
 *        globalThis.dumpModuleContracts()
 *   3. it writes /data/UserData/schwung/module-contracts.json
 *   4. copy that off the device and diff it against the fixture:
 *        node tools/param-pages/regenerate.mjs /path/to/module-contracts.json
 *
 * NEVER write the output to /tmp — the Move root filesystem is tiny and full.
 */

/* Slot used for probing. 3 is the least likely to hold something the user
 * cares about, but this is still destructive — see the warning above. */
var PROBE_SLOT = 3;
var MODULE_ROOT = "/data/UserData/schwung/modules";
var OUT_PATH = "/data/UserData/schwung/module-contracts.json";
/* A module load is confirmed by watching chain_params CHANGE, not by the
 * return value of the write -- see loadModule. A heavy module (surge, osirus,
 * minijv) can take seconds to appear. This is a busy-spin inside the shadow UI
 * tick, so it is also the per-module ceiling on how long the UI is frozen. */
var LOAD_CONFIRM_MS = 6000;

/* Category directory -> the chain component a module of that type loads into. */
var CATEGORIES = [
    { dir: "sound_generators", component: "synth" },
    { dir: "audio_fx", component: "fx1" },
    { dir: "midi_fx", component: "midi_fx1" },
];

function readJsonFile(path) {
    try {
        var raw = host_read_file(path);
        return raw ? JSON.parse(raw) : null;
    } catch (e) { return null; }
}

/*
 * A category that cannot be scanned must SAY SO. This used to be
 * `catch (e) { continue; }`, which cannot tell "no modules of this kind
 * installed" from "os is not defined" -- and it was the latter: the trigger
 * evaluated this file in global scope, where the shadow UI's `os` import is
 * not visible. All three scans threw, the capture wrote module_count 0 in
 * 18ms, and the run looked like a success. Silence about a failure is worse
 * than the failure.
 */
function listInstalledModules(scanErrors) {
    var out = [];
    for (var c = 0; c < CATEGORIES.length; c++) {
        var cat = CATEGORIES[c];
        var entries;
        try {
            entries = os.readdir(MODULE_ROOT + "/" + cat.dir)[0] || [];
        } catch (e) {
            scanErrors.push({ dir: cat.dir, error: String(e) });
            continue;
        }
        for (var i = 0; i < entries.length; i++) {
            var id = entries[i];
            if (id === "." || id === "..") continue;
            var mj = readJsonFile(MODULE_ROOT + "/" + cat.dir + "/" + id + "/module.json");
            out.push({
                id: id,
                category: cat.dir === "sound_generators" ? "sound_generator"
                        : cat.dir === "audio_fx" ? "audio_fx" : "midi_fx",
                componentKey: cat.component,
                name: mj && mj.name ? mj.name : null,
                version: mj && mj.version ? mj.version : null,
            });
        }
    }
    return out;
}

/* Loading is asynchronous for modules with ROMs or sample banks (Virus, minijv),
 * and a module that is still loading republishes its hierarchy when it finishes
 * — capturing early would record the small tree instead of the real one. Poll
 * is_loading, which no module declares in chain_params but every module serves. */
function waitUntilReady(component, timeoutMs) {
    var waited = 0;
    while (waited < timeoutMs) {
        var loading = shadow_get_param(PROBE_SLOT, component + ":is_loading");
        if (loading !== "1") return true;
        spin(100);
        waited += 100;
    }
    return false;
}

function spin(ms) {
    /* No sleep binding in this context; burn the wait. This runs inside the
     * shadow UI tick, so the UI is frozen for the duration -- acceptable for a
     * one-shot capture, and the alternative (returning to the tick between
     * modules) would mean rewriting the tool as a state machine. */
    var until = Date.now() + ms;
    while (Date.now() < until) { /* wait */ }
}

/*
 * Load a module and CONFIRM it, rather than believing the write's return value.
 *
 * shadow_set_param returns false when the parameter channel refused or timed
 * out, which is not the same as the module failing to load -- the same
 * three-answer distinction that bit the READ path. The first real run recorded
 * 50 of 96 modules as "load failed", and the failures were exactly the heavy
 * ones (surge, osirus, minijv, cloudseed): they load slowly enough that the
 * write did not come back inside the channel's timeout. Believing that produced
 * a capture that was silently missing more than half the fleet, and missing it
 * in a biased way -- the complicated modules, i.e. the ones a contract fixture
 * is for.
 *
 * So: confirm from the device instead of from the return value.
 *
 * WHAT to confirm against took a second run to get right. Reading
 * `<comp>:module` back is the obvious choice and it is WRONG -- nothing serves
 * a GET for it (the shadow UI knows the loaded module from its own chain
 * config, not from a param), so the read errors every time. That run burned
 * both attempts on all 96 modules -- observed as each one loading exactly twice,
 * LOAD_CONFIRM_MS apart, and `param_giveup ... last_key=synth:module` in the
 * log -- and would have finished with the whole fleet marked load-failed. A
 * confirm against a key that cannot be read is worse than no confirm: it turns
 * every success into a slow failure.
 *
 * The signal that does exist is the contract itself. `chain_params` is served
 * by the LOADED module, so waiting for it to CHANGE says a different module is
 * now answering. It is also the thing being captured, so the wait costs nothing
 * extra -- the value that ends the wait is the value that gets recorded.
 *
 * Returns the confirmed chain_params string, or null if it never changed.
 */
function loadModule(component, id, prevChainParams) {
    try { shadow_set_param(PROBE_SLOT, component + ":module", id); } catch (e) { /* below */ }
    for (var waited = 0; waited < LOAD_CONFIRM_MS; waited += 250) {
        var cp = shadow_get_param(PROBE_SLOT, component + ":chain_params");
        if (cp && cp !== prevChainParams) return cp;
        spin(250);
    }
    return null;
}

globalThis.dumpModuleContracts = function () {
    var scanErrors = [];
    var mods = listInstalledModules(scanErrors);
    print("dumping " + mods.length + " modules from slot " + PROBE_SLOT);
    /* Refuse to overwrite a good capture with an empty one. A zero here is
     * never a fact about the device -- there is always a fleet -- so it is a
     * failure of this tool, and writing it would destroy the fixture source. */
    if (mods.length === 0) {
        throw new Error("fleet scan found no modules under " + MODULE_ROOT +
            (scanErrors.length ? "; scan errors: " + JSON.stringify(scanErrors)
                               : "; no scan errors, so the tree is unexpectedly empty"));
    }

    var restore = {};
    for (var c = 0; c < CATEGORIES.length; c++) {
        var comp = CATEGORIES[c].component;
        restore[comp] = shadow_get_param(PROBE_SLOT, comp + "_module") ||
                        shadow_get_param(PROBE_SLOT, comp + ":module") || "";
    }

    var out = [];
    var failures = [];
    /* Per component, the chain_params of the module loaded there before this
     * one -- the baseline loadModule watches for a change against. */
    var prevChainParams = {};

    for (var i = 0; i < mods.length; i++) {
        var m = mods[i];
        var comp2 = m.componentKey;
        var confirmed = loadModule(comp2, m.id, prevChainParams[comp2] || null);
        if (confirmed) {
            prevChainParams[comp2] = confirmed;
            waitUntilReady(comp2, 15000);
        }

        /*
         * An unconfirmed load is NOT skipped. Capture whatever the component
         * serves and label it, rather than recording a bare "load-failed" and
         * moving on: this tool exists to produce a contract fixture, and a
         * module that answers is worth having even if the confirm did not fire
         * (two modules with byte-identical chain_params in a row would look
         * unconfirmed, and so would a module whose contract is genuinely
         * empty). Dropping it would repeat the bias the confirm was added to
         * fix -- silently missing exactly the awkward cases.
         */
        var hierRaw = shadow_get_param(PROBE_SLOT, comp2 + ":ui_hierarchy");
        var cpRaw = confirmed || shadow_get_param(PROBE_SLOT, comp2 + ":chain_params");
        if (!confirmed && !cpRaw && !hierRaw) {
            failures.push({ id: m.id, reason: "load not confirmed and nothing served" });
            out.push({ id: m.id, category: m.category, component_key: comp2, status: "load-failed" });
            print("  " + m.id + ": load failed");
            continue;
        }
        var hier = null, cp = null;
        try { hier = hierRaw ? JSON.parse(hierRaw) : null; } catch (e) { hier = null; }
        try { cp = cpRaw ? JSON.parse(cpRaw) : null; } catch (e) { cp = null; }

        var presets = null;
        if (hier && hier.levels) {
            for (var lname in hier.levels) {
                var lvl = hier.levels[lname];
                if (lvl && lvl.list_param && lvl.count_param) {
                    var cntRaw = shadow_get_param(PROBE_SLOT, comp2 + ":" + lvl.count_param);
                    presets = {
                        list_param: lvl.list_param,
                        count_param: lvl.count_param,
                        name_param: lvl.name_param || null,
                        /* Never capture 2427 names one call at a time. */
                        count: cntRaw ? parseInt(cntRaw, 10) || 0 : 0,
                        names: null,
                    };
                    break;
                }
            }
        }

        out.push({
            id: m.id, category: m.category, component_key: comp2,
            status: confirmed ? "ok" : "unconfirmed",
            name: m.name, version: m.version,
            ui_hierarchy: hier, chain_params: cp, presets: presets,
        });
        print("  " + m.id + ": " +
              (hier && hier.levels ? Object.keys(hier.levels).length : 0) + " levels, " +
              (cp ? cp.length : 0) + " params");
    }

    for (var comp3 in restore) {
        if (restore[comp3]) shadow_set_param(PROBE_SLOT, comp3 + ":module", restore[comp3]);
    }

    var doc = {
        _source: "Captured from a device by tools/param-pages/dump_contracts_device.js.",
        generated_at: new Date().toISOString(),
        module_count: out.length,
        modules: out,
        failures: failures,
        scan_errors: scanErrors,
    };
    host_write_file(OUT_PATH, JSON.stringify(doc));
    print("wrote " + OUT_PATH + " (" + out.length + " modules, " + failures.length + " failures)");
    return OUT_PATH;
};
