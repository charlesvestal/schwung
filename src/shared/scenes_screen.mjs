/*
 * scenes_screen.mjs -- the SCENES screen.
 *
 * TWO LAYERS (scene_doc.mjs): 32 SNAPSHOTS on the pads -- A1-A16 on the top
 * two rows, B1-B16 on the bottom two -- and 16 SCENES on the steps, each a
 * pairing of one A and one B (either may be none: that end is the knobs as
 * they are). The fader morphs the ACTIVE scene from its A to its B.
 *
 *   step                 pick the active scene
 *   tap an A pad         that snapshot becomes the scene's A (the lit one
 *                        again: none);  B pads the same
 *   hold a pad           EDIT that snapshot (latched; tap it to stop)
 *   jog / knob 8         the fader (1/64 per detent, Shift 1/256)
 *   jog click            snap the fader to the nearer end
 *   Shift+jog click      learn an external CC for the fader (CC Map)
 *   Copy + 2 steps       copy a scene's pairing;  Copy + 2 pads: a snapshot
 *   Delete + step        empty a scene's pairing; Delete + pad: a snapshot
 *   Undo                 undo the last copy or clear (one level; again = redo)
 *
 * EDITING IS LATCHED: it outlasts the screen, because the knobs being locked
 * are on other screens. The snapshot being edited plays at 100% whatever the
 * fader says, so what you hear is what you are building. Snapshots are
 * SHARED: editing A1 changes every scene that uses A1.
 *
 * The steps and pads are taken ONLY while this screen is up (the host restates
 * the claims every tick); Shift+step is never taken.
 *
 * PURE except through `io` and the draw context, so tests/host drive it
 * against the harness framebuffer.
 */

import { SCENE_COUNT, SNAP_COUNT, halfA, halfB } from "./scene_doc.mjs";

export const HOLD_MS = 500;
export const FADER_DETENT = 1 / 64;
export const FADER_FINE_DETENT = 1 / 256;
const ANNOUNCE_THROTTLE_MS = 400;

const CC_JOG = 14, CC_JOG_CLICK = 3, CC_COPY = 60, CC_DELETE = 119, CC_UNDO = 56;
const CC_KNOB8 = 78;
const NOTE_STEP_FIRST = 16;

/* Relative encoder value -> signed detents (1..63 up, 65..127 down). */
export function relDelta(v) {
    if (v === 0 || v === 64) return 0;
    return v < 64 ? v : v - 128;
}

/* Move's pad grid, top row first (92-99 is the top row, rows descend by 8). */
const PAD_ROWS = [92, 84, 76, 68];

/** A pad note -> { side: "a" | "b", i: snapshot 0..15 }, or null. */
export function padSnap(note) {
    for (let r = 0; r < 4; r++) {
        const first = PAD_ROWS[r];
        if (note >= first && note < first + 8)
            return { side: r < 2 ? "a" : "b", i: (r % 2) * 8 + (note - first) };
    }
    return null;
}

/** The pad of a snapshot. */
export function snapPad(side, i) {
    return PAD_ROWS[(side === "a" ? 0 : 2) + (i >= 8 ? 1 : 0)] + (i % 8);
}

export const snapHalf = (side, i) => (side === "a" ? halfA(i) : halfB(i));

/* Palette indices (constants.mjs). */
export const COLORS = {
    /* inScene = the active scene uses it; locked = holds locks; empty = none */
    a: { inScene: 16, locked: 95, empty: 96 },   /* AzureBlue / DarkAzure / VeryDarkAzure */
    b: { inScene: 3, locked: 72, empty: 68 },    /* BrightOrange / DarkOrange / VeryDarkOrangeRed */
    edit: 120,                                   /* White */
    stepActive: 120,                             /* White */
    stepPaired: 118,                             /* LightGrey: pairs something */
    stepEmpty: 0,
};

/** A snapshot's name: "A3", "B12". */
export function snapName(side, i) { return side.toUpperCase() + (i + 1); }

/** The badge the host draws on every screen while a snapshot is being edited. */
export function editLabel(editHalf) {
    if (!(editHalf >= 0)) return "";
    return editHalf >= SNAP_COUNT ? snapName("b", editHalf - SNAP_COUNT) : snapName("a", editHalf);
}

export function createScenesScreen(io) {
    const now = io.now || (() => Date.now());
    const pressAt = new Map();           /* note -> press time */
    const holdFired = new Map();
    const painted = new Map();           /* note -> colour last sent */
    let copyHeld = false, deleteHeld = false;
    let copySource = null;               /* { kind: "scene", k } | { kind: "snap", side, i } */
    let counts = null, countsRev = -1;   /* locks per HALF */
    let undo = null;
    let lastFaderAnnounce = 0;
    let learnPending = false;

    const st = () => io.state() || { edit: -1, xfade: 0, rev: 0, flash: 0 };
    const scn = () => io.scene() || { active: -1, pairs: [] };
    const pairOf = (k) => (scn().pairs[k] || [-1, -1]);
    const halfLocks = (h) => (counts ? counts[h] || 0 : 0);

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

    /* An edit that destroys something keeps what was there, so Undo can put it
     * back: the DSP bank AND the pairings (io.snapshot carries both). */
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
        undo = { snapshot: redo, label: undo.label };
        refreshCounts(true);
    }

    function selectScene(k) {
        io.setActive(k);
        const p = pairOf(k);
        io.announce("Scene " + (k + 1) + ", " + (p[0] >= 0 ? snapName("a", p[0]) : "no A") +
                    " to " + (p[1] >= 0 ? snapName("b", p[1]) : "no B"));
    }

    function stepPress(k) {
        if (copyHeld) {
            if (!copySource || copySource.kind !== "scene") {
                copySource = { kind: "scene", k };
                io.announce("Copy scene " + (k + 1) + ", pick destination");
            } else {
                const src = copySource.k;
                copySource = null;
                if (src === k) { io.announce("Copy cancelled"); return; }
                if (destructive("copy", () => { io.setPair(k, pairOf(src).slice()); return true; }))
                    io.announce("Scene " + (src + 1) + " copied to " + (k + 1));
            }
            return;
        }
        if (deleteHeld) {
            if (destructive("clear", () => { io.setPair(k, [-1, -1]); return true; }))
                io.announce("Scene " + (k + 1) + " emptied");
            return;
        }
        selectScene(k);
    }

    /* Start or stop editing a snapshot. */
    function toggleEditSnap(side, i) {
        const h = snapHalf(side, i);
        if (st().edit === h) {
            io.setEdit(-1);
            io.announce("Done editing " + snapName(side, i));
            return;
        }
        io.setEdit(h);
        io.announce("Editing " + snapName(side, i) + ". Turn knobs on any page to lock them.");
    }

    /* Shift+Up / Shift+Down: edit the ACTIVE scene's A / B. A scene with no
     * snapshot on that end gets the one matching its own number first. */
    function toggleEdit(side) {
        let k = scn().active;
        if (k < 0) { k = 0; io.setActive(0); }
        const p = pairOf(k).slice();
        const j = side === "a" ? 0 : 1;
        if (p[j] < 0) { p[j] = k; io.setPair(k, p); }
        toggleEditSnap(side, p[j]);
    }

    function padTap(side, i) {
        if (copyHeld) {
            if (!copySource || copySource.kind !== "snap") {
                copySource = { kind: "snap", side, i };
                io.announce("Copy " + snapName(side, i) + ", pick destination");
            } else {
                const src = copySource;
                copySource = null;
                if (src.side === side && src.i === i) { io.announce("Copy cancelled"); return; }
                if (destructive("copy", () => io.applyAll("copy", snapHalf(src.side, src.i) + " " + snapHalf(side, i))))
                    io.announce(snapName(src.side, src.i) + " copied to " + snapName(side, i));
            }
            return;
        }
        if (deleteHeld) {
            if (destructive("clear " + snapName(side, i), () => io.applyAll("clear", String(snapHalf(side, i)))))
                io.announce(snapName(side, i) + " cleared");
            return;
        }
        const h = snapHalf(side, i);
        if (st().edit === h) { toggleEditSnap(side, i); return; }
        let k = scn().active;
        if (k < 0) { k = 0; io.setActive(0); }
        const p = pairOf(k).slice();
        const j = side === "a" ? 0 : 1;
        p[j] = p[j] === i ? -1 : i;
        io.setPair(k, p);
        io.announce("Scene " + (k + 1) + " " + side.toUpperCase() + " " +
                    (p[j] >= 0 ? snapName(side, i) : "none"));
    }

    return {
        enter() {
            copyHeld = deleteHeld = false;
            copySource = null;
            pressAt.clear();
            holdFired.clear();
            painted.clear();
            learnPending = false;
            refreshCounts(true);
            const k = scn().active;
            io.announce(k >= 0 ? "Scenes. Scene " + (k + 1) : "Scenes. Pick a scene with the steps.");
        },

        /* Called by the host for Shift+Up / Down. */
        toggleEdit,

        tick() {
            const t = now();
            let changed = false;
            for (const [note, at] of pressAt) {
                const ps = padSnap(note);
                if (ps && at && !holdFired.get(note) && t - at >= HOLD_MS && !copyHeld && !deleteHeld) {
                    holdFired.set(note, true);
                    toggleEditSnap(ps.side, ps.i);
                    changed = true;
                }
            }
            const before = countsRev;
            refreshCounts(false);
            this.paintLeds();
            return changed || countsRev !== before;
        },

        claimedCcs() { return [CC_COPY, CC_DELETE, CC_UNDO]; },
        get learnPending() { return learnPending; },
        clearLearnPending() { learnPending = false; },

        /* Steps and pads, repainted only where they changed; `force` repaints
         * all (the screen just took them, and the shim had restored Move's). */
        paintLeds(force) {
            if (force) painted.clear();
            const send = (note, col) => {
                if (painted.get(note) === col) return;
                if (io.setLed && io.setLed(note, col) !== false) painted.set(note, col);
            };
            const s = st(), sc = scn();
            for (let k = 0; k < SCENE_COUNT; k++) {
                const p = pairOf(k);
                send(NOTE_STEP_FIRST + k, k === sc.active ? COLORS.stepActive
                                        : (p[0] >= 0 || p[1] >= 0) ? COLORS.stepPaired : COLORS.stepEmpty);
            }
            const ap = sc.active >= 0 ? pairOf(sc.active) : [-1, -1];
            for (const side of ["a", "b"]) {
                const c = COLORS[side];
                const used = side === "a" ? ap[0] : ap[1];
                for (let i = 0; i < SNAP_COUNT; i++) {
                    const h = snapHalf(side, i);
                    const col = s.edit === h ? COLORS.edit
                              : i === used ? c.inScene
                              : halfLocks(h) > 0 ? c.locked : c.empty;
                    send(snapPad(side, i), col);
                }
            }
        },

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
                if (d1 === CC_COPY) { copyHeld = d2 > 0; if (!copyHeld) copySource = null; return true; }
                if (d1 === CC_DELETE) { deleteHeld = d2 > 0; return true; }
                if (d1 === CC_UNDO) { if (d2 > 0) doUndo(); return true; }
                return false;
            }
            if (type !== 0x90 && type !== 0x80) return false;
            const on = type === 0x90 && d2 > 0;
            if (d1 >= NOTE_STEP_FIRST && d1 < NOTE_STEP_FIRST + SCENE_COUNT) {
                if (on) stepPress(d1 - NOTE_STEP_FIRST);
                this.paintLeds();
                return true;
            }
            const ps = padSnap(d1);
            if (!ps) return false;
            if (on) {
                pressAt.set(d1, now());
                holdFired.set(d1, false);
                if (copyHeld || deleteHeld) { holdFired.set(d1, true); padTap(ps.side, ps.i); }
            } else {
                const was = pressAt.get(d1);
                pressAt.delete(d1);
                if (was && !holdFired.get(d1)) padTap(ps.side, ps.i);
                holdFired.delete(d1);
            }
            this.paintLeds();
            return true;
        },

        /*
         * ctx: { fillRect, print, textWidth, drawHeader(title, right, inverted),
         *        drawFooter(hints) }. 128x64, footer band from y 55.
         */
        draw(ctx) {
            const s = st(), sc = scn();
            const k = sc.active;
            const editing = s.edit >= 0;
            const ap = k >= 0 ? pairOf(k) : [-1, -1];
            const pairText = (ap[0] >= 0 ? snapName("a", ap[0]) : "A-") + " " +
                             (ap[1] >= 0 ? snapName("b", ap[1]) : "B-");
            ctx.drawHeader(k >= 0 ? "Scene " + (k + 1) : "Scenes",
                           editing ? "Edit " + editLabel(s.edit) : (k >= 0 ? pairText : ""), editing);

            /* THE PADS, EXACTLY AS THEY SIT UNDER YOUR HANDS: four rows of
             * eight, A1-A16 on the top two, B1-B16 on the bottom two.
             * Filled = the snapshot holds locks; outline = empty; a centre
             * dot = the active scene uses it; box-in-box = being edited. */
            const gx = 9, pitch = 9, cw = 8, chh = 6;
            const rowY = [12, 19, 28, 35];
            ctx.print(1, 15, "A", 1);
            ctx.print(1, 31, "B", 1);
            for (const side of ["a", "b"]) {
                const used = side === "a" ? ap[0] : ap[1];
                for (let i = 0; i < SNAP_COUNT; i++) {
                    const r = (side === "a" ? 0 : 2) + (i >= 8 ? 1 : 0);
                    const x = gx + (i % 8) * pitch, y = rowY[r];
                    const h = snapHalf(side, i);
                    const has = halfLocks(h) > 0;
                    if (has) ctx.fillRect(x, y, cw, chh, 1);
                    else {
                        ctx.fillRect(x, y, cw, 1, 1); ctx.fillRect(x, y + chh - 1, cw, 1, 1);
                        ctx.fillRect(x, y, 1, chh, 1); ctx.fillRect(x + cw - 1, y, 1, chh, 1);
                    }
                    if (i === used) ctx.fillRect(x + 3, y + 2, 2, 2, has ? 0 : 1);
                    if (s.edit === h) {
                        ctx.fillRect(x + 1, y + 1, cw - 2, chh - 2, 0);
                        ctx.fillRect(x + 2, y + 2, cw - 4, chh - 4, 1);
                    }
                }
            }

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
            const pct = Math.round(Math.max(0, Math.min(1, s.xfade)) * 100) + "%";
            ctx.print(127 - ctx.textWidth(pct), fy, pct, 1);

            if (learnPending) ctx.drawFooter(["Move a fader..."]);
            else if (copyHeld) ctx.drawFooter([copySource ? "Copy: pick dest" : "Copy: pick source"]);
            else if (deleteHeld) ctx.drawFooter(["Clear: step or pad"]);
            else if (editing) ctx.drawFooter(["Tap pad: done", "Del+knob: off"]);
            else ctx.drawFooter(["Pad: pair", "Hold: edit"]);
        },
    };
}

/* THE EDIT BADGE: an inverted "A3" / "B12" over the top-right corner of EVERY
 * screen while a snapshot is being edited; FULL / N/A flash in its place when
 * a knob's write could not be taken. */
export const SCENE_FLASH_FULL = 1;
export const SCENE_FLASH_NA = 2;

export function armBadgeText(edit, flash) {
    if (flash === SCENE_FLASH_FULL) return "FULL";
    if (flash === SCENE_FLASH_NA) return "N/A";
    const l = editLabel(edit);
    return l ? "EDIT " + l : "";
}

export function drawArmBadge(ctx, text) {
    if (!text) return null;
    const w = ctx.textWidth(text) + 3;
    const x = 128 - w, y = 0, h = 9;
    ctx.fillRect(x - 1, y, w + 1, h + 1, 0);
    ctx.fillRect(x, y, w, h, 1);
    ctx.print(x + 2, y + 1, text, 0);
    return { x, y, w, h };
}
