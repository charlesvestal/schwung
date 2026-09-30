/*
 * footer_panel.mjs -- a transient band that SLIDES UP over the footer, and the
 * one place its motion is decided.
 *
 * Two things use it: the automation lock map (hold a step) and the scene
 * fader's A-B slider (move the fader off the Scenes screen). They are the same
 * gesture -- "something you are doing right now, shown in the cheapest rows on
 * the screen" -- so they must move the same way.
 *
 * WHY THE FOOTER. The header says where you are and the body is what you are
 * working on; the footer names gestures you already have your hands on, so for
 * the length of the gesture it is the cheapest nine rows on the screen.
 *
 * WHY IT SLIDES. Appearing and disappearing in place over an existing band
 * reads as a glitch; the motion is what says "this replaced the footer and the
 * footer is coming back". The panel keeps its last payload on the way OUT --
 * whatever produced it is usually gone the instant the gesture ends, and a
 * panel that vanishes mid-slide is the glitch the slide exists to avoid.
 *
 * Pure: the clock is handed in on every call, so a test can drive it.
 */
import { RULE_Y, FOOTER_Y, FOOTER_H } from "./list_geometry.mjs";

export const PANEL_BOTTOM = FOOTER_Y + FOOTER_H;   /* 64 -- the last row the footer owns */
export const PANEL_TOP = RULE_Y;                    /* the rule */
export const PANEL_H = PANEL_BOTTOM - PANEL_TOP;    /* 9 -- the rule and the footer */
export const PANEL_ANIM_MS = 110;

export function createFooterPanel({ animMs = PANEL_ANIM_MS } = {}) {
    let anim = null;                                /* { open, since, payload } */

    /*
     * Once per frame. `want` is the payload to show, or null for "not now".
     * Returns null when nothing is on screen, else { y, payload }: y is the
     * panel's top row this frame (PANEL_TOP when fully up).
     */
    function update(t, want) {
        if (want) {
            if (!anim || !anim.open) {
                /* Re-opening mid-close continues from where it is, not from
                 * the edge -- a gesture repeated during the slide-out must not
                 * make the panel drop and rise again. */
                let since = t;
                if (anim) {
                    const p = Math.min(1, Math.max(0, (t - anim.since) / animMs));
                    since = t - (1 - p) * animMs;
                }
                anim = { open: true, since, payload: want };
            } else {
                anim.payload = want;
            }
        } else if (anim && anim.open) {
            anim = { open: false, since: t, payload: anim.payload };
        }
        if (!anim) return null;
        let p = (t - anim.since) / animMs;
        if (!(p >= 0)) p = 0;
        if (p > 1) p = 1;
        if (!anim.open && p >= 1) { anim = null; return null; }
        /* Ease out on the way in (fast off the edge, settling onto the rule),
         * ease in on the way out. */
        const e = anim.open ? 1 - (1 - p) * (1 - p) : p * p;
        const off = Math.round((anim.open ? 1 - e : e) * PANEL_H);
        return { y: PANEL_TOP + off, payload: anim.payload };
    }

    /* Drop it at once, no slide: the screen underneath now shows the same
     * thing itself. */
    function hide() { anim = null; }

    /* On screen or moving: the caller must keep redrawing. */
    function busy() { return anim !== null; }

    return { update, hide, busy };
}

/*
 * Blank exactly the rows the panel covers, never the whole band -- the footer
 * is already in the framebuffer, so clearing only under the panel lets it be
 * covered on the way in and UNCOVERED row by row on the way out -- and draw
 * the panel's top rule. Returns the rect touched (for a blit onto Move's own
 * screen). ctx: { fillRect(x, y, w, h, c) }.
 */
export function beginFooterPanel(ctx, y, width = 128) {
    const h = PANEL_BOTTOM - y;
    ctx.fillRect(0, y, width, h, 0);
    ctx.fillRect(0, y, width, 1, 1);
    return { x: 0, y, w: width, h };
}
