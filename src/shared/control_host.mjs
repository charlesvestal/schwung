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
import { targetAddress, targetFromWrite, childKeyIndex, resolveTargetMeta,
         KIND_MASTER, KIND_SETTING, SETTINGS_CHAIN_PARAMS } from "./control_target.mjs";
import { buildMetaIndex } from "./param_pages/param_meta.mjs";

export const CONTROLS_FILE = "controls.json";
/* Another writer's edit (the web editor) is noticed within this. */
export const RECONCILE_MS = 1000;

/* SETTINGS_CHAIN_PARAMS lives in control_target.mjs (pure), so the web
 * editor, which imports that file, names the settings as the device does. */
export { SETTINGS_CHAIN_PARAMS };

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

    /* chain_params AND ui_hierarchy per MODULE ID, cached: blocking reads
     * the first time a module is met. A read that did not answer is not
     * cached, and is not retried more than once per RECONCILE_MS.
     *
     * The hierarchy is what names a child-level write (`pad3_wide`, declared
     * as `wide`), so it is read beside chain_params. `""` is a module with no
     * hierarchy, which is an answer; `null` is a read that did not complete,
     * and caching that as "no children" would refuse every per-pad key until
     * the module was swapped. */
    const metaCache = new Map();
    const metaTriedAt = new Map();
    /* An entry whose hierarchy read did not complete: served between retries. */
    const metaPartial = new Map();
    function metaOfModule(t) {
        const id = t.module;
        if (metaCache.has(id)) return metaCache.get(id);
        const t0 = now();
        if (t0 - (metaTriedAt.has(id) ? metaTriedAt.get(id) : -Infinity) < RECONCILE_MS) return metaPartial.get(id) || null;
        metaTriedAt.set(id, t0);
        const slot = t.kind === KIND_MASTER ? 0 : t.slot;
        const prefix = t.kind === KIND_MASTER ? "master_fx:fx" + t.fx : t.component;
        const raw = getParam(slot, prefix + ":chain_params");
        if (raw === null || raw === undefined || raw === "") return null;
        let cp = null;
        try { cp = JSON.parse(raw); } catch (e) { return null; }
        if (!Array.isArray(cp)) return null;
        const hraw = getParam(slot, prefix + ":ui_hierarchy");
        let hier = null;
        if (typeof hraw === "string" && hraw !== "") { try { hier = JSON.parse(hraw); } catch (e) { hier = null; } }
        const entry = { index: buildMetaIndex({ chainParams: cp }), children: childKeyIndex(hier) };
        /* A hierarchy read that did not complete still serves what chain_params
         * declares -- losing learn for a whole module to one timeout would be
         * worse -- but is not CACHED, so the children arrive on a later try. */
        if (hraw !== null && hraw !== undefined) { metaCache.set(id, entry); metaPartial.delete(id); }
        else metaPartial.set(id, entry);
        return entry;
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
            const e = metaOfModule(t);
            if (!e) return null;
            const r = resolveTargetMeta(e.index, e.children, t.key);
            return r ? r.meta : e.index.getOrGuess(t.key);
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
    /* The last key learn refused, so a turn announces it once. */
    let refusedKey = null;
    const learn = {
        /* quiet: a re-arm inside a learn MODE (the CC map), which announced
         * itself once rather than on every capture. */
        arm(owner, cb, quiet) {
            if (armed && armed.owner !== owner) { const prev = armed; armed = null; prev.cb(null); }
            armed = { owner, cb };
            refusedKey = null;
            if (!quiet) announce("Learn: move a parameter");
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
            const e = metaOfModule(t);
            const r = e ? resolveTargetMeta(e.index, e.children, t.key) : null;
            if (!r) {
                /* SAY SO. A refused write used to vanish, so a knob that
                 * could not be learned read as learn not working at all
                 * (#539). Not for the module's own UI plumbing (a pad hit
                 * writes `ui_current_pad`), and once per key per learn, since
                 * a knob turn is a stream of writes. Learn stays armed. */
                if (e && !e.children.plumbing.has(t.key) && refusedKey !== t.key) {
                    refusedKey = t.key;
                    log("controls: learn refused " + t.component + ":" + t.key + " (not declared)");
                    announce("Can't learn " + t.key);
                }
                return false;
            }
            label = r.label;
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
