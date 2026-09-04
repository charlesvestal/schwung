/**
 * widget_registry.mjs — the custom widgets this host knows how to draw.
 *
 * "custom:" IS A RESERVED PREFIX. No built-in viz kind may ever be named into
 * that namespace, so the two sets cannot collide as built-ins are added.
 *
 * REGISTRATION IS WHAT MAKES A CUSTOM KIND REAL. viz.mjs collectDeclared claims
 * a key's cell as it walks; a custom kind that is not registered here simply
 * does not claim, so its keys stay in the detector pool and a built-in widget
 * draws instead. That single behaviour is the whole fall-through story, and it
 * covers four different failures with one code path:
 *
 *   - an author typo in the kind name
 *   - a widget whose canvas.js failed to load
 *   - an OLDER HOST reading a NEWER module -- it has never heard of the name,
 *     so the module still draws something sensible rather than a hole
 *   - a widget disabled after throwing (drawCustom, below)
 *
 * Because they share a path, the forward-compatibility behaviour cannot rot
 * separately from the typo behaviour.
 *
 * NOT ON THE SPI CALLBACK. Everything here runs in the shadow_ui process at
 * SCHED_OTHER, so the realtime rules that govern module entry points
 * (create_instance, set_param, render_block) do not apply. The draw BUDGET
 * still does: a page render is 1.68ms.
 */

import { frameCtx } from "./frame_ctx.mjs";

const CUSTOM_PREFIX = "custom:";

/*
 * Bumped whenever the set of drawable widgets changes.
 *
 * Consumers CACHE viz resolution -- page_controller keys its vizCache on
 * fingerprint, page and child index, none of which change when a widget
 * registers. Without a generation in that key, a page resolved before the
 * module's canvas.js loaded keeps being handed back, and the widget never
 * appears no matter how correctly it registered. That is the same shape as the
 * stale-alias bug already recorded beside that cache.
 */
let generation = 0;
export function widgetsGeneration() { return generation; }

/** kind -> { draw, nominal } */
const widgets = new Map();
/** kinds disabled this session after a throw. Cleared only by clearWidgets. */
const disabled = new Set();

export function isCustomKind(kind) {
    return typeof kind === "string" && kind.startsWith(CUSTOM_PREFIX);
}

/**
 * @param {string} kind  full kind string, including the "custom:" prefix
 * @param {object} impl  { draw(ctx, payload), nominal?: {w, h} }
 * @returns {boolean} whether it was accepted
 */
export function registerWidget(kind, impl) {
    if (!isCustomKind(kind) || !impl || typeof impl.draw !== "function") return false;
    widgets.set(kind, impl);
    generation++;
    return true;
}

/** Registered AND not disabled. This is the predicate viz.mjs consults. */
export function isWidgetAvailable(kind) {
    return widgets.has(kind) && !disabled.has(kind);
}

export function getWidget(kind) {
    return isWidgetAvailable(kind) ? widgets.get(kind) : null;
}

/**
 * Test seam, and the reset used when a component is unloaded.
 *
 * The registry is process-global and shadow_ui is long-lived, so a widget left
 * registered would outlive its module -- and a later module declaring the same
 * custom: name would silently inherit the wrong art.
 */
export function clearWidgets() {
    if (widgets.size || disabled.size) generation++;
    widgets.clear();
    disabled.clear();
}

/* Injected so this module stays pure and node-testable. On device shadow_ui
 * sets it to debugLog. */
let logFn = null;
export function setWidgetLogger(fn) { logFn = typeof fn === "function" ? fn : null; }

/**
 * Draw a custom group into `rect`, contained.
 *
 * ONE STRIKE. The first throw disables the kind for the session and the page
 * falls back to whatever the detector would have drawn -- so a user whose
 * module ships a broken widget still sees a CORRECT page, just not a custom
 * one, and the author sees the throw in debug.log. Same posture
 * page_controller takes on an unresolved contract: keep something correct on
 * screen, never let a failure become a picture.
 *
 * Catching every frame instead would flood the log and burn the frame budget
 * forever; not catching at all would take the shadow UI down for a user who
 * merely installed a module.
 *
 * @returns {boolean} true if the widget drew; false if the caller must fall back
 */
export function drawCustom(ctx, rect, group, values, metaIndex, anim, nowMs, baseValues) {
    const impl = getWidget(group.kind);
    if (!impl) return false;
    const fctx = frameCtx(ctx, rect);
    try {
        impl.draw(fctx, { group, values, metaIndex, anim, nowMs, baseValues });
    } catch (e) {
        disabled.add(group.kind);
        generation++;   /* a one-strike disable changes what is drawable too */
        if (logFn) logFn(`widget ${group.kind} disabled after throw: ${e}`);
        return false;
    }
    return true;
}
