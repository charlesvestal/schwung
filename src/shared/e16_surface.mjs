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
import { enterMsg, exitMsg, isAck, packetize, parseOledUpdateReply } from "./e16_protocol.mjs";
import { ATOMIC_MAX_PACKETS, SHIFT_TAP_MS, createPresence, createSysexAssembler,
         createFocus, createBinding, createKnobFeel } from "./surface_core.mjs";
export { createSysexAssembler, MAX_SYSEX, SHIFT_TAP_MS } from "./surface_core.mjs";

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
 * How often the current screen is restated even though nothing changed.
 *
 * THE LINK LOSES PACKETS, and not because our messages are too big: a
 * 101-byte LABELS message (34 packets) still corrupts occasionally on
 * hardware, with every buffer on our side proven clean and the message
 * verified well-formed -- legal framing, correct CINs, byte-exact through the
 * device-side unpacking. The 394-packet framebuffer garbled more often for
 * the obvious reason, not a different one.
 *
 * We cannot stop the loss, so the screen is made SELF-HEALING instead: a
 * corruption is repaired within this interval by a message costing 34
 * packets. That was unthinkable when a repaint cost 394 and is nearly free
 * now, which is the real dividend of leaving the framebuffer behind.
 *
 * A RESTATE, not a retry -- there is no acknowledgement to wait for and
 * nothing to detect, the same reason the shim restates pad_block every frame
 * rather than tracking it. Idempotent by construction: the same labels set
 * the same labels.
 *
 * Rings still outrank it, so a heartbeat never interrupts a turn.
 */
export const SCREEN_HEARTBEAT_MS = 1500;

/*
 * The restate for the drawn view -- far rarer than LABELS' 1.5 s, because
 * the reason for 1.5 s is gone. That cadence existed because a corruption
 * could be neither prevented nor DETECTED. With regions, every update is
 * ACKed or NACKed (measured 2026-09-24: both corrupted rectangles in a 20 s
 * capture came back NACKed, naming themselves), and a NACK repairs its own
 * region at once. What is left for the heartbeat is only damage the device
 * could not see -- and it now costs eight acknowledged bands, not an
 * unacknowledged framebuffer.
 */
export const PARTIAL_HEARTBEAT_MS = 30000;

/*
 * NO REGION MESSAGE IS TALLER THAN STRIP_H ROWS.
 *
 * Measured on hardware 2026-09-24: 128x8 bands (~160 wire bytes, ~53 packets,
 * ~7 SPI frames) came back NACKed 15 times out of 18 during Shift
 * transitions, while one-value rectangles were almost always clean -- the same
 * shape as the original garbling, where exposure is how long ONE message is on
 * the wire. And a band that loses its first data packet can leave a bare 00
 * straight after the header, which the E16 reads as EXIT REMOTE MODE: the
 * device dropping to its stock screen until the next keepalive. A 128x2 strip
 * is ~42 wire bytes, ~14 packets, one or two frames, and a loss costs two rows.
 * Every region -- full repaints and large diff regions alike -- is cut to it.
 */
export const STRIP_H = 1;

/*
 * EVERY MESSAGE FITS ONE SPI FRAME -- the atomic limit of the outbound queue
 * (UI_MIDI_CARRY_ATOMIC_MAX in ui_midi_out_carry.h). A message that fits is
 * placed whole in a single frame after Move's own cable-2 packets, so Move's
 * notes can never be spliced into it; one that does not straddles frames and
 * can be. So: rows are one pixel tall (a full-width row is a 10-packet
 * SCANLINE, a trimmed one an <=11-packet RECTANGLE), and rings go at most
 * RING_CHUNKS_PER_MSG to a message (11 packets). STRIP_H was 2: a full-width
 * 2-row strip is ~17 packets -- two frames, and every one of them spliceable.
 */
/* Defined once for every surface (surface_core.mjs); re-exported here. */
export { ATOMIC_MAX_PACKETS };
export const RING_CHUNKS_PER_MSG = 3;

/*
 * PACKETS PER TICK, not messages per tick.
 *
 * "One message per tick" was written for a 394-packet framebuffer, where the
 * rule and the wire agreed. With small strips it becomes the bottleneck -- 32
 * strips would take 32 ticks -- so the display sends regions until this many
 * packets have gone out in the tick (always at least one message). ~46 is what
 * the carry drains in one 60 Hz tick at pace 8 (8 packets x ~5.7 SPI frames);
 * 40 leaves room for Move's own traffic on the same cable.
 */
export const TICK_PACKET_BUDGET = 40;

/*
 * The budget FOLLOWS THE PACE. `e16_pace` (packets placed per SPI frame) is a
 * runtime file the shim reads, and a fixed 40 would cap the surface below what
 * a raised pace can drain -- or, at the shim's default of 3, feed it faster
 * than it drains. A 60 Hz tick spans ~5.7 SPI frames; 5 per unit of pace
 * leaves room for Move's own traffic. Pace 8 gives the measured 40.
 */
export const BUDGET_FRAMES_PER_TICK = 5;
export const SHIM_DEFAULT_PACE = 3;

/*
 * A region the device never answered is re-sent after this long.
 *
 * NACK repair covers a message that ARRIVED damaged. A message lost outright
 * gets no reply at all, and the surface -- which advanced its belief when it
 * sent -- would believe forever that the device shows it: the Shift map
 * drawn "incompletely", the last two labels never cleared (hardware,
 * 2026-09-24). Measured ACK latency is ~4 SPI frames (~12 ms); 250 ms leaves
 * room for a queued tick and the inbound path without retrying live traffic.
 */
export const ACK_TIMEOUT_MS = 1000;

/*
 * LOSS IS DETECTED BY ORDER, and the clock is only the backstop.
 *
 * ACK_TIMEOUT_MS was 250 ms, from an idle measurement (~12 ms). Under a real
 * repaint the device queues and answers take longer -- p90 ~380 ms on
 * hardware, 2026-09-24 -- so live rows "timed out", were re-sent, and (since a
 * timed-out row also left the in-flight window) the extra traffic deepened the
 * queue until EVERY row timed out: the whole unchanged screen re-sent once a
 * second, forever, 7886 of 7915 rows answered on the wire the whole time.
 *
 * The device answers in the order it receives -- mostly: in 11,253 rows
 * (hardware, 2026-09-24) 92% came back in order, most of the rest 1-4 places
 * early, and ~2% as far as 20+ places early, all answered in the end; 36
 * (0.3%) were truly never answered. So a region is LOST once REORDER_LOSS
 * regions sent after it have been answered without it -- TCP's duplicate-ACK
 * rule, with a margin past the deepest reorder seen (8 declared ~8% of rows
 * lost against 0.3% real). The clock remains for the tail (the LAST
 * message lost has nothing after it to be answered), at a length no live
 * answer reaches.
 */
export const REORDER_LOSS = 24;

/*
 * FLOW CONTROL: PIXELS IN FLIGHT. The E16 draws each region before it takes
 * the next, and when it falls behind it does not push back -- it drops bytes
 * and NACKs the region "interrupted" (status 06). Measured 2026-09-24 with
 * frame-atomic placement, so no foreign bytes inside any message: narrow rows
 * (<50 px) failed 0 of 755; rows of 100-128 px failed 315 of 2276, and almost
 * all of those went out one frame after another wide row. ACKs came back a
 * median 8-10 frames after the send -- a queue on the device, and it is the
 * DEPTH of that queue, in pixels, that fails: ~8 x 20 px in flight is fine,
 * ~10 x 128 px is not.
 *
 * So the next region waits until the pixels sent but not yet answered, plus
 * its own, fit the window. The window is AIMD: halved by a NACK, grown a
 * little by each ACK, so it settles at whatever this device keeps up with
 * rather than at a number we guessed. Something always goes when nothing is
 * outstanding, and ACK_TIMEOUT_MS frees a lost answer, so it cannot stall.
 */
export const WINDOW_PX_START = 384;
export const WINDOW_PX_MIN = 128;
export const WINDOW_PX_MAX = 1024;
export const WINDOW_PX_GROW = 128;

/*
 * How long after Move's last transmission the restate is allowed to resume.
 *
 * THE HEARTBEAT IS PURE REPAIR, AND WHILE MOVE IS TALKING IT IS ALSO THE MOST
 * LIKELY THING TO BE BROKEN.
 *
 * Isolated on hardware 2026-09-11: Move's own notes, aftertouch and clock go
 * out on the cable our screen SysEx must share -- the E16 has to enumerate as
 * a SINGLE jack for Move's XMOS to carry SysEx at all, so there is exactly one
 * stream and no way to separate them (see docs/E16_REMOTE.md). A 34-packet
 * LABELS spans ~5 SPI frames, and anything Move emits inside that window is
 * spliced into the message, which a conformant receiver must then discard.
 *
 * At idle the restate is the ONLY traffic there is. So repeating it once a
 * second while Move plays does not repair a corruption -- it MANUFACTURES one,
 * on a screen that would otherwise have sat there correct, because nothing
 * changed and nothing needed sending.
 *
 * Suppressing it costs nothing by construction: a restate carries no new
 * information. What must NOT be suppressed is a real CHANGE -- a page turn
 * while the transport runs has to arrive, or the panel shows labels belonging
 * to a different page while the encoders drive this one. A stale screen that
 * looks correct is a worse failure than a garbled one that obviously isn't.
 *
 * So the two are separated: repair waits for quiet, change goes now and
 * retries. That distinction is also why an earlier blanket retry made things
 * WORSE -- applied to a once-a-second heartbeat it tripled the standing
 * traffic, while applied to a rare human action it is a few packets nobody
 * sees.
 *
 * 250 ms is about two beats of sixteenths at 120 BPM: long enough that a
 * continuous part keeps the restate parked, short enough that letting go of
 * the keys brings the text back before you look up.
 *
 * ---------------------------------------------------------------------------
 * IT DID NOT WORK, AND THE GATE IS OFF. Measured on hardware 2026-09-11: the
 * screen still garbled, and the slot page markedly WORSE than before.
 *
 * The reasoning above has a hole. "If we do not send, nothing breaks" is only
 * true if the restate is the ONLY thing we send -- and it is not. A change
 * must still go out, and every recovery of the device's presence repaints,
 * and those sends are corrupted exactly as before. Suppressing the repair
 * while leaving the corruption in place means a garble that used to be fixed
 * within 1.5 s now PERSISTS until something else happens to repaint. Strictly
 * worse, and obviously so on the device.
 *
 * Suppressing repair only helps once the corruption itself is gone. That needs
 * messages short enough not to span an SPI frame -- the Lua path plus
 * per-element addressing -- not a smarter send policy. Transport-level
 * mitigation is exhausted: quiet-start, retry and suppression have each been
 * built, measured and found not to fix it.
 */
export const FOREIGN_QUIET_MS = 250;

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

/*
 * With region updates the settle alone is not a price worth paying.
 * It existed because a digit could only move by redrawing all 1024 bytes;
 * a changed value is now one small acknowledged RECTANGLE. So during a turn
 * the reading is redrawn LIVE, at most every LIVE_PAINT_MS -- not every tick,
 * because the display sends one message per tick and a screen owed outranks
 * a ring, so an unthrottled repaint would starve the ring the hand is
 * watching. Reported on hardware 2026-09-24: "short turns update quickly but
 * long turns dont" -- the settle only fired once the hand stopped.
 */
export const LIVE_PAINT_MS = 80;

/*
 * How often the surface re-renders and diffs
 * against what the device shows, for changes that did NOT come from its own
 * encoders -- a knob turned on Move, an LFO, a preset load. Nothing else
 * repaints for those. It used to be covered by accident: the 1.5 s
 * whole-screen heartbeat redrew everything, stale or not. A diff of an
 * unchanged screen is empty, so this costs rendering, never the wire.
 */
export const LOOK_MS = 250;

/*
 * RING KEEPALIVE. A ring message gets no ACK and no NACK, so one lost or
 * garbled -- or LED state the device drops on its own -- stayed wrong until
 * that knob moved. Reported on hardware 2026-09-24: every ring went blank
 * with the screen still correct, each coming back only when touched. All
 * sixteen ride in one ~113-byte message, so restating them is cheap.
 */
export const RING_RESTATE_MS = 3000;

/* A view change's rings are sent again this long after, once (see the ring
 * context in createSurface's tick). */
export const RING_ECHO_MS = 400;

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
    return createPresence({
        probeMs: o.probeMs === undefined ? PROBE_MS : o.probeMs,
        keepaliveMs: o.keepaliveMs === undefined ? KEEPALIVE_MS : o.keepaliveMs,
        lossMs: o.lossMs === undefined ? LOSS_MS : o.lossMs,
        /* ENTER is the probe AND what puts a replugged E16 back in remote
         * mode; its ACK is the only evidence the device is there; EXIT is
         * the owed goodbye. */
        probeMsg: enterMsg,
        isReply: isAck,
        exitMsg,
        packetize,
    });
}

/* Inbound SysEx reassembly lives in surface_core.mjs (createSysexAssembler),
 * shared with every surface, and is re-exported above. */

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
import { labelsMsg, ringMsg, scanlineMsg, rectangleMsg, clearMsg } from "./e16_protocol.mjs";
import { diffFramebuffers } from "./e16_diff.mjs";
import { packRowMajor, WIDTH as E16_WIDTH } from "./e16_canvas.mjs";

/* Copy one rectangle of pixels between two page/column buffers. */
function copyRect(dst, src, x, y, w, h) {
    for (let yy = y; yy < Math.min(64, y + h); yy++) {
        const bit = 1 << (yy & 7), row = (yy >> 3) * E16_WIDTH;
        for (let xx = x; xx < Math.min(E16_WIDTH, x + w); xx++) {
            const i = row + xx;
            dst[i] = (src[i] & bit) ? (dst[i] | bit) : (dst[i] & ~bit);
        }
    }
}

function regionKey(r) {
    if (r.kind === "clear") return "clear";
    if (r.kind === "scanline") return "s," + r.y;
    return [r.x, r.y, r.w, r.h].join(",");
}

/* After a CLEAR: one strip per STRIP_H rows that holds any ink, trimmed to
 * that ink's x-range. A blank strip is not sent at all. */

/* EVERY strip is a RECTANGLE, full-width rows included. A full-width row
 * used to go out as a SCANLINE to save ONE packet (10 vs 11); measured on
 * hardware 2026-09-24, the only NACKs left after frame-atomic placement were
 * both SCANLINEs (2 of 52) against 0 of 194 RECTANGLEs. One opcode is one
 * thing to trust -- the packet it saved is not worth a second code path in
 * the device's firmware. */
/* What a region costs the device to draw, for the in-flight window. A CLEAR
 * is the whole screen: it goes alone. */
function regionPx(r) {
    if (r.kind === "clear") return Infinity;
    if (r.kind === "scanline") return E16_WIDTH;
    return r.w * r.h;
}

function stripRegion(x, y, w, h) {
    return { kind: "rect", x, y, w, h };
}

function toStrips(regions) {
    const out = [];
    for (const r of regions) {
        if (r.kind === "scanline") { out.push(stripRegion(0, r.y, E16_WIDTH, 1)); continue; }
        if (r.kind !== "rect") { out.push(r); continue; }
        /* As many rows of this width as fit ONE message (see rectPackets). */
        let hMax = 1;
        while (hMax < r.h && rectPackets(r.w, hMax + 1) <= ATOMIC_MAX_PACKETS) hMax++;
        for (let y = r.y; y < r.y + r.h; y += hMax) {
            out.push(stripRegion(r.x, y, r.w, Math.min(hMax, r.y + r.h - y)));
        }
    }
    return out;
}

/*
 * Packets a RECTANGLE message of w x h costs on the wire: 4 address bytes and
 * the row-major bits, 8-to-7 packed, inside F0, the 5-byte header, the id and
 * F7, three bytes to a USB-MIDI packet. A message must fit ATOMIC_MAX_PACKETS
 * (one SPI frame, the only placement Move's notes cannot splice into), so a
 * message holds ~16 bytes of pixels whatever its SHAPE: one full-width row, or
 * a 16-px-wide label six rows tall. Narrow content is where blocks save
 * messages -- and each message is ~13 bytes of header the pixels ride under.
 */
export function rectPackets(w, h) {
    const raw = 4 + Math.ceil(w / 8) * h;
    return Math.ceil((raw + Math.ceil(raw / 7) + 8) / 3);
}

/* The INK of a picture as blocks: each row trimmed to its inked extent, then
 * consecutive inked rows merged while the union still fits one message. */
function inkedBlocks(buf) {
    const ext = [];
    for (let y = 0; y < 64; y++) {
        let x0 = -1, x1 = -1;
        const bit = 1 << (y & 7), row = (y >> 3) * E16_WIDTH;
        for (let x = 0; x < E16_WIDTH; x++) if (buf[row + x] & bit) { if (x0 < 0) x0 = x; x1 = x; }
        ext.push(x0 < 0 ? null : [x0, x1]);
    }
    const out = [];
    for (let y = 0; y < 64; ) {
        if (!ext[y]) { y++; continue; }
        let [x0, x1] = ext[y];
        let h = 1;
        while (y + h < 64 && ext[y + h]) {
            const n0 = Math.min(x0, ext[y + h][0]), n1 = Math.max(x1, ext[y + h][1]);
            if (rectPackets(n1 - n0 + 1, h + 1) > ATOMIC_MAX_PACKETS) break;
            x0 = n0; x1 = n1; h++;
        }
        out.push(stripRegion(x0, y, x1 - x0 + 1, h));
        y += h;
    }
    return out;
}

/* A genuine picture change this many rows deep may be drawn as CLEAR + ink. */
export const CLEAR_MIN_ROWS = 16;

/* Rows whose pixels differ between two pictures (belief vs target, without
 * any repair marks). */
function changedRows(a, b) {
    if (!a) return 64;
    let n = 0;
    for (let y = 0; y < 64; y++) {
        const bit = 1 << (y & 7), row = (y >> 3) * E16_WIDTH;
        for (let x = 0; x < E16_WIDTH; x++) if ((a[row + x] ^ b[row + x]) & bit) { n++; break; }
    }
    return n;
}

/* What a list of regions costs on the wire, in packets. */
function listPackets(list) {
    let n = 0;
    for (const r of list) n += r.kind === "clear" ? 3 : r.kind === "scanline" ? 10 : rectPackets(r.w, r.h);
    return n;
}

export function createDisplay(opts) {
    const budgetOf = (opts && opts.budgetOf) || (() => TICK_PACKET_BUDGET);
    /* Told WHY each full repaint (CLEAR + everything) happens -- the visible
     * blank. "Periodic blanking" cost a session of inference; one log line
     * per blank would have named it. */
    const onFull = (opts && opts.onFull) || (() => {});
    /* Told of every region declared lost, and why -- the measurement that
     * separates "the device never answered" from "we misread the answer". */
    const onLost = (opts && opts.onLost) || (() => {});
    let nullReason = "first paint";
    let windowPx = WINDOW_PX_START;
    let acks = 0, nacks = 0;
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
    /*
     * PARTIAL-UPDATE STATE. Built against a draft spec -- see
     * docs/superpowers/specs/2026-09-22-e16-partial-oled-updates-design.md.
     *
     * lastSentBuf: what we believe the device's pixel buffer currently
     * shows, or null if unknown (never shown a framebuffer-mode screen yet,
     * or the device's state was invalidated -- a replug via forgetShown(),
     * or a NACK). Only advances on a CONFIRMED emit, same discipline as
     * fbOwed/shownKind above: a refused send must not update what we
     * believe is on the device.
     *
     * pendingRegions / pendingBuf: a diff can describe up to MAX_REGIONS
     * small updates, and this file's own "at most one message per tick"
     * rule (see the PACED DISPLAY block above) means they can't all go out
     * at once. pendingBuf is the buffer the pending regions were computed
     * against -- later regions are packed from IT, not from a freshly
     * re-rendered buffer, so a mid-drain repaint elsewhere in the surface
     * can't invalidate the geometry of a region already queued.
     */
    let lastSentBuf = null;
    let pendingRegions = [];
    let pendingBuf = null;
    /* Screens the DEVICE now shows in full: a region repaint counts when its
     * LAST region is accepted, a LABELS screen when it is. One repaint is 1-8
     * messages, so counting sends would count bands, not pictures. The
     * refresh meter and the tests read this; nothing decides behaviour on it. */
    let paintsCompleted = 0;
    /* Sent regions awaiting an ACK or NACK, IN SEND ORDER, keyed as the
     * device echoes them. An answer matches the OLDEST entry with its key (a
     * re-send can share a key with the original). */
    const outstanding = [];
    /* Pixels the belief must not be trusted for: re-sent by the next diff
     * whatever lastSentBuf says. It used to be an XOR poison ON the belief,
     * and two repairs of one row before its re-send cancelled each other --
     * the repair vanished silently. A mask cannot cancel. */
    const dirty = new Uint8Array(1024);
    let timeouts = 0;
    /* For the web mirror: the last ring sent per knob, and a counter of
     * changes to anything the mirror shows. */
    const shownRings = new Array(16).fill(null);
    let mirrorVersion = 0;

    /* A region the device will never answer (see REORDER_LOSS): out of the
     * window, and re-sent. A lost CLEAR means nothing we believe holds. */
    function lose(o, why) {
        timeouts++;
        onLost(regionKey(o.region), why, o.passed);
        if (o.region.kind === "clear") {
            lastSentBuf = null; pendingRegions = []; pendingBuf = null; dirty.fill(0);
            nullReason = "CLEAR never acknowledged";
        } else if (o.region.kind === "scanline") markDirty(0, o.region.y, E16_WIDTH, 1);
        else markDirty(o.region.x, o.region.y, o.region.w, o.region.h);
        fbOwed = true;
    }
    function markDirty(x, y, w, h) {
        /* No belief at all: nothing to repair against -- repaint. */
        if (!lastSentBuf) { pendingRegions = []; pendingBuf = null; return; }
        for (let yy = Math.max(0, y); yy < Math.min(64, y + h); yy++)
            for (let xx = Math.max(0, x); xx < Math.min(E16_WIDTH, x + w); xx++)
                dirty[(yy >> 3) * E16_WIDTH + xx] |= (1 << (yy & 7));
    }
    function clearDirty(x, y, w, h) {
        for (let yy = Math.max(0, y); yy < Math.min(64, y + h); yy++)
            for (let xx = Math.max(0, x); xx < Math.min(E16_WIDTH, x + w); xx++)
                dirty[(yy >> 3) * E16_WIDTH + xx] &= ~(1 << (yy & 7));
    }
    /* The belief the diff compares against: lastSentBuf, except that a dirty
     * pixel is taken to differ from the target, so it is always re-sent. */
    function believed(target) {
        if (!lastSentBuf) return null;
        const b = lastSentBuf.slice();
        for (let i = 0; i < 1024; i++) {
            const d = dirty[i];
            if (d) b[i] = (b[i] & ~d) | (~target[i] & d & 0xFF);
        }
        return b;
    }
    /* When the last screen actually went out, for the heartbeat below. */
    let shownAt = null;
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
         * @returns {"framebuffer"|"labels"|"rings"|"rect"|"scanline"|null}
         *        what went out, for tests and for a caller that wants to log
         *        its send budget.
         */
        tick(send, frameBytes, screen, nowMs, heardMs) {
            /* Regions the device never answered: treat as lost and re-send
             * (see ACK_TIMEOUT_MS). A lost CLEAR means nothing we believe
             * about the device holds, so that is a full repaint.
             *
             * AGED AGAINST THE LAST TIME REPLIES WERE READ (heardMs), not the
             * clock. Replies are delivered before the UI tick; if something in
             * the tick stalls (a slot switch's blocking param reads), every
             * answer that arrived meanwhile is still unread in the ring when
             * this runs. Aged by the clock, one slow frame declared every
             * outstanding region lost and re-sent it -- hardware, 2026-09-24:
             * 52 "timeouts" against 6 regions truly unanswered on the wire,
             * clustered on slot switches. */
            const ref = heardMs !== undefined ? heardMs : nowMs;
            if (ref !== undefined && outstanding.length) {
                for (let i = 0; i < outstanding.length; ) {
                    if (ref - outstanding[i].at < ACK_TIMEOUT_MS) { i++; continue; }
                    lose(outstanding.splice(i, 1)[0], "clock");
                }
            }
            const want = screen ? screen.kind : "framebuffer";
            const screenOwed = fbOwed || pendingRegions.length > 0 || shownKind !== want;
            if (!screenOwed) {
                if (!rings.size) {
                    if (!labelsOwed || want !== "labels") return null;
                    if (!emitMsg(send, labelsMsg(screen.title, screen.labels))) return null;
                    labelsOwed = false;
                    paintsCompleted++;
                    return "labels";
                }
                /* RINGS WAIT FOR AN IDLE DEVICE. A ring gets no ACK, so one
                 * the device drops -- as it drops screen rows it is still
                 * busy drawing ("interrupted") -- is never reported and never
                 * repaired. Hardware, 2026-09-24: rings "corrupted" on the
                 * knobs while every ring message on the wire was well
                 * formed. So they go only when no screen update is awaiting
                 * its answer, i.e. the device has drawn everything sent. */
                if (nowMs !== undefined && outstanding.length) return null;
                /* At most RING_CHUNKS_PER_MSG to a message, so each fits one
                 * frame; as many messages as the tick's budget allows, the
                 * rest on the next tick. Sent chunks leave the map only once
                 * their message is accepted. */
                let usedR = 0, sentAny = false;
                const all = Array.from(rings.values());
                for (let i = 0; i < all.length; i += RING_CHUNKS_PER_MSG) {
                    const part = all.slice(i, i + RING_CHUNKS_PER_MSG);
                    const bytes = ringMsg(part);
                    const packets = Math.ceil(bytes.length / 3);
                    if (sentAny && usedR + packets > budgetOf()) break;
                    if (!emitMsg(send, bytes)) break;
                    usedR += packets; sentAny = true;
                    for (const r of part) { rings.delete(r.enc); shownRings[r.enc] = r; }
                    mirrorVersion++;
                }
                return sentAny ? "rings" : null;
            }

            if (want !== "framebuffer") {
                /* Leaving framebuffer mode (or never entering it this tick)
                 * -- any queued partial-update state describes a screen the
                 * device is no longer being asked to show. */
                pendingRegions = []; pendingBuf = null; lastSentBuf = null;
                nullReason = "left the drawn view (LABELS)";
                const bytes = labelsMsg(screen.title, screen.labels);
                if (emitMsg(send, bytes)) {
                    fbOwed = false; labelsOwed = false; shownKind = want;
                    if (nowMs !== undefined) shownAt = nowMs;
                    paintsCompleted++;
                    return want;
                }
                return null;
            }

            if (shownKind !== "framebuffer") {
                /* Switching INTO framebuffer mode -- the device's last known
                 * pixel state, if any, belongs to a mode we've left (or this
                 * is the very first paint). A diff against it would describe
                 * a screen that was never drawn. */
                if (lastSentBuf) nullReason = "entered the drawn view";
                pendingRegions = []; pendingBuf = null; lastSentBuf = null;
            }

            /*
             * A NEWER PICTURE REPLACES THE REST OF THE QUEUE. It used to wait
             * for the current drain to finish, so a quick Shift tap drew the
             * whole map before the knob view even started, and flipping
             * through slots drew every intermediate slot in full (hardware,
             * 2026-09-24). That wait was only needed while the belief was
             * approximate; it now holds exactly what went out, region by
             * region, so re-diffing mid-drain yields exactly what is still
             * needed. Not before the leading CLEAR of a full repaint has gone
             * (no belief yet) -- that repaint finishes as queued.
             */
            if (pendingRegions.length === 0 || (fbOwed && lastSentBuf)) {
                const buf = frameBytes();
                /*
                 * NEVER ESCALATE A KNOWN SCREEN TO "FULL". The region-count and
                 * area caps existed because one framebuffer used to be cheaper
                 * than many regions; a full repaint is now every inked strip, of
                 * which "only the changed strips" is always a subset. Escalating
                 * made a feedback loop on hardware (2026-09-24): a few NACKed
                 * strips on different rows crossed MAX_REGIONS, the diff called
                 * it full, the full repaint's extra traffic drew more NACKs --
                 * about four whole-screen repaints a second. "Full" is now only
                 * for a screen we know nothing about (prev === null).
                 */
                const diff = diffFramebuffers(believed(buf), buf,
                    { maxRegions: Infinity, fullRepaintThreshold: Infinity });
                if (diff.kind === "none") {
                    /* The device already shows this picture -- including when
                     * a pre-empted drain had already put it all out. */
                    if (pendingRegions.length) paintsCompleted++;
                    pendingRegions = []; pendingBuf = null;
                    fbOwed = false;
                    shownKind = "framebuffer";
                    return null;
                }
                if (diff.kind === "full") {
                    onFull(nullReason);
                    /*
                     * A FULL REPAINT IS ONE CLEAR, THEN ONLY THE INK. CLEAR is
                     * 8 bytes and ACKed; after it a blank strip needs no
                     * message at all and an inked one only its inked width.
                     * The knob view is mostly empty space, so this is most of
                     * the win; the map, boxed on most rows, gains less.
                     */
                    pendingBuf = buf.slice();
                    pendingRegions = [{ kind: "clear" }].concat(inkedBlocks(pendingBuf));
                    fbOwed = false;
                } else {
                    pendingBuf = buf.slice();
                    pendingRegions = toStrips(diff.regions);
                    /*
                     * A SWITCH TO A SPARSER VIEW IS A CLEAR AND ITS INK. The
                     * diff re-sends every changed row -- rows that only need
                     * ERASING included -- while a CLEAR erases the whole panel
                     * in one 3-packet message and the new view then costs only
                     * its ink. Taken when it is fewer packets AND the PICTURE
                     * really changed across at least CLEAR_MIN_ROWS rows -- a
                     * view switch. Never for a repair: a heartbeat or a lost
                     * row on a sparse screen can also be "cheaper" via CLEAR,
                     * and that blanks the whole panel to fix one line.
                     */
                    const viaClear = [{ kind: "clear" }].concat(inkedBlocks(pendingBuf));
                    if (changedRows(lastSentBuf, pendingBuf) >= CLEAR_MIN_ROWS &&
                        listPackets(viaClear) < listPackets(pendingRegions)) {
                        onFull("view change: CLEAR + ink is " + listPackets(viaClear) +
                               " packets vs " + listPackets(pendingRegions) + " for the diff");
                        pendingRegions = viaClear;
                    }
                    fbOwed = false;
                }
            }

            /* Regions until the tick's PACKET budget is spent -- always at
             * least one message, so a refused or oversized first region still
             * makes progress on a later tick. A refusal stops the tick and
             * leaves that region at the head of the queue. */
            let used = 0;
            let first = null;
            let inFlight = 0;
            if (nowMs !== undefined) for (const o of outstanding) inFlight += regionPx(o.region);
            while (pendingRegions.length) {
                const region = pendingRegions[0];
                /* The window (see WINDOW_PX_START). Only where answers are
                 * tracked: without nowMs nothing is ever outstanding. */
                if (nowMs !== undefined && inFlight > 0 && inFlight + regionPx(region) > windowPx) break;
                const bytes = region.kind === "clear" ? clearMsg()
                    : region.kind === "scanline"
                    ? scanlineMsg(region.y, packRowMajor(pendingBuf, 0, region.y, E16_WIDTH, 1))
                    : rectangleMsg(region.x, region.y, region.w, region.h,
                                    packRowMajor(pendingBuf, region.x, region.y, region.w, region.h));
                const packets = Math.ceil(bytes.length / 3);
                if (first !== null && used + packets > budgetOf()) break;
                if (!emitMsg(send, bytes)) break;
                used += packets;
                if (first === null) first = region.kind;
                if (nowMs !== undefined) {
                    outstanding.push({ key: regionKey(region), region, at: nowMs, passed: 0 });
                    inFlight += regionPx(region);
                }
                pendingRegions.shift();
                shownKind = "framebuffer";
                /*
                 * BELIEF ADVANCES BY EXACTLY WHAT WENT OUT. This was
                 * `lastSentBuf = pendingBuf.slice()` -- the WHOLE target
                 * picture on every region -- which erased any repair mark a
                 * NACK or timeout had just set on an earlier region of the
                 * same drain: the next send wrote the target over it, and the
                 * failed strip was never re-sent. Hardware, 2026-09-24:
                 * missing lines, and view switches (a long drain) left
                 * incomplete. A CLEAR makes the belief blank; a region copies
                 * only its own rectangle; unsent regions keep the old belief,
                 * which is what the device really shows.
                 */
                if (!lastSentBuf || region.kind === "clear") lastSentBuf = new Uint8Array(1024);
                if (region.kind === "clear") dirty.fill(0);
                if (region.kind === "scanline") { copyRect(lastSentBuf, pendingBuf, 0, region.y, E16_WIDTH, 1); clearDirty(0, region.y, E16_WIDTH, 1); }
                else if (region.kind === "rect") { copyRect(lastSentBuf, pendingBuf, region.x, region.y, region.w, region.h); clearDirty(region.x, region.y, region.w, region.h); }
                mirrorVersion++;
                if (nowMs !== undefined) shownAt = nowMs;
                if (pendingRegions.length === 0) { pendingBuf = null; paintsCompleted++; }
            }
            return first;
        },

        /* Test seams. */
        get framebufferOwed() { return fbOwed; },
        get labelsTextOwed() { return labelsOwed; },
        get shownKind() { return shownKind; },
        get paintsCompleted() { return paintsCompleted; },
        get outstandingCount() { return outstanding.length; },
        get ackTimeouts() { return timeouts; },
        get windowPx() { return windowPx; },
        get acks() { return acks; },
        get nacks() { return nacks; },
        /* The device answered one region (ACK or NACK): it is no longer
         * outstanding. A NACK is repaired separately by the caller. */
        acked(reply) {
            const a = (reply && reply.addr) || {};
            const k = reply.cmd === 0x07 ? "clear"
                    : reply.cmd === 0x05 ? "s," + a.y
                    : [a.x, a.y, a.w, a.h].join(",");
            /* Returns false for an ACK of a CLEAR we never sent: a corrupted
             * message the device read as CLEAR -- it blanked ITSELF, and
             * nothing we believe about the screen holds. The caller repaints. */
            const idx = outstanding.findIndex((e) => e.key === k);
            const known = idx >= 0;
            const o = known ? outstanding.splice(idx, 1)[0] : null;
            if (known) {
                /* Everything sent BEFORE this one and still unanswered was
                 * passed over once more; REORDER_LOSS passes is a loss. */
                for (let i = 0; i < idx && i < outstanding.length; ) {
                    if (++outstanding[i].passed >= REORDER_LOSS) lose(outstanding.splice(i, 1)[0], "order");
                    else i++;
                }
                /* Additive increase of about one row per WINDOW of answers
                 * (per answer: a share proportional to its size), not per
                 * answer -- per answer grew ~1000 px across one repaint and
                 * went straight back to the depth that fails. */
                if (reply.ok) { acks++;
                    const px = Math.min(regionPx(o.region), WINDOW_PX_MAX);
                    windowPx = Math.min(WINDOW_PX_MAX, windowPx + Math.max(1, Math.round(WINDOW_PX_GROW * px / windowPx)));
                } else { nacks++; windowPx = Math.max(WINDOW_PX_MIN, windowPx >> 1); }
            }
            return known || !(reply.ok && reply.cmd === 0x07);
        },
        /* A repaint is in flight: some of its regions are still queued. */
        get repaintPending() { return fbOwed || pendingRegions.length > 0; },
        /* Milliseconds since the screen last went out, or null if never. */
        screenAge(nowMs) {
            return shownAt === null || nowMs === undefined ? null : nowMs - shownAt;
        },
        /* A replug wipes the panel, so what the device was told is no longer
         * true. Forgetting it is what makes the presence edge resend. If the
         * mode itself is still trustworthy and only the PIXELS are suspect,
         * invalidateBuf() below is the narrower tool -- see its comment. */
        forgetShown() { shownKind = null; lastSentBuf = null; pendingRegions = []; pendingBuf = null; dirty.fill(0);
                        nullReason = "forgetShown (replug / presence edge)"; },
        /* Something happened that means what we BELIEVE is on the device
         * might be wrong, even though our own rendered content hasn't
         * changed -- an OLED UPDATE NACK (a later task), or a heartbeat
         * repair (this file's own SCREEN_HEARTBEAT_MS: it exists to RESEND
         * identical content because we don't trust the WIRE, only our own
         * buffer). Narrower than forgetShown(): the device is still
         * believed to be in framebuffer mode (shownKind untouched), only
         * the PIXELS are suspect -- the next diff sees prev === null and
         * sends a full repaint regardless of whether content changed. */
        invalidateBuf(reason) { lastSentBuf = null; pendingRegions = []; pendingBuf = null; dirty.fill(0);
                                nullReason = reason || "invalidateBuf"; },

        /*
         * The device NACKed ONE region and named it. Make only that area
         * look changed to the next diff, which then re-sends a region the
         * size of the one that failed -- not the whole screen. Done by
         * inverting our BELIEF of those pixels: the diff compares belief
         * against the fresh render, so every pixel in the rect now differs.
         * Anything we cannot localise (no belief yet, a reply without an
         * address) falls back to invalidateBuf's full repaint.
         */
        invalidateRegion(x, y, w, h) {
            /* No belief at all: nothing to repair against -- repaint. */
            if (!lastSentBuf) { pendingRegions = []; pendingBuf = null; return; }
            /*
             * An address we cannot use is NOT a reason to blank the screen.
             * Measured 2026-09-24: an INTERRUPTED strip (NACK 0x06, Move's
             * notes spliced in) comes back with the address fields the device
             * never read filled with FF, decoded as null -- and this used to
             * throw the whole belief away, a CLEAR + full repaint on every
             * such NACK: the "periodic blanking while turning a knob". The
             * region that failed is still OUTSTANDING (its NACK could not name
             * it), so it times out and is re-sent by itself.
             */
            if (![x, y, w, h].every((v) => typeof v === "number")) return;
            markDirty(x, y, w, h);
        },
        get ringsPending() { return rings.size; },
        /* THE MIRROR (e16_mirror_shm.h): what the device is believed to show
         * -- the screen bytes it was sent and the last ring per knob -- and a
         * version that moves whenever either does. */
        mirror() { return { frame: lastSentBuf, rings: shownRings, version: mirrorVersion }; },
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
import { buildMap, setOrdinal } from "./e16_map.mjs";
import { createMixer, renderMixer } from "./e16_mixer.mjs";
import { renderMap, pageStep, drawTestPattern } from "./e16_view.mjs";

/* How long a Shift hold can live without a note-off.
 *
 * Ten seconds is a judgement, and the direction of the error is the point. The
 * map is a jump-target picker: every hold that produces a jump ends when the
 * jump does, so a hold that has produced nothing for ten seconds is far more
 * likely to be a lost note-off than a decision. Erring long strands the
 * surface; erring short costs one more press. */
export const MAP_MAX_HOLD_MS = 10000;

/*
 * THE MAP APPEARS AFTER A SHORT HOLD, NOT ON THE PRESS.
 *
 * Shift is also the page modifier: Shift+turn steps the parameter pages. With
 * the map drawn on the press, every page turn flashed the whole map first (a
 * view switch and back -- two repaints for a gesture that wanted neither).
 * Now the map is drawn only once Shift has been held this long with nothing
 * turned; a turn inside the hold pages the parameters and SUPPRESSES the map
 * for the rest of that hold. A push acts as if the map were up -- the slot
 * row is a fixed position, so Shift+top-row switches slot without waiting to
 * see it.
 */
export const MAP_SHOW_DELAY_MS = 400;

/*
 * A TAP OF SHIFT TOGGLES THE MIXER (e16_mixer.mjs) -- the same grammar as every
 * other surface (surface_core.mjs, SHIFT_TAP_MS): released within that long,
 * having done nothing. A tap, not a held state: nothing is left latched on a
 * lost note-off, which is the whole reason the map is a hold. A press that did
 * anything (a turn, a push, a map) is not a tap. It was a DOUBLE tap while the
 * E16 was the only surface; the EC4, which has no map on its hold, made the
 * single tap the natural gesture, and one grammar across devices won.
 */

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
    const showDelayMs = o.showDelayMs === undefined ? MAP_SHOW_DELAY_MS : o.showDelayMs;
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
    /* THE focus (surface_core createFocus): owned by the surface and handed
     * in, so the nav, the controller binding and the host see one. A nav
     * built alone (tests) makes its own. */
    const focus = o.focus || createFocus({
        chainOf, followFocusOf, slot: o.slot, component: o.component, follow: o.follow,
    });

    /* THE ONLY MODIFIER STATE. Null means no hold; a number is when it began.
     * Never a boolean -- see the header. */
    let shiftDownAt = null;
    /* A page turn happened during this hold: the map stays hidden until the
     * next press (see MAP_SHOW_DELAY_MS). */
    let turnedThisHold = false;
    /* The Mixer view is up (a tap of Shift; see SHIFT_TAP_MS). */
    const renderMixerView = o.renderMixer || null;
    let mixerOn = false;
    let actedThisHold = false;

    let mapPage = 0;
    let showBuses = false;

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
    /* The modifier is HELD (gestures mean map things) ... */
    const held = (now) =>
        !focus.follow && shiftDownAt !== null && (now - shiftDownAt) < maxHoldMs;
    /* ... and the map is SHOWN once it has been held MAP_SHOW_DELAY_MS with
     * no page turn (see MAP_SHOW_DELAY_MS). Both derived from the timestamp,
     * so the delay needs no timer: tick() notices the change and repaints. */
    const mapVisible = (now) =>
        held(now) && !turnedThisHold && !mixerOn && (now - shiftDownAt) >= showDelayMs;

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
        if (focus.poll()) invalidate();
    };

    const currentMap = () =>
        buildMap(chainOf(), { slot: focus.slot, page: mapPage, showBuses });

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
                if (focus.follow) return null;
                if (ev.down) {
                    /* Deliberately not a re-arm: a repeat down while up leaves
                     * the original deadline standing (see the header). */
                    if (held(now)) return null;
                    shiftDownAt = now;
                    turnedThisHold = false;
                    actedThisHold = false;
                    mapPage = 0;
                    showBuses = false;
                    /* With a delay, no repaint yet: the map is drawn after
                     * MAP_SHOW_DELAY_MS, by tick() noticing it is due. */
                    if (mapVisible(now)) { invalidate(); return { action: "map" }; }
                    return { action: "hold" };
                }
                const wasShown = mapVisible(now);
                /* A TAP: released quickly, having done nothing. It toggles
                 * the Mixer. */
                const tap = !!renderMixerView && !actedThisHold && !wasShown &&
                            shiftDownAt !== null && now - shiftDownAt < SHIFT_TAP_MS;
                shiftDownAt = null;
                turnedThisHold = false;
                if (tap) {
                    mixerOn = !mixerOn;
                    invalidate();
                    return { action: "mixer", on: mixerOn };
                }
                /* Only a map that was actually up needs taking down. A tap, a
                 * hold spent paging, or a hold that already ended (a jump, an
                 * expiry) leaves a screen that is already correct. */
                if (!wasShown) return null;
                invalidate();
                return { action: "params" };
            }

            /* A button release is inert. Pushes act on the DOWN edge, so acting
             * on the up edge too would run every gesture twice -- and a jump
             * run twice is a second onFocus for a component the user selected
             * once. */
            if (ev.type === "release") return null;

            if (ev.type === "push") {
                if (mixerOn && !mapVisible(now)) {
                    /* THE MIXER: a push, or Shift+push (solo, 100%). The
                     * Shift+push spends the hold -- no map under it. */
                    const sh = held(now);
                    if (sh) { turnedThisHold = true; actedThisHold = true; }
                    return { action: "mixerPush", enc: ev.enc, shift: sh };
                }
                if (held(now)) actedThisHold = true;
                if (!held(now)) return { action: "click", enc: ev.enc };
                if (!mapVisible(now)) {
                    /* Held, map not drawn yet (or suppressed by a page turn):
                     * a push asks for the map, so it appears NOW and the push
                     * acts on it -- a blind slot switch would change nothing
                     * the knobs drive, since that takes a module pick. */
                    turnedThisHold = false;
                    shiftDownAt = Math.min(shiftDownAt, now - showDelayMs);
                    invalidate();
                }
                const enc = ev.enc | 0;
                if (enc < SLOT_CELLS) {
                    if (enc === focus.slot) {
                        /* The bus view of a slot with no buses is an empty
                         * list -- every lower knob went dark, which read as a
                         * broken page (hardware, 2026-09-24). Only a slot that
                         * HAS buses toggles; otherwise the tap is inert. */
                        const hasBuses = buildMap(chainOf(), { slot: focus.slot, showBuses: true }).cells.slice(SLOT_CELLS).some(Boolean);
                        if (!showBuses && !hasBuses) return null;
                        showBuses = !showBuses;
                        mapPage = 0;
                        invalidate();
                        return { action: "buses", showBuses };
                    }
                    /* The slot is entered at the module last edited there
                     * (createFocus) -- never at the old slot's position name,
                     * which on this slot may be empty or another module. */
                    focus.enterSlot(enc);
                    mapPage = 0;
                    /* Leaving bus view on a slot change is not tidiness: the
                     * new slot's components would otherwise be hidden behind a
                     * mode the user set while looking at a different slot. */
                    showBuses = false;
                    invalidate();
                    return { action: "slot", slot: focus.slot };
                }
                const cell = currentMap().cells[enc];
                /* A hole is not a destination (e16_map.mjs). Nothing moves and
                 * nothing repaints -- in particular the hold is NOT consumed,
                 * or a mis-hit would drop the map the user is still reading. */
                if (!cell) return null;
                focus.set(cell.slot, cell.component);
                shiftDownAt = null;   /* the jump ends the gesture */
                mixerOn = false;      /* ...and lands on the module's knobs */
                onFocus(focus.slot, focus.component);
                invalidate();
                return { action: "focus", slot: focus.slot, component: focus.component };
            }

            if (ev.type === "turn") {
                if (mixerOn && !mapVisible(now)) {
                    /* THE MIXER: a turn, or Shift+turn (pan) -- which keeps
                     * the hold alive and the map hidden, as paging does. */
                    const sh = held(now);
                    if (sh) { turnedThisHold = true; actedThisHold = true; shiftDownAt = now; }
                    return { action: "mixerTurn", enc: ev.enc, ticks: ev.ticks, shift: sh };
                }
                if (!held(now)) {
                    return { action: "turn", enc: ev.enc, ticks: ev.ticks };
                }
                actedThisHold = true;
                if (!mapVisible(now)) {
                    /* SHIFT+TURN BEFORE THE MAP SHOWS PAGES THE PARAMETERS,
                     * from any encoder, and keeps the map hidden for the rest
                     * of this hold -- the hand is paging, not looking for a
                     * module. */
                    turnedThisHold = true;
                    /* PAGING KEEPS THE HOLD ALIVE. A Shift+turn is a
                     * legitimate reason to hold Shift for a long time, and
                     * the MAP_MAX_HOLD_MS expiry ended it mid-turn (hardware,
                     * 2026-09-24: "it lost the shift hold"). Each page step
                     * restarts the expiry, so it now means "10 s after the
                     * last turn". A stranded Shift stays escapable: the turn
                     * glyph shows it is held, and a tap of Shift clears it.
                     * (Map turns still do NOT re-arm -- see the header.) */
                    shiftDownAt = now;
                    const next = pageStep(focus.pageIndex, ev.ticks, pageCountOf());
                    if (!focus.setPage(next)) return { action: "page", pageIndex: focus.pageIndex };
                    invalidate();
                    return { action: "page", pageIndex: focus.pageIndex };
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
                const next = pageStep(focus.pageIndex, ev.ticks, pageCountOf());
                if (!focus.setPage(next)) return { action: "page", pageIndex: focus.pageIndex };
                invalidate();
                return { action: "page", pageIndex: focus.pageIndex };
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
            /* The park and the restore are the focus's (createFocus). */
            if (!focus.setFollow(on)) return;
            /* A hold in progress cannot survive: the map is gone, so its
             * modifier would be a note-off owed to a view that no longer
             * exists. The timestamp shape means dropping it is the whole job
             * -- there is no latch anywhere else to unwind. */
            if (focus.follow) shiftDownAt = null;
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
            applyFollow();
            if (mapVisible(now) !== shownMap) invalidate();
        },

        /**
         * Draw whichever view is current. Handed to the display's tick as the
         * frame producer, so it runs ONLY when a repaint is actually going out.
         */
        render(ctx, now) {
            const up = mapVisible(now);
            if (up) renderMap(ctx, currentMap(), { page: mapPage, showBuses });
            else if (mixerOn && renderMixerView) renderMixerView(ctx);
            else renderParams(ctx);
            shownMap = up;
        },

        mapVisible,

        get slot() { return focus.slot; },
        get component() { return focus.component; },
        get pageIndex() { return focus.pageIndex; },
        get mapPage() { return mapPage; },
        /** The slot map as it stands (whether or not it is on screen). */
        map() { return currentMap(); },
        get showBuses() { return showBuses; },
        get followEnabled() { return focus.follow; },
        get focus() { return focus; },
        /* The DISPLAY MODE depends on this: a map is a picture and the
         * parameter view is sixteen labels, and the two modes override each
         * other on the device. Exposed rather than re-derived at the call site
         * -- the hold has a timeout, so "is the map up" is a question only this
         * object can answer correctly. */
        mapVisible(now) { return mapVisible(now); },
        /** Shift is held and the knob view is still showing: a turn pages. */
        turnHint(now) { return !mixerOn && held(now) && !mapVisible(now); },
        /** The Mixer view is up. */
        get mixer() { return mixerOn; },
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
import { buildView, renderView, ringFor, ringsFor, labelsFor, applyTurn, applyClick,
         mapRings, moduleRgb, renderEmptySlot, pageHasKnobs }
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
    /*
     * "framebuffer" (default) or "labels".
     *
     * The framebuffer IS the interface -- a drawn 128x64 panel, two headers,
     * real names. LABELS was adopted as a reliability stopgap: 34 packets
     * against 394, so it garbles far less often, but it costs the picture and
     * caps every cell at FOUR characters, which is the device's own limit and
     * not a budget we can spend our way out of.
     *
     * That trade is the user's to make, not ours. The corruption is not fixed
     * either way (2026-09-11: it is Move's own notes sharing the one cable the
     * XMOS will carry SysEx on, and quiet-start, retry and restate-suppression
     * were each built and measured and none of them fix it) -- so the honest
     * choice is a better-looking screen that sometimes breaks versus a poorer
     * one that breaks less. Default to the real interface.
     *
     * Injected, and read from a file by the host, so switching is an echo:
     *   echo labels > /data/UserData/schwung/e16_screen
     *   rm          /data/UserData/schwung/e16_screen     (back to framebuffer)
     */
    const screenModeOf = o.screenModeOf || (() => "framebuffer");
    /*
     * The shim's running count of Move's own cable-2 packets that landed in
     * the mailbox mid-message. Injected like everything else here, and
     * defaulting to a constant so the surface stays pure and testable: with no
     * host it reads 0 forever, foreignBusy() is always false, and the
     * heartbeat behaves exactly as it did before this existed.
     */
    const foreignOf = o.foreignOf || (() => 0);

    const lifecycle = createLifecycle(o.lifecycle);
    /* Injected like everything else; a host that says nothing gets the
     * fixed budget, which is what every test without a pace expects. */
    const paceOf = o.paceOf || null;
    let lastFullLogAt = -Infinity;
    const display = createDisplay({
        /* The queue places at least one WHOLE message per frame (up to
         * ATOMIC_MAX_PACKETS), whatever the pace, so the budget is sized to
         * a frame's worth of the larger of the two. */
        budgetOf: paceOf ? () => Math.max(paceOf() || SHIM_DEFAULT_PACE, 11) * BUDGET_FRAMES_PER_TICK
                         : () => TICK_PACKET_BUDGET,
        /* One line per visible blank, rate-limited, naming its cause. */
        onFull: (reason) => {
            const t = now();
            if (t - lastFullLogAt < 1000) return;
            lastFullLogAt = t;
            console.log("e16: full repaint (CLEAR) -- " + reason);
        },
        onLost: (key, why, passed) => {
            const t = now();
            if (t - lostLogWindowAt >= 1000) { lostLogWindowAt = t; lostLogged = 0; }
            if (lostLogged++ >= 8) return;
            console.log("e16: lost " + key + " by " + why + " (passed " + passed + ")");
        },
    });

    /* The presence the last tick saw, so the false->true edge can repaint. */
    let wasPresent = false;

    /* THE focus and the controller bound to it -- surface_core.mjs, shared
     * with every surface. `ctl` and `loaded` are read through the binding. */
    const focus = createFocus({ chainOf, followFocusOf });
    const binding = createBinding({ makeController, focus });
    const { metaOf } = binding;
    /* The E16's rotation is taken as Move's (one pulse, one detent); what the
     * feel adds here is the Mixer through the knob engine. */
    const feel = createKnobFeel({ pulsesPerDetentOf: o.pulsesPerDetentOf });

    /* Rebuilt on demand rather than cached. buildView is pure and reads only
     * the two lookups above, so it costs no IPC -- and a cached view is a
     * fourth thing that can disagree with the controller about which page is
     * current. */
    /* The pages the E16 shows: only those with a knob on them (pageHasKnobs).
     * Page numbers on the E16 count THESE, so "2/3" means the second page you
     * can turn, not the controller's index. */
    /* The Mixer: only when the host gives it a way to the parameters. */
    const mixer = o.mixer ? createMixer(o.mixer) : null;
    /* Knob pages only, mapped back to the controller's own index (binding). */
    const knobPages = binding.knobPages;
    const viewNow = () => binding.view(focus.pageIndex);

    /* Each module in the set has its own colour (moduleRgb / setOrdinal);
     * the knobs wear the colour of the module they edit -- the same colour
     * its knob had on the slot map. */
    const cellRgb = (cell) => moduleRgb(setOrdinal(chainOf(), cell.slot, cell.component));
    const knobRgb = () => nav ? moduleRgb(setOrdinal(chainOf(), nav.slot, nav.component)) : moduleRgb(-1);
    /* The focused slot holds no module at all. */
    const slotEmpty = () => {
        if (!nav) return false;
        const m = buildMap(chainOf(), { slot: nav.slot });
        return !m.cells.slice(4).some(Boolean);
    };
    /*
     * ALL SIXTEEN RINGS FOR WHAT IS ON SCREEN. The map lights slots and
     * modules; the knob view lights knobs. Every knob is described -- the ones
     * with nothing to show as DARK -- because the E16 keeps whatever a ring
     * last showed: a page with no knobs (presets), an empty slot, or the view
     * you just left would otherwise keep its old rings lit.
     */
    const desiredRings = (t) => (nav && nav.mapVisible(t))
        ? mapRings(nav.map(), cellRgb)
        : (nav && nav.mixer && mixer) ? mixer.rings()
        : (slotEmpty() ? ringsFor(null) : ringsFor(viewNow(), knobRgb()));
    /* What the rings last described, so a change of view / slot / module /
     * page restates all sixteen at once rather than waiting for the look. */
    let ringContext = null;
    let ringEchoAt = null;
    let shownTurnHint = false;
    /* The web mirror's publisher (host_e16_mirror), if the host has one. */
    const mirrorOut = o.mirror || null;
    let mirrorShownVersion = -1, mirrorAt = -Infinity;

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
    /* LIVE_PAINT_MS / LOOK_MS bookkeeping. ringSeen holds the last ring each
     * encoder was seen with by the look, so only a CHANGED ring goes out. */
    let lastLivePaintAt = -Infinity;
    let lookAt = -Infinity;
    let ringRestateAt = -Infinity;
    const ringSeen = new Map();

    /*
     * "Has Move transmitted recently?" -- from the DELTA of a free-running
     * counter, never its value.
     *
     * A rise means Move put cable-2 traffic in the mailbox while a message of
     * ours was going out. The absolute number is meaningless (it never
     * resets), and a counter that has stopped rising is exactly the quiet we
     * are waiting for, so the delta is the whole signal.
     *
     * Called once per tick and only from the heartbeat gate. It must stay
     * cheap: it is a shared-memory word, not a param read.
     */
    /*
     * CURRENTLY UNUSED, AND DELIBERATELY KEPT. Gating the heartbeat on this
     * was tried on hardware 2026-09-11 and made things WORSE -- see
     * FOREIGN_QUIET_MS. The plumbing behind it (the shim's published counter,
     * host_ui_midi_foreign) is sound and measured, so the signal is here for
     * the next idea that needs "is Move transmitting right now"; only the
     * conclusion drawn from it was wrong.
     */
    let foreignSeen = -1;
    let foreignAt = -Infinity;
    // eslint-disable-next-line no-unused-vars
    function foreignBusy(t) {
        let n = 0;
        try { n = foreignOf() | 0; } catch (e) { return false; }
        if (foreignSeen < 0) { foreignSeen = n; return false; }
        if (n !== foreignSeen) {
            foreignSeen = n;
            foreignAt = t;
        }
        return (t - foreignAt) < FOREIGN_QUIET_MS;
    }

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
        focus,
        chainOf,
        followFocusOf,
        pageCountOf: () => Math.max(1, knobPages().length),
        /* The Mixer view (a tap of Shift), when the host can reach the
         * slot volumes and sends -- see e16_mixer.mjs. */
        renderMixer: mixer ? (ctx) => renderMixer(ctx, mixer) : null,
        renderParams: (ctx) => {
            /* A slot with no modules says so -- a blank screen reads as a
             * dead device. */
            if (slotEmpty()) renderEmptySlot(ctx, nav.slot);
            else renderView(ctx, viewNow(), { turnHint: nav.turnHint(now()) });
        },
        /* The jump has already moved nav's focus; the controller catches up in
         * syncFocus() on the next tick. Reloading from here instead would put a
         * contract read (two blocking param round trips) on the MIDI callback
         * that delivered the button press. */
        onFocus: () => {},
    });

    /* Rate-limited so a run of NACKs (unlikely, but the whole point of
     * having them is to react to the unlikely) can't flood the log the way
     * an unthrottled per-message line would. */
    const NACK_LOG_RATE_MS = 1000;
    let lastNackLogAt = -Infinity;
    const STATS_LOG_MS = 10000;
    let unparsed = 0, lostLogWindowAt = -Infinity, lostLogged = 0;
    let heardAt = null;
    let lastStatsAt = -Infinity, statsAcks = 0, statsNacks = 0, statsTo = 0;

    const asm = createSysexAssembler({
        onMessage: (body) => {
            if (lifecycle.onSysex(body, now())) return;
            const reply = parseOledUpdateReply(body);
            if (!reply) {
                /* Ours by header and id, but not a well-formed reply: an
                 * answer that arrived and could not be read. Counted, since
                 * it would otherwise look exactly like no answer at all. */
                if (body.length > 6 && body[0] === 0x00 && body[1] === 0x21 && body[2] === 0x5B &&
                    (body[5] === 0x53 || body[5] === 0x54)) unparsed++;
                return;
            }
            /* One line per STATS_LOG_MS while replies flow: the NACK rate and
             * where the window settled, measured rather than inferred from a
             * rate-limited NACK line. */
            const ts = now();
            if (ts - lastStatsAt >= STATS_LOG_MS) {
                if (lastStatsAt > -Infinity) console.log("e16: oled acks=" + (display.acks - statsAcks) +
                    " nacks=" + (display.nacks - statsNacks) + " timeouts=" + (display.ackTimeouts - statsTo) +
                    " window=" + display.windowPx + "px unparsed=" + unparsed);
                lastStatsAt = ts; statsAcks = display.acks; statsNacks = display.nacks; statsTo = display.ackTimeouts;
            }
            if (!display.acked(reply)) {
                /* The device ACKed a CLEAR we never sent: it read a corrupted
                 * message as CLEAR and blanked itself. Repaint now, not at the
                 * next heartbeat. */
                display.invalidateBuf("device CLEARED itself (unsolicited CLEAR ack)");
                display.invalidate();
                return;
            }
            if (reply.ok) return;   /* ACK: we already advanced optimistically on send */
            /* The NACK NAMES the region that failed, so only that region is
             * re-sent (invalidateRegion); a reply we cannot localise falls
             * back to invalidateBuf's full repaint. Never forgetShown(): the
             * device still knows what mode it is in, only some PIXELS are
             * wrong. AND invalidate(): clearing a belief does not by itself
             * mark a repaint OWED -- without it a NACKed region sits
             * uncorrected until something unrelated repaints. */
            const a = reply.addr || {};
            if (reply.cmd === 0x08) display.invalidateRegion(a.x, a.y, a.w, a.h);
            else if (reply.cmd === 0x05) display.invalidateRegion(0, a.y, 128, 1);
            else if (reply.cmd === 0x07) display.invalidateBuf("CLEAR NACKed");
            /* Any other command byte: the message was cut before its command
             * could be read, so the NACK cannot say WHICH region failed. It
             * is not a reason to blank the screen -- every region is tracked
             * and the one that failed times out and is re-sent by itself.
             * This branch used to force a full CLEAR repaint. */
            display.invalidate();
            const t = now();
            if (t - lastNackLogAt >= NACK_LOG_RATE_MS) {
                lastNackLogAt = t;
                console.log("e16: OLED update NACK, cmd=0x" + reply.cmd.toString(16) +
                            " status=0x" + reply.status.toString(16) +
                            " window=" + display.windowPx + "px");
            }
        },
    });

    const ensureController = () => binding.ensure();

    /*
     * Point the controller at whatever the nav is focused on.
     *
     * Guarded on the PAIR rather than calling load() every tick: load() is safe
     * to repeat, but it reads `<prefix>:ui_hierarchy` and `<prefix>:chain_params`
     * to decide whether anything changed, and two blocking reads per frame is
     * most of a frame.
     */
    function syncFocus() {
        /* A different component is a different screen. */
        if (binding.sync()) display.invalidate();
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
        for (const desc of desiredRings(now())) display.ringChanged(desc);
    }

    return {
        /** The setting. Idempotent; EXIT is sent by the lifecycle, once. */
        setEnabled(on) { lifecycle.setEnabled(!!on, now(), send); },

        /*
         * A parameter was WRITTEN by someone other than these encoders --
         * Schwung's own knob grid on Move, most often. Nothing told the E16:
         * it noticed only on the next LOOK_MS pass plus the controller's
         * staggered re-read, up to ~0.5 s (hardware, 2026-09-24: "moving the
         * move knob is very slow to update the e16"). The written value goes
         * straight into the controller's cache and rides the same path as a
         * turn of our own encoder: the ring now, the digits live and
         * throttled, a final redraw when the hand stops. Keys arrive with or
         * without the component prefix; only a cell on screen is touched.
         */
        noteParamWrite(slot, key, value) {
            if (!lifecycle.enabled) return;
            for (const cell of binding.noteWrite(slot, key, value, viewNow())) {
                display.ringChanged(ringFor(viewNow(), cell.enc, knobRgb()));
                turnedAt = now();
                settlePainted = false;
            }
        },

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
        /* The host has just delivered every pending reply (MIDI is read
         * before the UI tick): anything still unanswered after this point
         * really was unanswered. Call first thing in the frame. */
        markInputRead() { heardAt = now(); },

        feedMidi(data) {
            asm.feed(data);
            if (!lifecycle.enabled) return null;
            const ev = decode(data);
            if (!ev) return null;
            const t = now();
            const act = nav.handle(ev, t);
            if (act && mixer) {
                if (act.action === "mixer") {
                    /* Entering reads the whole mixer once (~18 round trips,
                     * one time); after that our own writes keep it, and the
                     * look re-reads one value at a time. */
                    if (act.on) mixer.load();
                    return act;
                }
                if (act.action === "mixerTurn" || act.action === "mixerPush") {
                    /* A turn through the knob engine, as a module's knob
                     * (surface_core createKnobFeel). */
                    const changed = act.action === "mixerTurn"
                        ? (feel.begin(act.enc, t),
                           feel.mixerTurn(mixer, act.enc, feel.detents(act.enc, act.ticks), act.shift, t))
                        : mixer.push(act.enc, act.shift);
                    if (changed) {
                        /* The ring at once; the digits (and a MUTE/SOLO
                         * label) follow as a live repaint, as a turn does. */
                        display.ringChanged(mixer.ringFor(act.enc));
                        turnedAt = t;
                        settlePainted = false;
                    }
                    return act;
                }
            }
            const ctl = binding.controller;
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
                    display.ringChanged(ringFor(viewNow(), act.enc, knobRgb()));
                    /*
                     * THE NUMBER FOLLOWS THE HAND. The ring moves on every
                     * detent; the printed digits are a region repaint, owed
                     * here and paid by the tick at most every LIVE_PAINT_MS
                     * while turning, plus once more after the hand stops
                     * (SETTLE_MS) so the final value is always drawn.
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
                    display.ringChanged(ringFor(viewNow(), act.enc, knobRgb()));
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
            /* THE WEB MIRROR: publish what the device is believed to show
             * when it changes, and at least once a second so the mirror can
             * tell a still screen from a stopped surface. Before every early
             * return below, so an absent device is published as inactive. */
            if (mirrorOut) {
                const mv = display.mirror();
                if (mv.version !== mirrorShownVersion || t - mirrorAt >= 1000) {
                    mirrorShownVersion = mv.version;
                    mirrorAt = t;
                    const rb = [];
                    for (let e = 0; e < 16; e++) {
                        const r = mv.rings[e] || { r: 0, g: 0, b: 0, amount: 0, bipolar: false };
                        rb.push(r.r | 0, r.g | 0, r.b | 0, ((r.amount | 0) >> 8) & 0xFF, (r.amount | 0) & 0xFF, r.bipolar ? 1 : 0);
                    }
                    try { mirrorOut(mv.frame, rb, lifecycle.present); } catch (e) {}
                }
            }

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
            binding.tick();
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
            if (!settlePainted) {
                const still = t - turnedAt >= SETTLE_MS;
                const live = t - lastLivePaintAt >= LIVE_PAINT_MS;
                if (still || live) {
                    display.invalidate();
                    lastLivePaintAt = t;
                    /* The last repaint of a gesture is still the one after the
                     * hand stops, so the final value is always drawn. */
                    if (still) settlePainted = true;
                }
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
            /* The turn hint follows Shift in the knob view (see renderView). */
            const hint = nav.turnHint(t);
            if (hint !== shownTurnHint) { shownTurnHint = hint; display.invalidate(); }
            /* The rings follow the VIEW at once: map up or down, another slot,
             * module or page -- all sixteen restated in the new view's colours
             * (the controller's `loaded` is in the key, so a module's values
             * are restated again once they have actually arrived). */
            if (probe < 0) {
                const rctx = [nav.mapVisible(t), nav.mixer, nav.slot, nav.component, nav.pageIndex,
                              nav.mapPage, nav.showBuses, binding.loaded].join("|");
                const restate = () => {
                    for (const desc of desiredRings(t)) {
                        display.ringChanged(desc);
                        ringSeen.set(desc.enc, JSON.stringify(desc));
                    }
                };
                if (rctx !== ringContext) {
                    ringContext = rctx;
                    restate();
                    ringEchoAt = t + RING_ECHO_MS;
                } else if (ringEchoAt !== null && t >= ringEchoAt) {
                    /* ONE ECHO after a view change: rings are unacknowledged,
                     * so the set is sent again once, shortly after, rather
                     * than leaving a lost one for the 3 s keepalive. */
                    ringEchoAt = null;
                    restate();
                }
            }
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
            /* The picture unless the user has asked for the text fallback.
             * mapScreen()/labelScreen() build the LABELS form; the framebuffer
             * form is drawn by nav.render() in the frameBytes callback below,
             * which never stopped working -- only the choice to use it. */
            const wantLabels = screenModeOf() === "labels";
            const screen = probe >= 0 ? { kind: "framebuffer" }
                         : (wantLabels
                             ? (nav.mapVisible(t) ? mapScreen() : labelScreen())
                             : { kind: "framebuffer" });

            /* THE LOOK -- see LOOK_MS. The drawn view only: LABELS mode does
             * not diff, so a look there would resend the text every time.
             * Skipped mid-gesture, where the live
             * repaint and the turn's own rings already carry the change. */
            if (!wantLabels && probe < 0 && settlePainted &&
                t - lookAt >= LOOK_MS) {
                lookAt = t;
                /* The mixer notices changes made elsewhere (Move's track
                 * volume, Slot Settings) one read per look. */
                if (mixer && nav.mixer && !nav.mapVisible(t)) mixer.refreshNext();
                display.invalidate();
                for (const desc of desiredRings(t)) {
                    const k = JSON.stringify(desc);
                    if (ringSeen.has(desc.enc) && ringSeen.get(desc.enc) !== k) {
                        display.ringChanged(desc);
                    }
                    ringSeen.set(desc.enc, k);
                }
            }

            /*
             * SELF-HEAL, BUT ONLY WHILE MOVE IS QUIET.
             *
             * The link drops packets, so a screen that has stood untouched is
             * restated -- 34 packets to repair a corruption we cannot prevent
             * and cannot detect. While Move is transmitting on the shared
             * cable, though, that restate is the most likely message to be
             * corrupted AND the only traffic we have, so it breaks more than
             * it fixes. See FOREIGN_QUIET_MS.
             *
             * Nothing else is gated. A real change -- a page turn, a focus
             * jump, new label text -- goes out through invalidate() regardless
             * of what Move is doing, because a stale screen that looks correct
             * is worse than a garbled one that obviously is not.
             */
            const age = display.screenAge(t);
            /* LABELS is neither diffed nor acknowledged, so the text fallback
             * keeps the old restate; the drawn view is repaired by its NACKs. */
            const heartbeatMs = screenModeOf() === "labels" ? SCREEN_HEARTBEAT_MS : PARTIAL_HEARTBEAT_MS;
            if (age !== null && age >= heartbeatMs &&
                settlePainted && !display.ringsPending) {
                /* invalidateBuf() FIRST: this repaint's whole job is to
                 * resend content that, as far as OUR buffer is concerned,
                 * has not changed at all -- that is what a repair is. Every
                 * other invalidate() call site in this file is fine leaving
                 * lastSentBuf alone, because their content genuinely IS
                 * different; without this line the diff engine would see no
                 * difference here and correctly (for a real change) send
                 * nothing, silently turning this heartbeat into a no-op. */
                /* IN PLACE, NO CLEAR: every strip re-sent over what is there.
                 * invalidateBuf() here forgot the screen, so the repair was
                 * CLEAR + scan-in -- a visible blank every heartbeat
                 * (hardware, 2026-09-24). Marking every pixel dirty keeps the
                 * belief and makes the diff re-send all 64 rows in place. */
                display.invalidateRegion(0, 0, 128, 64);
                display.invalidate();
            }

            /* RING KEEPALIVE -- see RING_RESTATE_MS. Not mid-gesture (the
             * turn is already sending the ring under the hand) and not over
             * the layout probe, which owns the picture. The map's rings are
             * restated too: they are what the map shows. */
            if (probe < 0 && settlePainted && t - ringRestateAt >= RING_RESTATE_MS) {
                ringRestateAt = t;
                for (const desc of desiredRings(t)) display.ringChanged(desc);
            }

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
            const paintsBefore = display.paintsCompleted;
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
            }, screen, t, heardAt === null ? t : heardAt);

            /* A PAINT is a completed picture on the device, never an intent --
             * the LAST region of a repaint, or a whole LABELS screen. */
            if (display.paintsCompleted !== paintsBefore) {
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
        get controller() { return binding.controller; },
        get nav() { return nav; },
        get focus() { return focus; },
        get display() { return display; },
        view: viewNow,
    };
}
