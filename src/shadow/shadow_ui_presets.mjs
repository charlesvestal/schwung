/*
 * Shadow UI - Module Presets (PRESETS list, PRESET_DETAIL).
 *
 * Module presets are *single-component* snapshots — one chain component's own
 * state (a synth, an audio FX, or a MIDI FX) — as opposed to chain "patches"
 * (shadow_ui_patches.mjs), which capture the whole signal chain into a single
 * global library.
 *
 * Design:
 *   - Generic across every chain component: a preset is the component's
 *     `<prefix>:state` blob (synth | fx1..fx4 | midi_fx1 — the same opaque
 *     state the host already reads for slot autosave), so no per-module code
 *     is needed.
 *   - Stored per-module under  /data/UserData/schwung/presets/<module-id>/<name>.json
 *     so the browser is filtered to the slot's current module *for free* — we
 *     only ever list that one folder.
 *   - Recall is the verified slot-load path: set_param("<prefix>:state", blob).
 *   - Live audition: scrolling the list applies the highlighted preset (after a
 *     short debounce, driven by tickPresetPreview from the host's global tick)
 *     so you hear it before committing. The slot's original :state is captured
 *     on entry; Back reverts to it. PICKING a preset LOADS it immediately and
 *     commits — there is no per-preset detail screen to Load or Delete from.
 *     If the original can't be captured, preview is disabled (load-on-pick only).
 *   - Save never overwrites: a name collision auto-appends a number
 *     ("Fat Brass" -> "Fat Brass 2"). Delete removes the single file.
 *
 * File format:
 *   { "name": "Fat Brass", "module": "obxd", "version": 1, "state": <blob> }
 *   `state` is the parsed synth:state object when it is JSON, otherwise the
 *   raw opaque string (mirrors how buildSlotPatchJson stores synth state).
 *
 * The browser is exactly ONE thing: choose a preset. Save, Save As and
 * Delete are NOT here — they live on the component's own knob-grid "My
 * Presets" page (shadow_ui.js componentTrailingMenus /
 * runComponentActionFromGrid), which is also where this file's grid-only
 * entry points below (overwriteUserPreset, enterPresetSaveAs,
 * enterPresetDeleteConfirm) are reached from. Reported from hardware, three
 * times over, all one cause: "loading a preset shouldnt show load/delete, it
 * should just load it (delete is on the main menu)"; "after deleting i get to
 * a menu of [save current] not the preset (none) page"; "i also see [save
 * current] if i load without saving" — the verbs had moved to the grid page
 * but the browser still offered its own copies.
 *
 * Entry points: Shift+Click any loaded chain component (synth / FX / MIDI FX)
 * -> module picker -> "[User Presets]" (indented row tucked just beneath the
 * loaded module; injected in enterComponentSelect as the __user_presets__
 * synthetic entry and cursor-defaulted there, routed from
 * applyComponentSelection with the component key + DSP prefix + module id) --
 * AND the knob grid's "My Presets" page's "Load…" action, which reaches
 * enterPresetBrowser directly. Either way, picking a row here loads it and
 * returns to the chain editor; maybeReturnToComponentGrid in shadow_ui.js
 * is what routes that arrival back onto the My Presets page for the
 * grid-driven flow (see its own note on the restorePageName it uses).
 *
 * State accessors come from the shared `ctx` (populated by shadow_ui.js); see
 * shadow_ui_ctx.mjs. As with the other view modules, only touch ctx.* inside
 * function bodies, never at top level.
 */
import * as os from 'os';
import { ctx } from './shadow_ui_ctx.mjs';
import { truncateText } from '/data/UserData/schwung/shared/chain_ui_views.mjs';
/* The knob-grid chrome. These screens are reached from a component and go
 * straight back to one, so they are steps inside that flow and should look
 * like it — see drawPresets. */
import { drawHeader as drawMovyHeader, drawFooter as drawMovyFooter, RULE_Y as MOVY_RULE_Y }
    from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';
import { renderPicker as renderMovyPicker }
    from '/data/UserData/schwung/shared/param_pages/render_page.mjs';
import { MENU_LIST_X as MOVY_LIST_X, MENU_LIST_Y as MOVY_LIST_Y, MENU_LIST_W as MOVY_LIST_W }
    from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
import {
    announce, announceMenuItem
} from '/data/UserData/schwung/shared/screen_reader.mjs';
import { openTextEntry } from '/data/UserData/schwung/shared/text_entry.mjs';

/* ---- Constants ---------------------------------------------------------- */

const PRESET_ROOT = "/data/UserData/schwung/presets";
const PRESET_VERSION = 1;

/* ---- Module-local state ------------------------------------------------- */

let presetModule = "";        /* module id for the active component (folder key) */
let presetModuleLabel = "";   /* human label (module name) for the header */
let presetPrefix = "synth";   /* DSP param prefix: synth | fx1..fx4 | midi_fx1 */
let presets = [];             /* [{ name, file }] sorted by name */
let selectedPreset = 0;       /* index into `presets` */

/* VIEWS.PRESET_DETAIL now has exactly one reason to exist: the delete
 * confirmation raised directly by enterPresetDeleteConfirm (the grid's
 * Delete action). There is no more Load/Delete choice screen to select
 * within it — picking a preset in the list loads it immediately. */
let confirmingDelete = false;
let confirmIndex = 0;         /* 0 = No, 1 = Yes */

/* ---- Live preview (audition on scroll) ---------------------------------
 * Scrolling the list applies the highlighted preset live so you hear it
 * before committing; backing out reverts to the slot's original sound. The
 * apply is debounced through the global tick so fast scrolling doesn't reload
 * state on every detent. Disabled when we can't capture a revertible original
 * (then load only happens on an explicit Load). */
const PREVIEW_NONE = -1;          /* sentinel: nothing queued */
const PREVIEW_DELAY_TICKS = 7;    /* ~160ms at ~44Hz before a preview applies */

let originalState = null;         /* slot's <prefix>:state captured on entry */
let previewEnabled = false;       /* true once originalState is captured */
let previewActive = false;        /* a non-original preset is currently applied */
let pendingPreviewIndex = PREVIEW_NONE; /* displayed-row index queued to apply */
let previewDelay = 0;             /* ticks left before applying pendingPreviewIndex */
let lastPreviewedIndex = -1;      /* last displayed-row index actually applied */

/* ---- Helpers ------------------------------------------------------------ */

function presetDir(moduleId) {
    return `${PRESET_ROOT}/${moduleId}`;
}

/* Turn a user-facing preset name into a safe-ish filename stem. Keep it simple:
 * strip path separators and trim; spaces are fine on the device filesystem. */
function safeFileStem(name) {
    return name.replace(/[\/\\\x00-\x1f]/g, "").trim() || "Preset";
}

/* Read the "name" field out of a preset JSON without a full parse. */
function parsePresetName(path) {
    try {
        if (typeof host_read_file !== "function") return null;
        const raw = host_read_file(path);
        if (!raw) return null;
        const match = raw.match(/"name"\s*:\s*"([^"]+)"/);
        if (match && match[1]) return match[1];
    } catch (e) {
        return null;
    }
    return null;
}

/* Populate `presets` from the current module's folder. Safe when the folder
 * does not exist yet (no presets saved) — yields an empty list. */
export function loadPresetList() {
    presets = [];
    if (!presetModule) return;
    let dir = [];
    try {
        dir = os.readdir(presetDir(presetModule)) || [];
    } catch (e) {
        dir = [];
    }
    const names = dir[0];
    if (!Array.isArray(names)) return;
    for (const name of names) {
        if (name === "." || name === "..") continue;
        if (!name.endsWith(".json")) continue;
        const path = `${presetDir(presetModule)}/${name}`;
        const presetName = parsePresetName(path) || name.replace(/\.json$/, "");
        presets.push({ name: presetName, file: name });
    }
    presets.sort((a, b) => {
        const al = a.name.toLowerCase();
        const bl = b.name.toLowerCase();
        if (al < bl) return -1;
        if (al > bl) return 1;
        return 0;
    });
}

/* True if any existing preset already uses this display name (case-insensitive). */
function nameExists(name) {
    const lower = name.toLowerCase();
    return presets.some((p) => p.name.toLowerCase() === lower);
}

/* Resolve a non-colliding name by appending " 2", " 3", … as needed. */
function uniquePresetName(name) {
    if (!nameExists(name)) return name;
    let n = 2;
    let candidate = `${name} ${n}`;
    while (nameExists(candidate)) {
        n++;
        candidate = `${name} ${n}`;
    }
    return candidate;
}

/* Resolve a non-colliding file path (defensive — display-name dedup usually
 * already guarantees this, but two names can map to the same safe stem). */
function uniquePresetPath(dir, stem) {
    let path = `${dir}/${stem}.json`;
    if (typeof host_file_exists !== "function" || !host_file_exists(path)) return path;
    let n = 2;
    path = `${dir}/${stem} ${n}.json`;
    while (host_file_exists(path)) {
        n++;
        path = `${dir}/${stem} ${n}.json`;
    }
    return path;
}

/* ---- Save --------------------------------------------------------------- */

function defaultSaveName(slot) {
    const { getSlotParam } = ctx;
    return getSlotParam(slot, presetPrefix + ":preset_name") || presetModuleLabel || "Preset";
}

/* Save/Save As live on the component's My Presets grid page now (up_save /
 * up_save_as, see shadow_ui.js runComponentActionFromGrid); there is no
 * "[Save current…]" row in this list to start a save flow from. Only
 * enterPresetSaveAs below opens the keyboard, straight through doSavePreset. */
function doSavePreset(slot, rawName) {
    const { getSlotStateWithRetry } = ctx;

    const stateJson = getSlotStateWithRetry(slot, presetPrefix + ":state");
    /*
     * A read has THREE answers, and only one of them is an error: `null` is a
     * FAILED read (writing it would replace a good preset with nothing --
     * see overwriteUserPreset's identical guard); "" is the module declaring
     * NO state, which is a real answer and is written through like any
     * other. `if (!stateJson)` collapsed the two, and this repo has a
     * documented case where exactly that collapse caused three separate bugs
     * in one day (see param_read_null_vs_empty). Pre-existing here, but
     * enterPresetSaveAs is a NEW entry point onto this same function, so a
     * component that legitimately declares "" state and is Saved As from the
     * grid would hit the bug for the first time through code added in this
     * task -- worth fixing now rather than widening what reaches it.
     */
    if (stateJson === null || stateJson === undefined) {
        showSaveError();
        return;
    }
    const savedPrefix = presetPrefix;

    const dir = presetDir(presetModule);
    if (typeof host_ensure_dir === "function") host_ensure_dir(dir);

    /* Never overwrite — dedup the display name, then the file path. */
    const name = uniquePresetName(rawName);
    const path = uniquePresetPath(dir, safeFileStem(name));

    /* Store the state as a parsed object when it is JSON, else the raw string
     * (matches buildSlotPatchJson's opaque-state fallback). */
    let state;
    try {
        state = JSON.parse(stateJson);
    } catch (e) {
        state = stateJson;
    }

    const payload = JSON.stringify({
        name: name,
        module: presetModule,
        version: PRESET_VERSION,
        state: state
    });

    let ok = false;
    if (typeof host_write_file === "function") ok = host_write_file(path, payload);

    if (!ok) {
        showSaveError();
        return;
    }

    loadPresetList();
    /* Keep the just-saved preset highlighted, in case the list is shown next. */
    const idx = presets.findIndex((p) => p.name === name);
    selectedPreset = idx >= 0 ? idx : 0;
    announce(`Saved ${name}`);
    ctx.needsRedraw = true;
    /* Tell the grid's My Presets row what is now loaded, whether or not the
     * grid is the thing that asked for this save — the hook is a no-op when
     * it is not up. Both writers that can produce a preset (this one, via
     * Save/Save As on the grid page) run through here, so the record can
     * never fall out of step with the disk. */
    if (typeof ctx.onPresetSaved === "function") {
        ctx.onPresetSaved(slot, savedPrefix, name, stateJson);
    }
}

function showSaveError() {
    if (typeof ctx.showWarning === "function") {
        ctx.showWarning("Save Failed", "Could not read module state. Try again.");
    } else {
        announce("Save failed");
    }
    ctx.needsRedraw = true;
}

/* ---- Load --------------------------------------------------------------- */

/* Read a preset file and return its state as a string blob (or null on any
 * error). Only announces on error when `loud` — the silent path is used by the
 * scroll-audition preview, which must not chatter. */
function readPresetStateString(entry, loud) {
    if (!entry) return null;
    let raw = null;
    try {
        if (typeof host_read_file === "function") {
            raw = host_read_file(`${presetDir(presetModule)}/${entry.file}`);
        }
    } catch (e) {
        raw = null;
    }
    if (!raw) {
        if (loud) announce("Could not read preset");
        return null;
    }
    let obj;
    try {
        obj = JSON.parse(raw);
    } catch (e) {
        if (loud) announce("Preset file is corrupt");
        return null;
    }
    /* Guard against a stray cross-module file (shouldn't happen — folders are
     * module-scoped — but the state blob is module-specific so be safe). */
    if (obj.module && presetModule && obj.module !== presetModule) {
        if (loud) announce("Preset is for a different module");
        return null;
    }
    const state = obj.state;
    return (typeof state === "string") ? state : JSON.stringify(state);
}

/* Apply a raw <prefix>:state blob to the slot. Recall == the slot-load restore
 * path. Setting <prefix>:state marks the slot dirty; the next autosave (~10s)
 * persists it into the slot. */
function applyStateBlob(slot, str) {
    if (str == null) return false;
    ctx.setSlotParam(slot, presetPrefix + ":state", str);
    return true;
}

/* Explicit Load (commit): read + apply, with error announcements. Scrolling
 * the list auditions live but must NOT reach this — nothing is committed
 * until Load, which is also the only place that updates the grid's "which
 * preset is loaded" record. */
function applyPreset(slot, entry) {
    const str = readPresetStateString(entry, true);
    const ok = applyStateBlob(slot, str);
    if (ok && typeof ctx.onPresetLoaded === "function") {
        ctx.onPresetLoaded(slot, presetPrefix, entry ? entry.name : "", str);
    }
    return ok;
}

/* ---- Live preview ------------------------------------------------------- */

/* Apply the preset at `rowIndex` in `presets`, silently. Every row here is a
 * real preset now — there is no synthetic "restore the original" row, since
 * the list no longer carries a save affordance of its own (see Back for the
 * actual revert). */
function applyPreviewForRow(rowIndex) {
    const slot = ctx.selectedSlot;
    const str = readPresetStateString(presets[rowIndex], false);
    if (str != null) {
        applyStateBlob(slot, str);
        previewActive = true;
    }
}

/* Queue a debounced preview of the highlighted row (no-op if disabled). */
function queuePreview(rowIndex) {
    if (!previewEnabled) return;
    pendingPreviewIndex = rowIndex;
    previewDelay = PREVIEW_DELAY_TICKS;
}

/* True while a non-original preset is being auditioned (applied but not yet
 * committed via Load). Lets the host skip autosave so an uncommitted audition
 * is never persisted into slot_N.json. */
export function isPresetPreviewActive() {
    return previewActive;
}

/* Re-apply the captured original (cancel an active preview). */
function revertToOriginal() {
    pendingPreviewIndex = PREVIEW_NONE;
    if (previewEnabled && previewActive && originalState != null) {
        applyStateBlob(ctx.selectedSlot, originalState);
    }
    previewActive = false;
    lastPreviewedIndex = -1;
}

/* Driven unconditionally from the host's global tick; self-gates on pending. */
export function tickPresetPreview() {
    if (!previewEnabled || pendingPreviewIndex === PREVIEW_NONE) return;
    if (previewDelay > 0) { previewDelay--; return; }
    const row = pendingPreviewIndex;
    pendingPreviewIndex = PREVIEW_NONE;
    if (row === lastPreviewedIndex) return;
    lastPreviewedIndex = row;
    applyPreviewForRow(row);
}

/* ---- Delete ------------------------------------------------------------- */

function doDeletePreset(entry) {
    if (!entry) return;
    try {
        os.remove(`${presetDir(presetModule)}/${entry.file}`);
    } catch (e) {
        /* ignore — refresh below reflects reality either way */
    }
    loadPresetList();
    /* The grid's record clears only when the DELETED name matches the one
     * currently loaded — deleting some other saved preset from the list must
     * not disturb it. That comparison is the host's (it owns the record); this
     * only reports what happened. */
    if (typeof ctx.onPresetDeleted === "function") {
        ctx.onPresetDeleted(ctx.selectedSlot, presetPrefix, entry.name);
    }
}

/* ---- Enter -------------------------------------------------------------- */

export function enterPresetBrowser(slotIndex, componentKey, moduleId, prefix) {
    const { setView, updateFocusedSlot, getSlotParam, VIEWS } = ctx;
    ctx.selectedSlot = slotIndex;
    updateFocusedSlot(slotIndex);

    /* Component-scoped: synth or any FX / MIDI-FX slot. The DSP prefix selects
     * which component's :state we snapshot; moduleId keys the presets folder. */
    presetPrefix = prefix || "synth";
    presetModule = moduleId || "";
    presetModuleLabel = getSlotParam(slotIndex, presetPrefix + ":name") || presetModule || "Module";

    loadPresetList();
    selectedPreset = 0;
    confirmingDelete = false;

    /* Capture the slot's current state so scroll-audition can revert on cancel.
     * If we can't read it, disable preview (no safe undo) and fall back to the
     * old behaviour: load only on an explicit Load.
     *
     * Gated on Global Settings -> Audition, the same switch the file browser
     * uses to decide whether highlighting a WAV plays it. Auditioning APPLIES
     * the highlighted preset to the live slot, so it is not something to do to
     * someone who did not ask for it — and this list is far easier to land on
     * than it used to be, now that it is a page at the end of every component
     * rather than an indented row inside a picker. Default is off.
     *
     * Off does not disable the list, only the audition: Load still loads. */
    originalState = null;
    previewEnabled = false;
    previewActive = false;
    pendingPreviewIndex = PREVIEW_NONE;
    previewDelay = 0;
    /* -1, not selectedPreset: nothing has been auditioned yet, so the first
     * jog (or the initial highlight, if it is ever flushed) is a real change
     * rather than a no-op match. */
    lastPreviewedIndex = -1;
    const auditionOn = typeof ctx.auditionEnabled === "function" ? !!ctx.auditionEnabled() : false;
    if (presetModule && auditionOn) {
        const cur = ctx.getSlotStateWithRetry(slotIndex, presetPrefix + ":state");
        if (cur) {
            originalState = cur;
            previewEnabled = true;
        }
    }

    setView(VIEWS.PRESETS);
    ctx.needsRedraw = true;

    if (!presetModule) {
        announce("Presets, no module in this slot");
    } else {
        announce(`${presetModuleLabel} Presets, ${presets.length} saved`);
    }
}

/* ---- Grid-driven actions -------------------------------------------------
 *
 * The knob grid's "My Presets" page (shadow_ui.js, componentTrailingMenus /
 * runComponentActionFromGrid) offers Load / Save / Save As / Delete without
 * detouring through the module picker. Load reuses enterPresetBrowser
 * outright, unchanged — picking a row there now loads it directly (see
 * handlePresetsSelect), so there is no per-preset detail screen in this flow
 * at all. Save and Delete need entry points below because neither existing
 * flow does what they need: Save never overwrites (a name collision
 * auto-appends a number, by design), and Delete needs to reach the SAME
 * confirm screen without ever going through the list.
 */

/*
 * Overwrite the named preset's state, in place. Refuses on a FAILED read
 * (null/undefined) — a timed-out `<prefix>:state` must never replace a good
 * preset with nothing. An EMPTY declared state ("") is a real answer from the
 * module and is written. Returns true on success.
 */
export function overwriteUserPreset(slot, prefix, moduleId, name) {
    if (!moduleId || !name) return false;
    const dsPrefix = prefix || "synth";
    const stateJson = ctx.getSlotStateWithRetry(slot, dsPrefix + ":state");
    if (stateJson === null || stateJson === undefined) {
        /* A FAILED read, not a declared-empty state ("" is written through
         * below like any other answer) -- refusing is silent otherwise, and
         * on-device that reads identically to a disk-write failure or a
         * missing entry: the user just sees "Save failed" with no way to
         * tell which. */
        if (typeof host_log === "function") {
            host_log("presets: overwrite refused, " + dsPrefix + ":state read failed for slot " + slot);
        }
        return false;
    }

    presetPrefix = dsPrefix;
    presetModule = moduleId;
    loadPresetList();
    const entry = presets.find((p) => p.name === name);
    if (!entry) return false;

    let state;
    try {
        state = JSON.parse(stateJson);
    } catch (e) {
        state = stateJson;
    }
    const payload = JSON.stringify({
        name: name,
        module: moduleId,
        version: PRESET_VERSION,
        state: state
    });
    let ok = false;
    if (typeof host_write_file === "function") {
        ok = host_write_file(`${presetDir(moduleId)}/${entry.file}`, payload);
    }
    if (ok) loadPresetList();
    return ok;
}

/*
 * Straight to the keyboard — no browser, no list. Commits through the SAME
 * doSavePreset (never-overwrite, auto-dedup name) as every other writer, so
 * there is one save implementation rather than two; doSavePreset itself
 * notifies ctx.onPresetSaved once the write lands, which is what updates the
 * grid's record and refreshes the trailing page.
 */
export function enterPresetSaveAs(slot, componentKey, moduleId, prefix) {
    if (!moduleId) {
        announce("No module in this slot");
        return;
    }
    presetPrefix = prefix || "synth";
    presetModule = moduleId;
    presetModuleLabel = ctx.getSlotParam(slot, presetPrefix + ":name") || presetModule || "Module";
    loadPresetList();
    openTextEntry({
        title: "",
        initialText: defaultSaveName(slot),
        onAnnounce: announce,
        onConfirm: (name) => {
            doSavePreset(slot, (name || "").trim() || "Preset");
        },
        onCancel: () => {
            announce("Save cancelled");
            ctx.needsRedraw = true;
        }
    });
}

/*
 * The SAME confirm-delete screen the detail view raises (VIEWS.PRESET_DETAIL,
 * confirmingDelete), entered directly rather than through the list — one
 * delete path, one confirmation. Confirming or backing out falls through to
 * the existing handlers unchanged.
 */
export function enterPresetDeleteConfirm(slot, componentKey, moduleId, prefix, name) {
    const { setView, updateFocusedSlot, VIEWS } = ctx;
    presetPrefix = prefix || "synth";
    presetModule = moduleId || "";
    loadPresetList();
    const idx = presets.findIndex((p) => p.name === name);
    if (idx < 0) {
        /* The grid's record and the disk have fallen out of step (the file
         * was removed from under it). Nothing safe to confirm against. */
        announce("Preset not found");
        return;
    }
    presetModuleLabel = ctx.getSlotParam(slot, presetPrefix + ":name") || presetModule || "Module";
    ctx.selectedSlot = slot;
    updateFocusedSlot(slot);
    selectedPreset = idx;
    confirmingDelete = true;
    confirmIndex = 0;
    setView(VIEWS.PRESET_DETAIL);
    ctx.needsRedraw = true;
    announce(`Delete ${name}?`);
}

/* ---- Draw --------------------------------------------------------------- */

/* One draw context for this file: the three globals the shared renderers use. */
function movyCtx() {
    return { fillRect: fill_rect, print, textWidth: text_width };
}

/* The list rect every screen in the page chrome shares — see MENU_LIST_*. */
function movyRect() {
    return { x: MOVY_LIST_X, y: MOVY_LIST_Y, w: MOVY_LIST_W, h: MOVY_RULE_Y - MOVY_LIST_Y };
}

export function drawPresets() {
    clear_screen();
    const c = movyCtx();
    drawMovyHeader(c, truncateText(presetModuleLabel, 16), "PRESETS", false);

    if (!presetModule) {
        print(MOVY_LIST_X, MOVY_LIST_Y + 8, "No module in slot", 1);
        drawMovyFooter(c, [["BACK", "EXIT"]]);
        return;
    }

    /* Nothing saved: a real state, not an empty list drawn as a blank rect --
     * Save/Save As live on the My Presets grid page now, so this screen has
     * nothing else to offer. */
    if (!presets.length) {
        print(MOVY_LIST_X, MOVY_LIST_Y + 8, "No presets saved", 1);
        drawMovyFooter(c, [["BACK", "EXIT"]]);
        return;
    }

    renderMovyPicker(c, {
        rect: movyRect(),
        entries: presets.map((p) => ({ name: p.name, value: "" })),
        index: selectedPreset,
        header: false,
    });
    /* CLK LOAD, not CLK OPEN: picking a row loads it immediately -- there is
     * no detail screen behind it any more (see handlePresetsSelect). */
    drawMovyFooter(c, [["JOG", "SEL"], ["CLK", "LOAD"], ["BACK", "EXIT"]]);
}

/*
 * The ONLY thing left on this screen: the delete confirmation, raised
 * directly by enterPresetDeleteConfirm (the grid's Delete action). There is
 * no more Load/Delete choice screen behind it — picking a preset in the list
 * loads it immediately (see handlePresetsSelect) — so confirmingDelete is
 * true on every arrival here and this draws nothing else.
 */
export function drawPresetDetail() {
    clear_screen();
    const c = movyCtx();
    const entry = presets[selectedPreset];
    const name = entry ? entry.name : "Preset";

    /*
     * The header right says what is being asked and the body is the two
     * answers, in the same list every other screen here uses. It used to draw
     * its own full-width rows in a second geometry, which made the most
     * destructive step in this flow the one that looked least like the rest
     * of it.
     */
    drawMovyHeader(c, truncateText(name, 16), "DELETE?", false);
    renderMovyPicker(c, {
        rect: movyRect(),
        entries: [{ name: "No", value: "" }, { name: "Yes", value: "" }],
        index: confirmIndex,
        header: false,
    });
    drawMovyFooter(c, [["JOG", "SEL"], ["CLK", "GO"], ["BACK", "OUT"]]);
}

/* ---- Jog ---------------------------------------------------------------- */

export function handlePresetsJog(delta) {
    if (!presets.length) return;
    selectedPreset = Math.max(0, Math.min(presets.length - 1, selectedPreset + delta));
    queuePreview(selectedPreset);
    announceMenuItem("Preset", presets[selectedPreset].name);
}

export function handlePresetDetailJog(delta) {
    /* Only the delete confirm remains here — see drawPresetDetail's note. */
    confirmIndex = Math.max(0, Math.min(1, confirmIndex + delta));
    announceMenuItem("Confirm", confirmIndex === 1 ? "Yes" : "No");
}

/* ---- Select ------------------------------------------------------------- */

export function handlePresetsSelect() {
    /*
     * A bare `return` here would be a dead button: the row is drawn, the
     * click does nothing, and NOTHING is announced. Reported from hardware
     * once already for the old save row ("when I choose save, simply nothing
     * happens") — the state is contradictory and worth saying so: this screen
     * is only reachable through a [User Presets] row, or the grid's Load…
     * action, that are shown ONLY when the same lookup found a module, so
     * arriving here without one means the config changed underneath us
     * between building the picker and this click.
     */
    if (!presetModule) {
        announce("No module in this slot");
        if (typeof host_log === "function") {
            host_log("presets: load refused, presetModule empty for prefix " + presetPrefix);
        }
        return;
    }
    if (!presets.length) return;

    /*
     * The browser is exactly ONE thing: choose a preset. Picking a row LOADS
     * it and commits, straight away — no per-preset Load/Delete detail
     * screen. Reported from hardware: "loading a preset shouldnt show
     * load/delete, it should just load it (delete is on the main menu)".
     */
    const { setView, VIEWS } = ctx;
    const entry = presets[selectedPreset];
    if (applyPreset(ctx.selectedSlot, entry)) {
        previewActive = false;
        pendingPreviewIndex = PREVIEW_NONE;
        announce(`Loaded ${entry.name}`);
        /* CHAIN_EDIT is the convergence point every hand-off from the grid's
         * My Presets page returns through — maybeReturnToComponentGrid (in
         * shadow_ui.js) is what routes a grid-driven arrival back onto that
         * page specifically; a [User Presets]-row arrival (no grid open)
         * lands plainly on the chain editor, as it always has. */
        setView(VIEWS.CHAIN_EDIT);
    }
    ctx.needsRedraw = true;
}

export function handlePresetDetailSelect() {
    /* Only the delete confirm remains here — see drawPresetDetail's note. */
    const { setView, VIEWS } = ctx;
    const entry = presets[selectedPreset];

    if (confirmIndex === 1) {
        doDeletePreset(entry);
        confirmingDelete = false;
        /* The deleted preset may have been the one being auditioned (only
         * possible via a stale audition from an earlier list visit, since
         * Delete no longer goes through the list) — restore the original
         * live sound rather than leave a deleted preset's state playing. */
        revertToOriginal();
        /*
         * Back to CHAIN_EDIT, not the list: Delete is reached exclusively
         * from the grid's My Presets page now, and that page is where the
         * result belongs. Reported from hardware: "after deleting i get to a
         * menu of [save current] not the preset (none) page".
         */
        setView(VIEWS.CHAIN_EDIT);
        announce("Preset deleted");
    } else {
        confirmingDelete = false;
        setView(VIEWS.CHAIN_EDIT);
    }
    ctx.needsRedraw = true;
}

/* ---- Back --------------------------------------------------------------- */

export function handlePresetsBack() {
    const { setView, VIEWS } = ctx;
    /* Entered from the module picker (Shift+Click on the synth block) or the
     * grid's Load… action; Back cancels the whole flow — revert any active
     * audition to the original sound — and exits to the chain editor. */
    revertToOriginal();
    setView(VIEWS.CHAIN_EDIT);
    announce("Chain Editor");
    ctx.needsRedraw = true;
}

export function handlePresetDetailBack() {
    /* The only screen behind this one now is the delete confirm — cancel it
     * outright, straight back to the chain editor. There is no Load/Delete
     * choice screen to fall back into any more. */
    const { setView, VIEWS } = ctx;
    confirmingDelete = false;
    setView(VIEWS.CHAIN_EDIT);
    announce("Chain Editor");
    ctx.needsRedraw = true;
}
