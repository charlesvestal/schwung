/*
 * layout_custom.mjs -- the CUSTOM navigation layout: pages of sixteen knobs,
 * each assigned to any parameter in the set. The third layout beside Map and
 * Knobs; layout_common.mjs has the contract, and
 * docs/superpowers/specs/2026-09-26-custom-surface-layout-design.md the design.
 *
 *   resting      the current page: sixteen assigned knobs, label and value
 *   Shift hold   the PAGE MAP -- sixteen cells, one per page; push jumps, a
 *                push on an empty cell adds a page. Shift + turn steps pages.
 *   Shift tap    the Mixer, as everywhere
 *   push         the parameter's own click (a toggle flips, a trigger fires),
 *                on RELEASE, so a hold can mean something else:
 *   push HOLD    LEARN: the next parameter moved on Move is this knob's. While
 *                armed, a PUSH on that knob or a CLOCKWISE turn clears it;
 *                anticlockwise, Shift, another learn or LEARN_TIMEOUT_MS
 *                cancels.
 *
 * The pages live in the per-set control document (control_map.mjs), which
 * the host owns: this layout reads `ctx.controls()` and changes it only
 * through `ctx.edit(fn)`. Values come through the host's TARGET IO, read ONE
 * cell per tick in rotation (plus a warm of the page when it changes), never a
 * read per cell per frame. A knob whose module has gone is DARK: shown, never
 * written.
 */
import { ENCODERS, RING_MAX, moduleRgb, RING_DARK, abbrev4 } from "./e16_view.mjs";
import { setOrdinal } from "./e16_map.mjs";
import { MIXER_ROW_RGB } from "./e16_mixer.mjs";
import { knobInit, knobStep, ENUM_DELTA_DIV } from "./knob_engine.mjs";
import { enumIndexOf, KIND_ENUM } from "./param_pages/param_meta.mjs";
import { formatParamForSet, learnEnumWireFormat } from "./param_format.mjs";
import { displayValue } from "./param_pages/render_page_movy.mjs";
import { SHIFT_TAP_MS } from "./surface_core.mjs";
import { KIND_PARAM, KIND_MASTER, targetAddress } from "./control_target.mjs";
import { addPage, assignKnob, clearKnob, KNOBS_PER_PAGE, MAX_PAGES } from "./control_map.mjs";
import { NAV_HOLD_MS, isChoice, choiceList, mixerReading, createReadings } from "./layout_common.mjs";

export const NAV_CUSTOM = "custom";

/* The Custom layout's host seams, with inert defaults so a surface built
 * without them (the tests of the other layouts) still constructs: no pages,
 * every target dark, no learn. */
export function customSeams(o) {
    const doc = { surface: { pages: [] } };
    return {
        controls: o.controls || (() => doc),
        edit: o.editControls || (() => null),
        targets: o.targets || { status: () => "dark", metaOf: () => null, read: () => null, write: () => false },
        learn: o.learn || null,
    };
}

/* Held this long, a push arms LEARN rather than clicking. */
export const LEARN_HOLD_MS = 600;
/* An armed learn nobody answered gives up after this. */
export const LEARN_TIMEOUT_MS = 10000;
/* Shift held this long (nothing turned) shows the page map. */
export const PAGEMAP_SHOW_MS = 400;
/* A Shift hold with no note-off expires, as the Map layout's does. */
export const PAGEMAP_MAX_HOLD_MS = 10000;

const LEARN_RGB = { r: 90, g: 90, b: 90 };
const DARK_RGB = { r: 8, g: 8, b: 8 };

export function createCustomLayout(ctx) {
    const c = ctx || {};
    const chainOf = c.chainOf || (() => ({ slots: [] }));
    const mixer = c.mixer || null;
    const feel = c.feel;
    const now = c.now || (() => Date.now());
    const sel = c.selector || { choice: null, nav: 1, slot: 1 };
    const controls = c.controls || (() => ({ surface: { pages: [] } }));
    const edit = c.edit || (() => null);
    const io = c.targets;
    const learn = c.learn || null;
    const invalidate = c.invalidate || (() => {});
    const invalidateLabels = c.invalidateLabels || (() => {});
    const ringChanged = c.ringChanged || (() => {});
    const valueMoved = c.valueMoved || (() => {});

    const readings = createReadings();
    let pageIndex = 0;
    let mixerOn = false;
    let mixerLoaded = false;
    let mixerRefreshAt = -Infinity;
    /* Shift: when it went down, whether anything was done under it, whether a
     * turn paged (which keeps the map hidden for the rest of the hold). */
    let shiftDownAt = null, shiftActed = false, shiftTurned = false;
    let shownMap = false;
    /* Push: when each encoder went down, and whether that press became a learn. */
    const pushDownAt = new Array(ENCODERS).fill(null);
    const pushLearned = new Array(ENCODERS).fill(false);
    /* The armed learn: { page, knob, at } or null. */
    let armed = null;
    /* Values by "page:knob", raw as the channel answers them. */
    const values = new Map();
    const knobStates = new Map();
    let cursor = 0;
    let warmedPage = null;

    const pages = () => (controls().surface || { pages: [] }).pages || [];
    const page = () => pages()[pageIndex] || null;
    const vkey = (k) => pageIndex + ":" + k;

    function clampPage() {
        const n = pages().length;
        if (pageIndex >= n) pageIndex = Math.max(0, n - 1);
    }

    const shiftHeld = (t) => shiftDownAt !== null && (t - shiftDownAt) < PAGEMAP_MAX_HOLD_MS;
    const mapVisible = (t) => shiftHeld(t) && !shiftTurned && !mixerOn && (t - shiftDownAt) >= PAGEMAP_SHOW_MS;
    const altHeld = (t) => shiftHeld(t) && (shiftActed || t - shiftDownAt >= SHIFT_TAP_MS);

    /* ---- a knob, as a cell ---- */

    function cellOf(k) {
        const p = page();
        const t = p ? p.knobs[k] : null;
        if (!t) return null;
        const status = io.status(t);
        const meta = status === "live" ? io.metaOf(t) : null;
        const min = meta && typeof meta.min === "number" ? meta.min : 0;
        const max = meta && typeof meta.max === "number" ? meta.max : 1;
        const label = t.label || (meta && (meta.short_name || meta.label || meta.name)) || t.key;
        return {
            enc: k, half: k >= 8 ? 1 : 0, slot: k % 8, target: t, status,
            key: targetAddress(t).key, label, meta: meta || {}, min, max, bipolar: min < 0,
            readOnly: !!(meta && meta.readOnly),
            value: status === "live" ? values.get(vkey(k)) : undefined,
        };
    }

    function viewNow() {
        const cells = new Array(ENCODERS).fill(null);
        for (let k = 0; k < KNOBS_PER_PAGE; k++) cells[k] = cellOf(k);
        const p = page();
        const headers = [p ? { name: p.name, index: pageIndex, count: pages().length } : null, null];
        return { cells, headers, pageIndex, pageCount: pages().length };
    }

    function colourOf(t) {
        if (t.kind === KIND_PARAM) return moduleRgb(setOrdinal(chainOf(), t.slot, t.component));
        if (t.kind === KIND_MASTER) return MIXER_ROW_RGB[3];
        return MIXER_ROW_RGB[0];
    }

    function fraction(cell) {
        const meta = cell.meta || {};
        if (meta.kind === KIND_ENUM && Array.isArray(meta.options) && meta.options.length > 1) {
            const i = enumIndexOf(meta, cell.value);
            return i >= 0 ? i / (meta.options.length - 1) : 0;
        }
        const v = Number(cell.value);
        if (!isFinite(v) || !(cell.max > cell.min)) return 0;
        return Math.max(0, Math.min(1, (v - cell.min) / (cell.max - cell.min)));
    }

    function ringOf(k) {
        const dark = { enc: k, r: RING_DARK.r, g: RING_DARK.g, b: RING_DARK.b, amount: 0, bipolar: false };
        if (armed && armed.page === pageIndex && armed.knob === k) {
            return { enc: k, r: LEARN_RGB.r, g: LEARN_RGB.g, b: LEARN_RGB.b, amount: RING_MAX, bipolar: false };
        }
        const cell = cellOf(k);
        if (!cell) return dark;
        if (cell.status !== "live") return { enc: k, r: DARK_RGB.r, g: DARK_RGB.g, b: DARK_RGB.b, amount: 0, bipolar: false };
        const rgb = colourOf(cell.target);
        return { enc: k, r: rgb.r, g: rgb.g, b: rgb.b,
                 amount: Math.round(fraction(cell) * RING_MAX), bipolar: cell.bipolar };
    }

    /* ---- reading values ---- */

    function readCell(k) {
        const p = page();
        const t = p ? p.knobs[k] : null;
        if (!t || io.status(t) !== "live") return;
        const raw = io.read(t);
        /* The tri-state: a read that did not answer keeps the last value. */
        if (raw === null || raw === undefined) return;
        /* An enum answers as a NAME or an INDEX depending on the module; the
         * grid learns which from what it reads, and writes it back the same
         * way. Without this a click wrote "1" to a module that takes "On". */
        const meta = io.metaOf(t);
        if (meta) learnEnumWireFormat(meta, raw);
        values.set(vkey(k), String(raw));
    }

    /* True when it read: the rotation then waits a tick, so no knob is read
     * twice in one. */
    function warmPage() {
        if (warmedPage === pageIndex) return false;
        warmedPage = pageIndex;
        for (let k = 0; k < KNOBS_PER_PAGE; k++) if (!values.has(vkey(k))) readCell(k);
        invalidate();
        return true;
    }

    function goPage(i, t, quiet) {
        const n = pages().length;
        if (!n) return false;
        const next = Math.max(0, Math.min(n - 1, i));
        if (next === pageIndex) return false;
        pageIndex = next;
        knobStates.clear();
        if (!quiet) readings.show({ context: "Custom", name: "Page", abbr: "",
            value: page().name, bar: null, list: { items: pages().map((p) => p.name), index: pageIndex } },
            t, NAV_HOLD_MS);
        invalidate();
        return true;
    }

    /* ---- learn ---- */

    function cancelLearn() {
        if (!armed) return;
        armed = null;
        if (learn) learn.cancel(learnOwner);
        invalidate();
    }
    const learnOwner = { id: "surface" };

    function armLearn(k, t) {
        if (!learn || !page()) return;
        armed = { page: pageIndex, knob: k, at: t };
        learn.arm(learnOwner, (target) => {
            const a = armed;
            armed = null;
            if (!a || !target) { invalidate(); return; }
            edit((doc) => assignKnob(doc, a.page, a.knob, target));
            values.delete(a.page + ":" + a.knob);
            knobStates.delete(a.page + ":" + a.knob);
            warmedPage = null;
            invalidate();
        });
        invalidate();
    }

    /* ---- input ---- */

    function turnKnob(k, pulses, t) {
        const cell = cellOf(k);
        if (!cell || cell.status !== "live" || cell.readOnly) return null;
        const meta = cell.meta;
        const d = (sel.choice && isChoice(meta))
            ? feel.steps(k, pulses, sel.choice) * ENUM_DELTA_DIV
            : feel.detents(k, pulses);
        if (!d) return null;
        let st = knobStates.get(vkey(k));
        if (!st) {
            const raw = cell.value;
            let start;
            if (meta.kind === KIND_ENUM) { const i = enumIndexOf(meta, raw); start = i >= 0 ? i : 0; }
            else { const n = Number(raw); start = isFinite(n) ? n : 0; }
            st = knobInit(start);
            knobStates.set(vkey(k), st);
        }
        const dir = d > 0 ? 1 : -1;
        let value = st.value;
        for (let i = 0; i < Math.abs(d); i++) value = knobStep(st, meta, dir, t, false);
        const wire = formatParamForSet(value, meta);
        if (!io.write(cell.target, wire)) return null;
        values.set(vkey(k), String(wire));
        ringChanged(ringOf(k));
        valueMoved(t);
        invalidateLabels();
        const after = cellOf(k);
        readings.show({ context: page().name, name: after.label, abbr: abbrev4(after.label),
            value: String(displayValue(after.value, after.meta)),
            bar: isChoice(after.meta) ? null : { frac: fraction(after), bipolar: after.bipolar },
            list: isChoice(after.meta) ? choiceList(after) : null }, t);
        return { action: "turn", enc: k };
    }

    /* The parameter's own click: a two-option value flips, a trigger fires. */
    function clickKnob(k, t) {
        const cell = cellOf(k);
        if (!cell || cell.status !== "live" || cell.readOnly) return null;
        const meta = cell.meta;
        const opts = Array.isArray(meta.options) ? meta.options : null;
        let wire = null;
        if (meta.kind === KIND_ENUM && opts && opts.length === 2) {
            const i = enumIndexOf(meta, cell.value);
            wire = formatParamForSet(i === 1 ? 0 : 1, meta);
        } else if (meta.type === "int" && meta.min === 0 && meta.max === 1) {
            wire = formatParamForSet(Number(cell.value) > 0 ? 0 : 1, meta);
        }
        if (wire === null) return null;
        if (!io.write(cell.target, wire)) return null;
        values.set(vkey(k), String(wire));
        knobStates.delete(vkey(k));
        ringChanged(ringOf(k));
        invalidateLabels();
        valueMoved(t);
        return { action: "click", enc: k };
    }

    function onShift(down, t) {
        if (down) {
            cancelLearn();
            shiftDownAt = t; shiftActed = false; shiftTurned = false;
            return { action: "hold" };
        }
        const wasMap = mapVisible(t);
        const tap = shiftDownAt !== null && !shiftActed && !wasMap && t - shiftDownAt < SHIFT_TAP_MS;
        const wasAlt = mixerOn && altHeld(t);
        shiftDownAt = null;
        if (tap) {
            mixerOn = !mixerOn && !!mixer;
            if (mixerOn && !mixerLoaded) { mixer.load(); mixerLoaded = true; }
            readings.clear();
            invalidate();
            return { action: "mixer", on: mixerOn };
        }
        if (wasMap || wasAlt) invalidate();
        return null;
    }

    function onTurn(k, pulses, t) {
        feel.begin(k, t);
        if (shiftDownAt !== null) shiftActed = true;
        if (mixerOn && mixer) {
            const alt = shiftDownAt !== null;
            const d = feel.detents(k, pulses);
            if (d && feel.mixerTurn(mixer, k, d, alt, t)) {
                ringChanged(mixer.ringFor(k)); valueMoved(t);
                readings.show(mixerReading(mixer, k, alt), t);
            }
            return { action: "mixerTurn", enc: k };
        }
        if (shiftHeld(t)) {
            /* Shift + turn pages, by angle; the map stays hidden for the rest
             * of the hold, and the hold's expiry restarts. */
            const n = feel.steps(k, pulses, sel.nav);
            shiftTurned = true;
            shiftDownAt = t;
            if (n) goPage(pageIndex + (n > 0 ? 1 : -1), t);
            return { action: "page", pageIndex };
        }
        if (armed && armed.page === pageIndex && armed.knob === k) {
            /*
             * THE ARMED KNOB ASKS "CLEAR?": clockwise clears, anticlockwise
             * cancels -- beside a second push, which also clears. Not a
             * triple-click: a quick push is the parameter's own click, so
             * three of them would flip an on/off parameter on the way.
             */
            const cleared = pulses > 0 && !!page() && !!page().knobs[k];
            armed = null;
            if (learn) learn.cancel(learnOwner);
            if (cleared) {
                edit((doc) => clearKnob(doc, pageIndex, k));
                values.delete(vkey(k));
                knobStates.delete(vkey(k));
            }
            invalidate();
            return { action: cleared ? "clear" : "cancel", enc: k };
        }
        return turnKnob(k, pulses, t);
    }

    function onPush(k, t) {
        if (shiftDownAt !== null) shiftActed = true;
        if (mixerOn && mixer) {
            if (mixer.push(k, shiftDownAt !== null)) {
                ringChanged(mixer.ringFor(k)); valueMoved(t);
                readings.show(mixerReading(mixer, k, false), t);
            }
            return { action: "mixerPush", enc: k };
        }
        if (shiftHeld(t)) {
            /* The page map, shown now if it was not yet: a push picks. */
            shiftTurned = false;
            shiftDownAt = Math.min(shiftDownAt, t - PAGEMAP_SHOW_MS);
            const n = pages().length;
            if (k < n) goPage(k, t, true);
            else if (n < MAX_PAGES) {
                edit((doc) => addPage(doc));
                pageIndex = pages().length - 1;
                warmedPage = null;
            }
            invalidate();
            return { action: "pagemap", pageIndex };
        }
        if (armed && armed.page === pageIndex && armed.knob === k) {
            /* A push on the armed knob CLEARS it -- the gesture in use on
             * hardware (2026-09-26); a clockwise turn does too (see onTurn). */
            armed = null;
            if (learn) learn.cancel(learnOwner);
            if (page() && page().knobs[k]) {
                edit((doc) => clearKnob(doc, pageIndex, k));
                values.delete(vkey(k));
                knobStates.delete(vkey(k));
            }
            pushLearned[k] = true;          /* its release is not a click */
            invalidate();
            return { action: "clear", enc: k };
        }
        pushDownAt[k] = t;
        pushLearned[k] = false;
        return { action: "press", enc: k };
    }

    function onRelease(k, t) {
        const down = pushDownAt[k];
        pushDownAt[k] = null;
        if (down === null || pushLearned[k]) return null;
        if (t - down < LEARN_HOLD_MS) return clickKnob(k, t);
        return null;
    }

    return {
        name: NAV_CUSTOM,
        pagesShown: 0,
        /* Its knobs address targets directly: the focus's page controller is
         * not used, so a device skips syncing and ticking it. */
        usesBinding: false,
        get mixerOn() { return !!(mixer && mixerOn); },
        get pageIndex() { return pageIndex; },
        get armed() { return armed ? { page: armed.page, knob: armed.knob } : null; },

        handle(ev, t) {
            if (!ev) return null;
            clampPage();
            if (ev.type === "shift") return onShift(!!ev.down, t);
            if (ev.type === "turn") return onTurn(ev.enc | 0, ev.ticks, t);
            if (ev.type === "push") return onPush(ev.enc | 0, t);
            if (ev.type === "release") return onRelease(ev.enc | 0, t);
            return null;
        },

        tick(t) {
            clampPage();
            /* A push held long enough becomes a learn, with no event. */
            for (let k = 0; k < ENCODERS; k++) {
                if (pushDownAt[k] !== null && !pushLearned[k] && t - pushDownAt[k] >= LEARN_HOLD_MS && page()) {
                    pushLearned[k] = true;
                    armLearn(k, t);
                }
            }
            if (armed && t - armed.at >= LEARN_TIMEOUT_MS) cancelLearn();
            if (shiftDownAt !== null && t - shiftDownAt >= PAGEMAP_MAX_HOLD_MS) { shiftDownAt = null; invalidate(); }
            if (mapVisible(t) !== shownMap) { shownMap = mapVisible(t); invalidate(); }
            if (mixer) {
                if (mixerOn && t - mixerRefreshAt >= 250) { mixerRefreshAt = t; mixer.refreshNext(); }
            }
            if (!page()) return;
            if (warmPage()) return;
            /* ONE read per tick, round the page's live knobs. */
            for (let i = 0; i < KNOBS_PER_PAGE; i++) {
                const k = cursor; cursor = (cursor + 1) % KNOBS_PER_PAGE;
                const p = page();
                if (p.knobs[k] && io.status(p.knobs[k]) === "live") {
                    const before = values.get(vkey(k));
                    readCell(k);
                    if (values.get(vkey(k)) !== before) { ringChanged(ringOf(k)); invalidateLabels(); }
                    break;
                }
            }
        },

        /* A write made elsewhere (Move's grid) to something on this page. */
        noteWrite(slot, key, value) {
            const p = page();
            if (!p) return;
            for (let k = 0; k < KNOBS_PER_PAGE; k++) {
                const t = p.knobs[k];
                if (!t) continue;
                const a = targetAddress(t);
                if (a.slot === (slot | 0) && a.key === String(key)) {
                    values.set(vkey(k), String(value));
                    knobStates.delete(vkey(k));
                    ringChanged(ringOf(k));
                    invalidateLabels();
                }
            }
        },

        screen(t) {
            if (mixer && mixerOn) return { kind: "mixer", mixer, alt: altHeld(t) };
            if (mapVisible(t)) {
                const names = new Array(ENCODERS).fill(null);
                pages().forEach((p, i) => { names[i] = p.name; });
                return { kind: "pagemap", names, current: pageIndex, canAdd: pages().length < MAX_PAGES };
            }
            if (!page()) return { kind: "message", slot: 0, component: "Custom", text: "Hold Shift: pages" };
            return { kind: "custom", view: viewNow(), turnHint: shiftHeld(t) && !mapVisible(t),
                     armed: armed && armed.page === pageIndex ? armed.knob : null };
        },

        rings(t) {
            if (mixer && mixerOn) return mixer.rings();
            const out = [];
            if (mapVisible(t)) {
                const n = pages().length;
                for (let e = 0; e < ENCODERS; e++) {
                    if (e >= n) out.push({ enc: e, r: 0, g: 0, b: 0, amount: 0, bipolar: false });
                    else {
                        const rgb = e === pageIndex ? LEARN_RGB : DARK_RGB;
                        out.push({ enc: e, r: rgb.r, g: rgb.g, b: rgb.b, amount: RING_MAX, bipolar: false });
                    }
                }
                return out;
            }
            for (let e = 0; e < ENCODERS; e++) out.push(ringOf(e));
            return out;
        },
        ring(enc) { return ringOf(enc); },
        view: viewNow,

        context(t) {
            return [NAV_CUSTOM, mixerOn, mapVisible(t), mixerOn && altHeld(t), pageIndex, pages().length,
                    armed ? armed.page + ":" + armed.knob : "", controls().rev || 0].join("|");
        },
        reading(t) { return readings.at(t); },
        turnHint(t) { return shiftHeld(t) && !mapVisible(t) && !mixerOn; },
        markDrawn() {},
        setFollow() {},
        reset() { mixerOn = false; shiftDownAt = null; cancelLearn(); readings.clear(); },
        /* The document changed under us (a load, another editor): forget values. */
        reload() { values.clear(); knobStates.clear(); warmedPage = null; clampPage(); invalidate(); },
    };
}
