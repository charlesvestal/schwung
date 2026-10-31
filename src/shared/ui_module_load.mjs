/* UI scripts share the host's globalThis. Capture their exports and restore
 * the host even if evaluation fails. Missing callbacks must not inherit the
 * host's handlers. The synchronous loader does not call init. */
const CALLBACKS = ["init", "tick", "onMidiMessageInternal", "onMidiMessageExternal",
                   "onUnload", "onResume"];
const EXPORTS = [...CALLBACKS, "chain_ui"];

export function loadUiModule(load, path, globals = globalThis) {
    const saved = {}, had = {};
    for (const name of EXPORTS) {
        had[name] = Object.prototype.hasOwnProperty.call(globals, name);
        saved[name] = globals[name];
    }
    try {
        globals.chain_ui = null;
        delete globals.onUnload;
        delete globals.onResume;
        if (!load(path)) return null;
        const callbacks = {};
        for (const name of CALLBACKS) {
            const fn = globals[name];
            callbacks[name] = typeof fn === "function" && fn !== saved[name] ? fn : null;
        }
        return { callbacks, chainUi: globals.chain_ui };
    } finally {
        for (const name of EXPORTS) {
            if (had[name]) globals[name] = saved[name];
            else delete globals[name];
        }
    }
}
