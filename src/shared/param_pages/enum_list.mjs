/*
 * THE ENUM OPTION SCREEN — one draw, two entries.
 *
 * It is reached two ways, and they mean opposite things:
 *
 *   PICKER  hold an enum knob and click (or jog-click an enum row in the list
 *           editor). Nothing has been written. The list is a QUESTION: Back is
 *           a real cancel, a click commits, and the `*` marks the value you
 *           would revert to.
 *
 *   PEEK    turn a divable enum. The detent ALREADY WROTE. The list is an
 *           ANSWER — where in the options you have landed, and what is either
 *           side — and it decays on its own.
 *
 * Those are opposite commit semantics, which is exactly why they must not be
 * the same VIEW (a Back that "cancels" a live value is a lie), and exactly why
 * they must be the same DRAW. A second list widget is how Master FX and the
 * chain editor drifted apart, and the device has already had a user report
 * about two module pickers that looked different from each other.
 *
 * THE LIST RECT IS 9, NOT MENU_LIST_Y (10).
 *
 * The movy bands cost vertical space the old chrome did not: a footer rule at
 * 55 with an 8-row hint band under it takes the bottom of the screen, where
 * drawMenuList's default indicator row (62) used to sit. Left at its defaults
 * the list would run its last row and its down-arrow straight through the
 * footer, and the device clips SILENTLY — nothing would say so.
 *
 * The obvious top is MENU_LIST_Y (10), the rect the knob grid's own menu pages
 * use. It costs a row: 10..54 is 44px, and at a 9px line that is FOUR options
 * where the old chrome showed FIVE. One row up is 45px and buys the fifth
 * back, and it is safe ONLY because this header is not inverted — drawHeader
 * fills the band only when told to, so under a plain header the glyphs stop at
 * row 5 and the selected row's highlight starting at row 8 still has clear air
 * above it. (A menu page cannot do the same: its bank bar owns row 7.)
 *
 * Losing the last option of a list to a band drawn over it is a failure this
 * codebase has already had, which is why test_enum_picker_chrome.sh asserts the
 * row COUNT and clipped() === 0 rather than eyeballing one render.
 */
/* RELATIVE, like every other shared/param_pages module — they are imported
 * under node by the host tests, and a device-absolute path (the convention in
 * the src/shadow files, which are not importable anyway) makes this module and
 * everything that pulls it in unresolvable there. */
import { RULE_Y, drawHeader, drawFooter } from "./render_page_movy.mjs";
import { LIST_LABEL_X, drawMenuList } from "../menu_layout.mjs";

export const ENUM_LIST_TOP_Y = 9;
export const ENUM_LIST_BOTTOM_Y = RULE_Y - 1;

/**
 * @param {object}   ctx           { fillRect, print, textWidth }
 * @param {object}   o
 * @param {string}   o.title       header left text
 * @param {string}   o.headerRight header right text ("SELECT" / "TURNING")
 * @param {string[]} o.options     the option list
 * @param {number}   o.index       cursor position
 * @param {number}   o.markIndex   which option wears the `*` (the LIVE value)
 * @param {Array}    o.footer      hint pairs for drawFooter
 */
export function drawEnumList(ctx, o) {
    drawHeader(ctx, o.title, o.headerRight, false);
    if (!o.options || o.options.length === 0) {
        ctx.print(LIST_LABEL_X, ENUM_LIST_TOP_Y + 8, "No options", 1);
        /* Still a footer. openEnumPicker refuses an empty list so this should
         * be unreachable, but a screen with nothing on it is the one place a
         * way out most needs naming. */
        drawFooter(ctx, [["BACK", "EXIT"]]);
        return;
    }
    /* The same list every other picker on this device uses. A second list
     * widget is how Master FX and the chain editor drifted apart. */
    drawMenuList({
        items: o.options,
        selectedIndex: o.index,
        listArea: { topY: ENUM_LIST_TOP_Y, bottomY: ENUM_LIST_BOTTOM_Y },
        getLabel: function(item) { return String(item); },
        /* Which option is CURRENTLY set, so scrolling away from it still reads
         * as "you have moved off the live value" rather than as nothing. */
        getValue: function(item, i) { return i === o.markIndex ? "*" : ""; },
        /* Both callers announce their own, richer string ("Room, 2 of 17"), so
         * the list must not also announce "Room: *". */
        announce: false,
    });
    drawFooter(ctx, o.footer);
}
