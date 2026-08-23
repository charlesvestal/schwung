/*
 * Shared menu layout helpers for title/list/footer screens.
 */

import { getMenuLabelScroller } from './text_scroll.mjs';
import { announceMenuItem, announceParameter } from './screen_reader.mjs';
import { truncateText } from './chain_ui_views.mjs';
/* The movy chrome, imported rather than restated. drawMenuHeader IS the movy
 * header band now — same font, same 7 rows, same measured left/right split —
 * so that a list reached from the knob grid and a list reached from the menu
 * are the same screen and not two products. */
import { drawHeader as drawMovyHeader,
         drawFooter as drawMovyFooter } from './param_pages/render_page_movy.mjs';
/* THE truncator. fitText drops characters from the end until the string
 * measures inside a pixel width; drawMenuList asks it for a COUNT (see
 * fitCharCount) because truncateText and the marquee scroller are both
 * character-budgeted. There is no second copy of that loop in this file. */
import { fitText } from './param_pages/render_page.mjs';
/* The geometry itself comes from the LEAF, ../list_geometry.mjs — the single
 * definition every screen shares. This file re-exports each name because ~41
 * call sites import them from here; chain_ui_views.mjs re-exports the same set
 * for its own ~7 consumers. One value, three doors to it. */
import {
    SCREEN_WIDTH, SCREEN_HEIGHT, RULE_Y as MOVY_RULE_Y,
    TITLE_Y, TITLE_RULE_Y, FOOTER_TEXT_Y, FOOTER_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT, LIST_HIGHLIGHT_OFFSET,
    LIST_LABEL_X, LIST_VALUE_X,
} from './list_geometry.mjs';
export {
    SCREEN_WIDTH, SCREEN_HEIGHT,
    TITLE_Y, TITLE_RULE_Y, FOOTER_TEXT_Y, FOOTER_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT, LIST_HIGHLIGHT_OFFSET,
    LIST_LABEL_X, LIST_VALUE_X,
};

/* ONE name for the bottom edge of the list rect, the footer rule at 55.
 *
 * There used to be three (LIST_INDICATOR_BOTTOM_Y, LIST_BOTTOM_WITH_FOOTER,
 * LIST_BOTTOM_CLEARANCE) plus a dead LIST_MAX_VISIBLE that drawMenuList never
 * read. The movy re-skin collapsed the with-footer and no-footer rects into one
 * (see list_geometry.mjs), so the three names became three spellings of 55 —
 * and a name that can only agree with its siblings is a name that will one day
 * disagree with them. menuLayoutDefaults still publishes listBottomWithFooter /
 * listBottomNoFooter as KEYS, because ~30 call sites read them, but both now
 * resolve to this single constant. */
export const LIST_INDICATOR_X = 120;
export const LIST_INDICATOR_BOTTOM_Y = MOVY_RULE_Y;

/* Text rendering */
export const DEFAULT_CHAR_WIDTH = 6;
export const DEFAULT_LABEL_GAP = 6;
export const DEFAULT_VALUE_PADDING_RIGHT = 2;
export const VALUE_RIGHT_CLEARANCE = 10;  /* Clearance from scroll-arrow column (LIST_INDICATOR_X = 120) */

/* Canonical footer-hint verb prefixes (cleanup step 9, U-2). The Move's jog
 * wheel click is "Click" (not "Push"); action words are lowercase. Build
 * hints with footerHint() so they don't drift back into verb soup:
 *   drawMenuFooter([footerHint("click", "edit"), footerHint("jog", "scroll")])
 * Existing string-literal hints are equally valid as long as they follow the
 * same canon — pinned by tests/shadow/test_footer_verb_consistency.sh. */
export const FOOTER_VERBS = Object.freeze({
    click: "Click",   // jog-wheel click
    jog:   "Jog",     // jog-wheel turn
    back:  "Back",    // Back button
    hold:  "Hold",    // long-press
});

export function footerHint(verb, action) {
    const prefix = FOOTER_VERBS[verb] || verb;
    return action ? `${prefix}: ${action}` : `${prefix}: `;
}

/* Screen reader state - track last announced item to avoid redundant announcements */
let lastAnnouncedIndex = -1;
let lastAnnouncedLabel = "";

/* render_page_movy's renderers draw through an injected ctx (that is what makes
 * them testable into a framebuffer); menu_layout reaches for the device globals
 * by name. This adapter is the seam, and it is now drawMenuList's DEFAULT ctx
 * rather than a special case: the page-chrome list (page_controller.mjs) passes
 * its own injected ctx instead, which is what lets the knob grid's menu pages
 * draw through the same one list every other screen uses.
 *
 * The arrow bodies resolve `fill_rect` / `print` / `set_pixel` at CALL time, not
 * at module load, so a probe that swaps the globals
 * (tools/param-pages/list_probe.mjs, tests/host/test_enum_picker_chrome.sh)
 * still sees every draw. */
const DEVICE_CTX = {
    fillRect: (x, y, w, h, color) => fill_rect(x, y, w, h, color),
    print: (x, y, text, color) => print(x, y, text, color),
    setPixel: (x, y, color) => set_pixel(x, y, color),
    textWidth: (text) => (typeof text_width === 'function'
        ? text_width(String(text))
        : String(text).length * DEFAULT_CHAR_WIDTH),
};

/* THE DEVICE FONT IS PROPORTIONAL. '!' and '|' are 2px, the quote family and
 * 'i'/'l'/'I'/'(' are 3-4, and only the 78 widest glyphs are 6. A budget of
 * `chars * DEFAULT_CHAR_WIDTH` therefore reserves the WIDEST glyph for every
 * character in the string and truncates text that would have fitted — on the
 * module picker "CloudSeed Algorithmic" came out "CloudSeed Algo" with visible
 * empty column to its right. Everything below measures instead, through the
 * same ctx the row is drawn with, which is what drawMenuHeader / drawMenuFooter
 * (and renderPicker, the thing drawMenuList replaced) always did.
 *
 * DEFAULT_CHAR_WIDTH survives in exactly two roles: the fallback when a ctx has
 * no textWidth (bare node, no harness, no device globals), and the worst-case
 * glyph unit for the one budget that is stated in CHARACTERS rather than pixels
 * — selectedMinLabelChars, a floor that must honour "at least N characters"
 * whatever those characters turn out to be. */
function measurer(ctx) {
    return (ctx && typeof ctx.textWidth === 'function')
        ? (s) => ctx.textWidth(String(s == null ? "" : s))
        : (s) => String(s == null ? "" : s).length * DEFAULT_CHAR_WIDTH;
}

/**
 * How many LEADING characters of `text` fit in `maxWidth` pixels.
 *
 * This is fitText asked for a count instead of a string — the row renderer
 * needs a character budget because truncateText ellipsises by characters and
 * the marquee scroller windows by characters. Measuring the whole string first
 * keeps the common "it fits" case exact (fitText asciiFolds, which can change a
 * string's LENGTH: "…" becomes "...").
 */
function fitCharCount(measure, text, maxWidth) {
    const s = String(text == null ? "" : text);
    if (!s || maxWidth <= 0) return 0;
    if (measure(s) <= maxWidth) return s.length;
    return fitText({ textWidth: measure }, s, maxWidth).length;
}

/**
 * truncateText, minus the space that would otherwise sit in front of the
 * ellipsis.
 *
 * With the label floor in place a value is cut where the LABEL stops needing
 * room, not at a word boundary, so the cut lands after a space often enough to
 * matter: "4 bars" fitted to 4 characters is `4 ...`, which reads as a
 * rendering fault rather than as a truncation — the eye sees a gap, then dots.
 * `4...` says the same thing in less room and looks deliberate. Nothing else
 * changes: a cut that does not land on whitespace is truncateText's own output.
 */
function ellipsize(text, maxChars) {
    const t = truncateText(text, maxChars);
    return (t.length > 3 && t.slice(-3) === "...")
        ? t.replace(/\s+\.\.\.$/, "...")
        : t;
}

/* A ctx that came from the param-page engine (render_page.mjs / the harness's
 * drawContext) has no setPixel — its surface is fillRect/print/textWidth. A 1x1
 * fill is the same pixel, so the arrows work through either. */
function px(ctx, x, y, color) {
    if (ctx.setPixel) ctx.setPixel(x, y, color);
    else ctx.fillRect(x, y, 1, 1, color);
}

/**
 * The movy header band: 7 rows, font4x5, title on the left and the optional
 * right-hand string measured first so the two can never overlap.
 *
 * It used to be the device 5x7 font at y=2 with a full-width rule at y=12, and
 * the rule is why this could not move on its own: the list now starts at y=10,
 * so a rule at 12 would be drawn straight through the first row. Header and
 * list rect are one change.
 */
export function drawMenuHeader(title, titleRight = "", inverted = false) {
    drawMovyHeader(DEVICE_CTX,
        title === undefined || title === null ? "" : String(title),
        titleRight ? String(titleRight) : "",
        inverted);
}

/**
 * A call site's pre-joined hint string -> movy's [KEY, ACTION] pair.
 *
 * The hint VOCABULARY is unchanged — FOOTER_VERBS / footerHint() still decide
 * the words, and tests/shadow/test_footer_verb_consistency.sh still pins them.
 * This only decides where the colon that was always in those strings becomes
 * the boundary between the inverted pill and the plain action.
 *
 * A string with no colon ("Press to continue", or an editor error) still has to
 * become a pair, because a pair is the only shape the movy footer draws. The
 * first word becomes the pill: a key of "" would draw a 4px stub of a pill,
 * which reads as a rendering fault rather than as a hint.
 */
function hintPair(text) {
    if (text === undefined || text === null) return null;
    const s = String(text).trim();
    if (!s) return null;
    const colon = s.indexOf(':');
    if (colon > 0) {
        const key = s.slice(0, colon).trim();
        const action = s.slice(colon + 1).trim();
        if (key) return [key, action];
    }
    const sp = s.indexOf(' ');
    return sp > 0 ? [s.slice(0, sp), s.slice(sp + 1)] : [s, ""];
}

/**
 * The movy hint footer: the rule at RULE_Y, then [KEY] ACTION pills in font4x5
 * centred in the 8-row band. It used to be the device 5x7 font printed flush
 * against the bottom edge — geometrically right (its rule already sat at 55)
 * but the last screen element still wearing the old chrome, so a list looked
 * like two products stacked.
 *
 * Takes either a bare hint string, or an ORDERED ARRAY of hint strings in
 * display order. `y` is accepted and ignored — the movy band centres its own
 * text — and no call site ever passed it.
 *
 * The old `{ left, right }` shape is GONE, and deliberately not aliased: movy's
 * own canon pulls a BACK hint to the right edge and reserves its room first,
 * wherever the caller put it, so `{ left: "Back: slots" }` rendered on the
 * RIGHT. A parameter named for a side it does not control is worse than no
 * name. Order is the only thing the caller actually decides; everything else is
 * movy's, and `["Back: slots", "Click: edit"]` still renders as
 * [CLICK] EDIT ... [BACK] SLOTS — the same place BACK sits on the knob grid.
 */
export function drawMenuFooter(hints, y = FOOTER_TEXT_Y) {
    if (!hints) return;
    const list = Array.isArray(hints) ? hints : [hints];
    drawMovyFooter(DEVICE_CTX, list.map(hintPair).filter(Boolean));
}

/* Measuring against the DEVICE font, for the widgets below that draw through
 * the globals rather than through an injected ctx. */
const deviceMeasure = measurer(DEVICE_CTX);

/**
 * The quoted name shared by drawConfirmModal and drawNamePreview, fitted to the
 * column it is actually printed in. It used to be a flat truncateText(name, 20)
 * — 20 glyphs at the assumed 6px is 120px, in a column that is 118px wide, so
 * the cap was simultaneously too generous for wide names and far too mean for
 * narrow ones ("Illustrious Little Fi" fits where "Illustrious Litt" did).
 */
function fitQuotedName(name) {
    const s = String(name ?? "");
    const room = SCREEN_WIDTH - LIST_LABEL_X - 2 - deviceMeasure('""');
    return '"' + truncateText(s, fitCharCount(deviceMeasure, s, room)) + '"';
}

/* Shared two-option confirmation modal (cleanup step 9, U-4): header +
 * optional quoted name + a vertical two-row selector + footer. Replaces the
 * four hand-rendered copies in the slot-preset (settings) and master-preset
 * (master_fx) overwrite/delete flows so the Yes/No widget is identical
 * everywhere. selectedIndex picks the highlighted row; labels default to
 * No/Yes (index 0 = No, the safe default the callers seed). */
export function drawConfirmModal({
    title,
    name,
    selectedIndex,
    labels = ["No", "Yes"],
    footer = "Back: cancel"
}) {
    drawMenuHeader(title);
    if (name !== undefined && name !== null && name !== "") {
        print(LIST_LABEL_X, LIST_TOP_Y, fitQuotedName(name), 1);
    }
    /* +16 rather than a fresh constant: the whole modal rides LIST_TOP_Y, so
     * the movy re-skin lifted it 5 rows with the list (name at 10, rows at 26
     * and 35, footer rule still 55). Nothing collides — the header band ends
     * at row 6 and the name's glyphs start at 10 — so the offset is unchanged. */
    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < labels.length; i++) {
        const rowY = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === selectedIndex;
        if (isSelected) {
            fill_rect(0, rowY - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, rowY, labels[i], isSelected ? 0 : 1);
    }
    drawMenuFooter(footer);
}

/* Shared "Save As" name preview: header + quoted name + a two-row Edit/OK
 * selector + footer. Replaces the byte-identical copies in drawChainSettings
 * (slot presets) and drawMasterNamePreview (master presets) -- the pair that
 * made "fix the slot list and MFX stays broken" concrete. Same widget as
 * drawConfirmModal, different labels. */
export function drawNamePreview({
    name,
    selectedIndex,
    title = "Save As",
    labels = ["Edit", "OK"],
    footer = "Back: cancel"
}) {
    drawMenuHeader(title);
    print(LIST_LABEL_X, LIST_TOP_Y, fitQuotedName(name), 1);
    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < labels.length; i++) {
        const rowY = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === selectedIndex;
        if (isSelected) {
            fill_rect(0, rowY - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, rowY, labels[i], isSelected ? 0 : 1);
    }
    drawMenuFooter(footer);
}

export function drawArrowUp(x, y, ctx = DEVICE_CTX) {
    px(ctx, x + 2, y, 1);
    px(ctx, x + 1, y + 1, 1);
    px(ctx, x + 3, y + 1, 1);
    for (let i = 0; i < 5; i++) {
        px(ctx, x + i, y + 2, 1);
    }
}

export function drawArrowDown(x, y, ctx = DEVICE_CTX) {
    for (let i = 0; i < 5; i++) {
        px(ctx, x + i, y, 1);
    }
    px(ctx, x + 1, y + 1, 1);
    px(ctx, x + 3, y + 1, 1);
    px(ctx, x + 2, y + 2, 1);
}

export function drawMenuList({
    /* The draw surface. Defaults to the device globals, which is what every
     * shadow view module wants and what this file always did. The param-page
     * engine (page_controller.mjs) renders through an INJECTED ctx instead —
     * that is what makes it testable into a framebuffer and previewable in
     * tools/param-pages — so the one list has to be able to draw through one
     * too, or the page chrome could never call it. */
    ctx = DEVICE_CTX,
    items,
    selectedIndex,
    listArea,
    topY = LIST_TOP_Y,
    lineHeight = LIST_LINE_HEIGHT,
    highlightHeight = LIST_HIGHLIGHT_HEIGHT,
    highlightOffset = LIST_HIGHLIGHT_OFFSET,
    labelX = LIST_LABEL_X,
    valueX = LIST_VALUE_X,
    valueAlignRight = false,
    valuePaddingRight = DEFAULT_VALUE_PADDING_RIGHT,
    labelGap = DEFAULT_LABEL_GAP,
    maxVisible = 0,
    keepOffLastRow = true,
    indicatorX = LIST_INDICATOR_X,
    indicatorBottomY = LIST_INDICATOR_BOTTOM_Y,
    getLabel,
    getValue,
    getSubLabel = null,
    subLabelOffset = 9,
    editMode = false,
    scrollSelectedValue = false,
    prioritizeSelectedValue = false,
    selectedMinLabelChars = 6,
    /* THE LABEL FLOOR, and it applies to EVERY row — see the long note above
     * the reservation itself. 0 opts a caller out entirely. */
    minLabelChars = 8,
    announce = true  /* set false when the caller emits its own richer
                        screen-reader announcements (e.g. file-browser) to
                        avoid double-announcing each selection move */
}) {
    const totalItems = items.length;
    /* Every width below is MEASURED through the ctx this row is drawn with, so
     * the budget and the glyphs can never disagree. See measurer(). */
    const measure = measurer(ctx);
    const itemHeight = getSubLabel ? (lineHeight + subLabelOffset) : lineHeight;
    const itemHighlightHeight = getSubLabel ? (lineHeight + subLabelOffset + 2) : highlightHeight;
    const resolvedTopY = listArea?.topY ?? topY;
    const resolvedBottomY = listArea?.bottomY ?? indicatorBottomY;
    const computedMaxVisible = maxVisible > 0
        ? maxVisible
        : Math.max(1, Math.floor((resolvedBottomY - resolvedTopY) / itemHeight));
    const effectiveMaxVisible = computedMaxVisible;
    let startIdx = 0;

    const maxSelectedRow = keepOffLastRow
        ? effectiveMaxVisible - 2
        : effectiveMaxVisible - 1;
    if (selectedIndex > maxSelectedRow) {
        startIdx = selectedIndex - maxSelectedRow;
    }
    let endIdx = Math.min(startIdx + effectiveMaxVisible, totalItems);

    /* The scroll arrows are drawn at indicatorX (120) and are 5px wide, so they
     * own the column 120..124. A right-aligned value laid out against
     * valuePaddingRight (2) runs to x=126 and is drawn straight THROUGH them —
     * an item valued "v9" renders with the arrow crossing the glyphs.
     *
     * The clearance applies ONLY when an arrow is actually drawn. Reserving 8px
     * of the value column on every list to guard against an arrow that is not
     * there would narrow (and so truncate) values on every non-scrolling
     * screen. The two arrow conditions are the same expressions used at the
     * bottom of this function; they are derivable here because startIdx /
     * endIdx / totalItems are all already settled. */
    const hasScrollArrow = (startIdx > 0) || (endIdx < totalItems);
    /* Derived from indicatorX rather than restated, so a caller that moves the
     * arrow column moves the value edge with it. At the default indicatorX=120
     * this is 118, i.e. exactly SCREEN_WIDTH - VALUE_RIGHT_CLEARANCE — the
     * constant is kept as the statement of the 10px budget and as the ceiling
     * on how far right a value may ever sit. */
    const valueRightEdgeClear = SCREEN_WIDTH - valuePaddingRight;
    const valueRightEdgeArrow = Math.min(indicatorX - 2,
                                         SCREEN_WIDTH - VALUE_RIGHT_CLEARANCE);
    /*
     * THE CLEARANCE IS CHARGED PER ROW, not to the whole list.
     *
     * The arrows are drawn at exactly two places — drawArrowUp at
     * resolvedTopY and drawArrowDown at resolvedBottomY - 3 — so they overlap
     * the FIRST visible row and the LAST visible row and nothing in between.
     * Reserving the column for the whole list therefore took 10px off every
     * row to make room for a glyph that touches two of them, and on the
     * page-chrome list (whose edge is already pulled in to 118 by the frame)
     * that left 100px for label + value. "Latency Comp" measures 71 and "On"
     * measures 12, which with the cursor prefix and the gap is 101 — so a row
     * that fits by a pixel was truncated to "Latency..." to pay for an arrow
     * three rows away. Reported from the device, twice.
     *
     * A middle row now gets the full 118. The two rows that really do sit
     * under an arrow still pull in to 108, which is the only place the
     * collision was ever possible.
     */
    const upArrowRow   = (startIdx > 0) ? startIdx : -1;
    const downArrowRow = (endIdx < totalItems) ? endIdx - 1 : -1;
    const valueRightEdgeFor = (i) =>
        (i === upArrowRow || i === downArrowRow) ? valueRightEdgeArrow
                                                 : valueRightEdgeClear;

    /* The no-value label column needs no equivalent: its budget is
     * `indicatorX - labelX - labelGap`, which stops at 120 whether or not an
     * arrow is there, so it cannot collide. */

    /* Get label scroller for selected item and tick it */
    const labelScroller = getMenuLabelScroller();
    labelScroller.setSelected(selectedIndex);
    labelScroller.tick();  /* Auto-tick during draw */

    /* Announce selected item to screen reader if changed */
    if (announce && selectedIndex >= 0 && selectedIndex < totalItems && items[selectedIndex]?.type !== 'divider') {
        const selectedItem = items[selectedIndex];
        const selectedLabel = getLabel(selectedItem, selectedIndex);
        const selectedValue = getValue ? getValue(selectedItem, selectedIndex) : "";

        /* Only announce if index or label changed */
        if (selectedIndex !== lastAnnouncedIndex || selectedLabel !== lastAnnouncedLabel) {
            announceMenuItem(selectedLabel, selectedValue);
            lastAnnouncedIndex = selectedIndex;
            lastAnnouncedLabel = selectedLabel;
        }
    }

    for (let i = startIdx; i < endIdx; i++) {
        const y = resolvedTopY + (i - startIdx) * itemHeight;
        const item = items[i];
        const isSelected = i === selectedIndex;

        /* Divider: horizontal rule across the row with optional small caption */
        if (item && item.type === 'divider') {
            const midY = y + Math.floor(itemHeight / 2);
            if (item.label) {
                const captionW = measure(item.label);
                const captionX = labelX;
                /* Line left of caption */
                ctx.fillRect(0, midY, captionX - 2, 1, 1);
                ctx.print(captionX, midY - 3, item.label, 1);
                /* Line right of caption */
                const rightStart = captionX + captionW + 2;
                ctx.fillRect(rightStart, midY, SCREEN_WIDTH - rightStart, 1, 1);
            } else {
                ctx.fillRect(2, midY, SCREEN_WIDTH - 4, 1, 1);
            }
            continue;
        }

        const labelPrefix = isSelected ? "> " : "  ";
        let label = getLabel(item, i);
        const fullLabel = label; /* Keep original for scrolling */
        const valueRaw = getValue ? getValue(item, i) : "";
        const fullValue = valueRaw ? String(valueRaw) : "";
        let displayValue = fullValue;
        let resolvedValueX = valueX;
        let maxLabelChars = 0;

        /* The selected row in edit mode prints "[value]", not "value", and the
         * brackets are real glyphs that have to come out of the same column.
         * The 6px budget used to hide this: it over-reserved by (6 - w) on
         * every character, which happened to absorb the two brackets. Measured
         * exactly, "[100%]" ran 4px off the right edge of the display — the
         * harness reported clipped=7. So the bracketed row right-aligns against
         * an edge pulled in by the CLOSING bracket, and is then shifted left by
         * the OPENING one, which puts the "]" precisely on valueRightEdge. */
        const bracketed = editMode && valueAlignRight && isSelected;
        const rowEdge = valueRightEdgeFor(i);
        const rowValueRightEdge = bracketed
            ? rowEdge - measure("]")
            : rowEdge;

        if (valueAlignRight && fullValue) {
            let valueXFloor = valueX;
            /* THE LABEL FLOOR.
             *
             * The value used to be laid out FIRST, at its full measured width,
             * and the label took whatever was left — with no floor at all on an
             * unselected row. On a knobs page drawn as a list that produced rows
             * that convey nothing:
             *
             *     A...        kick_01.wav
             *     A Le...       1/2 bar
             *     B...        kick_01.wav
             *
             * Two rows reading `kick_01.wav` with the labels `A...` and `B...`
             * — the sample the user is looking at cannot be identified at all.
             * A label cut to `A...` conveys strictly less than a value cut by a
             * few characters, so when the two compete the label wins first.
             *
             * The reservation is min(what the label actually needs, the floor),
             * which is why a short label still costs the value nothing: `Cutoff`
             * against `60` reserves 36px, the value wants nowhere near that far
             * left, and the row is unchanged. Only a label that would be crushed
             * takes room back, and only up to the floor.
             *
             * The floor is stated in CHARACTERS but it is MEASURED, because here
             * — unlike selectedMinLabelChars, which is resolved before the row's
             * label is in hand (see measurer()) — the N characters are a string
             * we already have: the label's own first N. Reserving N *
             * DEFAULT_CHAR_WIDTH instead would be the 6px-per-glyph assumption
             * this file spent a commit removing, and it over-reserves in exactly
             * the way that makes the trade a bad one: `KBD CV Input` against
             * `ESP CV` reserved 48px for eight glyphs that measure 46, so the
             * label gained two pixels it could not use and the value lost a
             * character to pay for them.
             *
             * The cap against rowValueRightEdge is a guard, not a policy: if a
             * caller's geometry ever left less than three worst-case glyphs for
             * the value, maxValueChars would come out 0 and the branch below
             * would print the value UNTRUNCATED, off the right edge. It cannot
             * bite at any shipped geometry (the widest floor here is 74 against
             * an edge of 108).
             *
             * Interaction with valueX: taking the MAX means a full-width list,
             * whose valueX floor is already 92, is bit-identical. Only lists that
             * hand the floor to their own left edge — the page-chrome list, which
             * passes `valueX: rect.x` precisely so the label budget does the work
             * — actually move.
             */
            if (minLabelChars > 0 && !(isSelected && prioritizeSelectedValue)) {
                /* slice() IS the min(): a label shorter than the floor reserves
                 * its own whole width and so costs the value nothing at all. */
                const reserve = measure(labelPrefix)
                    + measure(String(fullLabel).slice(0, minLabelChars | 0));
                const labelFloorX = labelX + reserve + labelGap;
                const valueGuardX = rowValueRightEdge - 3 * DEFAULT_CHAR_WIDTH;
                valueXFloor = Math.max(valueXFloor,
                                       Math.min(labelFloorX, valueGuardX));
            }
            if (isSelected && prioritizeSelectedValue) {
                /* The SELECTED row's own floor, and it REPLACES the general one
                 * rather than adding to it — that is what prioritizeSelectedValue
                 * asks for: the row you are on may give its value a larger share
                 * than any other row would, down to selectedMinLabelChars (6,
                 * against the general 8). Left as the worst-case-glyph budget it
                 * has always been so that the ~30 callers passing this flag are
                 * bit-identical; the general floor above is the measured one. */
                const selectedChars = Math.max(0, selectedMinLabelChars | 0);
                const minLabelWidth = measure(labelPrefix)
                    + (selectedChars * DEFAULT_CHAR_WIDTH) + labelGap;
                valueXFloor = labelX + minLabelWidth;
            }

            resolvedValueX = rowValueRightEdge - measure(fullValue);
            if (resolvedValueX < valueXFloor) {
                resolvedValueX = valueXFloor;
            }

            const maxValueWidth = Math.max(0, rowValueRightEdge - resolvedValueX);
            const maxValueChars = fitCharCount(measure, fullValue, maxValueWidth);
            if (maxValueChars > 0 && fullValue.length > maxValueChars) {
                if (isSelected && scrollSelectedValue) {
                    displayValue = labelScroller.getScrolledText(fullValue, maxValueChars);
                } else {
                    displayValue = ellipsize(fullValue, maxValueChars);
                }
            }

            /* RE-ALIGN A TRUNCATED VALUE, then budget the label against where it
             * actually ended up.
             *
             * resolvedValueX above is the position for the value at its FULL
             * width, clamped up to the label's floor. The value is then cut down
             * to whatever fits from there — but it was still being DRAWN at that
             * original x, so a cut value stopped short of the right edge and left
             * a hole, and the label's budget was measured against a value that no
             * longer reached that far. Both columns paid for space neither used:
             *
             *     "  Move..."  45px  +  "Schwu..."  38px  =  83px of 118
             *
             * on a row where both strings were truncated. Right-aligning the cut
             * value restores the edge `valueAlignRight` promises, and hands the
             * slack back to the label — which is the column that was crushed.
             *
             * One pass, deliberately, not a loop: a shorter label could in
             * principle free more for the value, but the value has already been
             * fitted and re-widening it would re-open the same negotiation. The
             * label is the side that was starved, so it takes the slack. */
            if (displayValue !== fullValue) {
                resolvedValueX = Math.max(resolvedValueX,
                                          rowValueRightEdge - measure(displayValue));
            }

            const maxLabelWidth = Math.max(0, resolvedValueX - labelX - labelGap);
            maxLabelChars = fitCharCount(measure, fullLabel, maxLabelWidth - measure(labelPrefix));
        } else {
            /* No value, label can use full width minus prefix and indicator */
            const maxLabelWidth = indicatorX - labelX - labelGap;
            maxLabelChars = fitCharCount(measure, fullLabel, maxLabelWidth - measure(labelPrefix));
        }

        if (maxLabelChars > 0) {
            if (isSelected && fullLabel.length > maxLabelChars) {
                /* Selected item with long text: use scroller */
                label = labelScroller.getScrolledText(fullLabel, maxLabelChars);
            } else {
                /* Truncate non-selected or short text */
                label = ellipsize(fullLabel, maxLabelChars);
            }
        }

        if (isSelected) {
            ctx.fillRect(0, y - highlightOffset, SCREEN_WIDTH, itemHighlightHeight, 1);
            ctx.print(labelX, y, `${labelPrefix}${label}`, 0);
            if (displayValue) {
                /* Show brackets around value when in edit mode */
                const shownValue = editMode ? `[${displayValue}]` : displayValue;
                /* When valueAlignRight and editMode, shift left to account for added brackets */
                const editValueX = bracketed
                    ? resolvedValueX - measure("[")  /* Shift left by the bracket's OWN width */
                    : resolvedValueX;
                ctx.print(editValueX, y, shownValue, 0);
            }
        } else {
            ctx.print(labelX, y, `${labelPrefix}${label}`, 1);
            if (displayValue) {
                ctx.print(resolvedValueX, y, displayValue, 1);
            }
        }

        if (getSubLabel) {
            const subLabel = getSubLabel(item, i);
            if (subLabel) {
                const subY = y + subLabelOffset;
                /* Indented to sit under the label, i.e. past the cursor prefix
                 * — measured, so it is the prefix's real width and not two
                 * worst-case glyphs. */
                const subX = labelX + measure(labelPrefix);
                ctx.print(subX, subY, subLabel, isSelected ? 0 : 1);
            }
        }
    }

    if (startIdx > 0) {
        drawArrowUp(indicatorX, resolvedTopY, ctx);
    }
    if (endIdx < totalItems) {
        /* -3, not -2: the arrow is 3 rows tall, so -2 put its tip ON
         * resolvedBottomY. Under the old chrome that row was blank screen;
         * under the movy chrome the bottom of the rect IS the footer rule, and
         * the arrow read as growing out of it. */
        drawArrowDown(indicatorX, resolvedBottomY - 3, ctx);
    }
}

export const menuLayoutDefaults = {
    footerY: FOOTER_TEXT_Y,
    listTopY: LIST_TOP_Y,
    /* One rect, so one number under both keys — see LIST_INDICATOR_BOTTOM_Y. */
    listBottomWithFooter: LIST_INDICATOR_BOTTOM_Y,
    listBottomNoFooter: LIST_INDICATOR_BOTTOM_Y
};


/* === Parameter Overlay === */
/* A centered overlay for showing parameter name and value feedback */

const OVERLAY_DURATION_TICKS = 240;  /* ~4 seconds at 60fps, dismissed on UI interaction */
const OVERLAY_WIDTH = 120;
const OVERLAY_HEIGHT = 28;

let overlayActive = false;
let overlayName = "";
let overlayValue = "";
let overlayTimeout = 0;

/**
 * Show the parameter overlay with a name and value
 * @param {string} name - Parameter name to display
 * @param {string} value - Value to display (e.g., "50%" or "3")
 * @param {number} [durationTicks] - Optional custom duration in ticks
 */
export function showOverlay(name, value, durationTicks) {
    const sameContent = overlayActive && overlayName === name && overlayValue === value;
    overlayActive = true;
    overlayName = name;
    overlayValue = value;
    overlayTimeout = durationTicks || OVERLAY_DURATION_TICKS;
    /* Announce only when content changes to avoid per-frame D-Bus spam. */
    if (!sameContent) {
        announceParameter(name, value);
    }
}

/**
 * Hide the overlay immediately
 */
export function hideOverlay() {
    overlayActive = false;
    overlayTimeout = 0;
}

/**
 * Check if a MIDI message should dismiss the overlay, and dismiss if so.
 * Call this at the start of onMidiMessage handlers.
 * @param {Uint8Array} msg - MIDI message
 * @returns {boolean} true if overlay was dismissed (caller should return early to consume input)
 */
export function dismissOverlayOnInput(msg) {
    if (!overlayActive || !msg || msg.length < 3) return false;

    const status = msg[0] & 0xF0;
    const data1 = msg[1];
    const data2 = msg[2];

    /* Dismiss on button press (CC with value > 63), note on, or knob turn */
    const isButtonPress = (status === 0xB0 && data2 > 63);
    const isNoteOn = (status === 0x90 && data2 > 0);
    const isKnobTurn = (status === 0xB0 && data1 >= 71 && data1 <= 79);

    if (isButtonPress || isNoteOn || isKnobTurn) {
        hideOverlay();
        return true;
    }
    return false;
}

/**
 * Check if overlay is currently active
 * @returns {boolean}
 */
export function isOverlayActive() {
    return overlayActive;
}

/**
 * Tick the overlay timer - call this in your tick() function
 * @returns {boolean} true if overlay state changed (needs redraw)
 */
export function tickOverlay() {
    if (overlayTimeout > 0) {
        overlayTimeout--;
        if (overlayTimeout === 0) {
            overlayActive = false;
            return true;  /* State changed, needs redraw */
        }
    }
    return false;
}

/**
 * Draw the overlay if active - call this at the end of your draw function
 */
export function drawOverlay() {
    if (!overlayActive || !overlayName) return;

    const boxX = (SCREEN_WIDTH - OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - OVERLAY_HEIGHT) / 2;

    /* Background and border */
    fill_rect(boxX, boxY, OVERLAY_WIDTH, OVERLAY_HEIGHT, 0);  /* Clear background */
    fill_rect(boxX, boxY, OVERLAY_WIDTH, 1, 1);     /* Top border */
    fill_rect(boxX, boxY + OVERLAY_HEIGHT - 1, OVERLAY_WIDTH, 1, 1);  /* Bottom border */
    fill_rect(boxX, boxY, 1, OVERLAY_HEIGHT, 1);     /* Left border */
    fill_rect(boxX + OVERLAY_WIDTH - 1, boxY, 1, OVERLAY_HEIGHT, 1);  /* Right border */

    /* Parameter name and value */
    const displayName = overlayName.length > 18 ? overlayName.substring(0, 18) : overlayName;
    print(boxX + 4, boxY + 2, displayName, 1);
    print(boxX + 4, boxY + 14, `Value: ${overlayValue}`, 1);
}

/* === Status Overlay === */
/* A centered overlay for status/loading messages */

const STATUS_OVERLAY_WIDTH = 120;
const STATUS_OVERLAY_HEIGHT = 40;

/* Helper to draw rectangle outline using fill_rect */
export function drawRect(x, y, w, h, color) {
    fill_rect(x, y, w, 1, color);           /* Top */
    fill_rect(x, y + h - 1, w, 1, color);   /* Bottom */
    fill_rect(x, y, 1, h, color);           /* Left */
    fill_rect(x + w - 1, y, 1, h, color);   /* Right */
}

/**
 * Draw a centered status overlay with title and message
 * Used for loading states, installation progress, etc.
 * @param {string} title - Title text (e.g., "Installing")
 * @param {string} message - Message text (e.g., "Mini-JV v0.2.0")
 */
export function drawStatusOverlay(title, message) {
    const boxX = (SCREEN_WIDTH - STATUS_OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - STATUS_OVERLAY_HEIGHT) / 2;

    /* Background and double border */
    fill_rect(boxX, boxY, STATUS_OVERLAY_WIDTH, STATUS_OVERLAY_HEIGHT, 0);
    drawRect(boxX, boxY, STATUS_OVERLAY_WIDTH, STATUS_OVERLAY_HEIGHT, 1);
    drawRect(boxX + 1, boxY + 1, STATUS_OVERLAY_WIDTH - 2, STATUS_OVERLAY_HEIGHT - 2, 1);

    /* Center title */
    const titleW = deviceMeasure(title);
    print(Math.floor((SCREEN_WIDTH - titleW) / 2), boxY + 10, title, 1);

    /* Center message */
    const msgW = deviceMeasure(message);
    print(Math.floor((SCREEN_WIDTH - msgW) / 2), boxY + 24, message, 1);
}

/**
 * Draw a post-install or error message overlay with multiple lines
 * @param {string} title - Title (e.g., "Install Complete" or "Load Error")
 * @param {string[]} messageLines - Pre-wrapped message lines
 * @param {boolean} showOk - Whether to show [OK] button (default: true)
 */
export function drawMessageOverlay(title, messageLines, showOk = true) {
    const lineCount = Math.min(messageLines ? messageLines.length : 0, 4);
    const boxHeight = showOk ? (36 + lineCount * 10) : (24 + lineCount * 10);
    const boxX = (SCREEN_WIDTH - STATUS_OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - boxHeight) / 2;

    /* Background and double border */
    fill_rect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 0);
    drawRect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 1);
    drawRect(boxX + 1, boxY + 1, STATUS_OVERLAY_WIDTH - 2, boxHeight - 2, 1);

    /* Center title */
    const titleW = deviceMeasure(title);
    print(Math.floor((SCREEN_WIDTH - titleW) / 2), boxY + 6, title, 1);

    /* Message lines */
    if (messageLines) {
        for (let i = 0; i < lineCount; i++) {
            const line = messageLines[i];
            const lineW = deviceMeasure(line);
            print(Math.floor((SCREEN_WIDTH - lineW) / 2), boxY + 18 + i * 10, line, 1);
        }
    }

    /* OK button - highlighted to show it's the action */
    if (showOk) {
        const okText = '[OK]';
        const okW = deviceMeasure(okText);
        const okX = Math.floor((SCREEN_WIDTH - okW) / 2);
        const okY = boxY + boxHeight - 14;
        fill_rect(okX - 4, okY - 2, okW + 8, 12, 1);
        print(okX, okY, okText, 0);
    }
}

/**
 * Draw a Yes/No confirm overlay. Caller manages the active state and reads input.
 * Footer is fixed: "Back:No  Jog:Yes".
 * @param {string} title - Title (e.g., "Speaker Feedback Risk")
 * @param {string[]} messageLines - Pre-wrapped message lines. Caller should
 *   wrap to ≤ 20 chars per line; lines beyond the first 5 are dropped.
 */
export function drawConfirmOverlay(title, messageLines, footer) {
    /* 8px body line spacing (vs 10 in drawMessageOverlay) lets a 5-line
     * confirm message fit the 64 px display without clipping the title. */
    const lineCount = Math.min(messageLines ? messageLines.length : 0, 5);
    const boxHeight = 36 + lineCount * 8;
    const boxX = (SCREEN_WIDTH - STATUS_OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - boxHeight) / 2;

    fill_rect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 0);
    drawRect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 1);
    drawRect(boxX + 1, boxY + 1, STATUS_OVERLAY_WIDTH - 2, boxHeight - 2, 1);

    const titleW = deviceMeasure(title);
    print(Math.floor((SCREEN_WIDTH - titleW) / 2), boxY + 6, title, 1);

    if (messageLines) {
        for (let i = 0; i < lineCount; i++) {
            const line = messageLines[i];
            const lineW = deviceMeasure(line);
            print(Math.floor((SCREEN_WIDTH - lineW) / 2), boxY + 18 + i * 8, line, 1);
        }
    }

    const footerText = footer || 'Back:No  Jog:Yes';
    const footerW = deviceMeasure(footerText);
    print(Math.floor((SCREEN_WIDTH - footerW) / 2), boxY + boxHeight - 12, footerText, 1);
}

/* Note: Label scroller is auto-ticked inside drawMenuList() */
