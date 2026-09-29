/*
 * scenes_screen.mjs -- the SCENES screen.
 *
 * A SCENE IS AN A/B PAIR (scene_doc.mjs): the fader morphs the ACTIVE scene
 * from its A to its B. Each side can be switched off -- that end is then "the
 * knobs as they are" -- and a side with no locks means the same thing.
 *
 *   steps 1-16           pick the active scene
 *   top half of pads     the active scene's A:  tap = on/off,  hold = EDIT A
 *   bottom half of pads  the active scene's B:  tap = on/off,  hold = EDIT B
 *   jog / knob 8         the fader (1/64 per detent, Shift 1/256)
 *   jog click            snap the fader to the nearer end
 *   Shift+jog click      learn an external CC for the fader (CC Map)
 *   Copy + 2 steps       copy a scene (source, destination)
 *   Delete + step        clear a scene;  Delete + pad: clear that side
 *   Undo                 undo the last copy or clear (one level; again = redo)
 *
 * EDITING IS LATCHED: it outlasts the screen, because the knobs being locked
 * are on other screens. A tap on the side being edited (or Shift+Vol+Up/Down
 * again, from anywhere) stops it. The side being edited plays at 100% whatever
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

/** A pad note -> "a" (top two rows) or "b" (bottom two), or null. */
export function padSide(note) {
    if (note >= 84 && note <= 99) return "a";
    if (note >= 68 && note <= 83) return "b";
    return null;
}
export const PADS_A = Array.from({ length: 16 }, (_, i) => 84 + i);
export const PADS_B = Array.from({ length: 16 }, (_, i) => 68 + i);

/* Palette indices (constants.mjs). */
export const COLORS = {
    a: { on: 16, onEmpty: 95, off: 96 },      /* AzureBlue / DarkAzure / VeryDarkAzure */
    b: { on: 3, onEmpty: 72, off: 68 },       /* BrightOrange / DarkOrange / VeryDarkOrangeRed */
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

    function padTap(side) {
        const k = scn().active;
        if (k < 0) { io.announce("Pick a scene with the steps first"); return; }
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

        /* Called by the host for Shift+Vol+Up / Down. */
        toggleEdit,

        tick() {
            const t = now();
            let changed = false;
            for (const [note, at] of pressAt) {
                const side = padSide(note);
                if (side && at && !holdFired.get(note) && t - at >= HOLD_MS && !copyHeld && !deleteHeld) {
                    holdFired.set(note, true);
                    toggleEdit(side);
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
                let col = 0;
                if (sc.active >= 0) {
                    const h = halfOf(sc.active, side);
                    const c = COLORS[side];
                    col = s.edit === h ? COLORS.edit
                        : !sideOn(sc.active, side) ? c.off
                        : halfLocks(h) > 0 ? c.on : c.onEmpty;
                }
                for (const note of side === "a" ? PADS_A : PADS_B) send(note, col);
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
            const side = padSide(d1);
            if (!side) return false;
            if (on) {
                pressAt.set(d1, now());
                holdFired.set(d1, false);
                if (deleteHeld) { holdFired.set(d1, true); padTap(side); }
            } else {
                const was = pressAt.get(d1);
                pressAt.delete(d1);
                if (was && !holdFired.get(d1)) padTap(side);
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

            /* The 16 scenes, as the steps: the active one boxed with a dot,
             * a scene holding locks filled. */
            const y0 = 13, pitch = 8, cw = 6, ch = 6;
            for (let i = 0; i < SCENE_COUNT; i++) {
                const x = 1 + i * pitch;
                const locked = halfLocks(halfA(i)) + halfLocks(halfB(i)) > 0;
                if (locked || i === k) ctx.fillRect(x, y0, cw, ch, 1);
                else {
                    ctx.fillRect(x, y0, cw, 1, 1); ctx.fillRect(x, y0 + ch - 1, cw, 1, 1);
                    ctx.fillRect(x, y0, 1, ch, 1); ctx.fillRect(x + cw - 1, y0, 1, ch, 1);
                }
                if (i === k) ctx.fillRect(x + 2, y0 + 2, cw - 4, ch - 4, 0);
            }

            /* The active scene's two sides. The side being edited is drawn
             * inverted -- that is where knob turns are going. */
            const drawSide = (side, y) => {
                const h = k >= 0 ? halfOf(k, side) : -1;
                const isEdit = h >= 0 && s.edit === h;
                const on = k >= 0 && sideOn(k, side);
                const n = h >= 0 ? halfLocks(h) : 0;
                const text = side.toUpperCase() + "  " + (k < 0 ? "-" : !on ? "off"
                           : n ? n + (n === 1 ? " lock" : " locks") : "knobs");
                if (isEdit) {
                    ctx.fillRect(0, y - 1, 128, 9, 1);
                    ctx.print(2, y, text, 0);
                    ctx.print(127 - ctx.textWidth("editing"), y, "editing", 0);
                } else {
                    ctx.print(2, y, text, 1);
                }
            };
            drawSide("a", 23);
            drawSide("b", 33);

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
            else ctx.drawFooter(["Step: scene", "Hold pad: edit"]);
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
