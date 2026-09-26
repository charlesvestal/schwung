/*
 * control_picker.mjs -- turning what the device reports into TARGETS a
 * control can drive, for the web editor (schwung-manager /controls). Pure:
 * the page fetches, this decides. Lives in shared/ beside control_target.mjs
 * so it is the device's own rules, served to the browser, and runs in
 * tests/host.
 *
 * Labels are made exactly as learn on the device makes them
 * (control_host.mjs observeWrite): short_name, else label, else name; a slot
 * setting is prefixed "S<n> ".
 */
import { normalizeTarget, sameTarget, KIND_PARAM, KIND_MASTER, KIND_SETTING,
         SLOT_SETTING_KEYS, MASTER_SETTING_KEYS, SETTINGS_CHAIN_PARAMS } from "./control_target.mjs";

const labelOf = (m, key) => String((m && (m.short_name || m.label || m.name)) || key).slice(0, 32);

/**
 * The components a slot holds, for a picker: [{ component, module, title }].
 * `slot` is { synth, fx: [], midiFx: [] } (module ids, "" for empty).
 */
export function componentsOfSlot(slot) {
    const out = [];
    const s = slot || {};
    (s.midiFx || []).forEach((m, i) => { if (m) out.push({ component: "midi_fx" + (i + 1), module: m, title: "MIDI FX " + (i + 1) }); });
    if (s.synth) out.push({ component: "synth", module: s.synth, title: "Synth" });
    (s.fx || []).forEach((m, i) => { if (m) out.push({ component: "fx" + (i + 1), module: m, title: "FX " + (i + 1) }); });
    return out;
}

/**
 * A module's chain_params as targets, in its own order. `where` is
 * { slot, component, module } or { fx, module }. Entries that are not a
 * drivable parameter (no key, a readout, a key the target rules refuse) are
 * dropped -- the same rules learn applies.
 */
export function paramTargets(chainParams, where) {
    const out = [];
    const seen = new Set();
    for (const m of Array.isArray(chainParams) ? chainParams : []) {
        if (!m || typeof m.key !== "string" || seen.has(m.key)) continue;
        if (m.access === "read" || m.readonly === true || m.read_only === true) continue;
        const t = where && typeof where.fx === "number"
            ? normalizeTarget({ kind: KIND_MASTER, fx: where.fx, key: m.key, module: where.module, label: labelOf(m, m.key) })
            : normalizeTarget({ kind: KIND_PARAM, slot: where.slot, component: where.component, key: m.key,
                                module: where.module, label: labelOf(m, m.key) });
        if (!t) continue;
        seen.add(m.key);
        out.push({ target: t, name: String(m.name || m.label || m.key) });
    }
    return out;
}

/** The mixer's settings as targets: slot 0-3, or null for the master's. */
export function settingTargets(slot) {
    const keys = slot === null ? MASTER_SETTING_KEYS : SLOT_SETTING_KEYS;
    return keys.map((key) => {
        const m = SETTINGS_CHAIN_PARAMS.find((p) => p.key === key);
        const name = labelOf(m, key);
        const label = slot === null ? name : "S" + (slot + 1) + " " + name;
        return { target: normalizeTarget({ kind: KIND_SETTING, slot, key, label }), name };
    }).filter((x) => x.target);
}

/** Where a target lives, for a person: "Slot 2 Synth (obxd)", "Master FX 3", "Mixer". */
export function scopeText(t) {
    if (!t) return "";
    if (t.kind === KIND_PARAM) {
        const c = t.component === "synth" ? "Synth"
            : t.component.startsWith("midi_fx") ? "MIDI FX " + t.component.slice(7)
            : "FX " + t.component.slice(2);
        return "Slot " + (t.slot + 1) + " " + c + " (" + t.module + ")";
    }
    if (t.kind === KIND_MASTER) return "Master FX " + t.fx + " (" + t.module + ")";
    return t.slot === null ? "Master" : "Slot " + (t.slot + 1) + " mixer";
}

/**
 * live | dark | unknown. Dark only when the chain POSITIVELY holds a
 * different module (or none) at the target's position; unknown when the page
 * has no chain to judge by.
 */
export function targetStatus(t, chain) {
    if (!t) return "unknown";
    if (t.kind === KIND_SETTING) return "live";
    if (!chain || !Array.isArray(chain.slots)) return "unknown";
    let id = "";
    if (t.kind === KIND_MASTER) {
        id = (chain.masterFx || [])[t.fx - 1] || "";
    } else {
        const s = chain.slots[t.slot] || {};
        if (t.component === "synth") id = s.synth || "";
        else if (t.component.startsWith("midi_fx")) id = (s.midiFx || [])[Number(t.component.slice(7)) - 1] || "";
        else id = (s.fx || [])[Number(t.component.slice(2)) - 1] || "";
    }
    return id === t.module ? "live" : "dark";
}

/** Which CC (if any) already drives this target: "Ch1 CC74", or "". */
export function ccOf(doc, t) {
    const b = ((doc && doc.cc) || []).find((x) => sameTarget(x.target, t));
    return b ? "Ch" + (b.channel + 1) + " CC" + b.cc : "";
}
