/*
 * step_menu_card.mjs -- the step menu, drawn over Move's screen.
 *
 * Hold one step, press Menu (src/host/step_menu.c owns the gesture and fills
 * the state; this only draws it). Three fields -- Chance, Length, Velocity --
 * and, whichever is focused, the whole displayed page's chance as a row of
 * 16 bars, so "which steps have a condition, and how much" is answered by one
 * look rather than by holding each step in turn.
 *
 *   ┌──────────────────────────────┐
 *   │█ Chance ███████████ Step 5 ██│   band: focused field, held step
 *   │  ▪ ▫ ▫          50%          │   field dots, the value
 *   │ ▌ ▌ █ ▌   ▌ ░ ▌ ▌  ▌  ▌ ▌ ▌ ▌│   16 bars: a % is a height, A:B hollow
 *   │         ▀▀                    │   marker under the held step
 *   └──────────────────────────────┘
 *
 * A bar's height is its percentage; an A:B condition has no height to give,
 * so it draws HOLLOW and full -- "sometimes, by rule" -- beside the solid
 * "sometimes, by chance". A step with a note and no condition is a full solid
 * bar (100%). A step with no note is a single baseline pixel; a button that is
 * no step at all (a triplet grid's dead fourth, or past the clip) is blank.
 *
 * Pure: takes a draw context, like the rest of the card family, so the
 * harness can render it and a test can read its pixels.
 */
import {
    SCREEN_WIDTH, SCREEN_HEIGHT, CARD_W, INSET, BAND_H, GAP_W, LINE_H, GUTTER,
    deviceCtx, contentW, drawCardFrame, drawCardBand,
} from './overlay_card.mjs';

export const SM_CELL_EMPTY = 255;
export const SM_CELL_OFF = 254;
export const SM_FIELD_NAMES = ["Chance", "Length", "Velocity"];
/* The track's slot has no Schwung synth (SM_FLAG_NO_SYNTH). */
export const SM_FLAG_NO_SYNTH = 0x01;

/* Chance on a track with no Schwung synth does nothing -- it gates notes into
 * Schwung's instrument, never Move's -- so the card SAYS so rather than
 * showing a value the jog cannot move. Length and Velocity are Move's and
 * work either way. */
export function chanceUnavailable(state) {
    return state.field === 0 && !!(state.flags & SM_FLAG_NO_SYNTH);
}

/* Mirrors src/host/step_chance.h -- test_step_menu_card.sh pins the two. */
const PERCENT_LADDER = [99, 98, 96, 94, 91, 87, 81, 75, 67, 59, 50, 41, 33, 25, 19, 13, 9, 6, 4, 3, 1];
const MAX_B = 8;

export function condCount() {
    return 1 + PERCENT_LADDER.length + (MAX_B * (MAX_B + 1) / 2 - 1);
}

/** { percent } or { a, b } for a condition index; out of range is 100%. */
export function condInfo(idx) {
    if (!(idx >= 1 && idx < condCount())) return { percent: 100 };
    if (idx <= PERCENT_LADDER.length) return { percent: PERCENT_LADDER[idx - 1] };
    let k = idx - (PERCENT_LADDER.length + 1);
    for (let b = 2; b <= MAX_B; b++) {
        if (k < b) return { a: k + 1, b };
        k -= b;
    }
    return { percent: 100 };
}

export function condName(idx) {
    const c = condInfo(idx);
    return c.b ? `${c.a}:${c.b}` : `${c.percent}%`;
}

/** The focused field's value, as Move would print it. */
export function fieldValue(state) {
    if (chanceUnavailable(state)) return "Move only";
    if (state.cond === SM_CELL_EMPTY) return "No note";
    /* A chord is a RANGE, as Move prints it ("2.0-16.0"): Move's hold-step
     * edit moves every note on the step and clamps each on its own. */
    const len = (c) => (c / 100).toFixed(2).replace(/0$/, "");
    if (state.field === 1) {
        const hi = state.lenMaxC || state.lenC;
        return hi !== state.lenC ? len(state.lenC) + "-" + len(hi) : len(state.lenC);
    }
    if (state.field === 2) {
        const hi = state.velMax || state.vel;
        return hi !== state.vel ? state.vel + "-" + hi : String(state.vel);
    }
    return condName(state.cond);
}

const BAR_W = 6;
const BAR_PITCH = 7;
const BAR_H = 14;
const MARK_H = 2;

export function drawStepMenuCard(ctx, state) {
    const c = ctx || deviceCtx();
    const w = CARD_W;
    const x = Math.floor((SCREEN_WIDTH - w) / 2);
    const h = INSET * 2 + BAND_H + GAP_W + LINE_H + BAR_H + 1 + MARK_H + 1;
    const y = Math.floor((SCREEN_HEIGHT - h) / 2);
    const r = { x, y, w, h };
    drawCardFrame(c, r);

    const cx = x + INSET, cw = contentW(w);
    let cy = y + INSET;
    drawCardBand(c, cx, cy, cw, SM_FIELD_NAMES[state.field] || "", "Step " + (state.step + 1));
    cy += BAND_H + GAP_W;

    /* Which of the three fields: a square for the focused one, a pixel for the
     * others. A hollow 3x3 against a filled 3x3 is 8 pixels against 9 -- the
     * first draft, and indistinguishable on the panel. */
    for (let i = 0; i < 3; i++) {
        const dx = cx + 2 + i * 6;
        if (i === state.field) c.fillRect(dx, cy + 2, 4, 4, 1);
        else c.fillRect(dx + 1, cy + 3, 2, 2, 1);
    }
    const val = fieldValue(state);
    const vw = c.textWidth(val);
    c.print(cx + Math.floor((cw - vw) / 2), cy, val, 1);
    cy += LINE_H;

    if (chanceUnavailable(state)) {
        const msg = "Slot " + ((state.track || 0) + 1) + " has no synth";
        const mw = c.textWidth(msg);
        c.print(cx + Math.floor((cw - mw) / 2), cy + 3, msg, 1);
        const gx0 = Math.max(0, x - GUTTER), gy0 = Math.max(0, y - GUTTER);
        return { ...r, blit: { x: gx0, y: gy0,
            w: Math.min(SCREEN_WIDTH - gx0, w + (x - gx0) + GUTTER),
            h: Math.min(SCREEN_HEIGHT - gy0, h + (y - gy0) + GUTTER) } };
    }
    const rowW = 16 * BAR_PITCH - 1;
    const bx0 = cx + Math.floor((cw - rowW) / 2);
    const base = cy + BAR_H - 1;
    for (let b = 0; b < 16; b++) {
        const cell = state.page ? state.page[b] : SM_CELL_OFF;
        const bx = bx0 + b * BAR_PITCH;
        if (cell === SM_CELL_OFF) continue;
        if (cell === SM_CELL_EMPTY) { c.fillRect(bx + 2, base, 2, 1, 1); continue; }
        const ci = condInfo(cell);
        if (ci.b) {
            /* hollow, full height */
            c.fillRect(bx, cy, BAR_W, 1, 1);
            c.fillRect(bx, base, BAR_W, 1, 1);
            c.fillRect(bx, cy, 1, BAR_H, 1);
            c.fillRect(bx + BAR_W - 1, cy, 1, BAR_H, 1);
        } else {
            const bh = Math.max(1, Math.round(BAR_H * ci.percent / 100));
            c.fillRect(bx, base - bh + 1, BAR_W, bh, 1);
        }
    }
    /* the held step */
    c.fillRect(bx0 + state.step * BAR_PITCH, base + 2, BAR_W, MARK_H, 1);

    const gx = Math.max(0, x - GUTTER), gy = Math.max(0, y - GUTTER);
    return {
        ...r,
        blit: {
            x: gx, y: gy,
            w: Math.min(SCREEN_WIDTH - gx, w + (x - gx) + GUTTER),
            h: Math.min(SCREEN_HEIGHT - gy, h + (y - gy) + GUTTER),
        },
    };
}
