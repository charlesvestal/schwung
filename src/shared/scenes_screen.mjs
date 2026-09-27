/*
 * scenes_screen.mjs -- the SCENES screen: pick A and B, move the crossfader,
 * arm a scene for editing, copy and clear scenes.
 *
 *   jog / knob 8   the fader (1/64 per detent, Shift 1/256)
 *   jog click      snap the fader to the nearer end
 *   Shift+click    learn an external CC for the fader (CC Map)
 *   knob 1 / 2     choose scene A / scene B (turn past 1 for none)
 *   step TAP       put that scene on the FAR end -- the one the fader is
 *                  heading away from, so the next move is a new transition
 *   step HOLD      arm that scene for editing (hold the armed one to disarm)
 *   Copy + 2 steps copy a scene (source, destination)
 *   Delete + step  clear a scene
 *   Undo           undo the last copy or clear (one level; again = redo)
 *
 * Shift+step is NOT available: the shim hands a Shift+step to Move (it is
 * Move's own shortcut vocabulary and dismisses our screen), so B has its own
 * knob instead.
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
const CC_KNOB1 = 71, CC_KNOB2 = 72, CC_KNOB8 = 78;
const NOTE_STEP_FIRST = 16;

/* Relative encoder value -> signed detents (1..63 up, 65..127 down). */
export function relDelta(v) {
    if (v === 0 || v === 64) return 0;
    return v < 64 ? v : v - 128;
}

export function sceneLabel(n) { return (n >= 0 && n < SCENE_COUNT) ? String(n + 1) : "-"; }

/** The end a tap assigns: the one the fader is FURTHER from (0.5 -> B). */
export function farEnd(xfade) { return xfade > 0.5 ? "a" : "b"; }

/** Step a scene choice by `delta`, through "none" (-1) at the bottom. */
export function stepScene(cur, delta) {
    let v = (cur >= 0 ? cur : -1) + delta;
    if (v < -1) v = -1;
    if (v > SCENE_COUNT - 1) v = SCENE_COUNT - 1;
    return v;
}

export function createScenesScreen(io) {
    const now = io.now || (() => Date.now());
    const pressAt = new Array(SCENE_COUNT).fill(0);   /* 0 = not down */
    const holdFired = new Array(SCENE_COUNT).fill(false);
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

    function stepTap(n) {
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
        setEnd(farEnd(s.xfade), n);
    }

    function stepHold(n) {
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
            pressAt.fill(0);
            holdFired.fill(false);
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
            for (let i = 0; i < SCENE_COUNT; i++) {
                if (pressAt[i] && !holdFired[i] && t - pressAt[i] >= STEP_HOLD_MS && !copyHeld && !deleteHeld) {
                    holdFired[i] = true;
                    stepHold(i);
                    changed = true;
                }
            }
            const before = countsRev;
            refreshCounts(false);
            return changed || countsRev !== before;
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
                if (d1 === CC_KNOB1 || d1 === CC_KNOB2) {
                    const d = relDelta(d2);
                    if (!d) return true;
                    const s = st();
                    const which = d1 === CC_KNOB1 ? "a" : "b";
                    const next = stepScene(which === "a" ? s.a : s.b, d > 0 ? 1 : -1);
                    if (next !== (which === "a" ? s.a : s.b)) setEnd(which, next);
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
            if ((type === 0x90 || type === 0x80) && d1 >= NOTE_STEP_FIRST && d1 < NOTE_STEP_FIRST + SCENE_COUNT) {
                const n = d1 - NOTE_STEP_FIRST;
                if (type === 0x90 && d2 > 0) {
                    pressAt[n] = now();
                    holdFired[n] = false;
                    /* With Copy or Delete held a press is a pick, not a hold:
                     * act on the press so a quick sequence of picks works. */
                    if (copyHeld || deleteHeld) { holdFired[n] = true; stepTap(n); }
                } else {
                    const was = pressAt[n];
                    pressAt[n] = 0;
                    if (was && !holdFired[n]) stepTap(n);
                    holdFired[n] = false;
                }
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

            /* 16 cells, 8 px pitch: filled when the scene holds locks. */
            const cellY = 13, cellH = 7;
            for (let i = 0; i < SCENE_COUNT; i++) {
                const x = i * 8;
                const has = counts ? counts[i] > 0 : false;
                if (has) ctx.fillRect(x + 1, cellY, 6, cellH, 1);
                else {
                    ctx.fillRect(x + 1, cellY, 6, 1, 1);
                    ctx.fillRect(x + 1, cellY + cellH - 1, 6, 1, 1);
                    ctx.fillRect(x + 1, cellY, 1, cellH, 1);
                    ctx.fillRect(x + 6, cellY, 1, cellH, 1);
                }
                if (i === s.edit) {
                    /* the armed scene: a bar ABOVE the cell */
                    ctx.fillRect(x + 1, cellY - 2, 6, 1, 1);
                }
            }
            /* A / B marks under their cells. */
            const markY = cellY + cellH + 2;
            const mark = (n, ch) => {
                if (n < 0) return;
                const w = ctx.textWidth(ch);
                ctx.print(n * 8 + 4 - Math.floor(w / 2), markY, ch, 1);
            };
            if (s.a >= 0 && s.a === s.b) mark(s.a, "*");
            else { mark(s.a, "A"); mark(s.b, "B"); }

            /* The fader: A ▕████░░░░▏ B, the position as a 3 px notch. */
            const fy = 33, fh = 7, fx0 = 9, fx1 = 118;
            ctx.print(1, fy, "A", 1);
            ctx.print(fx1 + 4, fy, "B", 1);
            ctx.fillRect(fx0, fy, fx1 - fx0, 1, 1);
            ctx.fillRect(fx0, fy + fh - 1, fx1 - fx0, 1, 1);
            ctx.fillRect(fx0, fy, 1, fh, 1);
            ctx.fillRect(fx1 - 1, fy, 1, fh, 1);
            const span = fx1 - fx0 - 4;
            const pos = fx0 + 2 + Math.round(Math.max(0, Math.min(1, s.xfade)) * span);
            ctx.fillRect(fx0 + 2, fy + 2, Math.max(0, pos - (fx0 + 2)), fh - 4, 1);
            ctx.fillRect(pos - 1, fy - 2, 3, fh + 4, 1);

            /* Lock counts for the two ends, and the position. */
            const ca = counts && s.a >= 0 ? counts[s.a] : 0;
            const cb = counts && s.b >= 0 ? counts[s.b] : 0;
            const pct = Math.round(Math.max(0, Math.min(1, s.xfade)) * 100) + "%";
            ctx.print(1, 44, "A:" + ca + "  B:" + cb, 1);
            ctx.print(127 - ctx.textWidth(pct), 44, pct, 1);

            if (learnPending) ctx.drawFooter(["Move a fader..."]);
            else if (copyHeld) ctx.drawFooter([copySource < 0 ? "Copy: pick source" : "Copy: pick dest"]);
            else if (deleteHeld) ctx.drawFooter(["Clear: pick scene"]);
            else if (armed) ctx.drawFooter(["Hold: done", "Del+knob: off"]);
            else ctx.drawFooter(["Step: set", "Hold: edit"]);
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
