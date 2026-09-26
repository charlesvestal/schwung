/*
 * control_target.mjs -- WHAT a control drives, one way for every source.
 *
 * A surface knob in the Custom layout drives a target; a CC-map binding (built
 * next) will drive one too. The target is the part they share, so it is
 * defined once, here, and both address a parameter the same way however it is
 * reached. docs/superpowers/specs/2026-09-26-custom-surface-layout-design.md.
 *
 *   param    a parameter of a module in a slot
 *            { kind, slot 0-3, component synth|fxN|midi_fxN, key, module, label }
 *   master   a parameter of a Master FX module
 *            { kind, fx 1-8, key, module, label }
 *   setting  a slot or master setting -- the mixer's values
 *            { kind, slot 0-3 | null (master), key, label }
 *
 * `module` is the module id at assignment. A target whose position no longer
 * holds that module is DARK (the host decides, from positive knowledge only).
 * A setting has no module: it is always there.
 *
 * Pure: no host calls. The host's target io does the reading and writing.
 */

export const KIND_PARAM = "param";
export const KIND_MASTER = "master";
export const KIND_SETTING = "setting";

export const SLOTS = 4;
export const MASTER_FX_POSITIONS = 8;

const COMPONENT_RE = /^(synth|fx\d+|midi_fx\d+)$/;

/* The settings a control may drive -- the values the Mixer turns, and no
 * others: an arbitrary slot key (a module swap, a preset load) is not a knob. */
export const SLOT_SETTING_KEYS = ["slot:volume", "slot:pan", "slot:muted", "slot:soloed",
                                  "buses:main_send1", "buses:main_send2"];
export const MASTER_SETTING_KEYS = ["master_fx:filter", "send1:return", "send2:return"];

/*
 * Keys that are not parameters, however they are spelled: module identity,
 * whole-state blobs, presets, bypass, and anything a module serves as a
 * readout or a derived view. Learn must never bind one -- a knob that swaps
 * the synth is not a knob. The host also refuses any key its module's
 * chain_params does not declare; this list is the pure half of that check.
 */
const NOT_PARAMS = new Set(["module", "state", "preset", "preset_name", "bypassed", "plugin_id",
                            "load", "save", "ui_hierarchy", "chain_params", "ui_pages", "view"]);

const isInt = (n, lo, hi) => Number.isInteger(n) && n >= lo && n <= hi;
const isKey = (k) => typeof k === "string" && k.length > 0 && k.length <= 64 &&
                     !k.includes(":") && !NOT_PARAMS.has(k);

/**
 * A valid target, or null. Tolerant by design: a document written by a later
 * build, or edited by hand, must load with the knobs it cannot use empty --
 * never throw, never guess.
 */
export function normalizeTarget(t) {
    if (!t || typeof t !== "object") return null;
    const label = typeof t.label === "string" ? t.label.slice(0, 32) : "";
    if (t.kind === KIND_PARAM) {
        if (!isInt(t.slot, 0, SLOTS - 1) || !COMPONENT_RE.test(String(t.component)) || !isKey(t.key)) return null;
        if (typeof t.module !== "string" || !t.module) return null;
        return { kind: KIND_PARAM, slot: t.slot, component: t.component, key: t.key, module: t.module, label };
    }
    if (t.kind === KIND_MASTER) {
        if (!isInt(t.fx, 1, MASTER_FX_POSITIONS) || !isKey(t.key)) return null;
        if (typeof t.module !== "string" || !t.module) return null;
        return { kind: KIND_MASTER, fx: t.fx, key: t.key, module: t.module, label };
    }
    if (t.kind === KIND_SETTING) {
        if (t.slot === null || t.slot === undefined) {
            return MASTER_SETTING_KEYS.includes(t.key) ? { kind: KIND_SETTING, slot: null, key: t.key, label } : null;
        }
        if (!isInt(t.slot, 0, SLOTS - 1) || !SLOT_SETTING_KEYS.includes(t.key)) return null;
        return { kind: KIND_SETTING, slot: t.slot, key: t.key, label };
    }
    return null;
}

/** Where a target is read and written: the param channel's (slot, key). A
 *  Master FX key is addressed at slot 0 by convention, as the grid does. */
export function targetAddress(t) {
    if (t.kind === KIND_PARAM) return { slot: t.slot, key: t.component + ":" + t.key };
    if (t.kind === KIND_MASTER) return { slot: 0, key: "master_fx:fx" + t.fx + ":" + t.key };
    return { slot: t.slot === null ? 0 : t.slot, key: t.key };
}

/** Is this the same thing to drive? (The label is presentation, not identity;
 *  neither is the module -- the same position and key is the same target.) */
export function sameTarget(a, b) {
    if (!a || !b || a.kind !== b.kind || a.key !== b.key) return false;
    if (a.kind === KIND_PARAM) return a.slot === b.slot && a.component === b.component;
    if (a.kind === KIND_MASTER) return a.fx === b.fx;
    return a.slot === b.slot;
}

/*
 * LEARN: a parameter write seen on Move, as a target -- or null when the
 * write is not something a control may drive.
 *
 * `moduleAt(scope)` is the host's answer to "which module is at this
 * position right now": { slot, component } or { fx }. It may return null
 * (the read did not answer), and then there is no target: learning a knob
 * against an unknown module would make it dark-or-wrong from birth.
 */
export function targetFromWrite(slot, key, moduleAt) {
    const k = String(key);
    let m = /^master_fx:fx(\d+):([^:]+)$/.exec(k);
    if (m) {
        const fx = Number(m[1]);
        if (!isInt(fx, 1, MASTER_FX_POSITIONS) || !isKey(m[2])) return null;
        const module = moduleAt({ fx });
        return module ? normalizeTarget({ kind: KIND_MASTER, fx, key: m[2], module }) : null;
    }
    if (MASTER_SETTING_KEYS.includes(k)) return normalizeTarget({ kind: KIND_SETTING, slot: null, key: k });
    if (SLOT_SETTING_KEYS.includes(k)) return normalizeTarget({ kind: KIND_SETTING, slot, key: k });
    m = /^([a-z_0-9]+):([^:]+)$/.exec(k);
    if (m && COMPONENT_RE.test(m[1]) && isKey(m[2])) {
        const module = moduleAt({ slot, component: m[1] });
        return module ? normalizeTarget({ kind: KIND_PARAM, slot, component: m[1], key: m[2], module }) : null;
    }
    return null;
}

/* Where a target lives, for a label with nothing better: "S2 OBXD", "MFX3",
 * "S1", "Master". */
export function targetScopeLabel(t) {
    if (t.kind === KIND_PARAM) return "S" + (t.slot + 1) + " " + t.module;
    if (t.kind === KIND_MASTER) return "MFX" + t.fx + " " + t.module;
    return t.slot === null ? "Master" : "S" + (t.slot + 1);
}
