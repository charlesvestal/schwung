/*
 * OXI E16 control surface -- LIFECYCLE ONLY. Finding the device, holding it,
 * and giving it back. Views live in later modules; nothing here draws.
 *
 * WHY A HEARTBEAT AND NOT A QUERY. There is no way to ask whether an E16 is
 * attached: devices on Move's USB-A never enumerate in Linux (docs/SYSEX.md,
 * issue #358), so no amount of host code can answer "is one plugged in".  The
 * surface therefore SEEKS -- it sends ENTER REMOTE MODE until an ACK comes
 * back.  Nine bytes every two seconds is cheap enough that this is the whole
 * detection mechanism rather than a fallback for one.
 *
 * WHY IT KEEPS PROBING AFTER THE ACK.  The E16 has no battery, so unplugging
 * it power-cycles it out of remote mode -- and it says nothing on the way out.
 * The only evidence that the device is still ours is a fresh ACK, so a slow
 * keepalive ENTER continues while present and silence past `lossMs` drops the
 * surface back to seeking.  That is what makes a replug self-heal with no user
 * action: the very next probe puts the device back into remote mode.
 *
 * ONE ASSUMPTION IS UNVERIFIED ON HARDWARE: that an E16 already in remote mode
 * ACKS a repeated ENTER.  If it does not, `present` expires every `lossMs` and
 * the surface falls back to the fast seek cadence -- so the failure is a probe
 * every 2 s instead of every 10, not a broken surface, because the input path
 * does not consult `present` and re-entering remote mode is idempotent.  Check
 * it when a device is on the bench; anything that gates DRAWING on `present`
 * needs the answer first.
 *
 * Everything is PURE and injected: `now` is a caller-supplied millisecond
 * clock and `send` a caller-supplied sender, so tests/host can drive the whole
 * machine with no device, no timers and no globals.  The host half is three
 * call sites in shadow_ui.js.
 */
import { enterMsg, exitMsg, isAck, packetize } from "./e16_protocol.mjs";

/* Seeking cadence.  Slow enough to be free, fast enough that plugging a device
 * in feels immediate. */
export const PROBE_MS = 2000;

/* Keepalive cadence once the device has acked, and how long we tolerate
 * silence before deciding it is gone.  The grace is deliberately longer than
 * one keepalive period: a single dropped ACK must not blank a working surface,
 * and the send path is lossy on purpose (a full buffer returns false). */
/*
 * THE KEEPALIVE IS ALSO THE ABSENCE DETECTOR, and it has to be fast enough to
 * SEE a replug.
 *
 * These were 10 s and 25 s. Nothing draws on `present`, so a slow expiry looked
 * free -- and it was not, because the presence EDGE is what repaints a device
 * that has come back. Unplug the cable and plug it in again inside 25 s and
 * `now - lastAck` never crosses the threshold: `present` stays true the whole
 * time, no edge occurs, and the surface believes it has already painted a
 * device whose screen and rings were wiped by losing power. The next keepalive
 * ENTER puts it back INTO remote mode, which is why the symptom is so
 * misleading -- measured on hardware 2026-09-10 as "it did go back into remote
 * mode" and "replug didnt recover".
 *
 * A gap test cannot help: a replug that lands between two keepalives produces a
 * perfectly ordinary 10 s gap. The only fix is to probe fast enough that an
 * absence must miss one. ENTER is NINE BYTES, so 2 s costs nothing on a wire
 * whose expensive message is 1171.
 *
 * LOSS_MS is three keepalives, not one: a single dropped ACK -- the send path
 * returns false when the buffer is full, on purpose -- must not expire a
 * working surface. And a spurious expiry is the SAFE direction, costing one
 * extra framebuffer and never a blank screen.
 */
export const KEEPALIVE_MS = 2000;
export const LOSS_MS = 6000;

/*
 * How long a hand has to be still before the printed numbers are redrawn.
 *
 * 180 ms is about one slow detent apart: fast enough that letting go of a knob
 * feels like the screen answers, slow enough that a continuous spin -- which
 * makes detents far closer than this -- pays ONE repaint at the end instead of
 * one per detent. The rings carry the value throughout, so nothing is missing
 * while this is pending; only the digits lag, and only while you are still
 * moving them.
 */
export const SETTLE_MS = 180;

/**
 * The seek / hold / release machine.
 *
 * @param {object}   [opts]
 * @param {number}   [opts.probeMs]      seeking cadence
 * @param {number}   [opts.keepaliveMs]  cadence once present
 * @param {number}   [opts.lossMs]       silence after which the device is gone
 */
export function createLifecycle(opts) {
    const o = opts || {};
    const probeMs = o.probeMs === undefined ? PROBE_MS : o.probeMs;
    const keepaliveMs = o.keepaliveMs === undefined ? KEEPALIVE_MS : o.keepaliveMs;
    const lossMs = o.lossMs === undefined ? LOSS_MS : o.lossMs;

    let enabled = false;
    let present = false;
    let lastProbe = -Infinity;   /* when a probe was actually SENT */
    let lastAck = -Infinity;
    /*
     * EXIT is owed, not sent-and-forgotten.  `send` returns false when the
     * outbound buffer is full, which means retry (docs/SYSEX.md, "OUT"); a
     * dropped EXIT leaves the device blank with the surface switched off, and
     * nothing would ever send another one.  So the intent is latched here and
     * drained by tick() -- which is why tick() does work even while disabled.
     */
    let exitOwed = false;

    /* A send that reports false has NOT gone out.  Treating it as sent is the
     * one mistake this wrapper exists to prevent: the probe clock would
     * advance past a message the device never saw. */
    const emit = (send, bytes) => {
        try { return send(packetize(bytes)) !== false; }
        catch (e) { return false; }
    };

    return {
        /**
         * Turn the surface on or off.  Idempotent -- a repeated `true` must not
         * re-send EXIT, and a repeated `false` must not send a second one.
         */
        setEnabled(on, now, send) {
            on = !!on;
            if (on === enabled) return;
            enabled = on;
            if (on) {
                /*
                 * Probe on the very next tick rather than after a full period:
                 * switching the setting on is the user asking for the device
                 * now.  `lastAck` is armed to `now` so the loss timer measures
                 * silence since we started looking, not since the epoch.
                 */
                lastProbe = -Infinity;
                lastAck = now;
                exitOwed = false;
                return;
            }
            present = false;
            exitOwed = true;
            if (emit(send, exitMsg())) exitOwed = false;
        },

        /**
         * One frame of the machine.  Sends at most one message.
         */
        tick(now, send) {
            if (exitOwed) {
                /* Owed from a disable whose send was refused.  Nothing else can
                 * happen until the device has been given back. */
                if (emit(send, exitMsg())) exitOwed = false;
                return;
            }
            if (!enabled) return;

            /* Presence expires on SILENCE, never on a probe going out: the
             * device answers a probe with an ACK, so the ACK clock is the only
             * evidence there is. */
            if (present && now - lastAck >= lossMs) {
                present = false;
                /* Seek again immediately -- a replug should not wait out the
                 * remainder of a keepalive period. */
                lastProbe = -Infinity;
            }

            const period = present ? keepaliveMs : probeMs;
            if (now - lastProbe < period) return;
            if (emit(send, enterMsg())) lastProbe = now;
        },

        /**
         * A reassembled inbound SysEx body (everything between F0 and F7).
         *
         * Anything that is not our ACK is ignored rather than rejected: the
         * external port is shared, and another device's manufacturer ID is not
         * an error.
         */
        onSysex(asm, now) {
            if (!isAck(asm)) return false;
            present = true;
            lastAck = (now === undefined || now === null) ? lastAck : now;
            return true;
        },

        get enabled() { return enabled; },
        get present() { return present; },
    };
}

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

/*
 * ---------------------------------------------------------------------------
 * THE PACED DISPLAY
 *
 * The rate discipline IS the display strategy here, not an optimisation on top
 * of one. Measured outbound on 2026-09-09: 31 packets sent alone arrived
 * byte-perfect; 34 packets sent amid other traffic lost 8 whole packets. A
 * framebuffer is 1024 raw bytes -> 1171 packed -> ~391 USB-MIDI packets, so it
 * is only ever sendable when nothing else is going out.
 *
 * Hence the asymmetry:
 *
 *   navigation   -> ONE framebuffer, and only on a discrete human action
 *   value change -> ONE LED RING chunk, ~15 bytes on the wire
 *
 * A value change must NEVER redraw the screen. It is the common case -- a knob
 * under a hand produces one of these per detent -- and turning it into a
 * repaint is how a surface that measured fine in isolation drops packets in
 * use. The rings carry values; the framebuffer carries layout.
 *
 * Three rules, all enforced here rather than at the call sites:
 *
 *   1. AT MOST ONE MESSAGE PER TICK. A framebuffer plus rings in the same tick
 *      is precisely the "amid other traffic" case that lost packets.
 *   2. NEVER MORE THAN ONE FRAMEBUFFER IN FLIGHT. `fbOwed` is a BOOLEAN, not a
 *      counter: three navigations inside one tick are one repaint, and a
 *      refused send stays owed rather than queueing a second copy.
 *   3. RINGS COALESCE PER ENCODER. A fast spin makes many events for one
 *      encoder and only the last position is true, so they collapse into one
 *      chunk keyed by encoder -- and every pending encoder rides in a single
 *      message, because sixteen chunks is still 113 bytes.
 *
 * Pure and injected like the lifecycle: `send` and the frame producer are the
 * caller's, so tests drive the whole thing with no device.
 * ---------------------------------------------------------------------------
 */
import { framebufferMsg, labelsMsg, ringMsg } from "./e16_protocol.mjs";

export function createDisplay() {
    let fbOwed = false;
    /*
     * THE MODE THE DEVICE IS IN, and why "nothing changed" is not "nothing to
     * send".
     *
     * FRAMEBUFFER and LABELS are mutually exclusive -- sending one OVERRIDES
     * the other (docs/E16_REMOTE.md). So leaving the map puts the device on a
     * screen the surface never chose: the framebuffer raised for the map is
     * still there, and the parameter view owes a LABELS message even though
     * not one label changed while Shift was held. Tracking what the DEVICE was
     * last told, rather than what the surface last decided, is what makes that
     * transition visible -- the same shape as the presence edge, one layer in.
     */
    let shownKind = null;
    /* The title/name text, owed separately from the screen KIND: a detent
     * changes the reading without changing the mode, and it must not cost a
     * repaint. */
    let labelsOwed = false;
    /* enc -> the latest ring descriptor for it. A Map because insertion order
     * is stable, so chunks go out in the order the encoders moved. */
    const rings = new Map();

    const emitMsg = (send, bytes) => {
        try { return send(packetize(bytes)) !== false; }
        catch (e) { return false; }
    };

    return {
        /**
         * The layout changed -- a page pair, a component, a focus jump. Marks
         * one repaint owed; calling it ten times still owes one.
         */
        invalidate() { fbOwed = true; },

        /**
         * The label text moved -- a new reading under the hand, a renamed
         * page. Costs 34 packets against the framebuffer's 394, and is
         * deliberately LOWER priority than a ring: the ring is the feedback a
         * turning hand is actually watching.
         */
        invalidateLabels() { labelsOwed = true; },

        /**
         * One encoder's value moved. Takes a descriptor from
         * `e16_view.ringFor()`; a null (empty cell) is ignored rather than
         * queueing a chunk for an encoder that drives nothing.
         */
        ringChanged(desc) {
            if (!desc || typeof desc.enc !== "number") return;
            rings.set(desc.enc, desc);
        },

        /**
         * Send at most one message.
         *
         * @param {function} send        packets -> boolean (false = refused)
         * @param {function} frameBytes  () -> 1024-byte framebuffer. Called
         *        ONLY when a repaint is actually going out, so a caller can
         *        render lazily and a tick that owes nothing costs no drawing.
         * @returns {"framebuffer"|"rings"|null} what went out, for tests and
         *        for a caller that wants to log its send budget.
         */
        tick(send, frameBytes, screen) {
            /*
             * A screen is owed when the surface says so OR when the device is
             * in the wrong MODE for what is being shown. The second half is
             * not an optimisation: without it, dismissing the map leaves the
             * framebuffer on the panel with every later value change going out
             * as rings nobody can read a name for.
             */
            const want = screen ? screen.kind : "framebuffer";
            if (fbOwed || shownKind !== want) {
                /* Rings deliberately wait: see rule 1. The screen is the
                 * expensive send and it goes out alone. */
                const bytes = want === "labels"
                    ? labelsMsg(screen.title, screen.labels)
                    : framebufferMsg(frameBytes());
                if (emitMsg(send, bytes)) {
                    fbOwed = false;
                    labelsOwed = false;
                    shownKind = want;
                    return want;
                }
                return null;
            }
            if (!rings.size) {
                if (!labelsOwed || want !== "labels") return null;
                if (!emitMsg(send, labelsMsg(screen.title, screen.labels))) return null;
                labelsOwed = false;
                return "labels";
            }
            const chunks = Array.from(rings.values());
            if (!emitMsg(send, ringMsg(chunks))) return null;
            /* Cleared only on an ACCEPTED send. A refused ring message leaves
             * the positions owed -- the same reason the lifecycle latches an
             * owed EXIT rather than trusting the send. */
            rings.clear();
            return "rings";
        },

        /* Test seams. */
        get framebufferOwed() { return fbOwed; },
        get labelsTextOwed() { return labelsOwed; },
        get shownKind() { return shownKind; },
        /* A replug wipes the panel, so what the device was told is no longer
         * true. Forgetting it is what makes the presence edge resend. */
        forgetShown() { shownKind = null; },
        get ringsPending() { return rings.size; },
    };
}

/*
 * ---------------------------------------------------------------------------
 * NAVIGATION -- the Shift-held map, and what the sixteen buttons mean.
 *
 * =================== THE MODIFIER CANNOT BE A LATCH =========================
 *
 * There is exactly one piece of modifier state here and it is a TIMESTAMP, not
 * a boolean. `mapVisible(now)` is COMPUTED from it on every read; nothing
 * anywhere stores "the map is up".
 *
 * That shape is chosen against a specific, expensive failure. CLAUDE.md's
 * account of `pad_block` is the precedent: a flag that could only be lowered by
 * an event stuck on a field device for THIRTEEN HOURS across two shim inits,
 * and enumerating the ways it could end failed on hardware TWICE -- once for a
 * jump that keeps the module loaded, once for co-run. The conclusion recorded
 * there is that such a flag is released by an INVARIANT restated every frame,
 * never by an exit list.
 *
 * Shift is the same problem with a worse channel. The note-off is one MIDI
 * message on a USB-A port that is known to drop whole packets under traffic
 * (docs/E16_REMOTE.md; 34 packets amid other traffic lost 8), the E16 has no
 * battery so unplugging it silently power-cycles it mid-hold, and the shim's
 * forwarding gate can be closed while a finger is down. Every one of those is a
 * note-on with no note-off, and NONE of them generates an event we could add to
 * an exit list -- which is precisely why the escape cannot be one.
 *
 * So the hold EXPIRES. `MAP_MAX_HOLD_MS` after the press, `mapVisible` answers
 * false because time moved, with no message, no tick, and no cooperation from
 * anything. There is no state that could be left behind, because the only
 * state is a number being compared against a clock that only goes forwards.
 * The map is momentary by design -- it exists while the modifier is held and
 * there is no mode to be lost in -- and this keeps that true even when the
 * device stops talking to us mid-gesture.
 *
 * Two consequences worth stating so they are not later "fixed":
 *
 *   - A DOWN WHILE THE MAP IS ALREADY UP DOES NOT RE-ARM IT. Refreshing the
 *     deadline from input would make a stranded hold immortal for exactly the
 *     user who is trying to work through it: a hand turning knobs would keep
 *     feeding the very state that is eating its turns.
 *   - A LOWER PUSH CONSUMES THE HOLD. The jump ends the gesture, so the surface
 *     you land on is the one you can see. It is also the common-case escape:
 *     most strandings end on the next thing the user does anyway.
 *
 * `tick()` is the restatement. It compares the DERIVED visibility against what
 * was last actually drawn and invalidates on a difference -- so the expiry
 * repaints itself with no event, the same way `reconcilePadBlock()` restates
 * its flag every frame rather than trusting the exits.
 *
 * =================== WHY THE CURRENT SLOT'S OWN CELL IS THE BUS DOOR ========
 *
 * The design asks for "a dedicated cell" that swaps the lower twelve to buses,
 * and `buildMap` leaves none: all sixteen are slots or components. Stealing one
 * of the twelve would shrink the component capacity of every slot to pay for a
 * view most slots have nothing to show in. So the door is the top-row cell of
 * the slot you are ALREADY on -- a press there cannot mean "switch to this
 * slot", which makes it free, and it reads as the slot's own handle: press it
 * to change what is listed beneath it, turn it to page that list.
 *
 * =================== FOLLOW FOCUS: ONE VARIABLE, TWO SOURCES ===============
 *
 * With follow on, Move's screen writes the focus and the E16 mirrors it. The
 * mode is deliberately narrow, and the narrowness is the design:
 *
 *   - ONE FOCUS VARIABLE. Follow does not add a focus; it decides who writes
 *     the one there is. Two owners is a surface that disagrees with itself.
 *   - THE MAP IS DISABLED. That is what stops the two surfaces fighting, and
 *     it is what lets the setting mean one sentence -- is the E16 showing what
 *     Move shows, or its own thing? A mode where both can navigate has no
 *     answer to "who wins", so there is no such mode.
 *   - ONE-WAY. Nothing on this side ever reports focus back: navigating the
 *     E16 must never move Move's screen. In code that is simply "applyFollow
 *     does not call onFocus", and it is pinned in tests/host because the
 *     tempting future edit -- telling the caller what we are showing, for
 *     symmetry -- would read as helpful.
 *   - OFF RESTORES, IT DOES NOT RESET. Follow is something you switch on to
 *     look at something; `parked` is what you come back to.
 *
 * Everything is pure and injected, like the rest of this file: the chain, the
 * page count, the parameter renderer, the follow source and the focus callback
 * are all the caller's. This module never reads or writes a parameter -- a
 * jump is REPORTED through `onFocus`, and `e16_view.mjs` remains the only path
 * a value can travel.
 * ---------------------------------------------------------------------------
 */
import { buildMap } from "./e16_map.mjs";
import { renderMap, pageStep, drawTestPattern } from "./e16_view.mjs";

/* How long a Shift hold can live without a note-off.
 *
 * Ten seconds is a judgement, and the direction of the error is the point. The
 * map is a jump-target picker: every hold that produces a jump ends when the
 * jump does, so a hold that has produced nothing for ten seconds is far more
 * likely to be a lost note-off than a decision. Erring long strands the
 * surface; erring short costs one more press. */
export const MAP_MAX_HOLD_MS = 10000;

/* The top row is the four slots; everything below is the selected slot's
 * content. Both halves of that split are already `e16_map.mjs`'s, and this is
 * the input side of the same fact. */
const SLOT_CELLS = 4;

export function createNav(opts) {
    const o = opts || {};
    const display = o.display || null;
    const chainOf = o.chainOf || (() => ({ slots: [] }));
    const onFocus = o.onFocus || (() => {});
    const renderParams = o.renderParams || (() => {});
    const pageCountOf = o.pageCountOf || (() => 1);
    const maxHoldMs = o.maxHoldMs === undefined ? MAP_MAX_HOLD_MS : o.maxHoldMs;
    /*
     * FOLLOW FOCUS -- the second source for the ONE focus variable below.
     *
     * `followFocusOf()` answers what Move's own screen is showing, as
     * `{ slot, component }`, or NULL when it could not answer. There is no
     * second focus here and there must never be one: `slot` / `component` /
     * `pageIndex` are the surface's focus whichever source is writing them, and
     * a mode with its own copy is a surface that disagrees with itself -- the
     * rings drawn for one component, the screen for another, with nothing
     * logged because both halves are behaving correctly.
     */
    const followFocusOf = o.followFocusOf || (() => null);

    /* THE ONLY MODIFIER STATE. Null means no hold; a number is when it began.
     * Never a boolean -- see the header. */
    let shiftDownAt = null;

    let slot = o.slot | 0;
    let component = o.component || "synth";
    let pageIndex = 0;
    let mapPage = 0;
    let showBuses = false;

    /* Is Move's screen the source right now? */
    let follow = !!o.follow;
    /*
     * The E16's OWN focus, set aside while follow owns the variables above.
     *
     * NOT a second owner: nothing reads this while follow is on, and nothing
     * writes it except the OFF->ON edge. It exists because the user turns
     * follow on TEMPORARILY -- to see what Move is doing -- and expects to come
     * back to where they were. Resetting to slot 0 / synth instead is the
     * plausible-looking wrong answer, and it is invisible on any rig whose own
     * focus happened to be slot 0 / synth.
     */
    let parked = null;

    /* What the last framebuffer that actually went out was showing. Compared
     * against the derived visibility in tick(); it is a record of the PAST, so
     * it cannot be the thing that decides the present. */
    let shownMap = false;

    /*
     * THE MAP IS DISABLED WHILE FOLLOW IS ON, and it is disabled HERE rather
     * than at each gesture.
     *
     * Every branch in handle() already asks this one question, so one term
     * closes all of them at once -- a push under a held Shift falls through to
     * the ordinary `click` path, a Shift+turn to the ordinary `turn`. Gating
     * the gestures individually instead would leave whichever branch was
     * written next as a way to navigate while following, and that is the mode
     * with no answer to "who wins": Move drives the E16, and the E16 must not
     * be able to drive back.
     */
    const mapVisible = (now) =>
        !follow && shiftDownAt !== null && (now - shiftDownAt) < maxHoldMs;

    const invalidate = () => { if (display) display.invalidate(); };

    /*
     * Take one reading from the follow source.
     *
     * A NULL IS NOT A PLAN. CLAUDE.md's tri-state rule: a read that did not
     * complete must never become a default. The source is consulted every
     * frame, so collapsing null into "slot 0, synth" would drag the surface to
     * slot 0 on any tick the shadow UI could not answer and drag it back on the
     * next -- which reads as a flickering surface rather than as a failed read.
     *
     * It reports NOTHING through onFocus. That silence is the one-way rule:
     * onFocus is the only channel this module has to move anything outside
     * itself, and firing it here would be Move's screen jumping because the
     * E16 mirrored Move's screen.
     */
    const applyFollow = () => {
        const f = followFocusOf();
        if (!f || typeof f.slot !== "number" || !f.component) return;
        if (f.slot === slot && f.component === component) return;
        slot = f.slot | 0;
        component = f.component;
        /* A different component is a different page plan, so the page index
         * from the last one means nothing on this one. */
        pageIndex = 0;
        invalidate();
    };

    const currentMap = () =>
        buildMap(chainOf(), { slot, page: mapPage, showBuses });

    return {
        /**
         * One surface event -- whatever `e16_input.decode()` produced.
         *
         * @returns a description of what happened, or null for an event that
         *          meant nothing here. A push or turn arriving while the map is
         *          DOWN is handed straight back as `click` / `turn` for the
         *          caller to route through the grid: this module decides what a
         *          gesture MEANS, never what a parameter becomes.
         */
        handle(ev, now) {
            if (!ev) return null;

            if (ev.type === "shift") {
                /* The modifier itself is inert while following. `mapVisible`
                 * already answers false, but without this the DOWN edge would
                 * still arm `shiftDownAt` and repaint -- and the hold would
                 * then be live the instant follow was switched off, with the
                 * map appearing under a finger that is no longer on Shift. */
                if (follow) return null;
                if (ev.down) {
                    /* Deliberately not a re-arm: a repeat down while up leaves
                     * the original deadline standing (see the header). */
                    if (mapVisible(now)) return null;
                    shiftDownAt = now;
                    mapPage = 0;
                    showBuses = false;
                    invalidate();
                    return { action: "map" };
                }
                if (!mapVisible(now)) {
                    /* The hold already ended -- by a jump, or by expiring. A
                     * repaint here would be a second framebuffer for a screen
                     * that is already correct. */
                    shiftDownAt = null;
                    return null;
                }
                shiftDownAt = null;
                invalidate();
                return { action: "params" };
            }

            /* A button release is inert. Pushes act on the DOWN edge, so acting
             * on the up edge too would run every gesture twice -- and a jump
             * run twice is a second onFocus for a component the user selected
             * once. */
            if (ev.type === "release") return null;

            if (ev.type === "push") {
                if (!mapVisible(now)) return { action: "click", enc: ev.enc };
                const enc = ev.enc | 0;
                if (enc < SLOT_CELLS) {
                    if (enc === slot) {
                        showBuses = !showBuses;
                        mapPage = 0;
                        invalidate();
                        return { action: "buses", showBuses };
                    }
                    slot = enc;
                    mapPage = 0;
                    /* Leaving bus view on a slot change is not tidiness: the
                     * new slot's components would otherwise be hidden behind a
                     * mode the user set while looking at a different slot. */
                    showBuses = false;
                    invalidate();
                    return { action: "slot", slot };
                }
                const cell = currentMap().cells[enc];
                /* A hole is not a destination (e16_map.mjs). Nothing moves and
                 * nothing repaints -- in particular the hold is NOT consumed,
                 * or a mis-hit would drop the map the user is still reading. */
                if (!cell) return null;
                slot = cell.slot;
                component = cell.component;
                pageIndex = 0;
                shiftDownAt = null;   /* the jump ends the gesture */
                onFocus(slot, component);
                invalidate();
                return { action: "focus", slot, component };
            }

            if (ev.type === "turn") {
                if (!mapVisible(now)) {
                    return { action: "turn", enc: ev.enc, ticks: ev.ticks };
                }
                if ((ev.enc | 0) < SLOT_CELLS) {
                    /* The slot row owns the map's own list, so turning it pages
                     * that list. Splitting the two paging axes by WHICH encoder
                     * moved keeps both available at once; deciding by whether
                     * the map happens to overflow would make one gesture mean
                     * two things depending on the rig. */
                    const count = currentMap().pageCount;
                    const next = Math.max(0, Math.min(count - 1,
                        mapPage + (ev.ticks > 0 ? 1 : -1)));
                    if (next === mapPage) return { action: "mapPage", mapPage };
                    mapPage = next;
                    invalidate();
                    return { action: "mapPage", mapPage };
                }
                const next = pageStep(pageIndex, ev.ticks, pageCountOf());
                if (next === pageIndex) return { action: "page", pageIndex };
                pageIndex = next;
                invalidate();
                return { action: "page", pageIndex };
            }

            return null;
        },

        /**
         * Restate the invariant. Call every frame, before the display's tick.
         *
         * This is the stranded-modifier escape made visible: the hold can end
         * with no event, so something has to notice that the screen no longer
         * matches the derived state. It compares against what was last DRAWN
         * rather than tracking transitions, so it cannot miss one -- a
         * transition counter has to be right at every site that changes the
         * state, and this has to be right once.
         *
         * (It reconciles what THIS process has drawn. A device still showing a
         * frame from a previous process is the lifecycle's problem, not this
         * one's -- see createLifecycle.)
         */
        /**
         * Switch the focus SOURCE. Idempotent, and the idempotence is not
         * tidiness: the park happens on the OFF->ON edge only, so a
         * setFollow(true) that parked unconditionally would, on its second
         * call, park the FOLLOW source as if it were the user's own focus --
         * and the user's real focus would be gone with no gesture that could
         * bring it back.
         */
        setFollow(on, now) {
            on = !!on;
            if (on === follow) return;
            follow = on;
            if (on) {
                parked = { slot, component, pageIndex, mapPage, showBuses };
                /* A hold in progress cannot survive: the map is gone, so its
                 * modifier would be a note-off owed to a view that no longer
                 * exists. The timestamp shape means dropping it is the whole
                 * job -- there is no latch anywhere else to unwind. */
                shiftDownAt = null;
                applyFollow();
                invalidate();
                return;
            }
            if (parked) {
                slot = parked.slot;
                component = parked.component;
                pageIndex = parked.pageIndex;
                mapPage = parked.mapPage;
                showBuses = parked.showBuses;
                parked = null;
            }
            invalidate();
        },

        tick(now) {
            /* Follow is a POLL, not a subscription: shadow_ui.js does not tell
             * anyone when its component changes, and adding a notification for
             * this one consumer would be a second thing to keep in step with
             * every path that moves that focus (see reconcileCcClaim, which
             * re-derives the same tuple for the same reason). Reading it is
             * cheap -- the shadow UI already holds it -- and applyFollow
             * repaints only on a CHANGE, so an unchanged source costs nothing
             * on the wire. */
            if (follow) applyFollow();
            if (mapVisible(now) !== shownMap) invalidate();
        },

        /**
         * Draw whichever view is current. Handed to the display's tick as the
         * frame producer, so it runs ONLY when a repaint is actually going out.
         */
        render(ctx, now) {
            const up = mapVisible(now);
            if (up) renderMap(ctx, currentMap(), { page: mapPage });
            else renderParams(ctx);
            shownMap = up;
        },

        mapVisible,

        get slot() { return slot; },
        get component() { return component; },
        get pageIndex() { return pageIndex; },
        get mapPage() { return mapPage; },
        get showBuses() { return showBuses; },
        get followEnabled() { return follow; },
        /* The DISPLAY MODE depends on this: a map is a picture and the
         * parameter view is sixteen labels, and the two modes override each
         * other on the device. Exposed rather than re-derived at the call site
         * -- the hold has a timeout, so "is the map up" is a question only this
         * object can answer correctly. */
        mapVisible(now) { return mapVisible(now); },
    };
}

/*
 * ---------------------------------------------------------------------------
 * THE SURFACE ITSELF -- the assembly.
 *
 * Everything above this line is a component, and Tasks 1-11 shipped every one
 * of them with green tests while NOTHING CONSTRUCTED ANY OF THEM: `e16Nav` was
 * null in shadow_ui.js, `createNav` and `createDisplay` were never called, and
 * a device attached to a Move running that code entered remote mode and then
 * drew nothing. Each half was correct; the seam between two files belonged to
 * neither task, so nobody owned it and nothing failed when it was missing.
 *
 * WHY THE ASSEMBLY IS HERE AND NOT IN shadow_ui.js. That file cannot be
 * imported under node -- it opens on `import * as os from "os"` and every path
 * in it is an on-device absolute -- so anything living there can only ever be
 * GREPPED, and a grep is precisely what could not see the gap this is being
 * written to close. Composed here, the whole path (a setting, a clock, raw MIDI
 * bytes in, USB-MIDI packets out) runs in tests/host with no device and no
 * shim. What is left in shadow_ui.js is five injected seams and two calls.
 *
 * THE SURFACE HOLDS ITS OWN CONTROLLER, and that is not tidiness. A page
 * controller has ONE current page, and a bottom-half turn has to
 * `goToPage(N+1)` before applying it (see applyTurn) -- so sharing Move's
 * controller would drag Move's screen to the next page on every lower-row knob.
 * `makeController` is therefore a FACTORY the host supplies, not a controller:
 * a host that handed over the one it already has could not be told apart from
 * one that built a second, and the symptom is two small in-range page indices
 * disagreeing, with nothing logged.
 *
 * NOTHING RUNS WHILE THE SETTING IS OFF. tick() drains an owed EXIT and then
 * returns before the nav, the controller reads and the display -- so an off
 * surface is a comparison, and in particular it puts no bytes on a port
 * somebody else's gear is listening to.
 * ---------------------------------------------------------------------------
 */
import { decode } from "./e16_input.mjs";
import { createCanvas } from "./e16_canvas.mjs";
import { buildView, renderView, ringFor, ringsFor, labelsFor, applyTurn, applyClick }
    from "./e16_view.mjs";

/**
 * @param {object} io
 * @param {function} [io.now]            () -> ms
 * @param {function} [io.send]           packets -> boolean (false = REFUSED)
 * @param {function} [io.chainOf]        () -> the chain shape buildMap wants
 * @param {function} [io.followFocusOf]  () -> {slot, component} | null
 * @param {function} io.makeController   (focus) -> a NEW page controller whose
 *        getParam/setParam read `focus.slot` LIVE. It is handed a live view of
 *        the surface's focus rather than a slot number because one controller
 *        outlives many jumps, and a controller closed over the slot it was born
 *        with would keep addressing that slot after the first one.
 * @param {object} [io.canvas]           test seam; an e16 canvas
 */
export function createSurface(io) {
    const o = io || {};
    const now = o.now || (() => Date.now());
    const send = o.send || (() => false);
    const chainOf = o.chainOf || (() => ({ slots: [] }));
    const followFocusOf = o.followFocusOf || (() => null);
    const makeController = o.makeController || null;
    const canvas = o.canvas || createCanvas();
    /* -1 means "draw the real view". Injected because the surface is pure and
     * the arming lives in a file the host owns; see drawTestPattern. */
    const testPattern = o.testPatternOf || (() => -1);

    const lifecycle = createLifecycle(o.lifecycle);
    const display = createDisplay();

    let ctl = null;
    /* "<slot>:<component>" of the load the controller is currently holding.
     * Null means it has never been loaded. */
    let loaded = null;
    /* The presence the last tick saw, so the false->true edge can repaint. */
    let wasPresent = false;

    /* The ONE focus, as a live view. Passed to the host's controller factory so
     * its param accessors follow a jump; read nowhere else. */
    const focus = {
        get slot() { return nav ? nav.slot : 0; },
        get component() { return nav ? nav.component : "synth"; },
    };

    const metaOf = (key) =>
        (ctl && ctl.metaIndex ? ctl.metaIndex.getOrGuess(key) : null);
    /* From the controller's own value cache, never a fresh read: this is called
     * per cell per view build, and an IPC round trip is ~2.8 ms against a 1.68
     * ms whole-page render (CLAUDE.md). The cache is what ctl.tick() refreshes
     * on its staggered cursor, exactly as the knob grid does. */
    const valueOf = (key) =>
        (ctl && ctl.state && ctl.state.values ? ctl.state.values[key] : undefined);

    /* Rebuilt on demand rather than cached. buildView is pure and reads only
     * the two lookups above, so it costs no IPC -- and a cached view is a
     * fourth thing that can disagree with the controller about which page is
     * current. */
    const viewNow = () =>
        buildView(ctl ? ctl.pages : [], nav ? nav.pageIndex : 0, { metaOf, valueOf });

    /*
     * THE ENCODER UNDER THE HAND, which is what the 16-character title names.
     *
     * A number, not a boolean: the title has to say WHICH parameter is moving,
     * and the labels beneath it are four characters each, so the title is the
     * only place a full name and a full reading ever appear. It is cleared by
     * nothing -- the last thing touched stays named, which is what you want
     * when you look up a second later to read the value you just set.
     */
    let focusEnc = null;
    /* When the last detent arrived, and whether the repaint it owes has gone
     * out. Two variables rather than one timestamp cleared on paint, because
     * "nothing has been turned yet" and "the turn has been drawn" are
     * different states and collapsing them repaints once at startup for no
     * reason. */
    let turnedAt = -Infinity;
    let settlePainted = true;

    /*
     * REFRESH METER state (test pattern 6).
     *
     * Counted HERE because the display is the only thing that knows when a
     * send actually completed -- a refused send is not a paint, and counting
     * invalidations instead would report a rate the device never achieved,
     * which is the one answer this instrument must not give.
     *
     * The rate is a moving average rather than an instant reading: a single
     * interval is dominated by whichever SPI frame the send happened to land
     * on, so the raw number jitters by a factor of two while the underlying
     * rate is steady, and a jittering number is one nobody can read off a
     * moving screen.
     */
    let paints = 0;
    let lastPaintAt = null;
    let paintFps = null;
    /*
     * The probe value the screen was last drawn FROM.
     *
     * Arming or disarming the probe changes what should be on the panel and
     * nothing else does -- no focus moved, no knob turned, no mode changed --
     * so without this the display sits believing the device is already
     * correct and the last probe frame stays up forever. Observed on hardware
     * as "it's still on the test screen, but frozen" after the probe file was
     * removed; the panel was not frozen at all, it was showing the last thing
     * anybody had sent it.
     *
     * Initialised to a value no probe can take, so the FIRST tick after the
     * surface comes up owes a frame rather than matching by accident.
     */
    let shownProbe = -2;

    /* The LABELS screen for this frame: sixteen four-character names plus the
     * title. Cheap enough to rebuild per tick (it is string work over a view
     * that is itself rebuilt per tick and costs no IPC), and rebuilding is what
     * keeps the reading in the title honest without a second staleness stamp to
     * get wrong. */
    /*
     * THE MODULE'S NAME, not the position id.
     *
     * nav.component is "synth" / "fx1" / "midi_fx2" -- an ADDRESS, not a name
     * -- and passing it straight to the title is why the first LABELS build
     * showed the word "SYNTH" on the device instead of "9W9". The one
     * affordance that makes a 4-character grid workable is a title naming what
     * you are actually touching, so getting this wrong disabled the feature
     * while appearing to work.
     */
    const moduleNameFor = (slot, component) => {
        const chain = chainOf() || {};
        const sl = (chain.slots || [])[slot] || {};
        if (component === "synth") return sl.synth || "";
        let m = /^fx(\d+)$/.exec(component);
        if (m) return (sl.fx || [])[Number(m[1]) - 1] || "";
        m = /^midi_fx(\d+)$/.exec(component);
        if (m) return (sl.midiFx || [])[Number(m[1]) - 1] || "";
        m = /^bus(\d+)$/.exec(component);
        if (m) return (sl.buses || [])[Number(m[1]) - 1] || "";
        return component || "";
    };

    const labelScreen = () => {
        const l = labelsFor(viewNow(), {
            component: moduleNameFor(nav ? nav.slot : 0, nav ? nav.component : ""),
            focusEnc,
            metaOf,
        });
        return { kind: "labels", title: l.title, labels: l.labels };
    };

    /*
     * THE MAP AS LABELS TOO, so the framebuffer is never sent at all.
     *
     * A map cell is a slot number or a module name, both of which fit four
     * characters as well as anything else does -- and the title says which
     * slot's components are listed, which the framebuffer's two headers used
     * to carry between them.
     *
     * The current slot is marked with a leading '>' rather than the inverted
     * box the picture drew: four characters is not much to spend one on, and
     * an unmarked map cannot say where you are.
     */
    const mapScreen = () => {
        /* Built here from the nav's public state rather than reaching into
         * its private one: buildMap is pure and cheap, and a second accessor
         * on the nav would be a second thing to keep in step. */
        const m = buildMap(chainOf(), {
            slot: nav ? nav.slot : 0,
            page: nav ? nav.mapPage : 0,
            showBuses: nav ? nav.showBuses : false,
        });
        const labels = new Array(16).fill("");
        for (let i = 0; i < 16; i++) {
            const c = m.cells[i];
            if (!c) continue;
            const name = c.kind === "slot" ? String(c.slot + 1) : String(c.label || "");
            labels[i] = (c.current ? ">" : "") + name;
        }
        return {
            kind: "labels",
            title: "SLOT " + ((nav ? nav.slot : 0) + 1) + " PICK",
            labels,
        };
    };

    const nav = createNav({
        display,
        chainOf,
        followFocusOf,
        pageCountOf: () => (ctl && ctl.pages ? ctl.pages.length : 1),
        renderParams: (ctx) => renderView(ctx, viewNow()),
        /* The jump has already moved nav's focus; the controller catches up in
         * syncFocus() on the next tick. Reloading from here instead would put a
         * contract read (two blocking param round trips) on the MIDI callback
         * that delivered the button press. */
        onFocus: () => {},
    });

    const asm = createSysexAssembler({
        onMessage: (body) => { lifecycle.onSysex(body, now()); },
    });

    function ensureController() {
        if (ctl || !makeController) return ctl;
        ctl = makeController(focus);
        return ctl;
    }

    /*
     * Point the controller at whatever the nav is focused on.
     *
     * Guarded on the PAIR rather than calling load() every tick: load() is safe
     * to repeat, but it reads `<prefix>:ui_hierarchy` and `<prefix>:chain_params`
     * to decide whether anything changed, and two blocking reads per frame is
     * most of a frame.
     */
    function syncFocus() {
        if (!ensureController()) return;
        const sig = nav.slot + ":" + nav.component;
        if (sig === loaded) return;
        loaded = sig;
        ctl.load({ slot: nav.slot, component: nav.component, prefix: nav.component });
        /* A different component is a different screen. */
        display.invalidate();
    }

    /*
     * Repaint when the device becomes ours.
     *
     * A frame sent before the ACK is a frame sent to nothing -- and, worse, the
     * device that acks a moment later is left showing whatever the last process
     * put on it, with the surface believing it has painted. The EDGE is watched
     * rather than the state, so a device that drops out and comes back repaints
     * itself; nothing DRAWS on presence (see createLifecycle's note on the
     * unverified re-ACK assumption), so a spurious expiry costs one extra
     * framebuffer, never a blank screen.
     */
    function syncPresence() {
        if (lifecycle.present === wasPresent) return;
        wasPresent = lifecycle.present;
        if (!wasPresent) return;
        /* A replug wipes the panel, so what the device was last TOLD is no
         * longer what it is showing -- and the mode is part of that. Without
         * this, a device that came back while the parameter view was up matched
         * `shownKind` and was sent nothing at all. */
        display.forgetShown();
        display.invalidate();
        /*
         * THE RINGS COME BACK TOO, and they are a SEPARATE resend.
         *
         * A ring is only ever sent when one CHANGES, which is right in use --
         * one chunk per detent instead of a repaint -- and wrong at exactly
         * this edge: a device that has just come back has sixteen dark rings
         * and no change has occurred, so the panel recovered its screen and
         * kept its values invisible until a knob was turned. Measured on
         * hardware 2026-09-10 by unplugging the cable: remote mode returned,
         * the screen returned, the rings did not.
         *
         * Restating them all here rather than tracking what the device has is
         * the same call the shim's pad_block flag makes: the E16 forgets
         * unilaterally and never says so, so a mirror of its LED state latches
         * and is wrong exactly when it matters. The queue coalesces per
         * encoder and all sixteen ride in one 113-byte message, so a full
         * restate costs one send.
         */
        if (!ensureController()) return;
        for (const desc of ringsFor(viewNow())) display.ringChanged(desc);
    }

    return {
        /** The setting. Idempotent; EXIT is sent by the lifecycle, once. */
        setEnabled(on) { lifecycle.setEnabled(!!on, now(), send); },

        /** Follow Focus. The surface parks its own focus on the OFF->ON edge,
         *  so this must be told the EDGE and not poll a setting. */
        setFollow(on) { nav.setFollow(!!on, now()); },

        /**
         * Cable-2 bytes, 1-3 at a time with the USB-MIDI CIN already stripped.
         *
         * The assembler is fed UNCONDITIONALLY -- a SysEx message arrives as a
         * run of fragments and a gate here would splice a message that began
         * before the setting was switched on onto one that began after. Every
         * other consumer is gated, so a shared port carries no risk of the
         * surface acting on somebody else's gear.
         */
        feedMidi(data) {
            asm.feed(data);
            if (!lifecycle.enabled) return null;
            const ev = decode(data);
            if (!ev) return null;
            const t = now();
            const act = nav.handle(ev, t);
            if (!act || !ctl) return act;

            if (act.action === "turn") {
                const moved = applyTurn(viewNow(), ctl, act.enc, act.ticks, t);
                /* ONE RING, NEVER A REPAINT. This is the common case -- a knob
                 * under a hand makes one of these per detent -- and turning it
                 * into a framebuffer is how a surface that measured fine in
                 * isolation drops packets in use (docs/E16_REMOTE.md). The view
                 * is rebuilt AFTER the write so the ring carries the new value.
                 */
                if (moved) {
                    display.ringChanged(ringFor(viewNow(), act.enc));
                    /*
                     * THE NUMBER FOLLOWS THE HAND, ONE REPAINT PER GESTURE.
                     *
                     * A framebuffer has no partial update, so the printed
                     * value cannot move without redrawing all 1024 bytes --
                     * 383 ms, which is not payable per detent and is what made
                     * the screen feel frozen while the rings moved. Paying it
                     * per GESTURE instead is the whole difference: the ring
                     * tracks the value live at 46 ms while the hand is moving,
                     * and the settle below redraws once the hand stops, so the
                     * digits are correct whenever anybody is actually reading
                     * them.
                     */
                    turnedAt = t;
                    settlePainted = false;
                    /* The TITLE is the only surface carrying the full name and
                     * the reading, so a turn owes one -- but as a separate,
                     * lower-priority debt than the ring. A spin makes many
                     * detents and the display sends one message per tick, so
                     * the ring (the thing being watched) goes first and the
                     * text catches up when the hand pauses. */
                    display.invalidateLabels();
                }
                focusEnc = act.enc;
                return act;
            }
            if (act.action === "click") {
                const before = ctl.pageIndex;
                const hit = applyClick(viewNow(), ctl, act.enc);
                /* A click can flip a value (a ring) or open a door (a new
                 * page). Only the second is worth a screen. */
                if (ctl.pageIndex !== before) display.invalidate();
                else if (hit) {
                    display.ringChanged(ringFor(viewNow(), act.enc));
                    display.invalidateLabels();
                }
                focusEnc = act.enc;
                return act;
            }
            return act;
        },

        /** One frame. Off, this is the lifecycle's two comparisons. */
        tick() {
            const t = now();
            /*
             * AT MOST ONE MESSAGE PER TICK, ACROSS BOTH PRODUCERS.
             *
             * createDisplay enforces that rule inside itself, but it can only
             * see its own sends -- and the lifecycle is a second producer on
             * the same port. A keepalive ENTER landing in the same tick as a
             * 391-packet framebuffer is exactly the "amid other traffic" case
             * that lost 8 whole packets on the wire (docs/E16_REMOTE.md). The
             * two are joined here because here is the only place that can see
             * both. A REFUSED send does not count: nothing went out, so nothing
             * was crowded.
             */
            let sentThisTick = false;
            const oneSend = (packets) => {
                const res = send(packets);
                if (res !== false) sentThisTick = true;
                return res;
            };
            lifecycle.tick(t, oneSend);
            if (!lifecycle.enabled) return;
            syncPresence();
            /*
             * NO DEVICE, NO READS.
             *
             * ctl.tick() is a staggered PARAMETER READ -- ~2.8 ms of IPC, more
             * than a whole page render costs (CLAUDE.md) -- and it exists only
             * to keep the values the rings and the screen show fresh. With
             * nothing on the port there is nothing to show them to, and a
             * surface left switched on with the E16 in a bag would otherwise
             * spend that every frame forever.
             *
             * Safe to skip wholesale: with no device there is no input either,
             * so the focus cannot move and nav.tick has no stranded modifier to
             * expire. The first tick after an ACK does all of it.
             */
            if (!lifecycle.present) return;
            /* Before the display's tick: nav.tick() is the stranded-Shift
             * escape and may invalidate, and a repaint noticed after the send
             * would wait a whole frame. */
            nav.tick(t);
            syncFocus();
            if (ctl) ctl.tick();
            /*
             * NOTHING IS DRAWN AT A DEVICE THAT HAS NOT ANSWERED.
             *
             * A frame sent while seeking goes nowhere -- and the E16 that acks
             * a moment later comes up showing whatever the last process left on
             * it, while the surface believes it has painted. The repaint is
             * therefore OWED across the wait (fbOwed is a boolean, so the whole
             * seek costs one frame however long it takes) and syncPresence
             * re-owes it on every false->true edge, which is what makes a
             * replug -- the E16 has no battery, so unplugging clears its screen
             * -- redraw itself with no user action.
             *
             * This gates the SEND, never the state: nothing here decides what
             * the surface IS from `present`, so the unverified re-ACK
             * assumption in createLifecycle can cost at worst a second of stale
             * rings, never a dead surface.
             */
            /* The settle. Deliberately BEFORE the send budget is spent, so the
             * repaint it owes is picked up by this same tick rather than the
             * next one. */
            if (!settlePainted && t - turnedAt >= SETTLE_MS) {
                settlePainted = true;
                display.invalidate();
            }

            if (sentThisTick) return;
            /*
             * WHICH MODE THIS FRAME WANTS.
             *
             * The map is a picture -- boxes, names, a highlighted slot -- and
             * has to be a framebuffer. The parameter view is sixteen names and
             * a reading, which LABELS carries for an eleventh of the cost, so
             * a detent no longer pays 383 ms to move a number. The probe is a
             * framebuffer by definition: it exists to show raw bit layout.
             *
             * Computed HERE and handed down, rather than asked for inside the
             * display, so that the display stays a pure pacing machine with no
             * opinion about what a map is.
             */
            const probe = testPattern();
            /*
             * LABELS FOR BOTH VIEWS. The framebuffer is reserved for the
             * layout probe and is otherwise never sent.
             *
             * It is 394 packets against 34, and after a day on hardware it
             * still garbled occasionally with every buffer on our side proven
             * clean. LABELS costs the two page headers and arbitrary text --
             * the device's own limit is four characters a cell, so Lua would
             * cost exactly the same and want a script installed on top.
             * Reliability is the thing being bought here; density is the thing
             * being spent.
             */
            const screen = probe >= 0 ? { kind: "framebuffer" }
                         : (nav.mapVisible(t) ? mapScreen() : labelScreen());

            /* Arming or disarming the probe is a screen change like any other. */
            if (probe !== shownProbe) {
                shownProbe = probe;
                display.invalidate();
            }

            /* The meter repaints CONTINUOUSLY -- that is the measurement. It
             * owes a frame every tick, so the rate it reports is the fastest
             * the wire and the pacing together can go, with no parameter read
             * anywhere in it. */
            if (probe === 6) display.invalidate();
            const sent = display.tick(oneSend, () => {
                /* A layout probe overrides the view. See drawTestPattern:
                 * "the screen is garbled" cannot tell a wrong bit direction
                 * from a wrong page order, and both look like noise. Armed by
                 * a file so it needs no rebuild to change pattern. */
                /* The HOISTED probe, not a second read: the file is checked
                 * once a second, so two reads in one tick can straddle an
                 * arming and paint a pattern the mode decision did not choose. */
                if (probe >= 0) drawTestPattern(canvas, probe, { paints, fps: paintFps });
                else nav.render(canvas, t);
                return canvas.toBuffer();
            }, screen);

            /* A PAINT is a completed send, never an intent. */
            if (sent === "framebuffer" || sent === "labels") {
                paints++;
                if (lastPaintAt !== null) {
                    const dt = t - lastPaintAt;
                    if (dt > 0) {
                        const inst = 1000 / dt;
                        paintFps = paintFps === null
                            ? inst : paintFps + (inst - paintFps) * 0.2;
                    }
                }
                lastPaintAt = t;
            }
        },

        /* Read-only views, for the host's settings rows and for tests. */
        get enabled() { return lifecycle.enabled; },
        get present() { return lifecycle.present; },
        get slot() { return nav.slot; },
        get component() { return nav.component; },
        get controller() { return ctl; },
        get nav() { return nav; },
        get display() { return display; },
        view: viewNow,
    };
}
