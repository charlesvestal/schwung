/*
 * scenes_screen.mjs -- the SCENES screen.
 *
 * A SCENE IS AN A/B PAIR (scene_doc.mjs): the fader morphs the ACTIVE scene
 * from its A to its B. Each side can be switched off -- that end is then "the
 * knobs as they are" -- and a side with no locks means the same thing.
 *
 *   top two pad rows     the 16 scenes' A sides (pad k = scene k's A)
 *   bottom two pad rows  the 16 scenes' B sides
 *     tap                another scene's pad: make that scene active;
 *                        the active scene's pad: that side on / off
 *     hold               make it active and EDIT that side
 *   steps 1-16           pick the active scene too
 *   jog / knob 8         the fader (1/64 per detent, Shift 1/256)
 *   jog click            snap the fader to the nearer end
 *   Shift+jog click      learn an external CC for the fader (CC Map)
 *   Copy + 2 steps       copy a scene (source, destination)
 *   Delete + step        clear a scene;  Delete + pad: clear that side
 *   Undo                 undo the last copy or clear (one level; again = redo)
 *
 * EDITING IS LATCHED: it outlasts the screen, because the knobs being locked
 * are on other screens. A tap on the side being edited (or Shift+Up/Down
 * again, from any Schwung screen) stops it. The side being edited plays at 100% whatever
 * the fader says, so what you hear is what you are building.
 *
 * The steps and pads are taken ONLY while this screen is up (the host restates
 * the claims every tick); Shift+step is never taken -- Move's Shift+step pages
 * stay reachable.
 *
 * PURE except through `io` and the draw context, so tests/host drive it
 * against the harness framebuffer.
 */

import { SCENE_COUNT, halfA, halfB } from "./scene_doc.mjs";

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

/** A pad note -> { side: "a" | "b", k: scene 0..15 }, or null. The A rows
 *  are the top two, the B rows the bottom two; pad k is scene k in both. */
export function padHalf(note) {
    for (let r = 0; r < 4; r++) {
        const first = PAD_ROWS[r];
        if (note >= first && note < first + 8)
            return { side: r < 2 ? "a" : "b", k: (r % 2) * 8 + (note - first) };
    }
    return null;
}

/** The pad that shows scene k's side. */
export function halfPad(side, k) {
    return PAD_ROWS[(side === "a" ? 0 : 2) + (k >= 8 ? 1 : 0)] + (k % 8);
}

/* Palette indices (constants.mjs). */
export const COLORS = {
    /* active = the active scene's side (bright), locked = another scene's
     * side holding locks, empty = on with nothing locked, off = switched off */
    a: { active: 16, locked: 95, empty: 96, off: 0 },   /* AzureBlue / DarkAzure / VeryDarkAzure */
    b: { active: 3, locked: 72, empty: 68, off: 0 },    /* BrightOrange / DarkOrange / VeryDarkOrangeRed */
    edit: 120,                                /* White */
    stepActive: 120,                          /* White */
    stepLocked: 118,                          /* LightGrey */
    stepEmpty: 0,
};

/** The badge the host draws on every screen while a side is being edited. */
export function editLabel(editHalf) {
    if (!(editHalf >= 0)) return "";
    return "S" + (Math.floor(editHalf / 2) + 1) + " " + (editHalf % 2 ? "B" : "A");
}

export function createScenesScreen(io) {
    const now = io.now || (() => Date.now());
    const pressAt = new Map();           /* note -> press time */
    const holdFired = new Map();
    const painted = new Map();           /* note -> colour last sent */
    let copyHeld = false, deleteHeld = false, copySource = -1;
    let counts = null, countsRev = -1;   /* locks per HALF */
    let undo = null;
    let lastFaderAnnounce = 0;
    let learnPending = false;

    const st = () => io.state() || { edit: -1, xfade: 0, rev: 0, flash: 0 };
    const scn = () => io.scene() || { active: -1, enables: [] };
    const sideOn = (k, side) => {
        const e = scn().enables[k];
        return !e || e[side === "a" ? 0 : 1] !== false;
    };
    const halfOf = (k, side) => (side === "a" ? halfA(k) : halfB(k));
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
        if (st().edit >= 0 && k !== scn().active) io.setEdit(-1);
        io.setActive(k);
        io.announce("Scene " + (k + 1));
    }

    function stepPress(k) {
        if (copyHeld) {
            if (copySource < 0) {
                copySource = k;
                io.announce("Copy scene " + (k + 1) + ", pick destination");
            } else {
                const src = copySource;
                copySource = -1;
                if (src === k) { io.announce("Copy cancelled"); return; }
                if (destructive("copy", () =>
                        io.applyAll("copy", halfA(src) + " " + halfA(k)) &&
                        io.applyAll("copy", halfB(src) + " " + halfB(k)))) {
                    if (io.copyEnables) io.copyEnables(src, k);
                    io.announce("Scene " + (src + 1) + " copied to " + (k + 1));
                }
            }
            return;
        }
        if (deleteHeld) {
            if (destructive("clear", () =>
                    io.applyAll("clear", String(halfA(k))) && io.applyAll("clear", String(halfB(k)))))
                io.announce("Scene " + (k + 1) + " cleared");
            return;
        }
        selectScene(k);
    }

    /* Start or stop editing one side of the active scene. Starting switches
     * the side on -- editing a side you cannot hear would be pointless. */
    function toggleEdit(side) {
        let k = scn().active;
        if (k < 0) { k = 0; io.setActive(0); }
        const h = halfOf(k, side);
        if (st().edit === h) {
            io.setEdit(-1);
            io.announce("Done editing scene " + (k + 1) + " " + side.toUpperCase());
            return;
        }
        if (!sideOn(k, side)) io.setEnable(k, side, true);
        io.setEdit(h);
        io.announce("Editing scene " + (k + 1) + " " + side.toUpperCase() +
                    ". Turn knobs on any page to lock them.");
    }

    function padTap(side, k) {
        if (k !== scn().active && !deleteHeld) { selectScene(k); return; }
        if (deleteHeld) {
            if (destructive("clear " + side.toUpperCase(), () => io.applyAll("clear", String(halfOf(k, side)))))
                io.announce("Scene " + (k + 1) + " " + side.toUpperCase() + " cleared");
            return;
        }
        if (st().edit === halfOf(k, side)) { toggleEdit(side); return; }
        const on = !sideOn(k, side);
        io.setEnable(k, side, on);
        io.announce("Scene " + (k + 1) + " " + side.toUpperCase() + (on ? " on" : " off"));
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
            const k = scn().active;
            io.announce(k >= 0 ? "Scenes. Scene " + (k + 1) : "Scenes. Pick a scene with the steps.");
        },

        /* Called by the host for Shift+Up / Down. */
        toggleEdit,

        tick() {
            const t = now();
            let changed = false;
            for (const [note, at] of pressAt) {
                const ph = padHalf(note);
                if (ph && at && !holdFired.get(note) && t - at >= HOLD_MS && !copyHeld && !deleteHeld) {
                    holdFired.set(note, true);
                    /* Hold selects too: the pad names its scene. */
                    if (ph.k !== scn().active) selectScene(ph.k);
                    toggleEdit(ph.side);
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
                const locked = halfLocks(halfA(k)) + halfLocks(halfB(k)) > 0;
                send(NOTE_STEP_FIRST + k, k === sc.active ? COLORS.stepActive
                                        : locked ? COLORS.stepLocked : COLORS.stepEmpty);
            }
            for (const side of ["a", "b"]) {
                const c = COLORS[side];
                for (let k = 0; k < SCENE_COUNT; k++) {
                    const h = halfOf(k, side);
                    const col = s.edit === h ? COLORS.edit
                              : !sideOn(k, side) ? c.off
                              : k === sc.active ? c.active
                              : halfLocks(h) > 0 ? c.locked : c.empty;
                    send(halfPad(side, k), col);
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
                if (d1 === CC_COPY) { copyHeld = d2 > 0; if (!copyHeld) copySource = -1; return true; }
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
            const ph = padHalf(d1);
            if (!ph) return false;
            if (on) {
                pressAt.set(d1, now());
                holdFired.set(d1, false);
                if (deleteHeld) { holdFired.set(d1, true); padTap(ph.side, ph.k); }
            } else {
                const was = pressAt.get(d1);
                pressAt.delete(d1);
                if (was && !holdFired.get(d1)) padTap(ph.side, ph.k);
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
            ctx.drawHeader(k >= 0 ? "Scene " + (k + 1) : "Scenes",
                           editing ? "Edit " + (s.edit % 2 ? "B" : "A") : "", editing);

            /* THE PADS, EXACTLY AS THEY SIT UNDER YOUR HANDS: four rows of
             * eight, the scenes' A sides on the top two, B on the bottom two
             * (pad k = scene k in both). Outline = on, nothing locked (the
             * knobs); filled = on, with locks; dotted outline = switched off;
             * a centre dot = the active scene; box-in-box = being edited. */
            const gx = 9, pitch = 9, cw = 8, chh = 6;
            const rowY = [12, 19, 28, 35];
            ctx.print(1, 15, "A", 1);
            ctx.print(1, 31, "B", 1);
            for (const side of ["a", "b"]) {
                for (let n = 0; n < SCENE_COUNT; n++) {
                    const r = (side === "a" ? 0 : 2) + (n >= 8 ? 1 : 0);
                    const x = gx + (n % 8) * pitch, y = rowY[r];
                    const h = halfOf(n, side);
                    const on = sideOn(n, side);
                    const has = halfLocks(h) > 0;
                    if (on && has) ctx.fillRect(x, y, cw, chh, 1);
                    else if (on) {
                        ctx.fillRect(x, y, cw, 1, 1); ctx.fillRect(x, y + chh - 1, cw, 1, 1);
                        ctx.fillRect(x, y, 1, chh, 1); ctx.fillRect(x + cw - 1, y, 1, chh, 1);
                    } else {
                        /* OFF: a DOTTED outline, strictly alternating all the
                         * way round -- the 8x6 border is 24 pixels, so the
                         * pattern closes with no two dots (or gaps) touching
                         * at a corner. */
                        let i = 0;
                        const dot = (px, py) => { if ((i++ & 1) === 0) ctx.fillRect(px, py, 1, 1, 1); };
                        for (let d = 0; d < cw - 1; d++) dot(x + d, y);
                        for (let d = 0; d < chh - 1; d++) dot(x + cw - 1, y + d);
                        for (let d = cw - 1; d > 0; d--) dot(x + d, y + chh - 1);
                        for (let d = chh - 1; d > 0; d--) dot(x, y + d);
                    }
                    /* The active scene: a dot in the middle (cut out of a filled cell). */
                    if (n === k) ctx.fillRect(x + 3, y + 2, 2, 2, on && has ? 0 : 1);
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
            else if (copyHeld) ctx.drawFooter([copySource < 0 ? "Copy: pick source" : "Copy: pick dest"]);
            else if (deleteHeld) ctx.drawFooter(["Clear: step or pad"]);
            else if (editing) ctx.drawFooter(["Tap pad: done", "Del+knob: off"]);
            else ctx.drawFooter(["Pad: pick/on", "Hold: edit"]);
        },
    };
}

/* THE EDIT BADGE: an inverted "S3 A" over the top-right corner of EVERY
 * screen while a side is being edited; FULL / N/A flash in its place when a
 * knob's write could not be taken. */
export const SCENE_FLASH_FULL = 1;
export const SCENE_FLASH_NA = 2;

export function armBadgeText(edit, flash) {
    if (flash === SCENE_FLASH_FULL) return "FULL";
    if (flash === SCENE_FLASH_NA) return "N/A";
    return editLabel(edit);
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
