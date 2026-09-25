/*
 * ec4_surface.mjs -- a Faderfox EC4 as Schwung's external control surface.
 *
 * TWO VIEWS, AND NOTHING HIDDEN BEHIND A GESTURE. A tap of Shift (released
 * within SHIFT_TAP_MS, nothing touched) switches between them; holding it is
 * the alternate layer, and the names change to it once it is a hold.
 *
 *   MODULE                                       MIXER (the E16's, e16_mixer.mjs)
 *   | CUTO | RESO | DRIV | ENVA |  page knobs     | Vol  Vol  Vol  Vol  |  alt: pan / solo
 *   | ATTA | DECA | SUST | REL  |  1-8            | SndA SndA SndA SndA |  alt push: 100%
 *   | <PG  | MAIN |  2/5 | PG>  |  pages          | SndB SndB SndB SndB |
 *   | SL 1 | OBXD | VOL  | PAN  |  slot / module  | RtnA RtnB Capt Filt |
 *
 * One page at a time, so a page on the EC4 is the page Move shows on its own
 * eight knobs. Every navigation control is a labelled knob: turn to move, and
 * the <PG / PG> pushes step. VOL and PAN are the focused slot's level (push:
 * mute) and pan (push: centre), the same numbers as the Mixer's.
 *
 * What the device forces:
 *   - PRESENCE. The EC4 has no remote mode. It is ours while it answers the
 *     setup/group request AND reports the setup holding Schwung's map; any
 *     other setup is the user's own and is never written to.
 *   - SHIFT IS SYSEX: its press and release, and Shift + push, are reports.
 *   - THE SCREEN IS TEXT: sixteen 4-character names and a 4x20 overlay that
 *     carries the reading while a control moves.
 *   - EVERY MESSAGE FITS ONE SPI FRAME (<= 12 packets): Move splices its own
 *     MIDI into a SysEx that spans frames (docs/E16_REMOTE.md), measured on
 *     the EC4 as a 206-byte write losing three cells (docs/EC4_SURFACE.md).
 *
 * The input map is the E16's (the setup Schwung installs, ec4_protocol.mjs
 * schwungSetupMsg, sends CC 1-16 relative and notes 0-15 on channel 1), so
 * e16_input.decode reads it and the shim's claim (src/host/e16_claim.h)
 * serves it unchanged.
 *
 * Pure: every host call is injected, so tests/host drives the whole path.
 */
import { createSysexAssembler } from "./e16_surface.mjs";
import { decode } from "./e16_input.mjs";
import { buildView, labelsFor, applyTurn, applyClick, pageHasKnobs, abbrev4, ENCODERS,
         ringAmount, RING_MAX } from "./e16_view.mjs";
import { buildMap } from "./e16_map.mjs";
import { createMixer, SEND_MAX, SEND_STEP, VOLUME_MAX, LEVEL_DB_STEP, LEVEL_DB_FLOOR,
         FILTER_STEP, PAN_STEP } from "./e16_mixer.mjs";
import { displayValue } from "./param_pages/render_page_movy.mjs";
import { ENUM_DELTA_DIV, knobInit, knobStep, KNOB_TYPE_FLOAT, detentsPerStep } from "./knob_engine.mjs";
import * as ec4 from "./ec4_protocol.mjs";

/* Setup 13, 0-based as the device reports it, until EC4 Setup records the one
 * the user installed into (slots 15 and 16 hold the factory Ableton setups). */
export const DEFAULT_SETUP = 12;

/* Asking "which setup?" -- every QUERY_MS until answered, then KEEPALIVE_MS.
 * The EC4 also reports a setup or group change unasked, so the keepalive is
 * only there to notice a device that has gone. */
export const QUERY_MS = 1000;
export const KEEPALIVE_MS = 2000;
/* Nothing heard from the EC4 for this long: it is gone. Every write is
 * acknowledged, so a surface in use hears from it constantly. */
export const LOSS_MS = 5000;

/* Characters per text message: 7 header + 3 page + 3 offset + 3 per character
 * + F7 is 35 bytes, 12 packets -- the most one SPI frame places whole. */
export const TEXT_CHUNK = 7;
/* Messages per tick. The carry drains ~3 packets a frame, so two 12-packet
 * messages a tick keep the queue moving without building one up. */
export const MSGS_PER_TICK = 2;
/* With nothing owed, one chunk of the screen is resent this often, so the
 * whole names page is restated every ~2.5 s. */
export const RESTATE_MS = 250;
/* The overlay stays up this long after the last turn. */
export const OVERLAY_HOLD_MS = 1500;

const NAMES_LEN = ec4.NAMES_CHARS;
const TOTAL_LEN = ec4.TOTAL_CHARS;
const pad = (s, n) => (String(s == null ? "" : s) + " ".repeat(n)).slice(0, n);

/* 7-bit ASCII the EC4's character ROM can show, plus the bar's block:
 * accents folded, the rest dropped. */
function ascii(s) {
    return String(s == null ? "" : s)
        .normalize("NFD").replace(/[̀-ͯ]/g, "")
        .replace(/[^\x20-\x7E█]/g, "");
}

/*
 * A VALUE BAR, one overlay row of whole blocks: 20 cells, so 5% a step.
 *
 * Whole blocks only. The ROM does hold partial-width bar glyphs (0xD0-0xD4
 * short, 0xD6-0xD9 tall), but in two heights that line up neither with each
 * other nor with the full block, so a bar ending in one read as ragged rather
 * than finer. A bipolar value fills from the centre, which is marked when the
 * value sits on it -- an empty row would read as "no value".
 */
export function barRow(frac, bipolar, width) {
    const w = width || ec4.TOTAL_COLS;
    const f = Math.max(0, Math.min(1, Number(frac) || 0));
    const pos = Math.round(f * w);
    if (!bipolar) return ec4.BLOCK.repeat(pos) + " ".repeat(w - pos);
    const mid = w / 2;
    let out = "";
    for (let i = 0; i < w; i++) {
        out += (i >= Math.min(mid, pos) && i < Math.max(mid, pos)) ? ec4.BLOCK : " ";
    }
    if (pos === mid) out = out.slice(0, mid) + "|" + out.slice(mid + 1);
    return out;
}

/*
 * THE TEXT WRITER: what the device should show, what it was last told, and
 * the smallest messages that close the gap.
 *
 * `shown` is a BELIEF about the device, and null means "unknown": a device
 * that has just become ours may be showing anything, so the first pass writes
 * all of it rather than diffing against a guess.
 */
function createPage(page, len) {
    let want = " ".repeat(len);
    let shown = null;
    let restateAt = 0;
    return {
        set(text) { want = pad(text, len); },
        forget() { shown = null; },
        get synced() { return shown === want; },
        /* The next message that brings the device closer, or null. */
        next() {
            if (shown === want) return null;
            let i = 0;
            if (shown !== null) while (i < len && shown[i] === want[i]) i++;
            const n = Math.min(TEXT_CHUNK, len - i);
            return { page, offset: i, text: want.slice(i, i + n) };
        },
        /* The next chunk of the round-robin restate. */
        restate() {
            const offset = restateAt;
            restateAt = (restateAt + TEXT_CHUNK) % len;
            return { page, offset, text: want.slice(offset, offset + Math.min(TEXT_CHUNK, len - offset)) };
        },
        /* The message went out: the device holds these characters now. */
        sent(run) {
            /* From "unknown", only the characters just sent are known: the
             * rest are marked with a byte no text contains, so the next diff
             * carries on from here rather than believing the whole page. */
            const base = shown === null ? "\u0000".repeat(len) : shown;
            shown = base.slice(0, run.offset) + run.text + base.slice(run.offset + run.text.length);
        },
    };
}

/*
 * THE OVERLAY'S HEADLINE: "[context] >> name (ABBR)", centred.
 *
 * Twenty characters, so something gives on a long line, in this order: the
 * abbreviation (it repeats what the knob's own label says), then the context
 * down to its own four-character form, and only then the name -- the name is
 * what the hand is on.
 */
export function headline(context, name, abbr) {
    const W = ec4.TOTAL_COLS;
    const ctx = ascii(context), nm = ascii(name), ab = ascii(abbr);
    const tries = [];
    if (ab) tries.push("[" + ctx + "] >> " + nm + " (" + ab + ")");
    tries.push("[" + ctx + "] >> " + nm);
    tries.push("[" + abbrev4(ctx) + "] >> " + nm);
    let line = tries.find((t) => t.length <= W);
    if (!line) line = ("[" + abbrev4(ctx) + "] >> " + nm).slice(0, W);
    return centre(line);
}

function centre(s) {
    const t = ascii(s).slice(0, ec4.TOTAL_COLS);
    const left = Math.floor((ec4.TOTAL_COLS - t.length) / 2);
    return " ".repeat(left) + t;
}

/*
 * A CHOICE AS A LIST: the options in a row, the current one centred in
 * brackets, its neighbours running off either edge -- so turning right brings
 * the next option in from the right. Replaces the bar for anything that is a
 * choice rather than an amount (enums, toggles, the selectors).
 */
export function listRow(items, index) {
    const W = ec4.TOTAL_COLS;
    const list = (items || []).map((x) => ascii(x));
    if (!list.length || index < 0 || index >= list.length) return "";
    const sel = ("[" + list[index] + "]").slice(0, W);
    const at = Math.max(0, Math.floor((W - sel.length) / 2));
    let left = "", right = "";
    for (let i = index - 1; i >= 0 && left.length < at; i--) left = list[i] + " " + left;
    for (let i = index + 1; i < list.length && right.length < W; i++) right += " " + list[i];
    return (left.slice(-at).padStart(at) + sel + right).slice(0, W).padEnd(W);
}

/* The value, centred in brackets; empty stays empty. */
const valueRow = (v) => (v === "" || v == null ? "" : centre("[" + ascii(v) + "]"));

/* The 4x4 cells of the MODULE view that are not page knobs. */
export const CELL_PREV = 8, CELL_PAGE = 9, CELL_COUNT = 10, CELL_NEXT = 11;
export const CELL_SLOT = 12, CELL_MODULE = 13, CELL_VOL = 14, CELL_PAN = 15;
const PAGE_KNOBS_N = 8;
/* A navigation reading is shorter than a value reading: it is confirming
 * where you went, and the names it covers are what you came to see. */
export const NAV_HOLD_MS = 800;
/* One Mixer value re-read this often, to notice changes made elsewhere. */
const MIXER_REFRESH_MS = 250;

/*
 * ONE FEEL FOR EVERY KNOB: the EC4's pulses are scaled to Move's detents
 * before anything sees them.
 *
 * Measured 2026-09-25: a Move knob sends ~210 detents a rotation (213 CC 71
 * messages of +/-1 over one turn, overtake MIDI trace). The EC4 sends ~72
 * pulses (firmware 2.0 update history; our setup has acceleration off, so
 * one message is one pulse). So one EC4 pulse is ~2.9 of Move's detents,
 * and a rotation of either covers the same ground once scaled. Everything
 * downstream speaks in Move detents -- page knobs through the knob engine
 * (onKnobTurn, one call a detent), the Mixer through the same engine -- so
 * the scaling happens ONCE, here.
 *
 * It is injected (pulsesPerDetentOf) and the host reads an override file,
 * since the EC4 half is the manufacturer's figure rather than a count.
 *
 * CHOICES ARE NOT SCALED THAT WAY. At Move's ratio an enum steps every ~1.4
 * EC4 pulses (ENUM_DELTA_DIV detents), ~50 choices a rotation, and a flick
 * flies past the one you wanted. So slot, module, page and every enum or
 * narrow-int parameter step once per SELECTOR_PULSES of the EC4's own
 * rotation -- a fixed physical angle, like a rotary switch.
 */
export const EC4_PULSES_PER_ROTATION = 72;
export const MOVE_DETENTS_PER_ROTATION = 210;
export const DEFAULT_PULSES_PER_DETENT = EC4_PULSES_PER_ROTATION / MOVE_DETENTS_PER_ROTATION;
/* One choice per 30 degrees: twelve a rotation. */
export const SELECTOR_PULSES = 6;
/* A slot is a bigger move than an option -- the whole surface changes under
 * the hand -- so it takes twice the turn: six a rotation. */
export const SLOT_PULSES = 12;
/* Shift released this soon, with nothing touched, is a TAP (switch view).
 * Any longer is a hold, even if nothing was touched under it. */
export const SHIFT_TAP_MS = 250;
/*
 * INSTALLING THE SCHWUNG SETUP FROM MOVE.
 *
 * The EC4 writes a received single setup into whichever setup is current
 * (measured; see ec4_protocol.mjs schwungSetupMsg), so installing is three
 * steps, each a deliberate press on Move:
 *
 *   pick     the user selects, on the EC4, the setup to replace; the surface
 *            keeps asking which setup is current, so Move can show it
 *   ready    that number is taken, and from here the surface sends NOTHING:
 *            the user now puts the EC4 in receive mode, and any stray message
 *            arriving in receive mode would be read as part of the dump
 *   sending  the ~14 KB message, INSTALL_CHUNK packets a tick (~7 s); a refused send
 *            is retried, never skipped, since a gap corrupts the page it is in
 *   done     the host is told which setup now holds Schwung's map, and the
 *            surface resumes. The setup keeps its old NAME: a single-setup
 *            download carries none (Faderfox's format), so the user names it
 *            on the EC4 if they want it labelled
 *
 * Every page carries a checksum, so a damaged transfer is refused by the
 * EC4 ("Receive Error") rather than stored.
 */
export const INSTALL_CHUNK = 12;   /* packets a tick: the same frame budget as everything else */

/* A pause this long drops a leftover fraction, so the next turn starts clean. */
export const TURN_IDLE_MS = 400;

/*
 * THE MIXER TURNS THROUGH THE KNOB ENGINE TOO.
 *
 * The Mixer model steps in fixed units -- 0.5 dB, 2/127, 0.02 of pan -- with
 * no acceleration, so its knobs felt nothing like a module's. It is not
 * changed (its mute, off-and-back memory and solo are its own); the engine
 * goes in FRONT of it. Each control is treated as a Schwung float knob over
 * its whole range, knobStep says how far that knob would have moved for this
 * detent, and the distance is paid out as the Mixer's own
 * ticks, with the remainder carried to the next detent.
 *
 * Ranges in the Mixer's units, and the size of one of its ticks.
 */
const LEVEL_RANGE = { span: 20 * Math.log10(VOLUME_MAX) - LEVEL_DB_FLOOR, tick: LEVEL_DB_STEP };
const SEND_RANGE = { span: SEND_MAX, tick: SEND_STEP };
const PAN_RANGE = { span: 2, tick: PAN_STEP };
const FILTER_RANGE = { span: 2, tick: FILTER_STEP };
const ENGINE_META = { type: KNOB_TYPE_FLOAT, min: 0, max: 1 };

/* Which range a Mixer encoder moves: row 1 level (alt: pan), rows 2-3 sends,
 * row 4 returns, capture (no travel) and the filter. */
export function mixerRange(enc, alt) {
    const row = Math.floor(enc / 4), col = enc % 4;
    if (row === 0) return alt ? PAN_RANGE : LEVEL_RANGE;
    if (row === 1 || row === 2) return SEND_RANGE;
    if (col < 2) return SEND_RANGE;
    return col === 3 ? FILTER_RANGE : null;
}

export function createEc4Surface(io) {
    const o = io || {};
    const now = o.now || (() => Date.now());
    const send = o.send || (() => false);
    const chainOf = o.chainOf || (() => ({ slots: [] }));
    const followFocusOf = o.followFocusOf || (() => null);
    const makeController = o.makeController || null;
    const setupOf = o.setupOf || (() => DEFAULT_SETUP);
    const pulsesPerDetentOf = o.pulsesPerDetentOf || (() => DEFAULT_PULSES_PER_DETENT);
    const onInstalled = o.onInstalled || (() => {});
    const log = o.log || (() => {});

    let enabled = false;
    /* null, or the install in progress (see INSTALLING THE SCHWUNG SETUP). */
    let install = null;
    /* The device's own report; null until it has answered. */
    let reportedSetup = null;
    let heardAt = -Infinity;
    let queriedAt = -Infinity;
    let wasActive = false;
    /* The names put back to "----" and the overlay hidden, owed after the
     * setting goes off. */
    let goodbye = false;

    const names = createPage(ec4.PAGE_NAMES, NAMES_LEN);
    const total = createPage(ec4.PAGE_TOTAL, TOTAL_LEN);
    let wantOverlay = false;
    /* null = unknown, so the first decision is always sent. */
    let shownOverlay = null;
    let lastRestateAt = -Infinity;
    let restateTurn = 0;
    let acks = 0, sentMsgs = 0;

    let overlayRows = null;
    let overlayUntil = -Infinity;

    /* ---- the focus: ONE of each, whoever is writing it ---- */
    let slot = 0;
    let component = "synth";
    let pageIndex = 0;
    let mixerOn = false;
    let follow = false;
    /* Where each slot and each module was left, so going back is going back. */
    const lastComponent = [null, null, null, null];
    const lastPage = new Map();

    /* Shift: when it went down (null = up), and whether anything was done
     * under it -- a press that did nothing is a TAP, and a tap switches view. */
    let shiftDownAt = null;
    let shiftActed = false;

    const mixerIo = o.mixer || null;
    const mixer = mixerIo ? createMixer(mixerIo) : null;
    let mixerLoaded = false;
    let mixerRefreshAt = -Infinity;

    let ctl = null;
    let loaded = null;
    const focus = { get slot() { return slot; }, get component() { return component; } };
    const metaOf = (key) => (ctl && ctl.metaIndex ? ctl.metaIndex.getOrGuess(key) : null);
    const valueOf = (key) => (ctl && ctl.state && ctl.state.values ? ctl.state.values[key] : undefined);
    /* Only pages with a knob on them, as on the E16 -- and a cell's page is
     * mapped back to the controller's own index, or a skipped page shifts
     * which page a knob drives (test_e16_skipped_pages.sh). */
    const knobPages = () => (ctl && ctl.pages ? ctl.pages.filter(pageHasKnobs) : []);
    const controllerPageOf = (j) => {
        if (!ctl || !ctl.pages) return j;
        let seen = -1;
        for (let i = 0; i < ctl.pages.length; i++) {
            if (pageHasKnobs(ctl.pages[i]) && ++seen === j) return i;
        }
        return j;
    };
    /* ONE page: buildView fills the top half (cells 0-7) from the list it is
     * given, so it is given only the current page. */
    const viewNow = () => {
        const kp = knobPages();
        const p = kp[pageIndex];
        return buildView(p ? [p] : [], 0, { metaOf, valueOf, pageIndexOf: () => controllerPageOf(pageIndex) });
    };
    const pageName = () => { const p = knobPages()[pageIndex]; return p ? String(p.name || "") : ""; };

    /* The slot's modules in chain order (MIDI FX, synth, audio FX), from the
     * same map the E16 uses -- holes are already dropped there. */
    function componentsOf(s) {
        const out = [];
        for (let pg = 0; ; pg++) {
            const m = buildMap(chainOf(), { slot: s, page: pg });
            for (const c of m.cells.slice(4)) if (c) out.push(c);
            if (pg + 1 >= m.pageCount) break;
        }
        return out;
    }
    const moduleNameFor = (s, comp) => {
        const c = componentsOf(s).find((x) => x.component === comp);
        return c ? String(c.label || "") : "";
    };

    function setFocus(s, comp) {
        if (s === slot && comp === component) return;
        lastPage.set(slot + ":" + component, pageIndex);
        slot = s;
        component = comp;
        lastComponent[s] = comp;
        pageIndex = lastPage.get(s + ":" + comp) || 0;
    }

    /* A slot is entered at the module it was left on, else its synth, else
     * its first module. */
    function enterSlot(s) {
        const comps = componentsOf(s);
        const pick = comps.find((c) => c.component === lastComponent[s]) ||
                     comps.find((c) => c.component === "synth") || comps[0];
        setFocus(s, pick ? pick.component : "synth");
    }

    /* ---- the screen ---- */

    const PAGE_COUNT_MAX = 4;
    function moduleNames() {
        const cells = new Array(ENCODERS).fill("");
        const comps = componentsOf(slot);
        const here = comps.find((c) => c.component === component);
        if (here) {
            const l = labelsFor(viewNow(), { metaOf });
            for (let e = 0; e < PAGE_KNOBS_N; e++) cells[e] = l.labels[e];
        }
        const n = knobPages().length;
        cells[CELL_PREV] = "<PG";
        cells[CELL_NEXT] = "PG>";
        cells[CELL_PAGE] = n ? abbrev4(pageName()) : "----";
        const count = (pageIndex + 1) + "/" + n;
        cells[CELL_COUNT] = !n ? "" : (count.length <= PAGE_COUNT_MAX ? count : "P" + (pageIndex + 1));
        cells[CELL_SLOT] = "SL " + (slot + 1);
        cells[CELL_MODULE] = here ? abbrev4(here.label) : "EMPT";
        const t = mixer ? mixer.tracks[slot] : null;
        /* Names of what the knob DOES; a state that changes what it does
         * (mute) is the only exception. A pan position is a value, and a
         * value is for the overlay. */
        cells[CELL_VOL] = t && t.muted ? "MUTE" : "VOL";
        cells[CELL_PAN] = "PAN";
        return cells;
    }

    function panLabel(p) {
        if (p === null || p === undefined || Math.abs(p) < 0.01) return "PAN";
        return (p < 0 ? "L" : "R") + Math.round(Math.abs(p) * 100);
    }

    /* The Mixer's names, or its alternate layer while Shift is held. */
    function mixerNames(shiftHeld) {
        const cells = new Array(ENCODERS).fill("");
        for (let e = 0; e < ENCODERS; e++) {
            const row = Math.floor(e / 4), col = e % 4;
            if (!shiftHeld) {
                /* The E16 Mixer's own label puts a panned track's position
                 * ("L 26") where its level's name goes; ours does not. */
                if (row === 0) {
                    const tr = mixer.tracks[col];
                    cells[e] = tr.soloed ? "SOLO" : tr.muted ? "MUTE" : "VOL";
                } else cells[e] = mixer.cell(e).label || "";
                continue;
            }
            if (row === 0) cells[e] = "PAN";
            else if (row === 1 || row === 2) cells[e] = "100%";
            else cells[e] = col < 2 ? "100%" : "";
        }
        return cells;
    }

    function namesNow() {
        const cells = (mixerOn && mixer) ? mixerNames(shiftHeldNow()) : moduleNames();
        return cells.map((c) => pad(ascii(c), 4)).join("");
    }

    function showReading(rows, t, holdMs) {
        overlayRows = rows.map((r) => pad(ascii(r), ec4.TOTAL_COLS));
        overlayUntil = t + (holdMs || OVERLAY_HOLD_MS);
    }

    function paramReading(enc, t) {
        const c = viewNow().cells[enc];
        if (!c) return;
        /* A bar only for a value that has one: an enum or a text value has
         * no position between min and max to fill to. */
        const numeric = isFinite(Number(c.value)) && c.max > c.min;
        let last = numeric ? barRow(ringAmount(c) / RING_MAX, c.bipolar) : "";
        if (isChoice(c.meta)) { const l = choiceList(c); last = listRow(l.items, l.index); }
        showReading([
            headline(pageName(), String(c.label || c.key), abbrev4(c.label || c.key)),
            "",
            valueRow(displayValue(c.value, metaOf(c.key) || {})),
            last,
        ], t);
    }

    /* The page knob's reading: the page, and the list you are moving along,
     * current one centred. */
    function pageReading(t) {
        const names = knobPages().map((p) => String(p.name || ""));
        showReading([headline(moduleNameFor(slot, component) || "Module", "Page"), "",
                     valueRow(pageName()), listRow(names, pageIndex)], t, NAV_HOLD_MS);
    }
    function slotLevelReading(which, t) {
        const tr = mixer.tracks[slot];
        if (which === "vol") {
            const c = mixer.cell(slot), r = mixer.ringFor(slot);
            showReading([headline("Slot " + (slot + 1), tr.muted ? "Volume (muted)" : "Volume"), "",
                         valueRow(c.value ? c.value + " dB" : ""), barRow(r.amount / RING_MAX, false)], t);
        } else {
            const p = tr.pan === null ? 0 : tr.pan;
            showReading([headline("Slot " + (slot + 1), "Pan"), "",
                         valueRow(Math.abs(p) < 0.01 ? "C" : panLabel(p)), barRow((p + 1) / 2, true)], t);
        }
    }

    function mixerReading(enc, t, alt) {
        const c = mixer.cell(enc), r = mixer.ringFor(enc);
        const track = mixer.nameOf(enc % 4);
        if (alt && enc < 4) {
            const p = mixer.tracks[enc].pan === null ? 0 : mixer.tracks[enc].pan;
            showReading([headline(track, "Pan"), "", valueRow(Math.abs(p) < 0.01 ? "C" : panLabel(p)),
                         barRow((p + 1) / 2, true)], t);
            return;
        }
        /* Row 1 is the LEVEL, whatever the E16 label says (it shows pan). */
        showReading([headline(track, enc < 4 ? "Volume" : (c.label || "")), "",
                     valueRow((c.value || "") + (c.off ? " (off)" : "")),
                     barRow(r.amount / RING_MAX, r.bipolar)], t);
    }

    function overlayNow(t) { return t < overlayUntil ? overlayRows : null; }

    /* ---- input ---- */

    /* Leftover fractions per encoder: pulses toward a detent, and detents
     * toward a selector step. A reversal or a pause starts either over --
     * half a detent the other way is not a reason to move. */
    const pulseAcc = new Array(ENCODERS).fill(0);
    const stepAcc = new Array(ENCODERS).fill(0);
    const turnedAt = new Array(ENCODERS).fill(-Infinity);

    function accumulate(acc, enc, amount, per) {
        if (Math.sign(acc[enc]) !== Math.sign(amount)) acc[enc] = 0;
        acc[enc] += amount;
        const out = Math.trunc(acc[enc] / per);
        acc[enc] -= out * per;
        return out;
    }

    /* EC4 pulses -> Move detents, for a continuous control. */
    function detents(enc, pulses) {
        const per = Number(pulsesPerDetentOf());
        return accumulate(pulseAcc, enc, pulses, per > 0 ? per : DEFAULT_PULSES_PER_DETENT);
    }

    /* EC4 pulses -> choices, one per SELECTOR_PULSES of rotation. */
    const choiceStep = (enc, pulses, per) => accumulate(stepAcc, enc, pulses, per || SELECTOR_PULSES);

    /* A parameter that is a CHOICE: an enum, or an int the engine already
     * steps like one. Those get the selector's physical step. */
    const isChoice = (meta) => !!meta && (meta.type === "enum" || meta.kind === "enum" ||
        meta.type === "bool" || Array.isArray(meta.options) || detentsPerStep(meta) > 1 ||
        (meta.type === "int" && meta.max - meta.min === 1));

    /* A choice's options and which one is current, for listRow. */
    function choiceList(cell) {
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

    /* Move detents -> the Mixer's own ticks, through the knob engine (see
     * THE MIXER TURNS THROUGH THE KNOB ENGINE TOO). One engine state and one
     * carry per control, keyed so a range change (alt) starts clean. */
    const engine = new Map();
    function engineTicks(key, range, d, t) {
        let e = engine.get(key);
        if (!e) { e = { st: knobInit(0.5), carry: 0 }; engine.set(key, e); }
        if (Math.sign(e.carry) !== Math.sign(d)) e.carry = 0;
        const dir = d > 0 ? 1 : -1;
        for (let i = 0; i < Math.abs(d); i++) {
            /* Re-centred each detent: the engine only measures the move, and
             * the real value lives in the Mixer, which does its own clamping. */
            e.st.value = 0.5;
            const moved = knobStep(e.st, ENGINE_META, dir, t) - 0.5;
            e.carry += moved * range.span / range.tick;
        }
        const out = Math.trunc(e.carry);
        e.carry -= out;
        return out;
    }
    function mixerTurn(enc, d, alt, t) {
        const range = mixerRange(enc, alt);
        if (!range) return false;
        const ticks = engineTicks(enc + (alt ? ":alt" : ""), range, d, t);
        return ticks ? mixer.turn(enc, ticks, alt) : false;
    }

    const step = (ticks) => (ticks > 0 ? 1 : -1);

    /* The <PG / PG> buttons step quietly; the page KNOB shows the list. */
    function stepPage(d, t, quiet) {
        const n = knobPages().length;
        const next = Math.max(0, Math.min(n - 1, pageIndex + d));
        if (n && next !== pageIndex) { pageIndex = next; }
        if (!quiet) pageReading(t);
    }

    function turn(enc, pulses, t) {
        if (shiftDownAt !== null) shiftActed = true;
        const alt = shiftDownAt !== null;
        if (t - turnedAt[enc] >= TURN_IDLE_MS) { pulseAcc[enc] = 0; stepAcc[enc] = 0; }
        turnedAt[enc] = t;
        if (mixerOn && mixer) {
            const d = detents(enc, pulses);
            if (d && mixerTurn(enc, d, alt, t)) mixerReading(enc, t, alt);
            return;
        }
        if (enc < PAGE_KNOBS_N) {
            const cell = viewNow().cells[enc];
            if (!cell || !ctl) return;
            /* A choice moves one option per SELECTOR_PULSES: the engine
             * gates an option at ENUM_DELTA_DIV detents, so that many are
             * handed over at once. */
            const d = isChoice(cell.meta) ? choiceStep(enc, pulses) * ENUM_DELTA_DIV : detents(enc, pulses);
            if (d && applyTurn(viewNow(), ctl, enc, d, t)) paramReading(enc, t);
            return;
        }
        const sel = (enc <= CELL_MODULE) ? choiceStep(enc, pulses, enc === CELL_SLOT ? SLOT_PULSES : SELECTOR_PULSES) : 0;
        const ticks = enc > CELL_MODULE ? detents(enc, pulses) : 0;
        if (enc <= CELL_NEXT) { if (sel) stepPage(step(sel), t); return; }
        if (enc === CELL_SLOT) {
            if (follow || !sel) return;
            const s = Math.max(0, Math.min(3, slot + step(sel)));
            /* No overlay: the slot and module cells already show where you
             * are, and a reading would cover them while you look. */
            if (s !== slot) enterSlot(s);
            return;
        }
        if (enc === CELL_MODULE) {
            if (follow || !sel) return;
            const comps = componentsOf(slot);
            const i = comps.findIndex((c) => c.component === component);
            const next = comps[Math.max(0, Math.min(comps.length - 1, (i < 0 ? 0 : i) + step(sel)))];
            if (next) setFocus(slot, next.component);
            return;
        }
        if (!mixer) return;
        if (!ticks) return;
        if (enc === CELL_VOL) {
            if (mixerTurn(slot, ticks, false, t)) slotLevelReading("vol", t);
            return;
        }
        if (enc === CELL_PAN) {
            /* The Mixer's pan is its level knob's alternate: row 1, Shift. */
            if (mixerTurn(slot, ticks, true, t)) slotLevelReading("pan", t);
        }
    }

    function push(enc, t) {
        if (shiftDownAt !== null) shiftActed = true;
        const alt = shiftDownAt !== null;
        if (mixerOn && mixer) {
            if (mixer.push(enc, alt)) mixerReading(enc, t, false);
            return;
        }
        if (enc < PAGE_KNOBS_N) {
            if (!ctl) return;
            const hit = applyClick(viewNow(), ctl, enc);
            if (hit) paramReading(enc, t);
            return;
        }
        if (enc === CELL_PREV) { stepPage(-1, t, true); return; }
        if (enc === CELL_NEXT) { stepPage(1, t, true); return; }
        if (!mixer) return;
        if (enc === CELL_VOL) { if (mixer.push(slot, false)) slotLevelReading("vol", t); return; }
        if (enc === CELL_PAN) {
            const tr = mixer.tracks[slot];
            if (mixerIo.setSlot(slot, "slot:pan", "0.00") !== false) tr.pan = 0;
            slotLevelReading("pan", t);
        }
    }

    /* Is Shift a HOLD yet? Once it has been down SHIFT_TAP_MS, or anything
     * was done under it -- before that it may still be a tap, so the names
     * do not flash the alternate layer on every view switch. */
    function shiftHeldNow() {
        return shiftDownAt !== null && (shiftActed || now() - shiftDownAt >= SHIFT_TAP_MS);
    }

    function shift(down, t) {
        if (down) { shiftDownAt = t; shiftActed = false; return; }
        const tap = shiftDownAt !== null && !shiftActed && t - shiftDownAt < SHIFT_TAP_MS;
        shiftDownAt = null;
        if (!tap) return;
        mixerOn = !mixerOn && !!mixer;
        overlayUntil = -Infinity;
        if (mixerOn && !mixerLoaded) { mixer.load(); mixerLoaded = true; }
    }

    /* ---- the wire ---- */

    const emit = (bytes) => {
        let ok = false;
        try { ok = send(ec4.packetize(bytes)) !== false; } catch (e) { ok = false; }
        if (ok) sentMsgs++;
        return ok;
    };

    const active = () => enabled && reportedSetup === setupOf() && now() - heardAt < LOSS_MS;

    function onDevice(events, t) {
        heardAt = t;
        /* The bare header is the acknowledgement every write gets. */
        if (events.length === 0) { acks++; return; }
        for (const ev of events) {
            if (ev.type === "setup") reportedSetup = ev.setup;
            else if (ev.type === "key" && active()) {
                if (ev.key === "shift") shift(ev.pressed, t);
                else if (/^push\d+$/.test(ev.key)) { if (ev.pressed) push(Number(ev.key.slice(4)), t); }
                else log("ec4: " + ev.key + (ev.pressed ? " down" : " up"));
            }
        }
    }

    const asm = createSysexAssembler({
        onMessage: (body) => {
            const events = ec4.parse([0xF0].concat(body, [0xF7]));
            if (events) onDevice(events, now());
        },
    });

    function syncFocus() {
        if (follow) {
            const f = followFocusOf();
            /* A null is not a plan (the tri-state rule): keep where we are. */
            if (f && typeof f.slot === "number" && f.component) setFocus(f.slot | 0, f.component);
        }
        if (!ctl && makeController) ctl = makeController(focus);
        if (!ctl) return;
        const sig = slot + ":" + component;
        if (sig === loaded) return;
        loaded = sig;
        ctl.load({ slot, component, prefix: component });
    }

    /* One tick's worth of messages: overlay visibility first when hiding (a
     * stale reading over the names is the worse screen), text next, showing
     * last so an overlay appears whole. */
    function drain(t, budget) {
        let n = 0;
        const out = (bytes) => { if (n >= budget) return false; if (!emit(bytes)) { n = budget; return false; } n++; return true; };
        if (shownOverlay !== false && !wantOverlay) {
            if (out(ec4.hideTotalMsg())) shownOverlay = false;
        }
        for (const p of wantOverlay ? [total, names] : [names]) {
            let run;
            while (n < budget && (run = p.next())) {
                if (!out(ec4.textMsg(run.page, [run]))) return n;
                p.sent(run);
            }
        }
        if (wantOverlay && shownOverlay !== true && total.synced) {
            if (out(ec4.showTotalMsg())) shownOverlay = true;
        }
        if (n === 0 && t - lastRestateAt >= RESTATE_MS) {
            lastRestateAt = t;
            /* Every fourth restate is the overlay's visibility, the rest are
             * text: a lost hide would otherwise strand a reading on screen. */
            restateTurn = (restateTurn + 1) % 4;
            if (restateTurn === 0) out(wantOverlay ? ec4.showTotalMsg() : ec4.hideTotalMsg());
            else {
                const run = (wantOverlay && restateTurn === 2) ? total.restate() : names.restate();
                out(ec4.textMsg(run.page, [run]));
            }
        }
        return n;
    }

    return {
        setEnabled(on) {
            on = !!on;
            if (on === enabled) return;
            const wasOurs = active();
            enabled = on;
            if (on) { goodbye = false; queriedAt = -Infinity; return; }
            /* Only a device showing OUR text is owed its names back: another
             * setup was never written to. */
            goodbye = wasOurs;
            if (goodbye) { names.set("----".repeat(16)); wantOverlay = false; }
        },

        /* Follow Move's screen: the focus comes from followFocusOf, and the
         * slot and module knobs stop moving it -- one-way, as on the E16. */
        setFollow(on) { follow = !!on; },

        noteParamWrite(s, key, value) {
            if (!enabled || !ctl || !ctl.state || !ctl.state.values) return;
            if ((s | 0) !== slot) return;
            const k = String(key);
            for (const cell of viewNow().cells) {
                if (cell && (k === cell.key || k === component + ":" + cell.key)) {
                    ctl.state.values[cell.key] = String(value);
                }
            }
        },

        feedMidi(data) {
            asm.feed(data);
            if (!active()) return null;
            const ev = decode(data);
            if (!ev) return null;
            const t = now();
            if (ev.type === "turn") turn(ev.enc, ev.ticks, t);
            else if (ev.type === "push") push(ev.enc, t);
            return ev;
        },

        tick() {
            const t = now();
            if (install && (install.phase === "ready" || install.phase === "sending")) {
                /* ready / sending: nothing but the install goes out. */
                if (install.phase !== "sending") return;
                while (install.at < install.packets.length) {
                    const chunk = install.packets.slice(install.at, install.at + INSTALL_CHUNK * 4);
                    let ok = false;
                    try { ok = send(chunk) !== false; } catch (e) { ok = false; }
                    if (!ok) return;            /* refused: the same chunk next tick */
                    install.at += chunk.length;
                    break;                      /* one chunk a tick */
                }
                if (install.at >= install.packets.length) {
                    /* Done, and the surface resumes at once: the EC4 has been
                     * in its own menus, so what it shows is not what we last
                     * told it. */
                    install.phase = "done";
                    names.forget(); total.forget(); shownOverlay = null;
                    queriedAt = -Infinity;
                    log("ec4: Schwung setup installed into setup " + (install.setup + 1));
                    onInstalled(install.setup);
                }
                return;
            }
            if (install && install.phase === "pick" && !enabled) {
                /* pick, with the surface off: still ask which setup is current. */
                if (t - queriedAt >= QUERY_MS && emit(ec4.queryMsg())) queriedAt = t;
                return;
            }
            if (goodbye) {
                drain(t, MSGS_PER_TICK);
                if (names.synced && shownOverlay === false) goodbye = false;
                return;
            }
            if (!enabled) return;
            /* Seeking (never answered, or silent a while) asks faster. */
            const seeking = reportedSetup === null || t - heardAt >= KEEPALIVE_MS * 2;
            if (t - queriedAt >= (seeking ? QUERY_MS : KEEPALIVE_MS)) {
                if (emit(ec4.queryMsg())) queriedAt = t;
            }
            const isActive = active();
            if (isActive !== wasActive) {
                wasActive = isActive;
                log("ec4: " + (isActive ? "active on setup " + (reportedSetup + 1)
                                        : "inactive (setup " + (reportedSetup === null ? "?" : reportedSetup + 1) + ")"));
                /* Whatever the device shows now, it is not what we last told
                 * it: another setup's names, or a power cycle. */
                names.forget(); total.forget(); shownOverlay = null;
                shiftDownAt = null;
            }
            if (!isActive) return;
            syncFocus();
            if (ctl) ctl.tick();
            if (mixer) {
                /* VOL and PAN read the Mixer's model, so it is loaded once
                 * the surface is ours, then kept by a slow rotation. */
                if (!mixerLoaded) { mixer.load(); mixerLoaded = true; }
                else if (t - mixerRefreshAt >= MIXER_REFRESH_MS) { mixerRefreshAt = t; mixer.refreshNext(); }
            }
            names.set(namesNow());
            const rows = overlayNow(t);
            wantOverlay = !!rows;
            if (rows) total.set(rows.map((r) => pad(ascii(r), ec4.TOTAL_COLS)).join(""));
            drain(t, MSGS_PER_TICK);
        },

        /* ---- installing the Schwung setup (see INSTALLING THE SCHWUNG SETUP) ---- */
        installBegin() { install = { phase: "pick", setup: null, packets: null, at: 0 }; queriedAt = -Infinity; },
        /* Take the EC4's current setup as the target and go silent. False if
         * the EC4 has not answered recently -- there is no setup to name. */
        installArm() {
            if (!install || install.phase !== "pick") return false;
            if (reportedSetup === null || now() - heardAt >= LOSS_MS) return false;
            install.setup = reportedSetup;
            install.phase = "ready";
            return true;
        },
        installSend() {
            if (!install || install.phase !== "ready") return false;
            install.packets = ec4.packetize(ec4.schwungSetupMsg(install.setup, 0));
            install.at = 0;
            install.phase = "sending";
            return true;
        },
        installEnd() {
            install = null;
            /* The EC4 has been in its own menus: whatever it shows now is not
             * what we last told it. */
            names.forget(); total.forget(); shownOverlay = null;
            queriedAt = -Infinity;
        },
        get installState() {
            if (!install) return null;
            const t = now();
            return {
                phase: install.phase,
                setup: install.setup,
                progress: install.packets ? install.at / install.packets.length : 0,
                current: (reportedSetup !== null && t - heardAt < LOSS_MS) ? reportedSetup : null,
            };
        },

        get enabled() { return enabled; },
        get present() { return active(); },
        get reportedSetup() { return reportedSetup; },
        get acks() { return acks; },
        get sent() { return sentMsgs; },
        get slot() { return slot; },
        get component() { return component; },
        get pageIndex() { return pageIndex; },
        get mixerOn() { return mixerOn; },
        get controller() { return ctl; },
        view: viewNow,
        /* What the device should be showing -- for tests and the log. */
        screen() { return { names: namesNow(), overlay: overlayNow(now()) }; },
    };
}
