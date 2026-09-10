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
export const KEEPALIVE_MS = 10000;
export const LOSS_MS = 25000;

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
