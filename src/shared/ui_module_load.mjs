/* UI scripts share the host's globalThis. Borrow their exports while loading,
 * then restore the host even if evaluation fails partway through. */
const CALLBACKS = ["init", "tick", "onMidiMessageInternal", "onMidiMessageExternal",
                   "onUnload", "onResume"];
const EXPORTS = [...CALLBACKS, "chain_ui"];

export function withUiGlobals(fn, extraExports = [], globals = globalThis) {
    const names = [...new Set([...EXPORTS, ...extraExports])];
    const saved = {}, had = {};
    for (const name of names) {
        had[name] = Object.prototype.hasOwnProperty.call(globals, name);
        saved[name] = globals[name];
    }
    try {
        return fn(saved);
    } finally {
        for (const name of names) {
            if (had[name]) globals[name] = saved[name];
            else delete globals[name];
        }
    }
}

/* The loader is synchronous and does not call init. Null means it returned
 * false; exceptions propagate after restoring globals. Missing callbacks must
 * stay null, never inherit a host (or a previously loaded module) callback. */
export function loadUiModule(load, path, globals = globalThis) {
    return withUiGlobals((saved) => {
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
    }, [], globals);
}
