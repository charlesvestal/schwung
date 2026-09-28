/*
 * scenes_screen.mjs -- the SCENES screen: pick A and B, move the crossfader,
 * arm a scene for editing, copy and clear scenes.
 *
 *   top two pad rows     scene A, 1-16 (tap the selected one again: none)
 *   bottom two pad rows  scene B, 1-16
 *   jog / knob 8         the fader (1/64 per detent, Shift 1/256)
 *   jog click            snap the fader to the nearer end
 *   Shift+click          learn an external CC for the fader (CC Map)
 *   pad HOLD             arm that scene for editing (hold it again to disarm)
 *   Copy + 2 pads        copy a scene (source, destination; either row)
 *   Delete + pad         clear a scene
 *   Undo                 undo the last copy or clear (one level; again = redo)
 *
 * PADS, NOT STEPS. The steps and Shift+steps are Move's everywhere -- its
 * sequencer and its Shift+step pages must always be reachable -- so the
 * scenes live on the pads, and only while this screen is up (pad_block
 * withholds them from Move; scene_pads strips Move's pad LED repaints). Two
 * rows per end is the whole bank at a glance: A on top, B below, the selected
 * scene bright, a scene with locks dim, the one being edited white.
 *
 * PURE except through `io` and the draw context, so tests/host can drive it
 * against the harness framebuffer. Everything it reads from the device (the
 * fader state, lock counts, dumps) comes through io; nothing here allocates a
 * model of the bank -- the DSP holds it (scene_doc.mjs).
 */

import { SCENE_COUNT } from "./scene_doc.mjs";

export const STEP_HOLD_MS = 500;
export const FADER_DETENT = 1 / 64;
export const FADER_FINE_DETENT = 1 / 256;
const ANNOUNCE_THROTTLE_MS = 400;

const CC_JOG = 14, CC_JOG_CLICK = 3, CC_COPY = 60, CC_DELETE = 119, CC_UNDO = 56;
const CC_KNOB8 = 78;

/* Move's pad grid, top row first (92-99 is the top row, rows descend by 8). */
const PAD_ROWS = [92, 84, 76, 68];

/** A pad note -> { end: "a" | "b", n: 0..15 }, or null. */
export function padScene(note) {
    for (let r = 0; r < 4; r++) {
        const first = PAD_ROWS[r];
        if (note >= first && note < first + 8) {
            return { end: r < 2 ? "a" : "b", n: (r % 2) * 8 + (note - first) };
        }
    }
    return null;
}

/** The pad that shows scene n on an end. */
export function scenePad(end, n) {
    const r = (end === "a" ? 0 : 2) + (n >= 8 ? 1 : 0);
    return PAD_ROWS[r] + (n % 8);
}

/* Palette indices (constants.mjs). Three levels per end, white for edit. */
export const PAD_COLORS = {
    a: { selected: 16, locked: 95, empty: 96 },   /* AzureBlue / DarkAzure / VeryDarkAzure */
    b: { selected: 3, locked: 72, empty: 68 },    /* BrightOrange / DarkOrange / VeryDarkOrangeRed */
    edit: 120,                                    /* White */
};

/** The colour a pad should show. */
export function padColor(end, n, st, counts) {
    if (st.edit === n) return PAD_COLORS.edit;
    const c = PAD_COLORS[end];
    if ((end === "a" ? st.a : st.b) === n) return c.selected;
    return counts && counts[n] > 0 ? c.locked : c.empty;
}

/* Relative encoder value -> signed detents (1..63 up, 65..127 down). */
export function relDelta(v) {
    if (v === 0 || v === 64) return 0;
    return v < 64 ? v : v - 128;
}

export function sceneLabel(n) { return (n >= 0 && n < SCENE_COUNT) ? String(n + 1) : "-"; }

export function createScenesScreen(io) {
    const now = io.now || (() => Date.now());
    /* Per PAD NOTE: when it went down (0 = up) and whether its hold fired. */
    const pressAt = new Map();
    const holdFired = new Map();
    /* What each pad was last painted, so a frame repaints only changes. */
    const painted = new Map();
    let copyHeld = false, deleteHeld = false, copySource = -1;
    let counts = null, countsRev = -1;
    let undo = null;              /* { snapshot, label } */
    let lastFaderAnnounce = 0;
    let learnPending = false;

    const st = () => io.state() || { a: -1, b: -1, edit: -1, xfade: 0, rev: 0, flash: 0 };

    function refreshCounts(force) {
        const s = st();
        if (!force && s.rev === countsRev) return;
        const c = io.lockCounts();
        if (c) { counts = c; countsRev = s.rev; }
    }

    function setFader(x, announce) {
        const v = Math.max(0, Math.min(1, x));
        io.setXfade(v);
        if (io.noteFaderMoved) io.noteFaderMoved(v);
        const t = now();
        if (announce && t - lastFaderAnnounce >= ANNOUNCE_THROTTLE_MS) {
            lastFaderAnnounce = t;
            io.announce("Fader " + Math.round(v * 100) + " percent");
        }
    }

    function setEnd(which, n) {
        const s = st();
        const a = which === "a" ? n : s.a;
        const b = which === "b" ? n : s.b;
        io.setAB(a, b);
        io.announce("Scene " + which.toUpperCase() + " " + (n >= 0 ? n + 1 : "none"));
    }

    /* A destructive edit keeps the bank as it was, so Undo can put it back. */
    function destructive(label, fn) {
        const snap = io.snapshot();
        if (!snap) { io.announce("Scenes busy, try again"); return false; }
        if (!fn()) { io.announce(label + " failed"); return false; }
        undo = { snapshot: snap, label };
        refreshCounts(true);
        return true;
    }

    function doUndo() {
        if (!undo) { io.announce("Nothing to undo"); return; }
        const redo = io.snapshot();
        if (!redo || !io.restore(undo.snapshot)) { io.announce("Undo failed"); return; }
        io.announce("Undo " + undo.label);
        /* One level, and it SWAPS: the same button is redo, which is the right
         * shape when the mistake is heard rather than seen. */
        undo = { snapshot: redo, label: undo.label };
        refreshCounts(true);
    }

    function padTap(end, n) {
        const s = st();
        if (s.edit === n) { io.setEdit(-1); io.announce("Scene " + (n + 1) + " disarmed"); return; }
        if (copyHeld) {
            if (copySource < 0) {
                copySource = n;
                io.announce("Copy scene " + (n + 1) + ", pick destination");
            } else {
                const src = copySource;
                copySource = -1;
                if (src === n) { io.announce("Copy cancelled"); return; }
                if (destructive("copy", () => io.applyAll("copy", src + " " + n)))
                    io.announce("Scene " + (src + 1) + " copied to " + (n + 1));
            }
            return;
        }
        if (deleteHeld) {
            if (destructive("clear", () => io.applyAll("clear", String(n))))
                io.announce("Scene " + (n + 1) + " cleared");
            return;
        }
        /* The selected one again clears that end. */
        setEnd(end, (end === "a" ? s.a : s.b) === n ? -1 : n);
    }

    function padHold(n) {
        const s = st();
        if (s.edit === n) {
            io.setEdit(-1);
            io.announce("Scene " + (n + 1) + " disarmed");
        } else {
            io.setEdit(n);
            io.announce("Editing scene " + (n + 1) + ". Turn any knob to lock it. Delete and turn to remove.");
        }
    }

    return {
        enter() {
            copyHeld = deleteHeld = false;
            copySource = -1;
            pressAt.clear();
            holdFired.clear();
            painted.clear();
            learnPending = false;
            refreshCounts(true);
            const s = st();
            io.announce("Scenes. A " + (s.a >= 0 ? s.a + 1 : "none") + ", B " + (s.b >= 0 ? s.b + 1 : "none") +
                        ", fader " + Math.round(s.xfade * 100) + " percent");
        },

        /* The step half of the hold gesture is timed here, while the finger is
         * still down: arming should happen when the hold is reached, not when
         * the finger comes off. */
        tick() {
            const t = now();
            let changed = false;
            for (const [note, at] of pressAt) {
                if (at && !holdFired.get(note) && t - at >= STEP_HOLD_MS && !copyHeld && !deleteHeld) {
                    holdFired.set(note, true);
                    padHold(padScene(note).n);
                    changed = true;
                }
            }
            const before = countsRev;
            refreshCounts(false);
            this.paintPads();
            return changed || countsRev !== before;
        },

        /* The pads, repainted only where they changed. `force` repaints all
         * (the screen was just entered, or the shim dropped our ownership). */
        paintPads(force) {
            if (!io.setPadLed) return;
            if (force) painted.clear();
            const s = st();
            for (const end of ["a", "b"]) {
                for (let n = 0; n < SCENE_COUNT; n++) {
                    const note = scenePad(end, n);
                    const col = padColor(end, n, s, counts);
                    if (painted.get(note) === col) continue;
                    if (io.setPadLed(note, col) !== false) painted.set(note, col);
                }
            }
        },

        /* The Delete/Copy/Undo claims this screen needs while it is up. */
        claimedCcs() { return [CC_COPY, CC_DELETE, CC_UNDO]; },

        get learnPending() { return learnPending; },
        clearLearnPending() { learnPending = false; },

        onMidi(status, d1, d2, shift) {
            const type = status & 0xF0;
            if (type === 0xB0) {
                if (d1 === CC_JOG || d1 === CC_KNOB8) {
                    const d = relDelta(d2);
                    if (d) setFader(st().xfade + d * (shift ? FADER_FINE_DETENT : FADER_DETENT), true);
                    return true;
                }
                if (d1 === CC_JOG_CLICK) {
                    if (d2 > 0) {
                        if (shift) {
                            learnPending = true;
                            if (io.learnFader) io.learnFader();
                        } else {
                            const x = st().xfade;
                            setFader(x >= 0.5 ? 1 : 0, false);
                            io.announce(x >= 0.5 ? "Fader at B" : "Fader at A");
                        }
                    }
                    return true;
                }
                if (d1 === CC_COPY) {
                    copyHeld = d2 > 0;
                    if (!copyHeld) copySource = -1;
                    return true;
                }
                if (d1 === CC_DELETE) { deleteHeld = d2 > 0; return true; }
                if (d1 === CC_UNDO) { if (d2 > 0) doUndo(); return true; }
                return false;
            }
            const pad = (type === 0x90 || type === 0x80) ? padScene(d1) : null;
            if (pad) {
                if (type === 0x90 && d2 > 0) {
                    pressAt.set(d1, now());
                    holdFired.set(d1, false);
                    /* With Copy or Delete held a press is a pick, not a hold:
                     * act on the press so a quick sequence of picks works. */
                    if (copyHeld || deleteHeld) { holdFired.set(d1, true); padTap(pad.end, pad.n); }
                } else {
                    const was = pressAt.get(d1);
                    pressAt.delete(d1);
                    if (was && !holdFired.get(d1)) padTap(pad.end, pad.n);
                    holdFired.delete(d1);
                }
                this.paintPads();
                return true;
            }
            return false;
        },

        /*
         * ctx: { fillRect, print, textWidth, drawHeader(title, right, inverted),
         *        drawFooter(hints) }. 128x64, footer band from y 55.
         */
        draw(ctx) {
            const s = st();
            const armed = s.edit >= 0;
            ctx.drawHeader(armed ? "Edit Scene " + (s.edit + 1) : "Scenes",
                           "A" + sceneLabel(s.a) + " B" + sceneLabel(s.b), armed);

            /* THE PADS, EXACTLY AS THEY SIT UNDER YOUR HANDS: four rows of
             * eight, A on the top two, B on the bottom two. Filled = the scene
             * holds locks; the selected scene has a dot cleared out of it; the
             * one being edited is drawn inverted-with-a-frame (a box inside a
             * box). The picture and the pads cannot disagree about where a
             * scene is, because both come from scenePad(). */
            const gx = 9, pitch = 9, cw = 8, chh = 6;
            const rowY = [12, 19, 28, 35];
            ctx.print(1, 15, "A", 1);
            ctx.print(1, 31, "B", 1);
            for (const end of ["a", "b"]) {
                const sel = end === "a" ? s.a : s.b;
                for (let n = 0; n < SCENE_COUNT; n++) {
                    const r = (end === "a" ? 0 : 2) + (n >= 8 ? 1 : 0);
                    const x = gx + (n % 8) * pitch, y = rowY[r];
                    const has = counts ? counts[n] > 0 : false;
                    if (has || n === sel) ctx.fillRect(x, y, cw, chh, 1);
                    else {
                        ctx.fillRect(x, y, cw, 1, 1);
                        ctx.fillRect(x, y + chh - 1, cw, 1, 1);
                        ctx.fillRect(x, y, 1, chh, 1);
                        ctx.fillRect(x + cw - 1, y, 1, chh, 1);
                    }
                    if (n === sel) ctx.fillRect(x + 2, y + 2, cw - 4, chh - 4, 0);
                    if (n === s.edit) {
                        ctx.fillRect(x + 1, y + 1, cw - 2, chh - 2, 0);
                        ctx.fillRect(x + 2, y + 2, cw - 4, chh - 4, 1);
                    }
                }
            }

            /* Right of the grid: each end's lock count and the position. */
            const ca = counts && s.a >= 0 ? counts[s.a] : 0;
            const cb = counts && s.b >= 0 ? counts[s.b] : 0;
            const pct = Math.round(Math.max(0, Math.min(1, s.xfade)) * 100) + "%";
            ctx.print(127 - ctx.textWidth(String(ca)), 15, String(ca), 1);
            ctx.print(127 - ctx.textWidth(String(cb)), 31, String(cb), 1);
            ctx.print(127 - ctx.textWidth(pct), 44, pct, 1);

            /* The fader: A |####....| B, the position as a notch. */
            const fy = 44, fh = 7, fx0 = 9, fx1 = 92;
            ctx.print(1, fy, "A", 1);
            ctx.fillRect(fx0, fy, fx1 - fx0, 1, 1);
            ctx.fillRect(fx0, fy + fh - 1, fx1 - fx0, 1, 1);
            ctx.fillRect(fx0, fy, 1, fh, 1);
            ctx.fillRect(fx1 - 1, fy, 1, fh, 1);
            const span = fx1 - fx0 - 4;
            const pos = fx0 + 2 + Math.round(Math.max(0, Math.min(1, s.xfade)) * span);
            ctx.fillRect(fx0 + 2, fy + 2, Math.max(0, pos - (fx0 + 2)), fh - 4, 1);
            ctx.fillRect(pos - 1, fy - 1, 3, fh + 2, 1);
            ctx.print(fx1 + 3, fy, "B", 1);

            if (learnPending) ctx.drawFooter(["Move a fader..."]);
            else if (copyHeld) ctx.drawFooter([copySource < 0 ? "Copy: pick source" : "Copy: pick dest"]);
            else if (deleteHeld) ctx.drawFooter(["Clear: pick scene"]);
            else if (armed) ctx.drawFooter(["Hold: done", "Del+knob: off"]);
            else ctx.drawFooter(["Pad: A/B", "Hold: edit"]);
        },
    };
}

/*
 * THE ARM BADGE: an inverted "SCN n" over the top-right corner of EVERY screen
 * while a scene is armed -- a latched mode with no sign of itself would record
 * into a scene without the user knowing. `flash` (FULL / N/A) replaces the
 * text for a moment when a knob's write could not be taken. Painted by the
 * host AFTER the view switch, so it lands on a module-drawn frame too.
 */
export const SCENE_FLASH_FULL = 1;
export const SCENE_FLASH_NA = 2;

export function armBadgeText(edit, flash) {
    if (flash === SCENE_FLASH_FULL) return "FULL";
    if (flash === SCENE_FLASH_NA) return "N/A";
    return edit >= 0 ? "SCN " + (edit + 1) : "";
}

export function drawArmBadge(ctx, text) {
    if (!text) return null;
    const w = ctx.textWidth(text) + 3;
    const x = 128 - w, y = 0, h = 9;
    ctx.fillRect(x - 1, y, w + 1, h + 1, 0);    /* a clear margin against what is under it */
    ctx.fillRect(x, y, w, h, 1);
    ctx.print(x + 2, y + 1, text, 0);
    return { x, y, w, h };
}
