/**
 * chain_editor_chrome.mjs — the bands around a chain editor's row of boxes, and
 * the module picker it opens on a position.
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

import { drawHeader, drawFooter, RULE_Y as MOVY_RULE_Y }
    from "./param_pages/render_page_movy.mjs";
import { fitText } from "./param_pages/render_page.mjs";
/* The list rect the knob grid`s own menu pages use, AND the row renderer that
 * fills it. Imported rather than restated: the two pickers below must occupy
 * the SAME rectangle and draw the SAME rows, and a second copy of
 * "x 8, y 10, w 112" — or a second row loop — is how they would come to stop
 * doing so. drawPageChromeList is drawMenuList, i.e. the one list every other
 * screen in the shadow UI already draws with. */
import { MENU_LIST_X, MENU_LIST_Y, MENU_LIST_W, drawPageChromeList }
    from "./param_pages/page_controller.mjs";
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

/**
 * Centred by MEASURING, not by counting glyphs.
 *
 * Both editors did `length * 5` inline and this inherited it. The device font
 * is PROPORTIONAL — `I` is 1px and `W` is 5 — so a character count overstates
 * the width of anything with narrow glyphs in it, and the line lands left of
 * centre by half the error. "Configure master FX" is full of them and visibly
 * sat off to the left; a run of capitals happened to look fine, which is why
 * it survived.
 *
 * render_page_movy.mjs already carries the same warning over `fitLine`: never
 * assume N glyphs are N advances wide.
 */
function centreX(ctx, text) {
    return Math.floor((SCREEN_WIDTH - ctx.textWidth(String(text))) / 2);
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
/** The footer at rest. Shift substitutes into these SLOTS, never reflows them. */
export const CHAIN_HINTS_AT_REST = Object.freeze(
    [["JOG", "SEL"], ["CLK", "OPEN"], ["BACK", "EXIT"]]);

/*
 * What Shift offers on the focused chain cell.
 *
 * THE RULE: an action that Shift does not change keeps its PLACE. The footer
 * is read by position -- the eye goes to the middle for the click -- so a pair
 * that slides left because the pair before it vanished reads as a different
 * control. Reported as: "if the same action is in shift and nonshift, dont
 * change its place".
 *
 * So this substitutes into the slots of CHAIN_HINTS_AT_REST rather than
 * building a list per case, and the honest consequence is that Shift on a cell
 * it does nothing to shows exactly the resting footer -- because nothing HAS
 * changed. It used to show a shorter, rearranged one, which announced a change
 * that was not there.
 *
 * Keyed on comp.kind, and the kinds are not what they look like: chain_model
 * only emits a `module` for a position that HOLDS one, so an empty position is
 * the `add` box and never a module with a null module. Reading "empty" as
 * `kind === "module" && !comp.module` produced a branch nothing could reach,
 * which is why no CLK hint appeared on either screen.
 */
export function shiftHintsFor(comp) {
    const kind = comp && comp.kind;

    /*
     * A filled position is the one case that cannot keep all three: Shift
     * changes BOTH the jog and the click, and JOG MOVE / CLK SWAP / BACK EXIT
     * measures one character over the bar -- the resting set only fits three
     * because SEL is three letters. drawFooter drops silently, so the pair is
     * dropped here, deliberately, from the END: the two that moved keep their
     * places and the one that goes is the one Shift does not change.
     */
    if (kind === "module") return [["JOG", "MOVE"], ["CLK", "SWAP"]];

    /*
     * The synth swaps like any module but has no position to move -- there is
     * exactly one of it -- so the jog keeps doing what it did, and says so in
     * the slot it already occupied.
     */
    if (kind === "synth") return [["JOG", "SEL"], ["CLK", "SWAP"], ["BACK", "EXIT"]];

    /*
     * The `+` IS the empty position. Shift+click opens the picker there, which
     * is what a plain click does too, so every word is unchanged -- and by the
     * rule above, so is every position.
     */
    return CHAIN_HINTS_AT_REST;
}

export function drawChainEditorBands(ctx, o) {
    drawHeader(ctx, o.headerLeft, o.headerRight, false);

    const label = o.label == null ? "" : String(o.label);
    ctx.print(centreX(ctx, label), LABEL_Y, label, 1);

    const info = truncateText(o.info == null ? "" : String(o.info), INFO_MAX_CHARS);
    ctx.print(centreX(ctx, info), INFO_Y, info, 1);

    drawFooter(ctx, o.hints);
}

/**
 * The module picker a chain editor opens on one of its positions — for BOTH
 * editors, because there is only one picker.
 *
 * WHY THIS EXISTS. The slot chain and Master FX already chose their rows from
 * the same list: the module scan plus this position`s Move Left / Move Right,
 * built by the same chainMoveEntries. They then drew that identical list two
 * completely different ways — the slot picker in the movy chrome
 * (drawHeader/renderPicker/drawFooter), Master FX in the older menu chrome
 * (drawMenuHeader/drawMenuList/"Back: cancel  Click: apply"). A user moving
 * between the two screens saw two products. That is the drift §1b of the
 * variable-length design exists to end, reported from the device in exactly
 * those words, and shared ENTRIES with unshared DRAWING is the shape it takes
 * when only half a screen is converged.
 *
 * WHY IT WENT UNSEEN. tests/host/test_chain_editor_snapshot.sh wires both
 * pickers to fail-if-reached stubs, so neither screen was ever rendered by the
 * only harness that renders these screens at all. Nothing compared them because
 * nothing drew them. Both are snapshotted now, from the same payload, so a
 * future divergence is a diff between two pictures built to be identical.
 *
 * The parameters are DATA, deliberately — a header line, rows, which row is
 * current, what to say when there are none. No branch in here may ask which
 * editor is calling: a shared function with a `kind === "master"` in it drifts
 * exactly as well as two functions did.
 *
 * @param ctx  fillRect / print / textWidth
 * @param o    { headerLeft, entries, index, currentId, emptyMessage }
 *             `entries` are the picker`s own rows ({id, name}), already
 *             including the Move rows — not the raw module scan.
 */
/*
 * The verb CLK earns on the row under the cursor.
 *
 * Read off the ROW rather than fixed on the screen, because the picker now
 * carries a row that does not load: the list-filter row cycles the filter in
 * place. A fixed "LOAD" there describes a different button than the one the
 * user is about to press.
 */
function pickerClickVerb(o) {
    const row = (o.entries || [])[o.index];
    return (row && row.clickVerb) || "LOAD";
}

export function drawChainPicker(ctx, o) {
    /* Delegates to drawListScreen rather than restating the rect, the
     * MENU_LIST_Y + 8 empty offset, the fitText and the row loop. This file
     * exists to keep those singular (see the header note), and a picker that
     * is "drawListScreen plus a currentId mapping and a fixed footer" written
     * out longhand is the third copy it exists to prevent. What is genuinely
     * the picker`s own is exactly what is passed in: the SELECT header, the
     * loaded-module mark, its empty message, and the two footers. */
    drawListScreen(ctx, {
        headerLeft: o.headerLeft,
        headerRight: "SELECT",
        entries: (o.entries || []).map((item) => ({
            name: item.name || item.id || "Unknown",
            /* An entry carrying its OWN value wins — that is the list-filter
             * row, whose value is the current list. Everything else gets the
             * loaded mark, drawn where a menu page puts its value.
             *
             * Both rules live HERE and not in either caller: a mark that only
             * one of the two pickers drew is the same bug one layer down, and
             * that is exactly how the two pickers came to look like different
             * products in the first place. */
            value: (item.value !== undefined && item.value !== null && item.value !== "")
                ? String(item.value)
                : ((o.currentId && item.id === o.currentId) ? "*" : ""),
        })),
        index: o.index,
        /* Fitted with the SAME fitText the list rows use, because the two
         * pickers name different things ("No modules available" against "No FX
         * modules available") and the longer one runs off the 128px screen at
         * this x. The device clips silently, so a message that overflowed would
         * simply lose its last word with nothing to say it had. */
        emptyMessage: o.emptyMessage || "No modules available",
        /* CLK does not always LOAD. The list-filter row cycles the filter and
         * loads nothing, so a fixed "LOAD" is a footer describing a different
         * button than the one under the cursor. A row that behaves unusually
         * names its own verb; everything else is a module and loads. */
        footer: [["JOG", "SEL"],
                 ["CLK", pickerClickVerb(o)],
                 ["BACK", "EXIT"]],
        /* An empty picker keeps its OWN, shorter footer: a screen with nothing
         * on it and no way out named is the one place a hint matters most, and
         * JOG/CLK name gestures that would do nothing. BACK is EXIT here by
         * FOOTER_CANON — it leaves the picker entirely and lands back on the
         * editor. */
        emptyFooter: [["BACK", "EXIT"]],
    });
}

/*
 * A plain header/list/footer screen in the picker's rectangle.
 *
 * The module-lists screens are the picker's neighbours — one click from it in
 * both directions — so they draw through the same rect and the same row
 * renderer for the reason stated at the top of this file. A hand-rolled list
 * in shadow_ui.js would be the third copy of a rectangle this file exists to
 * keep singular.
 *
 * Entries are `{ name, value }`, exactly as drawChainPicker passes; `value`
 * carries the loaded mark, the checkbox, the member count, or nothing.
 *
 * `emptyFooter` is separate from `footer` because a screen with no rows must
 * not name the gestures its rows would have answered — the picker`s empty
 * state has offered BACK alone since before this function existed, and
 * folding the two would have changed that silently.
 */
export function drawListScreen(ctx, o) {
    drawHeader(ctx, o.headerLeft, o.headerRight || "", false);
    const entries = o.entries || [];
    if (entries.length === 0) {
        ctx.print(MENU_LIST_X, MENU_LIST_Y + 8,
                  fitText(ctx, o.emptyMessage || "Empty", MENU_LIST_W), 1);
        drawFooter(ctx, o.emptyFooter || o.footer || [["BACK", "EXIT"]]);
        return;
    }
    drawPageChromeList(ctx,
        { x: MENU_LIST_X, y: MENU_LIST_Y,
          w: MENU_LIST_W, h: MOVY_RULE_Y - MENU_LIST_Y },
        entries.map((e) => ({ name: e.name, value: e.value || "" })),
        o.index);
    drawFooter(ctx, o.footer || [["BACK", "EXIT"]]);
}
