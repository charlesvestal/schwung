/*
 * e16_mixer.mjs -- the E16's MIXER view: four tracks, four rows.
 *
 *            Track 1   Track 2   Track 3   Track 4
 *   row 1    level     level     level     level      push: mute   shift+push: solo
 *   row 2    send A    send A    send A    send A     push: 0 <-> back   shift+push: 100%
 *   row 3    send B    send B    send B    send B     push: 0 <-> back   shift+push: 100%
 *   row 4    return A  return B  capture   filter
 *
 * ONE PUSH RULE ACROSS THE ROWS: a push takes the control to its "off" and a
 * second push brings the setting back (level -> mute, send / return -> 0),
 * so a quick kill never loses the mix. SWITCHED OFF IS A STATE, like MUTE,
 * not a value: the cell is inverted and still shows the level, a turn while
 * off sets the level it comes back at (dial a send to 100% silently, then
 * push it in), and Move sees 0 until it is switched on. Shift+push is the row's
 * hard set (solo; a send or return to 100%). Row 4's third knob saves the
 * Skipback buffer on a push; the fourth is the master filter
 * (master_filter.h): turn left low-pass, right high-pass; push off and back,
 * Shift+push reset.
 *
 * The LEVEL is the slot volume, the same value Move's own track volume
 * drives (shadow_dbus.c writes it on a track-volume announcement), so the
 * E16 and Move's Track+Volume gesture move one number.
 *
 * Pure and injected like the rest of the E16 code: every read and write goes
 * through `io`, so tests drive it with a fake. Reads are an IPC round trip
 * each (~2.8 ms), so the model is read ONCE on entry and then kept by our own
 * writes, with one value re-read per `refreshNext()` to notice changes made
 * elsewhere (Move's track volume, the Slot Settings page).
 */

import { RING_MAX, ENCODERS, WIDTH, HEADER_BAR_H } from "./e16_view.mjs";

export const TRACKS = 4;
export const SEND_MAX = 127;              /* buses:main_send<N> and send<N>:return */
export const VOLUME_MAX = 2;              /* slot:volume is linear 0..2 (+6 dB) */
export const LEVEL_DB_STEP = 0.5;         /* per detent */
export const LEVEL_DB_FLOOR = -60;        /* below this, the level is -inf (0) */
export const SEND_STEP = 2;               /* per detent, of 127 */
export const FILTER_STEP = 0.02;          /* per detent, of -1..1 */
export const PAN_STEP = 0.02;             /* per detent, of -1..1 */
export const FILTER_DEADBAND = 0.02;      /* must match master_filter.h */

/* One colour per ROW, so the four functions read apart at a glance. Green is
 * the slots' elsewhere; none of these are green. */
export const MIXER_ROW_RGB = [
    { r: 80, g: 80, b: 80 },    /* level   -- white  */
    { r: 100, g: 30, b: 0 },    /* send A  -- orange */
    { r: 0, g: 34, b: 100 },    /* send B  -- blue   */
    { r: 90, g: 0, b: 70 },     /* returns / master -- magenta */
];

const rowOf = (enc) => Math.floor(enc / TRACKS);
const colOf = (enc) => enc % TRACKS;

function volToDb(v) { return v > 0 ? 20 * Math.log10(v) : -Infinity; }
function dbToVol(db) { return db <= LEVEL_DB_FLOOR ? 0 : Math.min(VOLUME_MAX, Math.pow(10, db / 20)); }

export function createMixer(io) {
    const o = io || {};
    const getSlot = o.getSlot || (() => null);
    const setSlot = o.setSlot || (() => false);
    const getGlobal = o.getGlobal || (() => null);
    const setGlobal = o.setGlobal || (() => false);
    const skipback = o.skipback || (() => false);
    const nameOf = o.nameOf || ((s) => "Track " + (s + 1));

    /* null = not read (or the read failed): drawn as blank, never as zero --
     * a failed read must not become a picture (CLAUDE.md, the tri-state). */
    const tracks = [];
    for (let s = 0; s < TRACKS; s++) tracks.push({ vol: null, muted: null, soloed: null, pan: null, send: [null, null] });
    const returns = [null, null];
    let filter = null;
    let filterMem = null;
    /* What a push-to-off remembers, so the second push restores it. */
    const sendMem = [[null, null], [null, null], [null, null], [null, null]];
    const returnMem = [null, null];

    const num = (raw) => {
        if (raw === null || raw === undefined || raw === "") return null;
        const n = parseFloat(raw);
        return isFinite(n) ? n : null;
    };

    /* The reads, as a list, so load() and refreshNext() share one order. */
    const READS = [];
    for (let s = 0; s < TRACKS; s++) {
        READS.push(() => { tracks[s].vol = num(getSlot(s, "slot:volume")); });
        READS.push(() => { const v = num(getSlot(s, "slot:muted")); tracks[s].muted = v === null ? null : v > 0; });
        READS.push(() => { const v = num(getSlot(s, "slot:soloed")); tracks[s].soloed = v === null ? null : v > 0; });
        /* A send switched off here but found above 0 was turned on
         * elsewhere (Move, Slot Settings): it is on again. */
        READS.push(() => { tracks[s].send[0] = num(getSlot(s, "buses:main_send1")); if (tracks[s].send[0] > 0) sendMem[s][0] = null; });
        READS.push(() => { tracks[s].send[1] = num(getSlot(s, "buses:main_send2")); if (tracks[s].send[1] > 0) sendMem[s][1] = null; });
        READS.push(() => { tracks[s].pan = num(getSlot(s, "slot:pan")); });
    }
    READS.push(() => { returns[0] = num(getGlobal("send1:return")); if (returns[0] > 0) returnMem[0] = null; });
    READS.push(() => { returns[1] = num(getGlobal("send2:return")); if (returns[1] > 0) returnMem[1] = null; });
    READS.push(() => { filter = num(getGlobal("master_fx:filter")); if (filter !== null && Math.abs(filter) > FILTER_DEADBAND) filterMem = null; });
    let readAt = 0;

    const clampSend = (v) => Math.max(0, Math.min(SEND_MAX, Math.round(v)));

    function writeSend(s, i, v) {
        v = clampSend(v);
        if (setSlot(s, "buses:main_send" + (i + 1), String(v)) !== false) tracks[s].send[i] = v;
    }
    function writeFilter(v) {
        v = Math.max(-1, Math.min(1, Math.round(v * 1000) / 1000));
        if (setGlobal("master_fx:filter", v.toFixed(3)) !== false) filter = v;
    }
    function writeReturn(i, v) {
        v = clampSend(v);
        if (setGlobal("send" + (i + 1) + ":return", String(v)) !== false) returns[i] = v;
    }

    const mixer = {
        /** Read everything once -- on entering the view. */
        load() { for (const r of READS) r(); readAt = 0; },
        /** Re-read ONE value, round robin (changes made elsewhere). */
        refreshNext() { READS[readAt](); readAt = (readAt + 1) % READS.length; },

        /** A turn. `shift` is Shift held: pan, on the level row. Returns true
         *  when something changed. */
        turn(enc, ticks, shift) {
            const row = rowOf(enc), s = colOf(enc);
            if (!ticks) return false;
            if (row === 0) {
                const t = tracks[s];
                if (shift) {
                    /* PAN: Shift+turn the level knob. */
                    if (t.pan === null) return false;
                    const next = Math.max(-1, Math.min(1, Math.round((t.pan + ticks * PAN_STEP) * 100) / 100));
                    if (next === t.pan) return false;
                    if (setSlot(s, "slot:pan", next.toFixed(2)) !== false) t.pan = next;
                    return true;
                }
                if (t.vol === null) return false;
                const db = volToDb(t.vol);
                const from = isFinite(db) ? db : LEVEL_DB_FLOOR;
                const next = dbToVol(from + ticks * LEVEL_DB_STEP);
                if (next === t.vol) return false;
                if (setSlot(s, "slot:volume", next.toFixed(4)) !== false) t.vol = next;
                return true;
            }
            if (row === 1 || row === 2) {
                const i = row - 1, cur = tracks[s].send[i];
                /* SWITCHED OFF: the turn sets the value it comes back at, and
                 * has no effect until a push switches it on. */
                if (sendMem[s][i] !== null) { sendMem[s][i] = clampSend(sendMem[s][i] + ticks * SEND_STEP); return true; }
                if (cur === null) return false;
                writeSend(s, i, cur + ticks * SEND_STEP);
                return true;
            }
            if (s < 2) {                                  /* row 4: returns */
                if (returnMem[s] !== null) { returnMem[s] = clampSend(returnMem[s] + ticks * SEND_STEP); return true; }
                if (returns[s] === null) return false;
                writeReturn(s, returns[s] + ticks * SEND_STEP);
                return true;
            }
            if (s === 3) {                                /* the master filter */
                if (filterMem !== null) {
                    filterMem = Math.max(-1, Math.min(1, Math.round((filterMem + ticks * FILTER_STEP) * 1000) / 1000));
                    return true;
                }
                if (filter === null) return false;
                writeFilter(filter + ticks * FILTER_STEP);
                return true;
            }
            return false;                                 /* capture: push only */
        },

        /** A push. Returns true when something changed. */
        push(enc, shift) {
            const row = rowOf(enc), s = colOf(enc);
            if (row === 0) {
                const t = tracks[s];
                if (shift) {
                    const on = !t.soloed;
                    if (setSlot(s, "slot:soloed", on ? "1" : "0") !== false) t.soloed = on;
                } else {
                    const on = !t.muted;
                    if (setSlot(s, "slot:muted", on ? "1" : "0") !== false) t.muted = on;
                }
                return true;
            }
            if (row === 1 || row === 2) {
                const i = row - 1, cur = tracks[s].send[i];
                if (shift) { sendMem[s][i] = null; writeSend(s, i, SEND_MAX); return true; }
                if (sendMem[s][i] !== null) {                /* off -> on, at its value */
                    const v = sendMem[s][i]; sendMem[s][i] = null; writeSend(s, i, v); return true;
                }
                if (cur === null) return false;
                sendMem[s][i] = cur; writeSend(s, i, 0);      /* on -> off, keeping it */
                return true;
            }
            if (s < 2) {
                const cur = returns[s];
                if (shift) { returnMem[s] = null; writeReturn(s, SEND_MAX); return true; }
                if (returnMem[s] !== null) {
                    const v = returnMem[s]; returnMem[s] = null; writeReturn(s, v); return true;
                }
                if (cur === null) return false;
                returnMem[s] = cur; writeReturn(s, 0);
                return true;
            }
            if (s === 2 && !shift) { skipback(); return true; }
            if (s === 3) {
                if (shift) { filterMem = null; writeFilter(0); return true; }
                if (filterMem !== null) { const v = filterMem; filterMem = null; writeFilter(v); return true; }
                if (filter === null || Math.abs(filter) <= FILTER_DEADBAND) return false;
                filterMem = filter; writeFilter(0);
                return true;
            }
            return false;
        },

        /** The cell for an encoder: { label, value } for the screen. */
        cell(enc) {
            const row = rowOf(enc), s = colOf(enc);
            if (row === 0) {
                const t = tracks[s];
                /* Solo and mute name the cell; otherwise an off-centre pan
                 * does ("L 30"), and a centred track just says Vol. */
                const panned = t.pan !== null && Math.abs(t.pan) >= 0.01;
                const label = t.soloed ? "SOLO" : (t.muted ? "MUTE"
                    : panned ? (t.pan < 0 ? "L " : "R ") + Math.round(Math.abs(t.pan) * 100) : "Vol");
                if (t.vol === null) return { label, value: "" };
                const db = volToDb(t.vol);
                return { label, value: isFinite(db) ? (db > 0 ? "+" : "") + db.toFixed(1) : "-inf" };
            }
            const pct = (v) => (v === null ? "" : Math.round(v / SEND_MAX * 100) + "%");
            /* SWITCHED OFF by a push is drawn inverted, like MUTE, and shows
             * the value it keeps -- the level it comes back at, which a turn
             * sets while it is off. */
            if (row === 1 || row === 2) {
                const i = row - 1, mem = sendMem[s][i];
                if (mem !== null) return { label: row === 1 ? "SndA" : "SndB", value: pct(mem), off: true };
                return { label: row === 1 ? "SndA" : "SndB", value: pct(tracks[s].send[i]) };
            }
            if (s < 2) {
                if (returnMem[s] !== null) return { label: s === 0 ? "RtnA" : "RtnB", value: pct(returnMem[s]), off: true };
                return { label: s === 0 ? "RtnA" : "RtnB", value: pct(returns[s]) };
            }
            if (s === 2) return { label: "Capt", value: "push" };
            if (filter === null) return { label: "Filt", value: "" };
            /* Switched off by a push: inverted, reading the position it
             * comes back at. */
            if (filterMem !== null) {
                const fv = Math.abs(filterMem) <= FILTER_DEADBAND ? "off"
                    : (filterMem < 0 ? "LP " : "HP ") + Math.round(Math.abs(filterMem) * 100);
                return { label: "Filt", value: fv, off: true };
            }
            if (filter < -FILTER_DEADBAND) return { label: "Filt", value: "LP " + Math.round(-filter * 100) };
            if (filter > FILTER_DEADBAND) return { label: "Filt", value: "HP " + Math.round(filter * 100) };
            return { label: "Filt", value: "off" };
        },

        /** One ring descriptor for `ringMsg`. */
        ringFor(enc) {
            const row = rowOf(enc), s = colOf(enc);
            let c = MIXER_ROW_RGB[row], amount = 0;
            if (row === 0) {
                const t = tracks[s];
                if (t.vol !== null) {
                    const db = volToDb(t.vol);
                    const top = volToDb(VOLUME_MAX);
                    amount = isFinite(db) ? Math.max(0, (db - LEVEL_DB_FLOOR) / (top - LEVEL_DB_FLOOR)) : 0;
                }
                /* A muted track's ring goes dim -- the level is kept, the
                 * track is silent -- unless solo overrides the mute. */
                if (t.muted && !t.soloed) c = { r: 16, g: 16, b: 16 };
            } else if (row === 1 || row === 2) {
                const mem = sendMem[s][row - 1];
                const v = mem !== null ? mem : tracks[s].send[row - 1];
                amount = v === null ? 0 : v / SEND_MAX;
                if (mem !== null) c = { r: Math.round(c.r / 5), g: Math.round(c.g / 5), b: Math.round(c.b / 5) };
            } else if (s < 2) {
                const v = returnMem[s] !== null ? returnMem[s] : returns[s];
                amount = v === null ? 0 : v / SEND_MAX;
                if (returnMem[s] !== null) c = { r: Math.round(c.r / 5), g: Math.round(c.g / 5), b: Math.round(c.b / 5) };
            } else if (s === 2) {
                c = { r: 40, g: 0, b: 0 };               /* capture: a dim red button */
                amount = 1;
            } else {
                /* The filter: bipolar, centred when off; switched off, dim at
                 * the position it comes back at. */
                const x = filterMem !== null ? filterMem : (filter === null ? 0 : filter);
                if (filterMem !== null) c = { r: Math.round(c.r / 5), g: Math.round(c.g / 5), b: Math.round(c.b / 5) };
                return { enc, r: c.r, g: c.g, b: c.b,
                         amount: Math.max(0, Math.min(RING_MAX, Math.round((x + 1) / 2 * RING_MAX))),
                         bipolar: true };
            }
            return { enc, r: c.r, g: c.g, b: c.b,
                     amount: Math.max(0, Math.min(RING_MAX, Math.round(amount * RING_MAX))),
                     bipolar: false };
        },

        rings() { const out = []; for (let e = 0; e < ENCODERS; e++) out.push(mixer.ringFor(e)); return out; },

        nameOf,
        /* Test seams. */
        get tracks() { return tracks; },
        get returns() { return returns; },
        get filter() { return filter; },
    };
    return mixer;
}

/* ---- the screen ---------------------------------------------------------
 * An inverted bar naming the four tracks over their columns, then four rows
 * of 32 x 14 cells, each a label over a value -- the knob view's cell, on the
 * knob it belongs to. */
const MIXER_ROW_H = 14;
function clipTo(ctx, text, w) {
    let s = String(text === undefined || text === null ? "" : text);
    if (ctx.textWidth(s) <= w) return s;
    while (s.length && ctx.textWidth(s + ".") > w) s = s.slice(0, -1);
    return s + ".";
}

export function renderMixer(ctx, mixer) {
    ctx.clear();
    const colW = WIDTH / TRACKS;
    ctx.fillRect(0, 0, WIDTH, HEADER_BAR_H - 1, 1);
    for (let s = 0; s < TRACKS; s++) {
        ctx.print(s * colW + 1, 1, clipTo(ctx, mixer.nameOf(s), colW - 2), 0);
        if (s) ctx.fillRect(s * colW - 1, 0, 1, HEADER_BAR_H - 1, 0);
    }
    for (let e = 0; e < ENCODERS; e++) {
        const x = colOf(e) * colW, y = HEADER_BAR_H + rowOf(e) * MIXER_ROW_H;
        const c = mixer.cell(e);
        const inv = c.off || (rowOf(e) === 0 && (c.label === "MUTE" || c.label === "SOLO"));
        if (inv) ctx.fillRect(x, y, colW - 1, MIXER_ROW_H - 1, 1);
        ctx.print(x + 1, y + 1, clipTo(ctx, c.label, colW - 2), inv ? 0 : 1);
        if (c.value) ctx.print(x + 1, y + 7, clipTo(ctx, c.value, colW - 2), inv ? 0 : 1);
    }
}
