/*
 * layout_common.mjs -- what a NAVIGATION LAYOUT is, and what it hands a device.
 *
 * Every external surface is a 4x4 of push encoders with a screen, so how the
 * sixteen knobs navigate is not a property of the device. There are two
 * layouts and every surface can run either (Global Settings -> System ->
 * Surface Nav):
 *
 *   MAP    (layout_map.mjs)    all sixteen knobs are parameters, two pages at
 *                              once; hold Shift for the slot map, Shift+turn
 *                              pages.
 *   KNOBS  (layout_knobs.mjs)  eight parameters (the page Move shows) and
 *                              eight labelled navigation knobs: pages, slot,
 *                              module, VOL, PAN. Hold Shift for the alternate
 *                              layer.
 *
 * On both, a TAP of Shift is the Mixer (e16_mixer.mjs).
 *
 * THE SPLIT. A layout decides what the knobs DO and what each cell SHOWS; a
 * device decides how that is DRAWN and how it reaches the wire. So a layout
 * never draws. It answers:
 *
 *   handle(ev, t)   an event from e16_input.decode (turn / push / release /
 *                   shift). Returns an action or null.
 *   tick(t)         follow, stranded modifiers, the Mixer's slow re-read.
 *   screen(t)       a tagged model of what should be shown -- one of
 *                     { kind: "params", view, component, turnHint, focusEnc }
 *                     { kind: "map",    map, page, showBuses, slot }
 *                     { kind: "mixer",  mixer, alt }
 *                     { kind: "empty",  slot }
 *                     { kind: "knobs",  view, component, navCells, empty }
 *                   A device renders every kind; screenLabels() below turns
 *                   any of them into sixteen short names for a text device.
 *   rings(t)        sixteen value indicators ({enc, r, g, b, amount,
 *                   bipolar}), for a device with rings; ring(enc, t) one.
 *   context(t)      a string that changes exactly when the VIEW changes.
 *   reading(t)      what a control just did, for a device that can show it
 *                   (the EC4's overlay), or null.
 *
 * and tells the device what changed through callbacks on its ctx:
 * invalidate() (the screen), invalidateLabels() (the text only),
 * ringChanged(desc) and valueMoved(t) (a value is moving under a hand).
 *
 * A layout's ctx: { focus, binding, mixer, feel, chainOf, now, selector,
 * invalidate, invalidateLabels, ringChanged, valueMoved }. `selector` is the
 * device's step for choices ({ choice, nav, slot } pulses per step; a null
 * `choice` leaves enums to the knob engine, as Move's own knob does).
 */
import { abbrev4, labelsFor, ENCODERS, RING_MAX, ringAmount } from "./e16_view.mjs";
import { displayValue } from "./param_pages/render_page_movy.mjs";
import { detentsPerStep } from "./knob_engine.mjs";

/* A reading stays up this long after the last movement; a navigation reading
 * less, since it confirms where you went and covers what you came to see. */
export const READING_HOLD_MS = 1500;
export const NAV_HOLD_MS = 800;

export const NAV_MAP = "map";
export const NAV_KNOBS = "knobs";

/* A parameter that is a CHOICE: an enum, or an int the engine already steps
 * like one. A coarse device steps those by angle, and shows them as a list. */
export function isChoice(meta) {
    return !!meta && (meta.type === "enum" || meta.kind === "enum" ||
        meta.type === "bool" || Array.isArray(meta.options) || detentsPerStep(meta) > 1 ||
        (meta.type === "int" && meta.max - meta.min === 1));
}

/* A choice's options and which one is current. */
export function choiceList(cell) {
    const meta = cell.meta || {};
    const v = cell.value;
    if (Array.isArray(meta.options) && meta.options.length) {
        const opts = meta.options.map(String);
        let i = opts.findIndex((x) => x.toLowerCase() === String(v).toLowerCase());
        if (i < 0 && isFinite(Number(v))) i = Number(v) - (typeof meta.min === "number" ? meta.min : 0);
        return { items: opts, index: i };
    }
    const lo = typeof meta.min === "number" ? meta.min : 0, hi = typeof meta.max === "number" ? meta.max : 1;
    if (hi - lo === 1 && lo === 0) return { items: ["Off", "On"], index: Number(v) > 0 ? 1 : 0 };
    const items = [];
    for (let x = lo; x <= hi && items.length < 128; x++) items.push(String(x));
    return { items, index: Math.round(Number(v)) - lo };
}

export function panLabel(p) {
    if (p === null || p === undefined || Math.abs(p) < 0.01) return "C";
    return (p < 0 ? "L" : "R") + Math.round(Math.abs(p) * 100);
}

/*
 * THE MODULE'S NAME, not the position id: "synth" / "fx1" / "bus2" are
 * ADDRESSES, and a title or a cell showing one names nothing the user chose
 * (the first E16 LABELS build said "SYNTH" instead of "9W9").
 */
export function moduleNameFor(chain, slot, component) {
    const sl = ((chain || {}).slots || [])[slot] || {};
    if (component === "synth") return sl.synth || "";
    let m = /^fx(\d+)$/.exec(component);
    if (m) return (sl.fx || [])[Number(m[1]) - 1] || "";
    m = /^midi_fx(\d+)$/.exec(component);
    if (m) return (sl.midiFx || [])[Number(m[1]) - 1] || "";
    m = /^bus(\d+)$/.exec(component);
    if (m) return (sl.buses || [])[Number(m[1]) - 1] || "";
    return component || "";
}

/*
 * A READING: { context, name, abbr, value, bar: {frac, bipolar} | null,
 * list: {items, index} | null }. Device-neutral -- the EC4 draws it as a 4x20
 * overlay; a device with no room for it ignores it.
 */
export function paramReading(cell, context, metaOf) {
    const numeric = isFinite(Number(cell.value)) && cell.max > cell.min;
    const choice = isChoice(cell.meta);
    return {
        context,
        name: String(cell.label || cell.key),
        abbr: abbrev4(cell.label || cell.key),
        value: String(displayValue(cell.value, (metaOf && metaOf(cell.key)) || cell.meta || {})),
        bar: numeric && !choice ? { frac: ringAmount(cell) / RING_MAX, bipolar: !!cell.bipolar } : null,
        list: choice ? choiceList(cell) : null,
    };
}

export function pageReading(context, pageNames, index) {
    return { context, name: "Page", abbr: "", value: pageNames[index] || "",
             bar: null, list: { items: pageNames, index } };
}

/* A Mixer control's reading; `alt` on a level knob is its pan. */
export function mixerReading(mixer, enc, alt) {
    const col = enc % 4;
    const track = mixer.nameOf(col);
    if (alt && enc < 4) {
        const p = mixer.tracks[col].pan === null ? 0 : mixer.tracks[col].pan;
        return { context: track, name: "Pan", abbr: "", value: panLabel(p),
                 bar: { frac: (p + 1) / 2, bipolar: true }, list: null };
    }
    const c = mixer.cell(enc), r = mixer.ringFor(enc);
    const tr = enc < 4 ? mixer.tracks[col] : null;
    const name = enc < 4 ? (tr && tr.muted ? "Volume (muted)" : "Volume") : (c.label || "");
    return { context: track, name, abbr: "",
             value: (c.value || "") + (c.off ? " (off)" : ""),
             bar: { frac: r.amount / RING_MAX, bipolar: !!r.bipolar }, list: null };
}

/* The reading on screen, and until when. */
export function createReadings() {
    let current = null;
    let until = -Infinity;
    return {
        show(r, t, holdMs) { current = r; until = t + (holdMs || READING_HOLD_MS); },
        clear() { until = -Infinity; },
        at(t) { return t < until ? current : null; },
    };
}

/*
 * SIXTEEN SHORT NAMES AND A TITLE, for a text device (the EC4's names, the
 * E16's LABELS mode), from any screen. Names are not padded or case-folded:
 * each device's character set does that.
 */
export function screenLabels(screen, opts) {
    const o = opts || {};
    const labels = new Array(ENCODERS).fill("");
    const s = screen || { kind: "empty", slot: 0 };
    if (s.kind === "params") {
        return labelsFor(s.view, { component: s.component, focusEnc: s.focusEnc, metaOf: o.metaOf });
    }
    if (s.kind === "map") {
        const cells = (s.map && s.map.cells) || [];
        for (let i = 0; i < ENCODERS; i++) {
            const c = cells[i];
            if (!c) continue;
            const name = c.kind === "slot" ? String(c.slot + 1) : String(c.label || "");
            labels[i] = (c.current ? ">" : "") + name;
        }
        return { title: "SLOT " + ((s.slot | 0) + 1) + " PICK", labels };
    }
    if (s.kind === "mixer") {
        const m = s.mixer;
        for (let e = 0; e < ENCODERS; e++) {
            const row = Math.floor(e / 4), col = e % 4;
            if (s.alt) {
                /* The alternate layer names what a held Shift does. */
                if (row === 0) labels[e] = "PAN";
                else if (row === 1 || row === 2) labels[e] = "100%";
                else labels[e] = col < 2 ? "100%" : "";
                continue;
            }
            if (row === 0) {
                const tr = m.tracks[col];
                labels[e] = tr.soloed ? "SOLO" : tr.muted ? "MUTE" : "VOL";
            } else labels[e] = m.cell(e).label || "";
        }
        return { title: "MIXER", labels };
    }
    if (s.kind === "knobs") {
        if (!s.empty) {
            const l = labelsFor(s.view, { metaOf: o.metaOf });
            for (let e = 0; e < 8; e++) labels[e] = l.labels[e];
        }
        for (let i = 0; i < 8; i++) labels[8 + i] = (s.navCells[i] && s.navCells[i].short) || "";
        return { title: String(s.component || "").toUpperCase().slice(0, 16), labels };
    }
    return { title: "SLOT " + ((s.slot | 0) + 1) + " EMPTY", labels };
}
