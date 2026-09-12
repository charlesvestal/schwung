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
 * The module never arrived, and it never will.
 *
 * A FOURTH ANSWER, because with the FX load moved off the SPI callback there
 * is now a state that is neither "here" nor "on its way": a dlopen or a
 * create_instance that failed leaves the position genuinely EMPTY, which reads
 * exactly like a position whose module has not landed yet — and the hold above
 * never gives up on purpose, so that would wait forever on a module that is
 * not coming. The shim answers `<prefix>:load_error` with "1" for exactly that
 * case (shadow_chain_mgmt.c, shadow_fx_load_state), and it is the only thing
 * that can tell the two apart from out here.
 */
export const ENTRY_FAILED = "failed";

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
/*
 * How many failed hierarchy reads a NAMED, NOT-LOADING module gets before the
 * gate concludes the read is not going to complete because the module does not
 * serve the key at all.
 *
 * A module that draws its own pages (ui_chain.js) may answer `ui_hierarchy`
 * with an ERROR rather than "" -- from the shadow side that is the same null
 * as a timed-out read, and holding for it is holding forever: a swap into such
 * a module sat on "Loading..." until the user backed out (reported from
 * hardware for 9W9). Once the chain host has named the module and it reports
 * not loading, a read that STILL keeps failing is refused, not late. Fall
 * back, exactly as a served "" would.
 *
 * IT IS THE END OF THE FAST PHASE, and that is derived rather than chosen.
 * `isLoading` is the term that would make a short limit safe, and for a SLOT
 * component there is no such term: the chain host does not serve
 * `<prefix>:is_loading` at all (only the shim does, for Master FX, sends and
 * bus inserts), so the read answers "" or null and `!== "1"` is true for every
 * slot component in the fleet. What is left is "named, and three reads
 * failed", and a few consecutive timed-out reads are exactly what a large
 * contract over a busy param channel looks like -- so a short limit re-opens
 * the bug this whole gate exists to close, IRREVERSIBLY, and leaves the
 * MiniJV/Osirus blank editor one flaky window away.
 *
 * The fast phase is already the written-down answer to "could this still be
 * arriving?" -- HOLD_FAST_LIMIT probes at HOLD_FAST_INTERVAL_TICKS, sized to
 * cover a dlopen of a large plugin and a fork-and-boot like Osirus's. Past it
 * the probe SLOWS because the module is not expected any more, and that is the
 * honest place to conclude the key is not served. It costs a misbehaving
 * module ~20 s of "Loading..." before its editor opens -- the by-contract fix
 * is to answer "" (9W9 does now), and this is only the net under the modules
 * that do not. Nothing correct can be misjudged by it.
 */
export const HOLD_UNSERVED_READ_LIMIT = HOLD_FAST_LIMIT;

export function decideComponentEntry(read, parse, attempts = 0) {
    /*
     * ASKED FIRST, and only where the caller supplies it: a position that has
     * just failed to load answers "" to everything else, so every branch below
     * would read it as "still coming" and hold. Optional because the slot
     * chain's components have no such key — a reader without loadError behaves
     * exactly as it did before this existed.
     */
    if (read.loadError && read.loadError() === "1") {
        return { action: ENTRY_FAILED, reason: "load-failed" };
    }

    const rawHierarchy = read.hierarchy();

    if (rawHierarchy === null || rawHierarchy === undefined) {
        if (attempts >= HOLD_UNSERVED_READ_LIMIT) {
            const named = read.module();
            if (named && read.isLoading() !== "1") {
                return { action: ENTRY_FALLBACK, reason: "hierarchy-not-served" };
            }
        }
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
