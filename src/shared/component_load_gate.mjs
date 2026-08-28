/*
 * WHETHER A COMPONENT EDITOR MAY OPEN YET.
 *
 * Opening a component's editor starts with one read of `<prefix>:ui_hierarchy`,
 * and that read has THREE answers, not two (see the note of the same name in
 * CLAUDE.md, this gate's write-up in docs/SHADOW_UI.md, and
 * page_controller.mjs's `load`):
 *
 *   JSON   the component declared a hierarchy
 *   ""     the channel served us and the key produced nothing
 *   null   the read did not complete — claim refused, or the response timed out
 *
 * The entry gate used to collapse all three into `if (!hierarchy)` and commit,
 * irreversibly, to the preset-browser fallback. For a module that has no
 * `ui_chain.js` and whose preset reads are failing for the same reason the
 * hierarchy read failed, that fallback draws an editor with nothing in it —
 * and nothing ever retries, so it stays that way until the user navigates away
 * and back. Reported from the device for MiniJV and Osirus, the two slowest
 * things in the fleet to come up.
 *
 * What makes that gate the wrong place to give up is that everything which
 * knows how to WAIT lives behind it: the knob grid's "Loading..." hold, its
 * bounded contract retry and its ten-second recovery probe, and the list
 * editor's `is_loading` re-fetch. None of them get a chance, because the
 * decision to show the fallback was already made.
 *
 * So the gate stops deciding from one read. It answers ENTER / HOLD / FALLBACK,
 * and HOLD means "ask again shortly" rather than "this module has nothing".
 *
 * The reason the empty answer needs a second question is that "" is genuinely
 * ambiguous — a module that declares no hierarchy and a position whose module
 * has not finished arriving BOTH answer "". `<prefix>_module` separates them:
 * the chain host only publishes the name after `create_instance` returns
 * (chain_host.c:504), so an occupied position that cannot name its module is
 * one that is still loading. A module that IS named and declares no hierarchy
 * falls back immediately, exactly as before — the well-behaved fleet never
 * sees the hold.
 */

export const ENTRY_ENTER = "enter";
export const ENTRY_HOLD = "hold";
export const ENTRY_FALLBACK = "fallback";

/*
 * Probe cadence while held.
 *
 * The fast phase matches the knob grid's contract retry (page_controller.mjs)
 * because it is waiting on the same thing: ~0.5 s apart for ~20 s. That covers
 * a dlopen of a large plugin and a fork-and-boot like Osirus's.
 *
 * After it, the probe does not stop — it slows. Giving up entirely is what the
 * grid's CONTRACT_RECOVER_INTERVAL_TICKS exists to undo, and it costs one read
 * every ten seconds to make a screen heal itself instead of needing the user to
 * back out and come in again. There is no "give up and show the fallback"
 * ending here on purpose: a blank editor is the failure being fixed, and Back
 * is on screen the whole time for anyone who does not want to wait.
 */
export const HOLD_FAST_INTERVAL_TICKS = 30;
export const HOLD_FAST_LIMIT = 40;
export const HOLD_SLOW_INTERVAL_TICKS = 600;

/** Ticks to wait before the next probe, given how many have already been made. */
export function holdProbeIntervalTicks(attempts) {
    return attempts < HOLD_FAST_LIMIT ? HOLD_FAST_INTERVAL_TICKS : HOLD_SLOW_INTERVAL_TICKS;
}

/**
 * Decide what opening this component's editor should do right now.
 *
 * `read` supplies the three raw wire values LAZILY — `module` and `isLoading`
 * are only consulted on the ambiguous branch, so the ordinary case where the
 * hierarchy answers costs exactly the one read it always did. Each must return
 * the RAW value: null for a failed read, "" for an unserved key. Branching on
 * the raw value is the whole point; by the time it is parsed, `parse(null)` and
 * `parse("")` are both null and the distinction is gone.
 *
 * `parse` is injected so the caller keeps ownership of what a hierarchy is.
 * Unparseable JSON still falls back, which is what it did before — a truncated
 * declaration is a broken module, not a slow one.
 *
 * Returns { action, hierarchy?, reason }.
 */
export function decideComponentEntry(read, parse) {
    const rawHierarchy = read.hierarchy();

    if (rawHierarchy === null || rawHierarchy === undefined) {
        return { action: ENTRY_HOLD, reason: "hierarchy-read-failed" };
    }

    if (rawHierarchy !== "") {
        const hierarchy = parse(rawHierarchy);
        if (hierarchy) return { action: ENTRY_ENTER, hierarchy, reason: "declared" };
        return { action: ENTRY_FALLBACK, reason: "hierarchy-unparseable" };
    }

    /* "" — served, but empty. Who is in this position? */
    const rawModule = read.module();
    if (rawModule === null || rawModule === undefined) {
        return { action: ENTRY_HOLD, reason: "module-read-failed" };
    }
    if (rawModule === "") {
        /*
         * An occupied position that cannot yet name its module.
         *
         * Not reachable for a genuinely empty one: an empty box in the chain
         * diagram is a `+` and opens the picker, never the editor. So the only
         * way here is a position the user watched fill and clicked into before
         * the chain host finished putting the module in it.
         */
        return { action: ENTRY_HOLD, reason: "module-not-published" };
    }

    /* Named, so the module is in. Does it say it is still coming up? */
    if (read.isLoading() === "1") {
        return { action: ENTRY_HOLD, reason: "module-reports-loading" };
    }

    return { action: ENTRY_FALLBACK, reason: "declares-no-hierarchy" };
}
