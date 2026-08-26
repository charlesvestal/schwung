/*
 * list_geometry.mjs — THE definition of the movy chrome geometry.
 *
 * A LEAF: it imports nothing. That is deliberate and it is the whole point.
 *
 * Why it exists
 * -------------
 * These numbers used to live in TWO places. `menu_layout.mjs` was re-skinned to
 * the movy chrome (list rect at y=10, five rows), and `chain_ui_views.mjs` kept
 * a complete second copy of the OLD chrome (TITLE_Y 2, TITLE_RULE_Y 12,
 * LIST_TOP_Y 15, LIST_LABEL_X 4). Every shadow view module —
 * shadow_ui.js, shadow_ui_slots / _settings / _master_fx / _patches / _tools /
 * _store — imported `LIST_TOP_Y` from the SECOND copy and handed it straight
 * back to `drawMenuList` as `listArea: { topY: LIST_TOP_Y }`, at ~20 call
 * sites. So the re-skin was inert on the device: an 8-row dead band between the
 * header band (ends y=6) and the first row (y=15), and floor((55-15)/9) = 4
 * rows where the design says 5. The test that proved the re-skin imported the
 * NEW value and passed its own listArea, so it measured a rect no screen
 * rendered with — green, and testing the wrong thing.
 *
 * One definition. Everything else imports it.
 *
 * Why a leaf, rather than importing from render_page_movy.mjs
 * -----------------------------------------------------------
 * `render_page_movy.mjs` owned HEADER_H / RULE_Y / MENU_LIST_X/Y/W. Reading two
 * integers from there drags the whole page engine — five font tables, the viz
 * renderer, param_meta — into every host module that merely draws a list. It
 * would also make a cycle hard to see coming: `menu_layout.mjs` imports
 * `truncateText` from `chain_ui_views.mjs`, which is exactly why
 * `chain_ui_views` could not simply import the geometry back from
 * `menu_layout`. A module BELOW both, with no imports at all, cannot
 * participate in a cycle by construction.
 *
 * So the primitives moved DOWN here and `render_page_movy.mjs` re-exports them
 * under their existing names — its ~15 consumers are untouched, and every one
 * of these constants is still defined exactly once in the tree.
 * tests/host/test_list_behavior.sh pins that count at 1 per name.
 */

/* ---- Screen ------------------------------------------------------------- */
export const SCREEN_WIDTH = 128;
export const SCREEN_HEIGHT = 64;

/* ---- The three bands ----------------------------------------------------
 *
 * HEADER_H = 7: a 5-row glyph at y=1, with ONE CLEAR ROW ABOVE AND BELOW it.
 *
 * Both rows are load-bearing and the reason is the INVERTED header. When a knob
 * is touched the band fills solid and the glyphs are knocked out of it, so a
 * glyph flush against either edge has its ink running into the boundary — the
 * highlight bleeds into the border and the text stops having a shape.
 *
 * This was briefly 6, with the glyphs at y=0 on the argument that the bezel
 * already insets the top row so a margin there buys nothing. That is true of
 * the BAND against the panel edge and false of the GLYPHS against the band:
 * inverted, the thing the top row separates is text from its own highlight,
 * not the screen from its surround. Seen on hardware, put back.
 *
 * FOOTER_H = 7 at FOOTER_Y = 57, so the hint pills end on row 63 — the last
 * scanline — for the same bezel reason. The panel is inset in plastic, so a
 * dark row at the bottom is not a margin, it is a margin ON TOP of a margin.
 * RULE_Y = 55 is unchanged, and row 56 is now the clear row between it and the
 * pills.
 *
 * THESE ARE SHARED. Changing them moves every list screen in the shadow UI —
 * slots, settings, master FX, patches, tools, store, menus — not just the knob
 * grid, which is the whole point of this module being one definition. Anything
 * changed here is verified on HARDWARE, not by the suite: this module exists
 * because a re-skin once passed its own test while the device showed an 8-row
 * dead band, so green is not evidence about those screens. The 6-row header
 * above is the second time that has been demonstrated.
 */
export const HEADER_H = 7;
export const RULE_Y = 55;
export const FOOTER_Y = 57;
export const FOOTER_H = 7;

/* ---- The list rect, between the header band and that rule ----------------
 *
 * Five rows at a 9px stride: y = 10, 19, 28, 37, 46, glyphs ending at 52 and
 * the selected row's fill spanning 9..53, one clear row short of RULE_Y.
 *
 * It tracks HEADER_H rather than standing alone: three clear rows between the
 * band and the first row. It briefly went to 9 while the band was 6, and came
 * back with it. test_list_behavior.sh asserts the GAP for that reason — a
 * literal here would fail a change that preserved the spacing and pass one
 * that broke it.
 *
 * The knob grid's menu pages and drawMenuList — i.e. every list in the shadow
 * UI — occupy exactly this rect, or the two look subtly unlike each other.
 */
/* MENU_LIST_X is the LABEL x, one pixel clear of the frame left arm at x=4.
 * It was 8 to leave room for a "> " caret that no longer exists -- the
 * selected row is inverted, which says it already. With the caret gone the
 * label starts here, and x=5..7 stopped being empty for no reason. */
export const MENU_LIST_X = 9, MENU_LIST_Y = 10, MENU_LIST_W = 111;

/* ---- Header / footer text rows ------------------------------------------
 *
 * TITLE_Y is the glyph row INSIDE the band. TITLE_RULE_Y is no longer a rule at
 * all — the band carries its own clear row — but the name is kept and now means
 * "the first row below the header band", which is what every call site used it
 * for. A real rule at the old y=12 would be drawn straight THROUGH the first
 * list row at y=10; header and list rect are one change.
 */
export const TITLE_Y = 1;
export const TITLE_RULE_Y = HEADER_H;
export const FOOTER_TEXT_Y = SCREEN_HEIGHT - 7;
export const FOOTER_RULE_Y = RULE_Y;

/* ---- List rows ----------------------------------------------------------
 *
 * ONE rect, MENU_LIST_Y (10) .. RULE_Y (55), whether or not a footer is on
 * screen. The old chrome reserved a shorter rect for the footer case and paid a
 * whole row for it: floor((54-15)/9) = 4 against floor((62-15)/9) = 5. The
 * movy footer rule is at 55 and the last row's highlight ends at 53, so the
 * same rect serves both — the footer case GAINS that row back (4 -> 5) and the
 * no-footer case holds at 5.
 */
export const LIST_TOP_Y = MENU_LIST_Y;
export const LIST_LINE_HEIGHT = 9;              /* 5x7px font + 2px spacing */
export const LIST_HIGHLIGHT_HEIGHT = LIST_LINE_HEIGHT;
export const LIST_HIGHLIGHT_OFFSET = 1;         /* shift the fill up 1px to centre it */
export const LIST_LABEL_X = MENU_LIST_X;
export const LIST_VALUE_X = 92;
