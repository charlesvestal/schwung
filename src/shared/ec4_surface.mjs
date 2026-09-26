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
import { decode } from "./e16_input.mjs";
import { abbrev4 } from "./e16_view.mjs";
import { createMixer } from "./e16_mixer.mjs";
import { SHIFT_TAP_MS, TURN_IDLE_MS, ATOMIC_MAX_PACKETS, createSysexAssembler, createPresence,
         createFocus, createBinding, createKnobFeel } from "./surface_core.mjs";
import { NAV_MAP, NAV_KNOBS, NAV_HOLD_MS, screenLabels } from "./layout_common.mjs";
import { createMapLayout } from "./layout_map.mjs";
import { createKnobsLayout } from "./layout_knobs.mjs";
import { createCustomLayout, customSeams, NAV_CUSTOM } from "./layout_custom.mjs";
export { CELL_PREV, CELL_PAGE, CELL_COUNT, CELL_NEXT, CELL_SLOT, CELL_MODULE, CELL_VOL, CELL_PAN }
    from "./layout_knobs.mjs";
export { NAV_HOLD_MS };
import * as ec4 from "./ec4_protocol.mjs";

/* The focus, the controller binding, the knob feel, presence and the Shift
 * grammar are every surface's (surface_core.mjs); what is left in this file is
 * what the EC4 can show and how it is found. */
export { SHIFT_TAP_MS, TURN_IDLE_MS };
export { mixerRange } from "./e16_mixer.mjs";

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
 * + F7 is 35 bytes, 12 packets -- ATOMIC_MAX_PACKETS, the most one SPI frame
 * places whole. */
export const TEXT_CHUNK = 7;
/* Checked, not assumed: a message one packet longer is spliceable. */
if (Math.ceil((7 + 3 + 3 + 3 * TEXT_CHUNK + 1) / 3) > ATOMIC_MAX_PACKETS) {
    throw new Error("ec4: TEXT_CHUNK no longer fits one SPI frame");
}
/* Messages per tick. The carry drains ~3 packets a frame, so two 12-packet
 * messages a tick keep the queue moving without building one up. */
export const MSGS_PER_TICK = 2;
/* With nothing owed, one chunk of the screen is resent this often, so the
 * whole names page is restated every ~2.5 s. */
export const RESTATE_MS = 250;
/* The overlay's hold is the layout's reading hold (layout_common). */
export { READING_HOLD_MS as OVERLAY_HOLD_MS } from "./layout_common.mjs";

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
/* The EC4's step for choices and selectors, for whichever layout is live. */
export const EC4_SELECTOR = { choice: SELECTOR_PULSES, nav: SELECTOR_PULSES, slot: SLOT_PULSES };
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

    /* Seek / keepalive / loss (surface_core createPresence). The probe is the
     * setup request, and any well-formed EC4 message is the device answering:
     * every write is acknowledged, so a surface in use hears from it
     * constantly. No single-message goodbye -- the names are restored by
     * drain() instead (see `goodbye`). */
    const wrap = (body) => [0xF0].concat(body, [0xF7]);
    const presence = createPresence({
        probeMs: QUERY_MS, keepaliveMs: KEEPALIVE_MS, lossMs: LOSS_MS,
        probeMsg: ec4.queryMsg,
        isReply: (body) => ec4.parse(wrap(body)) !== null,
        packetize: ec4.packetize,
    });
    /* null, or the install in progress (see INSTALLING THE SCHWUNG SETUP). */
    let install = null;
    /* The device's own report; null until it has answered. */
    let reportedSetup = null;
    /* Only for asking while the surface is OFF (installing; see tick). */
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


    /* ---- the focus and its controller: every surface's (surface_core) ---- */
    const focus = createFocus({ chainOf, followFocusOf });
    const binding = createBinding({ makeController, focus });
    const feel = createKnobFeel({ pulsesPerDetentOf });
    const { metaOf } = binding;
    const mixer = o.mixer ? createMixer(o.mixer) : null;

    /*
     * ---- the layouts (layout_common.mjs) ----
     *
     * What the sixteen knobs do is the layout's; this file only shows it --
     * sixteen names diffed onto the device, and the layout's READING as the
     * 4x20 overlay. So the layout's repaint and ring callbacks are no-ops
     * here: every tick restates what should be shown and the text writer
     * sends only what differs.
     */
    const navigationOf = o.navigationOf || (() => NAV_KNOBS);
    const layoutCtx = { focus, binding, mixer, feel, chainOf, now, selector: EC4_SELECTOR };
    const layouts = { [NAV_MAP]: createMapLayout(layoutCtx), [NAV_KNOBS]: createKnobsLayout(layoutCtx),
                      [NAV_CUSTOM]: createCustomLayout(Object.assign({}, layoutCtx, customSeams(o))) };
    let layout = layouts[NAV_KNOBS];
    function syncLayout() {
        let want = NAV_KNOBS;
        try { want = navigationOf(); } catch (e) {}
        const next = layouts[want] || layouts[NAV_KNOBS];
        if (next === layout) return;
        layout.reset();
        layout = next;
    }
    const viewNow = () => layout.view();

    function namesNow() {
        const l = screenLabels(layout.screen(now()), { metaOf });
        return l.labels.map((c) => pad(ascii(c), 4)).join("");
    }

    /* The layout's reading as four overlay rows: "[context] >> name", the
     * value, and a bar -- or, for a choice, the options in a row. */
    function overlayNow(t) {
        const r = layout.reading(t);
        if (!r) return null;
        const last = r.list ? listRow(r.list.items, r.list.index)
                   : r.bar ? barRow(r.bar.frac, r.bar.bipolar) : "";
        return [headline(r.context, r.name, r.abbr), "", valueRow(r.value), last]
            .map((row) => pad(ascii(row), ec4.TOTAL_COLS));
    }

    /* ---- the wire ---- */

    const emit = (bytes) => {
        let ok = false;
        try { ok = send(ec4.packetize(bytes)) !== false; } catch (e) { ok = false; }
        if (ok) sentMsgs++;
        return ok;
    };

    /* Present (answering) AND reporting the setup that holds Schwung's map:
     * any other setup is the user's own. */
    const heard = (t) => t - presence.lastReply < LOSS_MS;
    const active = () => presence.enabled && presence.present && reportedSetup === setupOf();

    function onDevice(events, t) {
        /* The bare header is the acknowledgement every write gets. */
        if (events.length === 0) { acks++; return; }
        for (const ev of events) {
            if (ev.type === "setup") reportedSetup = ev.setup;
            else if (ev.type === "key" && active()) {
                /* Shift, and Shift + push, arrive as reports: handed to the
                 * layout as the same events the E16 decodes from notes. */
                if (ev.key === "shift") { syncLayout(); layout.handle({ type: "shift", down: !!ev.pressed }, t); }
                else if (/^push\d+$/.test(ev.key)) {
                    /* Both edges: the Custom layout tells a click from a
                     * learn HOLD by how long the push lasted. */
                    syncLayout();
                    const enc = Number(ev.key.slice(4));
                    layout.handle(ev.pressed ? { type: "push", enc } : { type: "release", enc }, t);
                }
                else log("ec4: " + ev.key + (ev.pressed ? " down" : " up"));
            }
        }
    }

    const asm = createSysexAssembler({
        onMessage: (body) => {
            const events = ec4.parse(wrap(body));
            if (!events) return;
            presence.onSysex(body, now());
            onDevice(events, now());
        },
    });


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
            if (on === presence.enabled) return;
            const wasOurs = active();
            presence.setEnabled(on, now(), send);
            if (on) { goodbye = false; return; }
            /* Only a device showing OUR text is owed its names back: another
             * setup was never written to. */
            goodbye = wasOurs;
            if (goodbye) { names.set("----".repeat(16)); wantOverlay = false; }
        },

        /* Follow Move's screen: the focus comes from followFocusOf, and the
         * slot and module knobs stop moving it -- one-way, as on the E16. */
        setFollow(on) {
            /* The focus parks once (the map layout's nav owns the edge). */
            layouts[NAV_MAP].setFollow(!!on, now());
            layouts[NAV_KNOBS].setFollow(!!on, now());
        },

        /* A write from elsewhere (Move's own grid): into the cache now, so
         * the next reading shows it (surface_core createBinding). */
        noteParamWrite(s, key, value) {
            if (!presence.enabled) return;
            layouts[NAV_CUSTOM].noteWrite(s, key, value);
            binding.noteWrite(s, key, value, viewNow());
        },

        feedMidi(data) {
            asm.feed(data);
            if (!active()) return null;
            const ev = decode(data);
            if (!ev) return null;
            syncLayout();
            layout.handle(ev, now());
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
                    presence.reprobe();
                    log("ec4: Schwung setup installed into setup " + (install.setup + 1));
                    onInstalled(install.setup);
                }
                return;
            }
            if (install && install.phase === "pick" && !presence.enabled) {
                /* pick, with the surface off: still ask which setup is current. */
                if (t - queriedAt >= QUERY_MS && emit(ec4.queryMsg())) queriedAt = t;
                return;
            }
            if (goodbye) {
                drain(t, MSGS_PER_TICK);
                if (names.synced && shownOverlay === false) goodbye = false;
                return;
            }
            if (!presence.enabled) return;
            /* The setup request: fast while seeking, slow while present. */
            presence.tick(t, (packets) => { const ok = send(packets) !== false; if (ok) sentMsgs++; return ok; });
            const isActive = active();
            if (isActive !== wasActive) {
                wasActive = isActive;
                log("ec4: " + (isActive ? "active on setup " + (reportedSetup + 1)
                                        : "inactive (setup " + (reportedSetup === null ? "?" : reportedSetup + 1) + ")"));
                /* Whatever the device shows now, it is not what we last told
                 * it: another setup's names, or a power cycle. */
                names.forget(); total.forget(); shownOverlay = null;
                /* A Shift that went down on another setup has no release here. */
                layout.reset();
            }
            if (!isActive) return;
            syncLayout();
            layout.tick(t);
            if (layout.usesBinding !== false) {
                binding.sync(layout.pagesShown);
                binding.tick();
            }
            names.set(namesNow());
            const rows = overlayNow(t);
            wantOverlay = !!rows;
            if (rows) total.set(rows.map((r) => pad(ascii(r), ec4.TOTAL_COLS)).join(""));
            drain(t, MSGS_PER_TICK);
        },

        /* ---- installing the Schwung setup (see INSTALLING THE SCHWUNG SETUP) ---- */
        installBegin() { install = { phase: "pick", setup: null, packets: null, at: 0 }; queriedAt = -Infinity; presence.reprobe(); },
        /* Take the EC4's current setup as the target and go silent. False if
         * the EC4 has not answered recently -- there is no setup to name. */
        installArm() {
            if (!install || install.phase !== "pick") return false;
            if (reportedSetup === null || !heard(now())) return false;
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
            presence.reprobe();
        },
        get installState() {
            if (!install) return null;
            const t = now();
            return {
                phase: install.phase,
                setup: install.setup,
                progress: install.packets ? install.at / install.packets.length : 0,
                current: (reportedSetup !== null && heard(t)) ? reportedSetup : null,
            };
        },

        get enabled() { return presence.enabled; },
        get present() { return active(); },
        get reportedSetup() { return reportedSetup; },
        get acks() { return acks; },
        get sent() { return sentMsgs; },
        get slot() { return focus.slot; },
        get component() { return focus.component; },
        get pageIndex() { return focus.pageIndex; },
        get focus() { return focus; },
        get mixerOn() { return layout.mixerOn; },
        get layout() { return layout; },
        /** The control document changed (a set load, the web editor). */
        reloadControls() { layouts[NAV_CUSTOM].reload(); },
        get controller() { return binding.controller; },
        view: () => layout.view(),
        /* What the device should be showing -- for tests and the log. */
        screen() { return { names: namesNow(), overlay: overlayNow(now()) }; },
    };
}
