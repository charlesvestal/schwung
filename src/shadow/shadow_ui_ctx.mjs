/*
 * Shared context for shadow UI view modules.
 *
 * Core shadow_ui.js populates this object with state accessors and utility
 * functions after its own declarations execute.  View modules import `ctx`
 * and access its properties *inside function bodies* (never at top-level),
 * since the context is only fully populated after shadow_ui.js init.
 */
export const ctx = {};
