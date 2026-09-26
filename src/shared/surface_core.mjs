/*
 * surface_core.mjs -- what every external control surface shares.
 *
 * Two devices drive Schwung from outside Move today: the OXI E16
 * (e16_surface.mjs, a drawn 128x64 panel and LED rings) and the Faderfox EC4
 * (ec4_surface.mjs, sixteen 4-character names and a 4x20 text overlay). They
 * differ in what they can SHOW and how they are FOUND. They must not differ in
 * what they DRIVE -- and they did: the EC4 arrived with its own copy of the
 * focus, the controller binding and the noteParamWrite path, each already
 * drifting from the E16's (the EC4 remembered a slot's module and page; the
 * E16's write path restated a ring and the EC4's did not). A fix to one would
 * have been a bug left in the other. So the half that is not about the device
 * lives here, once:
 *
 *   createFocus     THE focus: slot / component / page, Follow Focus, and the
 *                   parked focus Follow gives back
 *   createBinding   the surface's OWN page controller, bound to that focus
 *   createKnobFeel  device pulses -> Move detents, choice stepping, and the
 *                   Mixer driven through the knob engine
 *   createPresence  seek / keepalive / loss, with the device's probe injected
 *   createSysexAssembler  inbound SysEx reassembly
 *
 * ONE SHIFT GRAMMAR. A TAP of Shift (released within SHIFT_TAP_MS with nothing
 * done under it) switches Module <-> Mixer on every surface. A HOLD is the
 * device's own modifier layer: the slot map on the E16, the alternate names on
 * the EC4, and on both, in the Mixer, pan / solo / 100%.
 *
 * ONE WIRE LIMIT. A message longer than ATOMIC_MAX_PACKETS straddles SPI frames
 * and Move splices its own MIDI into it (docs/E16_REMOTE.md, "The garbling");
 * measured on both devices.
 *
 * Everything is pure and injected, so tests/host drives it with no device.
 */
import { buildMap } from "./e16_map.mjs";
import { buildView, pageHasKnobs } from "./e16_view.mjs";
import { knobInit, knobStep, KNOB_TYPE_FLOAT } from "./knob_engine.mjs";
import { mixerRange } from "./e16_mixer.mjs";

/* The most USB-MIDI packets one SPI frame places whole (UI_MIDI_CARRY_ATOMIC_MAX
 * in ui_midi_out_carry.h). Every live message on every surface fits it. */
export const ATOMIC_MAX_PACKETS = 12;

/* Shift released this soon, with nothing touched, is a TAP (switch view). */
export const SHIFT_TAP_MS = 250;

/* A knob idle this long drops a leftover fraction, so the next turn starts clean. */
export const TURN_IDLE_MS = 400;

const SLOTS = 4;

/* ------------------------------------------------------------------------ */

/*
 * Inbound SysEx reassembly.
 *
 * `onMidiMessageExternal` hands over 1-3 bytes at a time with the USB-MIDI CIN
 * already stripped, and there is no length field to trust -- an end packet's
 * trailing bytes are padding.  docs/SYSEX.md spells out the four behaviours;
 * the last two are the ones that get skipped:
 *
 *   - start on F0, complete on F7
 *   - SKIP anything >= F8.  System realtime (clock, start, stop) legitimately
 *     interleaves inside a SysEx message and is not part of it.
 *   - ABORT on any other status byte >= 0x80.  The message was interrupted,
 *     and splicing what follows onto it yields a plausible third message that
 *     is pure fiction -- worse than a dropped one, because it parses.
 *   - CAP the buffer, or one lost F7 leaks until the process dies.
 */
export const MAX_SYSEX = 1024;

export function createSysexAssembler(opts) {
    const o = opts || {};
    const max = o.max === undefined ? MAX_SYSEX : o.max;
    const onMessage = o.onMessage || (() => {});
    let buf = null;

    return {
        /** @param {ArrayLike<number>} bytes 1-3 bytes as delivered. */
        feed(bytes) {
            for (let i = 0; i < bytes.length; i++) {
                const b = bytes[i] & 0xFF;
                if (b >= 0xF8) continue;
                if (b === 0xF0) { buf = []; continue; }
                if (b === 0xF7) {
                    if (buf) { const done = buf; buf = null; onMessage(done); }
                    continue;
                }
                if (b >= 0x80) { buf = null; continue; }   /* interrupted */
                if (!buf) continue;                        /* stray data byte */
                if (buf.length >= max) { buf = null; continue; }
                buf.push(b);
            }
        },
        /** Test seam: is a message part-assembled right now? */
        get pending() { return buf ? buf.length : -1; },
    };
}

/* ------------------------------------------------------------------------ */

/*
 * PRESENCE: seek, hold, release.
 *
 * Neither device can be ASKED about from Linux -- nothing on Move's USB-A
 * enumerates (docs/SYSEX.md) -- so a surface SEEKS: it sends its probe until
 * something answers, keeps probing slowly while present, and treats silence
 * past `lossMs` as gone. The probe is the device's: ENTER REMOTE MODE on the
 * E16 (which is also what puts a replugged E16 back into remote mode), the
 * setup request on the EC4.
 *
 * The goodbye is OWED, not sent-and-forgotten: `send` returns false when the
 * outbound buffer is full, and a refused EXIT would leave an E16 blank with the
 * surface switched off. tick() drains it -- which is why tick() does work while
 * disabled. A device with no single-message goodbye (the EC4, which restores
 * sixteen names) passes no `exitMsg` and does its own.
 *
 * @param {object}   opts
 * @param {function} opts.probeMsg     () -> SysEx bytes
 * @param {function} opts.isReply      (body) -> true if this body is the device
 * @param {function} [opts.exitMsg]    () -> SysEx bytes, or absent
 * @param {function} opts.packetize    bytes -> USB-MIDI packets
 */
export function createPresence(opts) {
    const o = opts || {};
    const probeMs = o.probeMs;
    const keepaliveMs = o.keepaliveMs;
    const lossMs = o.lossMs;
    const probeMsg = o.probeMsg;
    const isReply = o.isReply;
    const exitMsg = o.exitMsg || null;
    const packetize = o.packetize;

    let enabled = false;
    let present = false;
    let lastProbe = -Infinity;   /* when a probe was actually SENT */
    let lastReply = -Infinity;
    let exitOwed = false;

    /* A send that reports false has NOT gone out: the probe clock must not
     * advance past a message the device never saw. */
    const emit = (send, bytes) => {
        try { return send(packetize(bytes)) !== false; }
        catch (e) { return false; }
    };

    return {
        /** Idempotent: a repeated `true` must not re-arm, nor a repeated
         *  `false` send a second goodbye. */
        setEnabled(on, now, send) {
            on = !!on;
            if (on === enabled) return;
            enabled = on;
            if (on) {
                /* Probe on the very next tick: switching the setting on is the
                 * user asking for the device now. The loss clock measures
                 * silence since we started looking, not since the epoch. */
                lastProbe = -Infinity;
                lastReply = now;
                exitOwed = false;
                return;
            }
            present = false;
            if (!exitMsg) return;
            exitOwed = true;
            if (emit(send, exitMsg())) exitOwed = false;
        },

        /** One frame of the machine. Sends at most one message. */
        tick(now, send) {
            if (exitOwed) {
                if (emit(send, exitMsg())) exitOwed = false;
                return;
            }
            if (!enabled) return;
            /* Presence expires on SILENCE, never on a probe going out. */
            if (present && now - lastReply >= lossMs) {
                present = false;
                /* Seek again at once: a replug must not wait out a keepalive. */
                lastProbe = -Infinity;
            }
            const period = present ? keepaliveMs : probeMs;
            if (now - lastProbe < period) return;
            if (emit(send, probeMsg())) lastProbe = now;
        },

        /** A reassembled inbound body. Anything not the device is ignored --
         *  the port is shared, and another maker's ID is not an error. */
        onSysex(body, now) {
            if (!isReply(body)) return false;
            present = true;
            lastReply = (now === undefined || now === null) ? lastReply : now;
            return true;
        },

        /** Probe again next tick (after the device was busy elsewhere). */
        reprobe() { lastProbe = -Infinity; },

        get enabled() { return enabled; },
        get present() { return present; },
        get lastReply() { return lastReply; },
    };
}

/* ------------------------------------------------------------------------ */

/* A slot's modules in chain order (MIDI FX, synth, audio FX), holes dropped --
 * the same list the E16's map draws. */
export function componentsOf(chain, slot) {
    const out = [];
    for (let pg = 0; ; pg++) {
        const m = buildMap(chain, { slot, page: pg });
        for (const c of m.cells.slice(SLOTS)) if (c) out.push(c);
        if (pg + 1 >= m.pageCount) break;
    }
    return out;
}

/*
 * THE FOCUS -- one of each, whoever is writing it.
 *
 * `slot` / `component` / `pageIndex` are the surface's focus whichever source
 * is writing them: the user's own navigation, or Follow Focus mirroring Move's
 * screen. A mode with its own copy is a surface that disagrees with itself --
 * the rings drawn for one component, the screen for another, with nothing
 * logged because both halves are behaving correctly.
 *
 * GOING BACK IS GOING BACK. Each module remembers its page and each slot its
 * module, so leaving a module and returning lands where you were, and entering
 * a slot lands on the module you last edited there (else its synth, else its
 * first module). The EC4 had this and the E16 reset to page 1.
 *
 * FOLLOW IS ONE-WAY. While on, followFocusOf() writes the focus and nothing a
 * surface does may move it; the surface's own focus is PARKED on the OFF->ON
 * edge and restored on the way back, because Follow is turned on to look and
 * turned off to carry on. A null from followFocusOf is not a plan (the
 * tri-state rule): the focus stays where it is.
 */
export function createFocus(opts) {
    const o = opts || {};
    const chainOf = o.chainOf || (() => ({ slots: [] }));
    const followFocusOf = o.followFocusOf || (() => null);
    const onChange = o.onChange || (() => {});

    let slot = o.slot | 0;
    let component = o.component || "synth";
    let pageIndex = 0;
    let follow = !!o.follow;
    let parked = null;
    const lastComponent = new Array(SLOTS).fill(null);
    const lastPage = new Map();
    const key = (s, c) => s + ":" + c;

    function set(s, comp) {
        s = s | 0;
        if (s === slot && comp === component) return false;
        lastPage.set(key(slot, component), pageIndex);
        slot = s;
        component = comp;
        lastComponent[s] = comp;
        pageIndex = lastPage.get(key(s, comp)) || 0;
        onChange();
        return true;
    }

    function pickIn(s) {
        const comps = componentsOf(chainOf(), s);
        const pick = comps.find((c) => c.component === lastComponent[s]) ||
                     comps.find((c) => c.component === "synth") || comps[0];
        return pick ? pick.component : "synth";
    }

    return {
        get slot() { return slot; },
        get component() { return component; },
        get pageIndex() { return pageIndex; },
        get follow() { return follow; },

        /** Jump to a module. False if already there. */
        set,
        /** Enter a slot at the module it was left on. */
        enterSlot(s) { return set(s, pickIn(s | 0)); },
        setPage(i) {
            i = Math.max(0, i | 0);
            if (i === pageIndex) return false;
            pageIndex = i;
            onChange();
            return true;
        },
        /** The slot's modules in chain order. */
        components(s) { return componentsOf(chainOf(), s === undefined ? slot : s); },

        /** Idempotent, and the idempotence matters: a second setFollow(true)
         *  that parked again would park the FOLLOW source as if it were the
         *  user's own focus, and the real one would be gone. */
        setFollow(on) {
            on = !!on;
            if (on === follow) return false;
            follow = on;
            if (on) {
                parked = { slot, component, pageIndex };
                this.poll();
            } else if (parked) {
                slot = parked.slot;
                component = parked.component;
                pageIndex = parked.pageIndex;
                parked = null;
            }
            onChange();
            return true;
        },

        /** Take one reading from Move's screen while following. Returns true
         *  on a change. A poll, not a subscription: see e16_surface createNav. */
        poll() {
            if (!follow) return false;
            const f = followFocusOf();
            if (!f || typeof f.slot !== "number" || !f.component) return false;
            return set(f.slot | 0, f.component);
        },
    };
}

/* ------------------------------------------------------------------------ */

/*
 * THE SURFACE'S OWN PAGE CONTROLLER, bound to a focus.
 *
 * Its own, never Move's: a controller has ONE current page, and a turn on a
 * cell of another page has to goToPage() first (e16_view applyTurn) -- sharing
 * Move's would drag Move's screen with it. `makeController` is a FACTORY
 * handed a live view of the focus, so one controller follows every jump.
 *
 * Loaded on a CHANGE of slot:component only: load() reads the contract (two
 * blocking round trips), which per frame would be most of a frame.
 *
 * Pages are the ones with a KNOB on them (pageHasKnobs), and a surface page
 * number counts those -- mapped back to the controller's own index by
 * controllerPageOf, or a skipped page shifts which page a knob drives
 * (test_e16_skipped_pages.sh).
 */
export function createBinding(opts) {
    const o = opts || {};
    const makeController = o.makeController || null;
    const focus = o.focus;

    let ctl = null;
    let loaded = null;
    /* The surface page the controller was last moved to (see sync). */
    let syncedPage = null;

    const live = {
        get slot() { return focus.slot; },
        get component() { return focus.component; },
    };

    const metaOf = (key) => (ctl && ctl.metaIndex ? ctl.metaIndex.getOrGuess(key) : null);
    /* From the controller's cache, never a fresh read: called per cell per
     * view build, and an IPC round trip costs more than a page render. */
    const valueOf = (key) => (ctl && ctl.state && ctl.state.values ? ctl.state.values[key] : undefined);
    const knobPages = () => (ctl && ctl.pages ? ctl.pages.filter(pageHasKnobs) : []);
    const controllerPageOf = (j) => {
        if (!ctl || !ctl.pages) return j;
        let seen = -1;
        for (let i = 0; i < ctl.pages.length; i++) {
            if (pageHasKnobs(ctl.pages[i]) && ++seen === j) return i;
        }
        return j;
    };

    return {
        get controller() { return ctl; },
        /** "<slot>:<component>" the controller holds, or null. */
        get loaded() { return loaded; },
        metaOf,
        valueOf,
        knobPages,
        controllerPageOf,
        ensure() {
            if (!ctl && makeController) ctl = makeController(live);
            return ctl;
        },
        /** Point the controller at the focus. True if it (re)loaded. */
        /** `shown`: how many pages the live layout puts on the knobs (the
         *  map layout two, the knobs layout one). */
        sync(shown) {
            if (!this.ensure()) return false;
            const sig = focus.slot + ":" + focus.component;
            if (sig !== loaded) {
                loaded = sig;
                syncedPage = null;
                ctl.load({ slot: focus.slot, component: focus.component, prefix: focus.component });
                return true;
            }
            /*
             * THE CONTROLLER GOES WHERE THE SURFACE PAGES. Its read rotation
             * walks ITS current page (plus a one-off warm of the adjacent
             * pages' uncached keys), and nothing moved it when the surface
             * paged -- only a turn did (applyTurn's goToPage). So a page
             * reached by paging showed empty values and dark rings until a
             * knob was turned (hardware, Teng, 2026-09-26). goToPage warms
             * the page it lands on.
             *
             * On a CHANGE of the surface's page only: a bottom-half turn
             * rightly moves the controller to page N+1, and restating N every
             * tick would drag it back and forth.
             */
            /* EVERY page on the knobs is warmed, last first, so the
             * controller ends on the top one: the adjacent-page prefetch never
             * filled the map layout's bottom half by itself. */
            const n = Math.max(1, shown | 0);
            const key = focus.pageIndex + "x" + n;
            if (key !== syncedPage && knobPages().length && typeof ctl.goToPage === "function") {
                syncedPage = key;
                const count = knobPages().length;
                for (let i = n - 1; i >= 0; i--) {
                    const j = focus.pageIndex + i;
                    if (j >= count) continue;
                    const want = controllerPageOf(j);
                    if (ctl.pageIndex !== want) ctl.goToPage(want, { remember: false });
                }
            }
            return false;
        },
        tick() { if (ctl) ctl.tick(); },

        /*
         * WHETHER THERE IS ANYTHING TO TURN, three ways, because a blank
         * screen answers none of them (hardware, 2026-09-26):
         *   "loading"  no plan yet -- the contract read has not completed.
         *              The planner appends My Presets and Module to every
         *              component, so an EMPTY plan is never a real module.
         *   "none"     planned, and not one page puts a key under a knob
         *   "ok"
         */
        controls() {
            if (!ctl) return "loading";
            /* A module whose contract read NEVER answers (Teng errors its
             * ui_hierarchy read, 2026-09-26) is given up on after
             * CONTRACT_RETRY_LIMIT: that is "none", or the surface would say
             * Loading forever. */
            if (ctl.state && ctl.state.contractGaveUp) return "none";
            if (ctl.contractUnresolved || !ctl.pages || !ctl.pages.length) return "loading";
            return knobPages().length ? "ok" : "none";
        },

        /** Every knob page, current one first-class (the E16's 16 cells). */
        view(pageIndex) {
            return buildView(knobPages(), pageIndex | 0, { metaOf, valueOf, pageIndexOf: controllerPageOf });
        },
        /** ONE page on cells 0-7 (the EC4). */
        pageView(pageIndex) {
            const pages = knobPages();
            const p = pages[pageIndex | 0];
            const v = buildView(p ? [p] : [], 0, { metaOf, valueOf, pageIndexOf: () => controllerPageOf(pageIndex | 0) });
            /* The header counts the module's pages, not the one-page list the
             * view was built from ("2/5", never "1/1"). */
            if (v.headers[0]) { v.headers[0].index = pageIndex | 0; v.headers[0].count = pages.length; }
            return v;
        },
        pageName(pageIndex) {
            const p = knobPages()[pageIndex | 0];
            return p ? String(p.name || "") : "";
        },

        /*
         * A parameter was WRITTEN by someone other than this surface -- most
         * often Schwung's own knob grid on Move. The written value goes
         * straight into the controller's cache, so the surface shows it now
         * instead of on the next staggered re-read (~0.5 s on hardware,
         * 2026-09-24). Keys arrive with or without the component prefix; only
         * cells in `view` are touched, and those are returned so a surface can
         * restate what it draws for them.
         */
        noteWrite(slot, key, value, view) {
            if (!ctl || !ctl.state || !ctl.state.values) return [];
            if ((slot | 0) !== focus.slot) return [];
            const k = String(key);
            const hit = [];
            for (const cell of view.cells) {
                if (!cell) continue;
                if (k !== cell.key && k !== focus.component + ":" + cell.key) continue;
                ctl.state.values[cell.key] = String(value);
                hit.push(cell);
            }
            return hit;
        },
    };
}

/* ------------------------------------------------------------------------ */

/*
 * ONE FEEL FOR EVERY KNOB.
 *
 * Everything downstream speaks in MOVE DETENTS: a page knob goes through the
 * knob engine (onKnobTurn, one call a detent), exactly as Move's own knob does.
 * A device whose rotation is a different number of pulses is scaled ONCE, here
 * (pulsesPerDetentOf; the E16's is 1). A leftover fraction carries to the next
 * pulse; a reversal or a pause (TURN_IDLE_MS) starts it over.
 *
 * CHOICES may step by ANGLE instead (selector: true): at Move's ratio a coarse
 * encoder steps an enum every pulse or two and a flick flies past the option.
 *
 * THE MIXER TURNS THROUGH THE ENGINE TOO. Its model steps in fixed units
 * (0.5 dB, 2/127, 0.02 of pan) with no acceleration, so its knobs felt nothing
 * like a module's. The engine goes in FRONT of it: each control is a float knob
 * over its whole range (mixerRange), knobStep says how far that knob would
 * have moved, and the distance is paid out in the Mixer's own ticks with the
 * remainder carried.
 */
const ENGINE_META = { type: KNOB_TYPE_FLOAT, min: 0, max: 1 };

export function createKnobFeel(opts) {
    const o = opts || {};
    const pulsesPerDetentOf = o.pulsesPerDetentOf || (() => 1);
    /* A device that accelerates by itself (the E16 sends 2 / 4 / 8 ticks a
     * message when turned fast) can have that compressed before anything else
     * sees it -- see e16Curve. Identity by default. */
    const curve = o.pulseCurve || ((p) => p);
    const encoders = o.encoders || 16;
    const idleMs = o.idleMs === undefined ? TURN_IDLE_MS : o.idleMs;

    const pulseAcc = new Array(encoders).fill(0);
    const stepAcc = new Array(encoders).fill(0);
    const turnedAt = new Array(encoders).fill(-Infinity);
    const engine = new Map();

    function accumulate(acc, enc, amount, per) {
        if (Math.sign(acc[enc]) !== Math.sign(amount)) acc[enc] = 0;
        acc[enc] += amount;
        const out = Math.trunc(acc[enc] / per);
        acc[enc] -= out * per;
        return out;
    }

    function engineTicks(k, range, d, t) {
        let e = engine.get(k);
        if (!e) { e = { st: knobInit(0.5), carry: 0 }; engine.set(k, e); }
        if (Math.sign(e.carry) !== Math.sign(d)) e.carry = 0;
        const dir = d > 0 ? 1 : -1;
        for (let i = 0; i < Math.abs(d); i++) {
            /* Re-centred each detent: the engine only measures the move; the
             * value lives in the Mixer, which does its own clamping. */
            e.st.value = 0.5;
            const moved = knobStep(e.st, ENGINE_META, dir, t) - 0.5;
            e.carry += moved * range.span / range.tick;
        }
        const out = Math.trunc(e.carry);
        e.carry -= out;
        return out;
    }

    return {
        /** Call on every pulse before the conversions below. */
        begin(enc, t) {
            if (t - turnedAt[enc] >= idleMs) { pulseAcc[enc] = 0; stepAcc[enc] = 0; }
            turnedAt[enc] = t;
        },
        /** Device pulses -> Move detents. */
        detents(enc, pulses) {
            const p = curve(pulses);
            const per = Number(pulsesPerDetentOf());
            if (!(per > 0) || per === 1) return p === pulses ? pulses : accumulate(pulseAcc, enc, p, 1);
            return accumulate(pulseAcc, enc, p, per);
        },
        /** Device pulses -> choices, one per `per` pulses of rotation. */
        steps(enc, pulses, per) { return accumulate(stepAcc, enc, curve(pulses), per); },
        /**
         * Move detents on a Mixer control -> the Mixer's own turn. Returns
         * mixer.turn()'s answer, or false if nothing moved.
         */
        mixerTurn(mixer, enc, d, alt, t) {
            const range = mixerRange(enc, alt);
            if (!range || !d) return false;
            const ticks = engineTicks(enc + (alt ? ":alt" : ""), range, d, t);
            return ticks ? mixer.turn(enc, ticks, alt) : false;
        },
    };
}
