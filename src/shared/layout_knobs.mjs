/*
 * layout_knobs.mjs -- the KNOBS navigation layout: nothing hidden behind a
 * gesture. It began as the Faderfox EC4's (legsmechanical, PR #539) and is now
 * one of the two layouts every surface can run; layout_common.mjs has the
 * contract.
 *
 *   PARAMETERS                                   MIXER (e16_mixer.mjs)
 *   | CUTO | RESO | DRIV | ENVA |  page knobs     | Vol  Vol  Vol  Vol  |  alt: pan / solo
 *   | ATTA | DECA | SUST | REL  |  1-8            | SndA SndA SndA SndA |  alt push: 100%
 *   | <PG  | MAIN |  2/5 | PG>  |  pages          | SndB SndB SndB SndB |
 *   | SL 1 | OBXD | VOL  | PAN  |  slot / module  | RtnA RtnB Capt Filt |
 *
 * One page at a time, so a page on the surface is the page Move shows on its
 * own eight knobs. Every navigation control is a labelled knob: turn to move,
 * and the <PG / PG> pushes step. VOL and PAN are the focused slot's level
 * (push: mute) and pan (push: centre), the same numbers as the Mixer's. A TAP
 * of Shift is the Mixer; a HOLD is the alternate layer.
 */
import { ENCODERS, RING_MAX, abbrev4, ringFor, ringsFor, moduleRgb, applyTurn, applyClick,
         SLOT_RGB, RING_DARK } from "./e16_view.mjs";
import { setOrdinal } from "./e16_map.mjs";
import { MIXER_ROW_RGB } from "./e16_mixer.mjs";
import { ENUM_DELTA_DIV } from "./knob_engine.mjs";
import { SHIFT_TAP_MS } from "./surface_core.mjs";
import { NAV_KNOBS, NAV_HOLD_MS, isChoice, panLabel, moduleNameFor, paramReading, pageReading,
         mixerReading, createReadings } from "./layout_common.mjs";

/* The cells that are not page knobs. */
export const CELL_PREV = 8, CELL_PAGE = 9, CELL_COUNT = 10, CELL_NEXT = 11;
export const CELL_SLOT = 12, CELL_MODULE = 13, CELL_VOL = 14, CELL_PAN = 15;
export const PAGE_KNOBS_N = 8;
/* VOL and PAN read the Mixer's model, kept by one re-read this often. */
export const MIXER_REFRESH_MS = 250;

const PAGE_COUNT_MAX = 4;
const BUTTON_RGB = { r: 20, g: 20, b: 20 };

export function createKnobsLayout(ctx) {
    const c = ctx || {};
    const focus = c.focus;
    const binding = c.binding;
    const mixer = c.mixer || null;
    const feel = c.feel;
    const now = c.now || (() => Date.now());
    const chainOf = c.chainOf || (() => ({ slots: [] }));
    const sel = c.selector || { choice: null, nav: 1, slot: 1 };
    const invalidate = c.invalidate || (() => {});
    const invalidateLabels = c.invalidateLabels || (() => {});
    const ringChanged = c.ringChanged || (() => {});
    const valueMoved = c.valueMoved || (() => {});

    const readings = createReadings();
    let mixerOn = false;
    let mixerLoaded = false;
    let mixerRefreshAt = -Infinity;
    /* Shift: when it went down (null = up), and whether anything was done
     * under it -- a press that did nothing is a TAP, and a tap switches view. */
    let shiftDownAt = null;
    let shiftActed = false;

    const viewNow = () => binding.pageView(focus.pageIndex);
    const pageName = () => binding.pageName(focus.pageIndex);
    const pageCount = () => binding.knobPages().length;
    const components = () => focus.components(focus.slot);
    const here = () => components().find((x) => x.component === focus.component) || null;
    const moduleName = () => moduleNameFor(chainOf(), focus.slot, focus.component);
    const knobRgb = () => moduleRgb(setOrdinal(chainOf(), focus.slot, focus.component));

    /* Is Shift a HOLD yet? Once down SHIFT_TAP_MS, or anything was done under
     * it -- before that it may still be a tap, so the names do not flash the
     * alternate layer on every view switch. */
    const shiftHeld = (t) => shiftDownAt !== null && (shiftActed || t - shiftDownAt >= SHIFT_TAP_MS);

    /* ---- what the eight navigation cells show ---- */

    function navCells() {
        const n = pageCount();
        const h = here();
        const t = mixer ? mixer.tracks[focus.slot] : null;
        const count = (focus.pageIndex + 1) + "/" + n;
        const vol = mixer ? mixer.cell(focus.slot) : null;
        const pan = t && t.pan !== null ? t.pan : 0;
        return [
            { short: "<PG", label: "Prev", value: "" },
            { short: n ? abbrev4(pageName()) : "----", label: "Page", value: n ? pageName() : "" },
            { short: !n ? "" : (count.length <= PAGE_COUNT_MAX ? count : "P" + (focus.pageIndex + 1)),
              label: "Pages", value: n ? count : "" },
            { short: "PG>", label: "Next", value: "" },
            { short: "SL " + (focus.slot + 1), label: "Slot", value: String(focus.slot + 1) },
            { short: h ? abbrev4(h.label) : "EMPT", label: "Module", value: h ? String(h.label) : "Empty" },
            /* Names of what the knob DOES; a state that changes what it does
             * (mute) is the only exception. */
            { short: t && t.muted ? "MUTE" : "VOL", label: "Vol",
              value: t && t.muted ? "Muted" : (vol && vol.value ? vol.value : "") },
            { short: "PAN", label: "Pan", value: t ? panLabel(pan) : "" },
        ];
    }

    /* The navigation cells' rings, for a device that has them: where you
     * are along each list. */
    function navRing(enc) {
        const full = (rgb, frac, bipolar) => ({ enc, r: rgb.r, g: rgb.g, b: rgb.b,
            amount: Math.max(0, Math.min(RING_MAX, Math.round(frac * RING_MAX))), bipolar: !!bipolar });
        const dark = { enc, r: RING_DARK.r, g: RING_DARK.g, b: RING_DARK.b, amount: 0, bipolar: false };
        const n = pageCount();
        if (enc === CELL_PREV || enc === CELL_NEXT) return full(BUTTON_RGB, 1, false);
        if (enc === CELL_PAGE) return n ? full(knobRgb(), n > 1 ? focus.pageIndex / (n - 1) : 1, false) : dark;
        if (enc === CELL_COUNT) return dark;
        if (enc === CELL_SLOT) return full(SLOT_RGB, focus.slot / 3, false);
        if (enc === CELL_MODULE) {
            const comps = components();
            const i = comps.findIndex((x) => x.component === focus.component);
            return i < 0 ? dark : full(knobRgb(), comps.length > 1 ? i / (comps.length - 1) : 1, false);
        }
        if (!mixer) return dark;
        const t = mixer.tracks[focus.slot];
        if (enc === CELL_VOL) {
            const r = mixer.ringFor(focus.slot);
            return { ...r, enc };
        }
        const p = t.pan === null ? 0 : t.pan;
        return full(MIXER_ROW_RGB[0], (p + 1) / 2, true);
    }

    function slotEmpty() { return components().length === 0; }

    /* ---- readings ---- */

    function paramShown(enc, t) {
        const cell = viewNow().cells[enc];
        if (cell) readings.show(paramReading(cell, pageName(), binding.metaOf), t);
    }
    function pageShown(t) {
        readings.show(pageReading(moduleName() || "Module",
            binding.knobPages().map((p) => String(p.name || "")), focus.pageIndex), t, NAV_HOLD_MS);
    }
    function slotLevelShown(which, t) {
        const tr = mixer.tracks[focus.slot];
        const context = "Slot " + (focus.slot + 1);
        if (which === "vol") {
            const cl = mixer.cell(focus.slot), r = mixer.ringFor(focus.slot);
            readings.show({ context, name: tr.muted ? "Volume (muted)" : "Volume", abbr: "",
                            value: cl.value ? cl.value + " dB" : "",
                            bar: { frac: r.amount / RING_MAX, bipolar: false }, list: null }, t);
        } else {
            const p = tr.pan === null ? 0 : tr.pan;
            readings.show({ context, name: "Pan", abbr: "", value: panLabel(p),
                            bar: { frac: (p + 1) / 2, bipolar: true }, list: null }, t);
        }
    }

    /* ---- input ---- */

    const step = (n) => (n > 0 ? 1 : -1);

    /* The <PG / PG> buttons step quietly; the page KNOB shows the list. */
    function stepPage(d, t, quiet) {
        const n = pageCount();
        if (n && focus.setPage(Math.max(0, Math.min(n - 1, focus.pageIndex + d)))) invalidate();
        if (!quiet) pageShown(t);
    }

    function turn(enc, pulses, t) {
        if (shiftDownAt !== null) shiftActed = true;
        const alt = shiftDownAt !== null;
        feel.begin(enc, t);
        if (mixerOn && mixer) {
            const d = feel.detents(enc, pulses);
            if (d && feel.mixerTurn(mixer, enc, d, alt, t)) {
                ringChanged(mixer.ringFor(enc));
                valueMoved(t);
                readings.show(mixerReading(mixer, enc, alt), t);
            }
            return { action: "mixerTurn", enc };
        }
        if (enc < PAGE_KNOBS_N) {
            const view = viewNow();
            const cell = view.cells[enc];
            const ctl = binding.controller;
            if (!cell || !ctl) return null;
            /* A choice on a coarse device moves one option per selector
             * angle: the engine gates an option at ENUM_DELTA_DIV detents, so
             * that many are handed over at once. */
            const d = (sel.choice && isChoice(cell.meta))
                ? feel.steps(enc, pulses, sel.choice) * ENUM_DELTA_DIV
                : feel.detents(enc, pulses);
            if (d && applyTurn(view, ctl, enc, d, t)) {
                ringChanged(ringFor(viewNow(), enc, knobRgb()));
                valueMoved(t);
                invalidateLabels();
                paramShown(enc, t);
            }
            return { action: "turn", enc };
        }
        if (enc <= CELL_MODULE) {
            const n = feel.steps(enc, pulses, enc === CELL_SLOT ? sel.slot : sel.nav);
            if (!n) return null;
            if (enc <= CELL_NEXT) { stepPage(step(n), t); return { action: "page", pageIndex: focus.pageIndex }; }
            if (focus.follow) return null;
            if (enc === CELL_SLOT) {
                const s = Math.max(0, Math.min(3, focus.slot + step(n)));
                /* No reading: the slot and module cells already say where you
                 * are, and a reading would cover them while you look. */
                if (s !== focus.slot && focus.enterSlot(s)) invalidate();
                return { action: "slot", slot: focus.slot };
            }
            const comps = components();
            const i = comps.findIndex((x) => x.component === focus.component);
            const next = comps[Math.max(0, Math.min(comps.length - 1, (i < 0 ? 0 : i) + step(n)))];
            if (next && focus.set(focus.slot, next.component)) invalidate();
            return { action: "focus", slot: focus.slot, component: focus.component };
        }
        if (!mixer) return null;
        const d = feel.detents(enc, pulses);
        if (!d) return null;
        /* The Mixer's pan is its level knob's alternate: row 1, Shift. */
        if (feel.mixerTurn(mixer, focus.slot, d, enc === CELL_PAN, t)) {
            ringChanged(navRing(enc));
            valueMoved(t);
            slotLevelShown(enc === CELL_VOL ? "vol" : "pan", t);
        }
        return { action: "mixerTurn", enc };
    }

    function push(enc, t) {
        if (shiftDownAt !== null) shiftActed = true;
        const alt = shiftDownAt !== null;
        if (mixerOn && mixer) {
            if (mixer.push(enc, alt)) {
                ringChanged(mixer.ringFor(enc));
                valueMoved(t);
                readings.show(mixerReading(mixer, enc, false), t);
            }
            return { action: "mixerPush", enc };
        }
        if (enc < PAGE_KNOBS_N) {
            const ctl = binding.controller;
            if (!ctl) return null;
            const pageBefore = ctl.pageIndex;
            const hit = applyClick(viewNow(), ctl, enc);
            if (ctl.pageIndex !== pageBefore) invalidate();
            else if (hit) {
                ringChanged(ringFor(viewNow(), enc, knobRgb()));
                invalidateLabels();
                paramShown(enc, t);
            }
            return { action: "click", enc };
        }
        if (enc === CELL_PREV) { stepPage(-1, t, true); return { action: "page", pageIndex: focus.pageIndex }; }
        if (enc === CELL_NEXT) { stepPage(1, t, true); return { action: "page", pageIndex: focus.pageIndex }; }
        if (!mixer) return null;
        if (enc === CELL_VOL) {
            if (mixer.push(focus.slot, false)) { ringChanged(navRing(enc)); valueMoved(t); slotLevelShown("vol", t); }
            return { action: "mixerPush", enc };
        }
        if (enc === CELL_PAN) {
            if (mixer.centrePan(focus.slot)) { ringChanged(navRing(enc)); valueMoved(t); }
            slotLevelShown("pan", t);
            return { action: "mixerPush", enc };
        }
        return null;
    }

    function shift(down, t) {
        if (down) { shiftDownAt = t; shiftActed = false; return { action: "hold" }; }
        const tap = shiftDownAt !== null && !shiftActed && t - shiftDownAt < SHIFT_TAP_MS;
        const wasHeld = shiftHeld(t);
        shiftDownAt = null;
        if (!tap) {
            /* The alternate names go when the hold does. */
            if (wasHeld && mixerOn) invalidate();
            return null;
        }
        mixerOn = !mixerOn && !!mixer;
        readings.clear();
        if (mixerOn && !mixerLoaded) { mixer.load(); mixerLoaded = true; }
        invalidate();
        return { action: "mixer", on: mixerOn };
    }

    /* The alternate names appear once a press BECOMES a hold, with no event
     * to mark it; tick() notices. */
    let shownAlt = false;

    return {
        name: NAV_KNOBS,
        get mixerOn() { return !!(mixer && mixerOn); },

        handle(ev, t) {
            if (!ev) return null;
            if (ev.type === "shift") return shift(!!ev.down, t);
            if (ev.type === "turn") return turn(ev.enc | 0, ev.ticks, t);
            if (ev.type === "push") return push(ev.enc | 0, t);
            return null;
        },

        tick(t) {
            if (focus.poll()) invalidate();
            if (mixer) {
                /* VOL and PAN read the Mixer's model, so it is loaded once,
                 * then kept by a slow rotation. */
                if (!mixerLoaded) { mixer.load(); mixerLoaded = true; }
                else if (t - mixerRefreshAt >= MIXER_REFRESH_MS) { mixerRefreshAt = t; mixer.refreshNext(); }
            }
            const alt = mixerOn && shiftHeld(t);
            if (alt !== shownAlt) { shownAlt = alt; invalidate(); }
        },

        screen(t) {
            if (mixer && mixerOn) return { kind: "mixer", mixer, alt: shiftHeld(t) };
            return { kind: "knobs", view: viewNow(), component: moduleName(), pageName: pageName(),
                     navCells: navCells(), empty: slotEmpty(), slot: focus.slot };
        },

        rings(t) {
            if (mixer && mixerOn) return mixer.rings();
            const top = slotEmpty() ? ringsFor(null) : ringsFor(viewNow(), knobRgb());
            const out = [];
            for (let e = 0; e < ENCODERS; e++) out.push(e < PAGE_KNOBS_N ? top[e] : navRing(e));
            return out;
        },
        ring(enc) { return enc < PAGE_KNOBS_N ? ringFor(viewNow(), enc, knobRgb()) : navRing(enc); },
        /** The parameter view the knobs drive (one page, cells 0-7). */
        view: viewNow,

        context(t) {
            return [NAV_KNOBS, mixerOn, mixerOn && shiftHeld(t), focus.slot, focus.component,
                    focus.pageIndex, pageCount()].join("|");
        },
        reading(t) { return readings.at(t); },
        turnHint() { return false; },
        markDrawn() {},
        setFollow() {},
        /* Leaving this layout: drop the Mixer and any hold. */
        reset() { mixerOn = false; shiftDownAt = null; readings.clear(); },
        get shiftHeld() { return shiftHeld(now()); },
    };
}
