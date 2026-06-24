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
 *     on entry; Back reverts to it, while the detail screen's Load commits.
 *     If the original can't be captured, preview is disabled (load-on-Load only).
 *   - Save never overwrites: a name collision auto-appends a number
 *     ("Fat Brass" -> "Fat Brass 2"). Delete removes the single file.
 *
 * File format:
 *   { "name": "Fat Brass", "module": "obxd", "version": 1, "state": <blob> }
 *   `state` is the parsed synth:state object when it is JSON, otherwise the
 *   raw opaque string (mirrors how buildSlotPatchJson stores synth state).
 *
 * Entry point: Shift+Click any loaded chain component (synth / FX / MIDI FX) ->
 * module picker -> "[User Presets]" (indented row tucked just
 * beneath the loaded module; injected in enterComponentSelect as the
 * __user_presets__ synthetic entry and cursor-defaulted there, routed from
 * applyComponentSelection with the component key + DSP prefix + module id).
 *
 * State accessors come from the shared `ctx` (populated by shadow_ui.js); see
 * shadow_ui_ctx.mjs. As with the other view modules, only touch ctx.* inside
 * function bodies, never at top level.
 */
import * as os from 'os';
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    LIST_TOP_Y,
    LIST_LABEL_X,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    announce, announceMenuItem
} from '/data/UserData/schwung/shared/screen_reader.mjs';
import { openTextEntry } from '/data/UserData/schwung/shared/text_entry.mjs';

/* ---- Constants ---------------------------------------------------------- */

const PRESET_ROOT = "/data/UserData/schwung/presets";
const PRESET_VERSION = 1;

/* Synthetic first row of the list — opens the keyboard to save current state. */
const SAVE_ROW_LABEL = "[Save current…]";

/* Detail-screen actions for a selected preset. */
const DETAIL_LOAD = 0;
const DETAIL_DELETE = 1;

/* ---- Module-local state ------------------------------------------------- */

let presetModule = "";        /* module id for the active component (folder key) */
let presetModuleLabel = "";   /* human label (module name) for the header */
let presetPrefix = "synth";   /* DSP param prefix: synth | fx1..fx4 | midi_fx1 */
let presets = [];             /* [{ name, file }] sorted by name, save-row excluded */
let selectedPreset = 0;       /* index into the *displayed* list (0 = save row) */

let selectedDetailItem = DETAIL_LOAD;
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

/* The displayed list = save row + presets. */
function displayedRows() {
    return [SAVE_ROW_LABEL, ...presets.map((p) => p.name)];
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

function startSaveFlow() {
    const slot = ctx.selectedSlot;
    if (!presetModule) {
        announce("No module in this slot");
        return;
    }
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

function doSavePreset(slot, rawName) {
    const { getSlotStateWithRetry } = ctx;

    const stateJson = getSlotStateWithRetry(slot, presetPrefix + ":state");
    if (!stateJson) {
        showSaveError();
        return;
    }

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
    /* Keep the just-saved preset highlighted. */
    const idx = presets.findIndex((p) => p.name === name);
    selectedPreset = idx >= 0 ? idx + 1 : 0;
    announce(`Saved ${name}`);
    ctx.needsRedraw = true;
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

/* Explicit Load (commit): read + apply, with error announcements. */
function applyPreset(slot, entry) {
    const str = readPresetStateString(entry, true);
    return applyStateBlob(slot, str);
}

/* ---- Live preview ------------------------------------------------------- */

/* Apply whatever the given displayed-row index represents, silently. Row 0
 * (the save row) means "restore the original live sound". */
function applyPreviewForRow(rowIndex) {
    const slot = ctx.selectedSlot;
    if (rowIndex <= 0) {
        if (originalState != null) applyStateBlob(slot, originalState);
        previewActive = false;
        return;
    }
    const str = readPresetStateString(presets[rowIndex - 1], false);
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

/* Apply the highlighted row immediately, cancelling any pending debounce —
 * used when leaving the list (into detail / save) so what you hear matches
 * what you'll act on. */
function flushPreview(rowIndex) {
    pendingPreviewIndex = PREVIEW_NONE;
    if (!previewEnabled) return;
    if (lastPreviewedIndex === rowIndex) return;
    lastPreviewedIndex = rowIndex;
    applyPreviewForRow(rowIndex);
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
     * old behaviour: load only on an explicit Load. */
    originalState = null;
    previewEnabled = false;
    previewActive = false;
    pendingPreviewIndex = PREVIEW_NONE;
    previewDelay = 0;
    lastPreviewedIndex = -1;       /* selectedPreset 0 already == original */
    if (presetModule) {
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

function enterPresetDetail(presetIndex) {
    const { setView, VIEWS } = ctx;
    selectedPreset = presetIndex;
    /* Land the preview now so the audition matches what Load will apply. */
    flushPreview(presetIndex);
    selectedDetailItem = DETAIL_LOAD;
    confirmingDelete = false;
    setView(VIEWS.PRESET_DETAIL);
    ctx.needsRedraw = true;
    const entry = presets[presetIndex - 1];
    announce(`${entry ? entry.name : "Preset"}, Load`);
}

/* ---- Draw --------------------------------------------------------------- */

export function drawPresets() {
    clear_screen();
    drawHeader(`${truncateText(presetModuleLabel, 12)} Presets`);

    if (!presetModule) {
        print(LIST_LABEL_X, LIST_TOP_Y, "No module in slot", 1);
        drawFooter("Back");
        return;
    }

    const rows = displayedRows();
    drawMenuList({
        items: rows.map((label) => ({ label })),
        selectedIndex: selectedPreset,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        getLabel: (item) => item.label
    });
    drawFooter({ left: "Back", right: "Click: open" });
}

export function drawPresetDetail() {
    clear_screen();
    const entry = presets[selectedPreset - 1];
    const name = entry ? entry.name : "Preset";
    drawHeader(truncateText(name, 18));

    if (confirmingDelete) {
        print(LIST_LABEL_X, LIST_TOP_Y, `Delete ${truncateText(name, 12)}?`, 1);
        const opts = ["No", "Yes"];
        for (let i = 0; i < opts.length; i++) {
            const y = LIST_TOP_Y + 14 + i * 12;
            const sel = i === confirmIndex;
            if (sel) fill_rect(0, y - 1, SCREEN_WIDTH, 12, 1);
            print(LIST_LABEL_X, y, `${sel ? "> " : "  "}${opts[i]}`, sel ? 0 : 1);
        }
        drawFooter({ left: "Back", right: "Click: confirm" });
        return;
    }

    const items = ["Load", "Delete"];
    for (let i = 0; i < items.length; i++) {
        const y = LIST_TOP_Y + i * 12;
        const sel = i === selectedDetailItem;
        if (sel) fill_rect(0, y - 1, SCREEN_WIDTH, 12, 1);
        print(LIST_LABEL_X, y, `${sel ? "> " : "  "}${items[i]}`, sel ? 0 : 1);
    }
    drawFooter({ left: "Back", right: "Click: select" });
}

/* ---- Jog ---------------------------------------------------------------- */

export function handlePresetsJog(delta) {
    const rows = displayedRows();
    selectedPreset = Math.max(0, Math.min(rows.length - 1, selectedPreset + delta));
    queuePreview(selectedPreset);
    announceMenuItem("Preset", rows[selectedPreset]);
}

export function handlePresetDetailJog(delta) {
    if (confirmingDelete) {
        confirmIndex = Math.max(0, Math.min(1, confirmIndex + delta));
        announceMenuItem("Confirm", confirmIndex === 1 ? "Yes" : "No");
        return;
    }
    selectedDetailItem = Math.max(DETAIL_LOAD, Math.min(DETAIL_DELETE, selectedDetailItem + delta));
    announceMenuItem("Action", selectedDetailItem === DETAIL_DELETE ? "Delete" : "Load");
}

/* ---- Select ------------------------------------------------------------- */

export function handlePresetsSelect() {
    if (!presetModule) return;
    if (selectedPreset === 0) {
        /* Save snapshots the live state — make sure a lingering preview has
         * reverted to the original first. */
        flushPreview(0);
        startSaveFlow();
        return;
    }
    enterPresetDetail(selectedPreset);
}

export function handlePresetDetailSelect() {
    const { setView, VIEWS } = ctx;
    const entry = presets[selectedPreset - 1];

    if (confirmingDelete) {
        if (confirmIndex === 1) {
            doDeletePreset(entry);
            confirmingDelete = false;
            selectedPreset = 0;
            /* The deleted preset was the one being auditioned — restore the
             * original live sound now that we're back on the save row. */
            revertToOriginal();
            setView(VIEWS.PRESETS);
            announce("Preset deleted");
        } else {
            confirmingDelete = false;
        }
        ctx.needsRedraw = true;
        return;
    }

    if (selectedDetailItem === DETAIL_DELETE) {
        confirmingDelete = true;
        confirmIndex = 0;
        announce(`Delete ${entry ? entry.name : "preset"}?`);
        ctx.needsRedraw = true;
        return;
    }

    /* Load (commit): apply and return to the chain editor. Clear preview state
     * so the kept preset isn't reverted on the way out. */
    if (applyPreset(ctx.selectedSlot, entry)) {
        previewActive = false;
        pendingPreviewIndex = PREVIEW_NONE;
        announce(`Loaded ${entry ? entry.name : "preset"}`);
        setView(VIEWS.CHAIN_EDIT);
    }
    ctx.needsRedraw = true;
}

/* ---- Back --------------------------------------------------------------- */

export function handlePresetsBack() {
    const { setView, VIEWS } = ctx;
    /* Entered from the module picker (Shift+Click on the synth block); Back
     * cancels the whole flow — revert any active audition to the original
     * sound — and exits to the chain editor. */
    revertToOriginal();
    setView(VIEWS.CHAIN_EDIT);
    announce("Chain Editor");
    ctx.needsRedraw = true;
}

export function handlePresetDetailBack() {
    const { setView, VIEWS } = ctx;
    if (confirmingDelete) {
        confirmingDelete = false;
        ctx.needsRedraw = true;
        return;
    }
    setView(VIEWS.PRESETS);
    announce("Presets");
    ctx.needsRedraw = true;
}
