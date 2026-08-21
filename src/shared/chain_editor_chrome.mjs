/**
 * chain_editor_chrome.mjs — the bands around a chain editor's row of boxes.
 *
 * There are two chain editors: the slot chain (drawChainEdit in shadow_ui.js)
 * and Master FX (drawMasterFx in shadow_ui_master_fx.mjs). Step 4a-2 of the
 * Master FX variable-length design converged their PLUMBING — one chain target,
 * one LFO map, one bypass read — but deliberately left the CHROME alone so that
 * every pixel that moved in that step would be a bug. This module is 4a-3, the
 * other half: the two screens now wear the same furniture because they draw it
 * with the same code.
 *
 * What was there before, so a future reader does not have to guess whether the
 * difference was deliberate: Master FX drew the OLDER header (menu_layout's
 * drawMenuHeader — device 5x7 font plus a horizontal rule, ~18 rows), no footer
 * at all, and sat its boxes 6px lower (y=20) with the label and info band one
 * row further apart again. The movy header band is what every other screen
 * adopted, and a screen with no hints is a screen whose gestures are not
 * discoverable — neither difference was a design decision, they were just the
 * order the screens were written in.
 *
 * The MIDDLE of the two screens is genuinely different and stays that way: the
 * slot editor owns a four-slot indicator column at x 0..3 and offsets its strip
 * to clear it, Master FX has no such column and centres instead. So this shares
 * the bands, not the whole draw.
 *
 * Pure, like every other renderer in this directory: it draws through the
 * injected ctx (`fillRect` / `print`), reads no parameters and owns no state,
 * so it can be rendered into tools/param-pages/harness.mjs and inspected pixel
 * by pixel (tests/host/test_chain_editor_snapshot.sh).
 */

import { drawHeader, drawFooter } from "./param_pages/render_page_movy.mjs";
import { DEFAULT_Y as DIAGRAM_Y, BOX_H as DIAGRAM_BOX_H } from "./chain_diagram.mjs";
import { SCREEN_WIDTH, truncateText } from "./chain_ui_views.mjs";

/**
 * The column, as chain_diagram.mjs's DEFAULT_Y comment states it:
 *   0-6 header | 8-11 marks | 14-29 boxes | 33-39 label | 44-50 info
 *   55 rule | 56-63 footer
 * Derived from the diagram's own geometry rather than restated, because a
 * second copy of "the boxes are 16 tall and start at 14" is how the two
 * editors came to sit 6px apart in the first place.
 */
export const LABEL_Y = DIAGRAM_Y + DIAGRAM_BOX_H + 3;
export const INFO_Y = LABEL_Y + 11;

/** The info line is the widest thing under the boxes; 24 device glyphs at a
 *  5px advance is 120 of the 128 columns. */
export const INFO_MAX_CHARS = 24;

/** Centred on the device's fixed 5px advance — the same arithmetic both
 *  editors did inline. */
function centreX(text) {
    return Math.floor((SCREEN_WIDTH - String(text).length * 5) / 2);
}

/**
 * Header band, the two centred text lines under the boxes, and the hint footer.
 *
 * Drawn in one call because none of the four bands overlaps the diagram or the
 * slot indicators, so the order the caller draws its middle in cannot matter —
 * and a single call is what makes it impossible for one editor to quietly grow
 * a band the other does not have.
 *
 * @param ctx  fillRect / print
 * @param o    { headerLeft, headerRight, label, info, hints }
 *             `hints` is [key, action] pairs for drawFooter, which drops the
 *             tail rather than squeezing and pins BACK to the right edge.
 */
export function drawChainEditorBands(ctx, o) {
    drawHeader(ctx, o.headerLeft, o.headerRight, false);

    const label = o.label == null ? "" : String(o.label);
    ctx.print(centreX(label), LABEL_Y, label, 1);

    const info = truncateText(o.info == null ? "" : String(o.info), INFO_MAX_CHARS);
    ctx.print(centreX(info), INFO_Y, info, 1);

    drawFooter(ctx, o.hints);
}
