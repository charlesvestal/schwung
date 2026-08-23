#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE ENUM OPTION PICKER WEARS THE KNOB GRID'S CHROME.
#
# The picker is reached by holding an enum knob in the movy grid and clicking.
# It shipped drawing the generic device list chrome instead -- drawHeader's big
# 5x7 title plus a rule, and NO footer at all -- so the screen you land on from
# the grid looked like it belonged to a different product, and the two gestures
# it does have (jog scrolls, click sets) were named nowhere. A user looking at it
# on hardware asked why.
#
# WHY THIS IS PINNED ON PIXELS AND NOT ON CODE.
#
# The footer is set in font4x5, which draws every glyph as fillRect pixels: a
# recording print() sees nothing at all, so "does this screen have a footer" is
# not a question the source can answer. The header is the same. The header and
# footer bands are therefore compared against a REFERENCE render of
# render_page_movy's own drawHeader/drawFooter -- byte for byte, not "has some
# lit pixels" -- so the assertion is "it is the movy band", not "it is a band".
#
# THE ROW COUNT IS THE PART THAT SILENTLY BREAKS.
#
# The movy bands take the top and the bottom of a 64px screen. drawMenuList's
# DEFAULTS (top 15, indicator row 62) would put the last row and the down-arrow
# straight through the footer, and the device clips silently -- the picker would
# simply lose its last option with nothing to say it had. So this asserts the
# visible row COUNT (5, unchanged from the old chrome) and clipped() === 0, over
# a range of list lengths and scroll positions, not just one render.
#
# It also pins the things this change was NOT allowed to touch: the list is
# still menu_layout's drawMenuList (one list widget, which is what Master FX and
# the chain editor drifted apart for want of), the `*` still marks the option
# currently SET, the draw path still announces nothing of its own (the screen
# emits its own richer TTS from openEnumPicker/enumPickerJog), and the draw path
# still performs NO parameter reads.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the enum picker chrome tests" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import * as RM from "./src/shared/param_pages/render_page_movy.mjs";
import { LIST_LABEL_X, LIST_VALUE_X, LIST_LINE_HEIGHT, drawMenuList }
  from "./src/shared/menu_layout.mjs";

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };

/* ------------------------------------------------------------------ lifting */
/* Same lift as test_chain_editor_snapshot.sh / test_chain_edit_read_budget.sh:
   pull the top-level function out of shadow_ui.js -- which cannot be imported,
   being full of host globals and device-absolute import paths -- and hand it
   its dependencies as parameters, so what runs is the REAL body.

   The dependency list is EXPLICIT and the trap is documented in
   test_chain_edit_read_budget.sh: a free identifier under the lift is a
   ReferenceError, and the tempting fix -- a typeof guard -- makes a whole block
   silently unreachable, leaving the test measuring a screen with the feature
   switched off. Nothing is guarded here; a missing name throws. */
const uiSrc = readFileSync("src/shadow/shadow_ui.js", "utf8");
const ppSrc = readFileSync("src/shadow/shadow_ui_param_pages.mjs", "utf8");
/* The DRAW moved to enum_list.mjs, so that drawEnumPicker and the knob grid`s
   turn-raised PEEK are one screen -- they have opposite commit semantics (the
   peek`s detent has already written, so its Back has nothing to cancel) and so
   cannot be one view. Everything this file asserts is unchanged; it just has
   two source files to look in now. enum_list.mjs imports through the DEVICE
   absolute paths and cannot be imported under node, so it is lifted like the
   rest. */
const elSrc = readFileSync("src/shared/param_pages/enum_list.mjs", "utf8");
function liftFrom(src, what, name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone from " + what); return () => () => {}; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name + " in " + what); return () => () => {}; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}
const lift = (name, deps) => liftFrom(uiSrc, "shadow_ui.js", name, deps);

/* The hints are built in shadow_ui_param_pages.mjs, next to footerHints(), so
   the wording stays in one place -- but that file imports through the DEVICE
   absolute paths (/data/UserData/schwung/...) and cannot be imported under
   node either. Lifted the same way, with the REAL orderedHints under it, so
   what is asserted is the ordering rule the grid`s own footer obeys. */
const orderedHints = liftFrom(ppSrc, "shadow_ui_param_pages.mjs", "orderedHints", [])();
const enumPickerFooterHints = liftFrom(ppSrc, "shadow_ui_param_pages.mjs",
  "enumPickerFooterHints", ["orderedHints"])(orderedHints);

/* The two rect constants are read out of the source rather than restated, so a
   future change to them moves this test`s expectations with it instead of
   quietly disagreeing. */
function constOf(src, what, name) {
  const m = src.match(new RegExp("const\\s+" + name + "\\s*=\\s*([^;]+);"));
  if (!m) { fail(what + " no longer defines " + name); return null; }
  return m[1].trim();
}
const TOP_Y = Number(constOf(elSrc, "enum_list.mjs", "ENUM_LIST_TOP_Y"));
if (!isFinite(TOP_Y)) fail("ENUM_LIST_TOP_Y is not a literal any more");
if (constOf(elSrc, "enum_list.mjs", "ENUM_LIST_BOTTOM_Y") !== "RULE_Y - 1")
  fail("the list bottom must be DERIVED from the footer rule, not restated -- " +
       "a second copy of 54 is how the list comes to overrun the footer again");
const BOTTOM_Y = RM.RULE_Y - 1;

/* The shared draw, lifted with the real widgets under it. */
const LIST_DEPS = ["drawHeader", "drawFooter", "drawMenuList", "LIST_LABEL_X",
  "ENUM_LIST_TOP_Y", "ENUM_LIST_BOTTOM_Y"];
const drawEnumList = liftFrom(elSrc, "enum_list.mjs", "drawEnumList", LIST_DEPS)(
  RM.drawHeader, RM.drawFooter, drawMenuList, LIST_LABEL_X, TOP_Y, BOTTOM_Y);

/* drawEnumPicker is now the CALLER: it supplies the title, the two indices and
   the footer, and delegates the drawing. That is the whole change this file
   had to absorb -- the header word, the `*` and the rect all still come out
   the same, which is what the pixel sections below check. */
const DRAW_DEPS = ["clear_screen", "fill_rect", "print", "text_width",
  "drawEnumList", "enumPickerFooterHints", "enumPickerTitle",
  "enumPickerOptions", "enumPickerIndex", "enumPickerOpenIndex"];
const mkDraw = lift("drawEnumPicker", DRAW_DEPS);

/* ----------------------------------------------------------------- rendering */

const GLOBAL_NAMES = ["print", "fill_rect", "set_pixel", "text_width",
  "shadow_get_param", "shadow_get_display_mode", "host_send_screenreader"];

/* menu_layout.mjs reaches for print / fill_rect / set_pixel by NAME, exactly as
   it does on the device, so they have to be real globals rather than deps. */
function render(c) {
  const fb = createFramebuffer();
  const reads = [], spoken = [];
  const g = {
    print: fb.print,
    fill_rect: fb.fillRect,
    set_pixel: fb.setPixel,
    text_width: fb.textWidth,
    /* A read here is a bug, not a value to supply: the picker resolved
       everything it shows once, at open, precisely so the draw path is free of
       IPC. Recorded rather than thrown so the message names the key. */
    shadow_get_param: (k) => { reads.push(String(k)); return ""; },
    shadow_get_display_mode: () => 1,
    host_send_screenreader: (t) => { spoken.push(String(t)); },
  };
  for (const k of GLOBAL_NAMES) globalThis[k] = g[k];
  try {
    mkDraw(() => fb.clearScreen(), fb.fillRect, fb.print, fb.textWidth,
      drawEnumList, enumPickerFooterHints,
      c.title, c.options, c.index, c.openIndex)();
  } finally {
    for (const k of GLOBAL_NAMES) delete globalThis[k];
  }
  return { fb, reads, spoken };
}

const at = (fb, x, y) => fb.pixels[y * fb.width + x];
function band(fb, y0, y1) {
  const out = [];
  for (let y = y0; y <= y1; y++)
    for (let x = 0; x < fb.width; x++) out.push(at(fb, x, y));
  return out.join("");
}
function litIn(fb, x0, x1, y0, y1) {
  let n = 0;
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) if (at(fb, x, y)) n++;
  return n;
}

const OPTS = ["Hall", "Room", "Plate", "Cave", "Chamber", "Spring",
              "Shimmer", "Gated", "Reverse", "Ambience", "Church", "Tunnel"];
const TITLE = "Reverb Mode";

/* ======================================================================= 1 ==
 * THE HEADER IS THE MOVY BAND -- compared against the movy band itself.
 */
{
  const { fb } = render({ title: TITLE, options: OPTS, index: 0, openIndex: 3 });

  const ref = createFramebuffer();
  RM.drawHeader(drawContext(ref), TITLE, "SELECT", false);

  /* Rows 0..7: the 7-row band plus the clear row under it, everything above the
     first list row`s highlight. */
  if (band(fb, 0, 7) !== band(ref, 0, 7))
    fail("the picker`s top band is not render_page_movy`s drawHeader. It drew " +
         "the generic device chrome (drawHeader`s 5x7 title + a rule at y=12), " +
         "which is what made the screen you reach from the knob grid look like " +
         "a different product.");

  /* And the OLD chrome is gone, named explicitly: a full-width rule at
     TITLE_RULE_Y is its signature. Rendered with the selection on the THIRD
     row, because a selected first row inverts a full-width band across rows
     8..16 and would light row 12 whether the old rule were there or not. */
  const { fb: fb2 } = render({ title: TITLE, options: OPTS, index: 2, openIndex: 3 });
  let ruleRow12 = 0;
  for (let x = 0; x < 128; x++) if (at(fb2, x, 12)) ruleRow12++;
  if (ruleRow12 > 100)
    fail("row 12 is still a full-width rule -- the old drawHeader title rule is " +
         "being drawn under the movy band");
}

/* ======================================================================= 2 ==
 * THE FOOTER EXISTS, IS THE MOVY FOOTER, AND SAYS WHAT THE CONTROLS DO.
 */
{
  const hints = enumPickerFooterHints();
  /* Words first: they cannot be read back off the framebuffer (font4x5 draws
     glyphs as fillRect pixels), so the wording is asserted on the list and the
     PIXELS are asserted against a render of that same list. */
  const keys = hints.map((h) => h[0]);
  if (keys[0] !== "JOG" || keys[1] !== "CLK")
    fail("hints must be ordered jog first, click second, like every other " +
         "movy footer; got " + JSON.stringify(keys));
  for (const [k] of hints)
    if (!RM.FOOTER_CANON.keys.includes(k))
      fail(k + " is not a key in FOOTER_CANON -- the keys name physical " +
           "controls and are fixed by the hardware, not by taste");
  const back = hints.find((h) => h[0] === "BACK");
  if (!back) fail("no BACK hint: the picker`s only cancel is unnamed");
  else if (!RM.FOOTER_CANON.backActions.includes(back[1]))
    fail("BACK says " + JSON.stringify(back[1]) + "; the canon is EXIT (leaves " +
         "the view) or OUT (rises a level). The picker leaves the view.");
  else if (back[1] !== "EXIT")
    fail("BACK should be EXIT here -- it leaves the picker entirely and lands " +
         "back on the editor, the same as the module picker`s BACK");
  /* Three pairs only fit while every word is <= 4 characters; drawFooter drops
     what does not fit rather than squeezing, so a 5-letter action silently
     costs a hint. */
  for (const [k, a] of hints)
    if (a.length > 4) fail("hint action " + JSON.stringify(a) + " is over 4 " +
      "characters; drawFooter would drop a pair rather than squeeze it");

  const { fb } = render({ title: TITLE, options: OPTS, index: 0, openIndex: 3 });
  const ref = createFramebuffer();
  RM.drawFooter(drawContext(ref), hints);
  if (band(fb, RM.RULE_Y, 63) !== band(ref, RM.RULE_Y, 63))
    fail("rows " + RM.RULE_Y + "..63 are not render_page_movy`s drawFooter of " +
         "the declared hints. Before this change the picker had NO footer at " +
         "all, so its two gestures were named nowhere.");

  /* All three pairs actually landed. drawFooter returns how many it drew, but
     from the pixels: the reference is the authority and it matched above, so
     assert the reference itself did not silently drop one. */
  const drawn = RM.drawFooter(drawContext(createFramebuffer()), hints);
  if (drawn !== hints.length)
    fail("drawFooter drew " + drawn + " of " + hints.length + " hints -- the " +
         "row is too wide and a pair was dropped");
}

/* ======================================================================= 3 ==
 * NOTHING CLIPS, AND THE LIST STILL SHOWS FIVE OPTIONS.
 *
 * This is the assertion the change was most likely to break silently. The
 * bands take the top and bottom of the screen; left on drawMenuList`s defaults
 * the last row and the down-arrow run through the footer and the device
 * discards those pixels without a word.
 */
{
  /* A row is "visible" if anything is lit in the label column across its 7
     glyph rows. Derived from the rect the source declares, so the count and the
     geometry cannot disagree. */
  function visibleRows(fb) {
    let n = 0;
    for (let k = 0; k < 8; k++) {
      const y = TOP_Y + k * LIST_LINE_HEIGHT;
      /* Stop at the list rect, not at the screen. Past it lies the footer,
         whose hints sit in the same label column -- counting those as options
         is how a footer that ate a row would still look like five. */
      if (y + 6 > BOTTOM_Y) break;
      if (litIn(fb, LIST_LABEL_X, LIST_LABEL_X + 60, y, y + 6)) n++;
    }
    return n;
  }

  /* CAPACITY is what the chrome change could take away: how many rows fit
     between the header and the footer rule. drawMenuList computes it exactly
     this way from the rect it is given. Five is what the old chrome (top 15,
     indicator row 62) held, and five is the bar. */
  const CAPACITY = Math.max(1, Math.floor((BOTTOM_Y - TOP_Y) / LIST_LINE_HEIGHT));
  const OLD_CAPACITY = Math.max(1, Math.floor((62 - 15) / LIST_LINE_HEIGHT));
  if (CAPACITY !== OLD_CAPACITY)
    fail("the movy bands cost the list a row: " + OLD_CAPACITY + " options fit " +
         "under the old chrome, " + CAPACITY + " fit now. The rect is " + TOP_Y +
         ".." + BOTTOM_Y + ". MENU_LIST_Y (10) is one row too low here -- this " +
         "header is not inverted, so the list can start at 9.");

  const cases = [
    { what: "top of a long list",    index: 0,  openIndex: 3,  options: OPTS },
    { what: "scrolled mid-list",     index: 6,  openIndex: 0,  options: OPTS },
    { what: "last option selected",  index: 11, openIndex: 11, options: OPTS },
    { what: "exactly five options",  index: 4,  openIndex: 1,  options: OPTS.slice(0, 5) },
    { what: "two options",           index: 1,  openIndex: 0,  options: ["Off", "On"] },
    { what: "one option",            index: 0,  openIndex: 0,  options: ["Only"] },
    { what: "very long option names", index: 2, openIndex: 2,
      options: OPTS.map((o) => o + " Extra Long Name That Overruns") },
    { what: "very long title",       index: 0,  openIndex: 0,  options: OPTS,
      title: "An Absurdly Long Parameter Name For The Header Band" },
  ];

  let anyFail = false;
  for (const c of cases) {
    const { fb } = render({ title: c.title || TITLE, options: c.options,
                            index: c.index, openIndex: c.openIndex });
    if (fb.clipped() !== 0) {
      fail("[" + c.what + "] drew " + fb.clipped() + " pixels off the 128x64 " +
           "display. The device discards them silently, so an overrun here is " +
           "invisible on hardware until an option goes missing.");
      anyFail = true;
    }
    /* drawMenuList`s own windowing rule, restated so the expectation is not
       "5" -- at the END of a list it legitimately shows four, because
       keepOffLastRow holds the selection one row off the bottom. That is
       unchanged from the old chrome and is not what this test is hunting. The
       CAPACITY assertion above is what pins the row the bands could have
       eaten. */
    const start = Math.max(0, c.index - (CAPACITY - 2));
    const want = Math.min(start + CAPACITY, c.options.length) - start;
    const got = visibleRows(fb);
    if (got !== want) {
      fail("[" + c.what + "] shows " + got + " option rows, expected " + want +
           ". The old chrome showed five; a movy band that eats one is the " +
           "regression this file exists for.");
      anyFail = true;
    }
    /* Nothing may be drawn INTO the footer band by the list -- that is how the
       fifth row would be "visible" and still unreadable. */
    if (litIn(fb, 0, 127, RM.RULE_Y - 1, RM.RULE_Y - 1) > 100) {
      fail("[" + c.what + "] the list drew a full-width run on row " +
           (RM.RULE_Y - 1) + ", immediately above the footer rule");
      anyFail = true;
    }
  }
  if (!anyFail) console.log("PASS: five options, no clipping, across 8 list states");
}

/* ======================================================================= 4 ==
 * THE LIST BODY IS UNCHANGED: still drawMenuList, still marking what is SET.
 */
{
  /* The source still calls the shared widget. One list widget is a hard
     requirement -- a second one is exactly how Master FX and the chain editor
     came to look like different products. */
  const at0 = elSrc.indexOf("function drawEnumList(");
  const body = elSrc.slice(at0, elSrc.indexOf("\n}\n", at0));
  if (!/drawMenuList\(/.test(body))
    fail("drawEnumList no longer calls drawMenuList -- this change was chrome " +
         "only, and a second list widget is the drift it must not cause");
  if (!/announce:\s*false/.test(body))
    fail("drawEnumList stopped passing announce:false -- the screen emits its " +
         "own richer TTS (\"Room, 2 of 17\"), so the list must not also say " +
         "\"Room: *\"");

  /* And drawEnumPicker must still go THROUGH it. Extracting the draw only buys
     one screen for as long as both entries use the extraction; a picker that
     grew its own drawMenuList back would be the two-list-widgets drift with an
     extra file. */
  const pAt = uiSrc.indexOf("function drawEnumPicker(");
  const pBody = uiSrc.slice(pAt, uiSrc.indexOf("\n}\n", pAt));
  if (!/drawEnumList\(/.test(pBody))
    fail("drawEnumPicker no longer delegates to drawEnumList -- the peek and " +
         "the picker have to stay one screen");
  if (/drawMenuList\(/.test(pBody))
    fail("drawEnumPicker draws its own list again -- that is the second list " +
         "widget the extraction exists to prevent");

  /* The `*`, on the pixels. Chosen so the marked row is NOT the selected one:
     a selected row is inverted full-width, which would light the value column
     whether the mark were drawn or not. */
  const { fb } = render({ title: TITLE, options: OPTS, index: 0, openIndex: 3 });
  const rowY = (k) => TOP_Y + k * LIST_LINE_HEIGHT;
  const markLit = (k) => litIn(fb, LIST_VALUE_X, LIST_VALUE_X + 8, rowY(k), rowY(k) + 6);

  if (!markLit(3))
    fail("the option currently SET (row 3) has no `*` in the value column. " +
         "Without it, scrolling away from the live value reads as nothing.");
  for (const k of [1, 2, 4])
    if (markLit(k)) fail("row " + k + " drew a mark and is not the set option");
}

/* ======================================================================= 5 ==
 * THE DRAW PATH READS NO PARAMETERS AND ANNOUNCES NOTHING.
 */
{
  const { reads, spoken } = render({ title: TITLE, options: OPTS, index: 2, openIndex: 3 });
  if (reads.length)
    fail("the draw path performed " + reads.length + " parameter read(s) (" +
         reads.slice(0, 3).join(", ") + "). A read is ~2.8ms against a 1.68ms " +
         "whole-page render; everything this screen shows was resolved once, at " +
         "open, on purpose.");
  if (spoken.length)
    fail("the draw path announced " + JSON.stringify(spoken.slice(0, 2)) + ". " +
         "The picker announces from openEnumPicker/enumPickerJog, where it can " +
         "say \"Room, 2 of 17\"; a draw-path announcement would double it.");
  if (failures === 0) console.log("PASS: no IPC and no TTS on the draw path");
}

if (failures) process.exit(1);
console.log("PASS: the enum picker wears the movy header and footer, from both " +
            "entry points, without losing a row");
'
