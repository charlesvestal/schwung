/*
 * control_host.mjs -- the host half of the control foundation, assembled
 * OUTSIDE shadow_ui.js so the whole path runs in tests/host (that file cannot
 * be imported under node; logic left there can only be grepped).
 *
 *   the per-set control document  load / reconcile / load-modify-save edits
 *   the TARGET IO                 status (live | dark), meta, read, write
 *   the LEARN broker              one learn at a time, fed by parameter writes
 *
 * docs/superpowers/specs/2026-09-26-custom-surface-layout-design.md. The
 * Custom surface layout uses all three; the generic CC map will too.
 *
 * Every host call is injected:
 *   fs            { exists(path), read(path), write(path, text), ensureDir(dir) }
 *   stateDir()    the active set's directory
 *   getParam(slot, key) / setParam(slot, key, value)   the param channel
 *   chainShape()  { slots: [{ synth, fx: [], midiFx: [] }] }   module ids
 *   masterFx()    { fx1: { module }, ... }
 *   now(), log(line), announce(text), onReload()
 */
import { parseControls, serializeControls, emptyControls } from "./control_map.mjs";
import { targetAddress, targetFromWrite, KIND_MASTER, KIND_SETTING } from "./control_target.mjs";
import { buildMetaIndex } from "./param_pages/param_meta.mjs";

export const CONTROLS_FILE = "controls.json";
/* Another writer's edit (the web editor) is noticed within this. */
export const RECONCILE_MS = 1000;

/* The settings a control may drive, declared as chain_params so they get the
 * same meta, stepping and formatting as any module parameter. */
export const SETTINGS_CHAIN_PARAMS = [
    { key: "slot:volume", name: "Volume", type: "float", min: 0, max: 2, step: 0.01 },
    { key: "slot:pan", name: "Pan", type: "float", min: -1, max: 1, step: 0.02 },
    { key: "slot:muted", name: "Mute", type: "int", min: 0, max: 1 },
    { key: "slot:soloed", name: "Solo", type: "int", min: 0, max: 1 },
    { key: "buses:main_send1", name: "Send A", type: "int", min: 0, max: 127 },
    { key: "buses:main_send2", name: "Send B", type: "int", min: 0, max: 127 },
    { key: "master_fx:filter", name: "Filter", type: "float", min: -1, max: 1, step: 0.02 },
    { key: "send1:return", name: "Return A", type: "int", min: 0, max: 127 },
    { key: "send2:return", name: "Return B", type: "int", min: 0, max: 127 },
];

export function createControlHost(io) {
    const o = io || {};
    const fs = o.fs;
    const stateDir = o.stateDir;
    const getParam = o.getParam;
    const setParam = o.setParam;
    const chainShape = o.chainShape || (() => ({ slots: [] }));
    const masterFx = o.masterFx || (() => ({}));
    const now = o.now || (() => Date.now());
    const log = o.log || (() => {});
    const announce = o.announce || (() => {});
    const onReload = o.onReload || (() => {});
    const onWrite = o.onWrite || (() => {});

    /* ---------------- the document ---------------- */

    let doc = emptyControls();
    doc.rev = 0;
    let path = "";
    let text = null;
    /* The file was there and could not be read: never save over it. */
    let broken = false;
    let checkedAt = -Infinity;

    const file = () => stateDir() + "/" + CONTROLS_FILE;

    function readFile(p) {
        try { return fs.exists(p) ? fs.read(p) : null; } catch (e) { return undefined; }
    }

    function load() {
        const p = file();
        const t = readFile(p);
        if (t === undefined) return;   /* the read itself failed: keep what we have */
        const parsed = parseControls(t);
        parsed.doc.rev = (doc.rev || 0) + 1;
        doc = parsed.doc;
        path = p;
        text = t;
        broken = !parsed.ok;
        if (broken) log("controls: " + p + " could not be read; left untouched");
        onReload();
    }

    /* Load on a set change; pick up another writer's edit by CONTENT, at most
     * once per RECONCILE_MS. */
    function reconcile() {
        if (file() !== path) { load(); return; }
        const t0 = now();
        if (t0 - checkedAt < RECONCILE_MS) return;
        checkedAt = t0;
        const t = readFile(path);
        if (t !== undefined && t !== text) load();
    }

    /*
     * AN EDIT IS LOAD-MODIFY-SAVE: the file is re-read first and the edit
     * applied to what is on disk, so the device and the web editor cannot
     * silently undo each other. Refused while the file is unreadable -- a typo
     * in a hand-edited file must not be answered by deleting it.
     */
    function edit(fn) {
        checkedAt = -Infinity;
        reconcile();
        if (broken) { announce("Control layout file unreadable"); return doc; }
        const next = fn(doc);
        if (!next || next === doc) return doc;
        const out = serializeControls(next);
        try {
            fs.ensureDir(stateDir());
            fs.write(file(), out);
        } catch (e) { log("controls: save failed: " + e); return doc; }
        next.rev = (doc.rev || 0) + 1;
        doc = next;
        path = file();
        text = out;
        return doc;
    }

    /* ---------------- the target io ---------------- */

    /* A control's OWN writes are never learned (see learnObserveWrite). */
    let muted = 0;
    function write(slot, key, value) {
        muted++;
        try { return setParam(slot, key, value); } finally { muted--; }
    }

    function moduleAt(scope) {
        if (scope && typeof scope.fx === "number") {
            const e = (masterFx() || {})["fx" + scope.fx];
            return e && e.module ? String(e.module) : null;
        }
        const sl = ((chainShape() || {}).slots || [])[scope.slot] || {};
        const comp = String(scope.component || "");
        if (comp === "synth") return sl.synth || null;
        let m = /^fx(\d+)$/.exec(comp);
        if (m) return (sl.fx || [])[Number(m[1]) - 1] || null;
        m = /^midi_fx(\d+)$/.exec(comp);
        if (m) return (sl.midiFx || [])[Number(m[1]) - 1] || null;
        return null;
    }

    /* chain_params per MODULE ID, cached: one blocking read the first time a
     * module is met. A read that did not answer is not cached, and is not
     * retried more than once per RECONCILE_MS. */
    const metaCache = new Map();
    const metaTriedAt = new Map();
    function metaIndexOf(t) {
        const id = t.module;
        if (metaCache.has(id)) return metaCache.get(id);
        const t0 = now();
        if (t0 - (metaTriedAt.has(id) ? metaTriedAt.get(id) : -Infinity) < RECONCILE_MS) return null;
        metaTriedAt.set(id, t0);
        const key = t.kind === KIND_MASTER ? "master_fx:fx" + t.fx + ":chain_params" : t.component + ":chain_params";
        const raw = getParam(t.kind === KIND_MASTER ? 0 : t.slot, key);
        if (raw === null || raw === undefined || raw === "") return null;
        let cp = null;
        try { cp = JSON.parse(raw); } catch (e) { return null; }
        if (!Array.isArray(cp)) return null;
        const index = buildMetaIndex({ chainParams: cp });
        metaCache.set(id, index);
        return index;
    }

    const settingsMeta = buildMetaIndex({ chainParams: SETTINGS_CHAIN_PARAMS });

    const targets = {
        /* live | dark -- dark only on POSITIVE knowledge, from the mirrors the
         * UI already holds, never from a read that failed. */
        status(t) {
            if (t.kind === KIND_SETTING) return "live";
            const id = moduleAt(t.kind === KIND_MASTER ? { fx: t.fx } : { slot: t.slot, component: t.component });
            return id === t.module ? "live" : "dark";
        },
        metaOf(t) {
            if (t.kind === KIND_SETTING) return settingsMeta.get(t.key) || null;
            const index = metaIndexOf(t);
            return index ? (index.get(t.key) || index.getOrGuess(t.key)) : null;
        },
        read(t) {
            const a = targetAddress(t);
            return getParam(a.slot, a.key);
        },
        write(t, value) {
            const a = targetAddress(t);
            const ok = write(a.slot, a.key, value);
            if (ok) onWrite(a.slot, a.key);
            return ok;
        },
    };

    /* ---------------- the learn broker ---------------- */

    let armed = null;   /* { owner, cb } */
    const learn = {
        arm(owner, cb) {
            if (armed && armed.owner !== owner) { const prev = armed; armed = null; prev.cb(null); }
            armed = { owner, cb };
            announce("Learn: move a parameter");
        },
        cancel(owner) { if (armed && armed.owner === owner) armed = null; },
        get armed() { return !!armed; },
    };

    /*
     * Every successful parameter write is offered here. A write a control
     * made is not the user choosing a parameter (muted); neither is a key the
     * module does not DECLARE -- a module swap, a state blob, a derived view.
     */
    function observeWrite(slot, key, value) {
        if (!armed || muted > 0) return false;
        const t = targetFromWrite(slot, key, moduleAt);
        if (!t) return false;
        let label = "";
        if (t.kind === KIND_SETTING) {
            const m = settingsMeta.get(t.key);
            label = m ? (m.label || m.name || "") : "";
            if (t.slot !== null) label = "S" + (t.slot + 1) + " " + label;
        } else {
            const index = metaIndexOf(t);
            const m = index ? index.get(t.key) : null;
            if (!m) return false;
            label = m.short_name || m.label || m.name || t.key;
        }
        t.label = String(label).slice(0, 32);
        const a = armed;
        armed = null;
        announce("Learned " + t.label);
        a.cb(t);
        return true;
    }

    return {
        controls: () => doc,
        edit,
        reconcile,
        load,
        targets,
        learn,
        observeWrite,
        /** For a control's own non-target writes (a Mixer, a page controller). */
        write,
        moduleAt,
        get broken() { return broken; },
    };
}
