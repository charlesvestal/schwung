#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE HELP VIEWER IS ONE SCREEN, AND IT USED TO LOOK LIKE TWO.
#
# Reported from hardware, on the "Module Help" row added to a component's Module
# page: "we're using the wrong scrollbars" and "the footer shows back braids but
# that's not always true when you're up a menu".
#
# Both are the same shape of bug -- a second implementation of something the
# shared engine already does, drifting from it:
#
#   1. drawMenuList draws a SCROLLBAR (a dotted track with a solid thumb, one
#      column at x=126). scrollable_text.mjs -- the help DETAIL, one click
#      further in from the help LIST -- went on drawing the 5px ARROWS the bar
#      replaced. Same session, same jog, two idioms.
#
#   2. Both help draws computed "Back goes to" as `the frame BELOW the top one`.
#      That is right for a nested list and WRONG for the other two screens: a
#      DETAIL's Back lands on the frame it was opened from (not that frame's
#      parent), and the FIRST frame's Back leaves the viewer entirely -- for the
#      module, when the session came from a Module page.
#
# So this pins the ONE scrollbar (geometry, and that nothing else draws its own)
# and the footer's three cases. Pixel-level where pixels are the only evidence:
# the bar is set_pixel/fill_rect, which a source grep cannot see.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the help viewer chrome tests" >&2
  exit 1
fi

UI="src/shadow/shadow_ui.js"
[ -f "$UI" ] || { echo "FAIL: missing $UI" >&2; exit 1; }

# ============================================================================
# 1. ONE SCROLLBAR: nothing in the shipped shared/shadow draw paths draws its
#    own arrows, and scrollable_text goes through drawScrollbar.
# ============================================================================

# menu_layout.mjs still DEFINES drawArrowUp/Down (external modules may import
# them); the assertion is that no shipped draw path CALLS them any more.
arrow_callers=$(command grep -rln "drawArrow\(Up\|Down\)(" src/shared src/shadow 2>/dev/null \
  | command grep -v "^src/shared/menu_layout.mjs$" || true)
if [ -n "$arrow_callers" ]; then
  echo "FAIL: these shipped draw paths still draw scroll ARROWS instead of the shared bar:" >&2
  echo "$arrow_callers" >&2
  exit 1
fi
echo "  ok  no shared/shadow draw path calls drawArrowUp/drawArrowDown"

if ! command -v grep >/dev/null 2>&1; then :; fi
command grep -q "drawScrollbar" src/shared/scrollable_text.mjs \
  || { echo "FAIL: scrollable_text.mjs does not use the shared drawScrollbar" >&2; exit 1; }
command grep -q "^export function drawScrollbar" src/shared/menu_layout.mjs \
  || { echo "FAIL: drawScrollbar is not exported from menu_layout.mjs" >&2; exit 1; }
echo "  ok  drawScrollbar is exported from menu_layout.mjs and used by scrollable_text.mjs"

# The help viewer must ASK how many lines fit, not count them. A hard-coded 4 is
# what silently lost the fifth line when the pitch and the rect stopped agreeing,
# and it is invisible to the pixel probe below (which builds its own state).
help_sites=$(command grep -c "visibleLines: visibleLinesFor(LIST_TOP_Y, FOOTER_RULE_Y)" "$UI" || true)
[ "$help_sites" -eq 2 ] || {
  echo "FAIL: expected both help createScrollableText() sites to size themselves with "        "visibleLinesFor(LIST_TOP_Y, FOOTER_RULE_Y), found $help_sites" >&2; exit 1; }
if command grep -n "visibleLines: [0-9]" "$UI" >/dev/null 2>&1; then
  echo "FAIL: a help createScrollableText() site still hard-codes visibleLines:" >&2
  command grep -n "visibleLines: [0-9]" "$UI" >&2
  exit 1
fi
echo "  ok  both help detail sites size themselves from the rect (visibleLinesFor), none hard-coded"

# ============================================================================
# 2. BEHAVIOUR: drawScrollbar geometry, on real pixels.
# ============================================================================

node --input-type=module -e '
import { drawScrollbar } from "./src/shared/menu_layout.mjs";
import { drawScrollableText, createScrollableText, visibleLinesFor } from "./src/shared/scrollable_text.mjs";
import { LIST_LINE_HEIGHT } from "./src/shared/menu_layout.mjs";
import { LIST_TOP_Y, FOOTER_RULE_Y } from "./src/shared/list_geometry.mjs";

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };

const W = 128, H = 64;
let fb;
const reset = () => { fb = new Uint8Array(W * H); };
const put = (x, y, c) => {
  x = Math.round(x); y = Math.round(y);
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  fb[y * W + x] = c ? 1 : 0;
};
/* DEVICE_CTX resolves these globals at CALL time, which is the whole reason a
   probe like this can see anything at all. */
globalThis.set_pixel = put;
globalThis.fill_rect = (x, y, w, h, c) => {
  for (let yy = 0; yy < h; yy++) for (let xx = 0; xx < w; xx++) put(x + xx, y + yy, c);
};
globalThis.print = (x, y, text, c) => {
  /* A 5x7 block per glyph on a 6px pitch -- enough to prove the text and the
     bar do not collide, which is all this test asks of it. */
  for (let i = 0; i < String(text).length; i++)
    for (let yy = 0; yy < 7; yy++) for (let xx = 0; xx < 5; xx++) put(x + i * 6 + xx, y + yy, c);
};
globalThis.text_width = (t) => String(t).length * 6;

const column = (x) => { const ys = []; for (let y = 0; y < H; y++) if (fb[y * W + x]) ys.push(y); return ys; };
const litRight = () => { const xs = new Set(); for (let y = 0; y < H; y++) for (let x = 120; x < W; x++) if (fb[y*W+x]) xs.add(x); return [...xs].sort((a,b)=>a-b); };

/* --- a list that does not scroll draws NO bar, and gives up no column ------ */
reset();
drawScrollbar({ topY: 10, bottomY: 55, rowHeight: 9, rowInk: 7, windowRows: 5, total: 5, startIdx: 0 });
if (column(126).length !== 0) fail("a fully visible list must draw no scrollbar, got " + JSON.stringify(column(126)));

/* --- the track covers the ROWS, not the rect ------------------------------ */
reset();
drawScrollbar({ topY: 10, bottomY: 55, rowHeight: 9, rowInk: 7, windowRows: 5, total: 20, startIdx: 0 });
const track = column(126);
if (!track.length) fail("a scrolling list must draw a scrollbar");
if (track[0] !== 10) fail("the track must start at topY, starts at " + track[0]);
/* topY + (windowRows-1)*rowHeight + rowInk = 10 + 36 + 7 = 53, exclusive. */
if (track[track.length - 1] > 52)
  fail("the track must not overhang the last row of ink (<=52), ends at " + track[track.length - 1]);
if (track[track.length - 1] < 45)
  fail("the track must reach the last row of ink, ends at " + track[track.length - 1]);

/* --- the thumb reports POSITION ------------------------------------------- */
const thumbSpan = (startIdx, total, windowRows) => {
  reset();
  drawScrollbar({ topY: 10, bottomY: 55, rowHeight: 9, rowInk: 7, windowRows, total, startIdx });
  /* The thumb is the solid run; the track is dotted (every other row). */
  const ys = column(126);
  let best = null, run = null;
  for (const y of ys) {
    if (run && y === run.end + 1) run.end = y;
    else run = { start: y, end: y };
    if (!best || (run.end - run.start) > (best.end - best.start)) best = { ...run };
  }
  return best;
};
const top = thumbSpan(0, 20, 5);
const bottom = thumbSpan(15, 20, 5);
if (!(top.start < bottom.start)) fail("the thumb must move down as the list scrolls: " + JSON.stringify([top, bottom]));
if (top.start !== 10) fail("at the top of the list the thumb must sit at topY, got " + top.start);

/* --- the 2px thumb FLOOR --------------------------------------------------- */
const tiny = thumbSpan(0, 400, 5);
if ((tiny.end - tiny.start + 1) < 2)
  fail("the thumb has a 2px floor (a 1px thumb is a tick of the track), got " +
       (tiny.end - tiny.start + 1) + "px for 5 of 400");

/* --- THE TEXT AREA FITS THE SAME ROWS THE LIST DOES ----------------------- */
/* The help LIST and the help DETAIL draw into the same rect (LIST_TOP_Y ..
   FOOTER_RULE_Y). The text pitch was 10px against the list is 9, so the list
   fitted five rows there and the text fitted four -- one line of help thrown
   away per screen, and the reason the layout read as wrong. */
if (LIST_LINE_HEIGHT !== 9)
  fail("the shared list pitch moved (" + LIST_LINE_HEIGHT + ") -- the numbers below are stale");
const fits = visibleLinesFor(LIST_TOP_Y, FOOTER_RULE_Y);
if (fits !== 5)
  fail("the help rect (" + LIST_TOP_Y + ".." + FOOTER_RULE_Y + ") must hold 5 lines of text, " +
       "the same as the list holds rows -- got " + fits);
/* ...and the last line must be INSIDE the rect, not clipped by the footer rule:
   topY + (fits-1)*pitch + ink. */
const lastInk = LIST_TOP_Y + (fits - 1) * LIST_LINE_HEIGHT + 7;
if (lastInk > FOOTER_RULE_Y)
  fail("the " + fits + "th line of text runs into the footer rule (ink ends " + lastInk +
       ", rule at " + FOOTER_RULE_Y + ")");

/* --- HELP DETAIL: the bar, and no arrows, and no collision with the text --- */
const lines = [];
for (let i = 0; i < 18; i++) lines.push("line " + i + " of the help");
const state = createScrollableText({ lines, actionLabel: "Back", visibleLines: fits });

reset();
drawScrollableText({ state, topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y, actionY: -1 });
/* All five lines are actually drawn -- ink on the last row, not just room for
   it. A row count the draw loop ignores would otherwise pass everything above. */
{
  const inkRows = new Set();
  for (let y = 0; y < H; y++) for (let x = 0; x < 120; x++) if (fb[y * W + x]) { inkRows.add(y); break; }
  const lastRowY = LIST_TOP_Y + (fits - 1) * LIST_LINE_HEIGHT;
  if (!inkRows.has(lastRowY))
    fail("the " + fits + "th line of text is not drawn (no ink at y=" + lastRowY + ")");
}
const barCol = column(126);
if (!barCol.length) fail("the help detail must draw the shared scrollbar");
/* The old arrows lived at x=122..126 and were 3 rows tall at each end. The bar
   owns ONE column, so anything lit at 122..125 is an arrow that came back. */
const right = litRight();
if (right.some((x) => x >= 122 && x <= 125))
  fail("the help detail is drawing in the old arrow column (122..125): " + JSON.stringify(right));
/* Text at x=4, 20 chars max on a 6px pitch, ends at 124 -- but these lines are
   short, so the real assertion is that the bar column is never text. */
if (!barCol.length || barCol[0] !== LIST_TOP_Y)
  fail("the help detail track must start at topY, got " + JSON.stringify(barCol.slice(0, 3)));
if (barCol[barCol.length - 1] > lastInk - 1)
  fail("the help detail track must not overhang the last line of ink (<=" + (lastInk - 1) +
       "), ends at " + barCol[barCol.length - 1]);

/* Text that FITS gets no bar at all. */
reset();
const shortState = createScrollableText({ lines: ["one", "two"], actionLabel: "Back", visibleLines: fits });
drawScrollableText({ state: shortState, topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y, actionY: -1 });
if (column(126).length !== 0) fail("help text that fits on one screen must draw no scrollbar");

if (failures) process.exit(1);
console.log("  ok  drawScrollbar: no bar when everything fits; track covers the rows and not " +
            "the rect; thumb tracks position with a 2px floor; the help DETAIL draws it and " +
            "nothing in the old arrow column; the text area holds the SAME 5 rows the list " +
            "does, and draws all of them");
'

# ============================================================================
# 3. BEHAVIOUR: helpBackTarget names the screen Back actually reaches.
# ============================================================================

node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const re = new RegExp("^function helpBackTarget\\([^]*?^}", "m");
const m = src.match(re);
if (!m) fail("could not lift helpBackTarget() out of shadow_ui.js");

const run = (stack, fromComponent, inDetail) => {
    const body = [
        "let helpNavStack = " + JSON.stringify(stack) + ";",
        "let componentHelpReturnSlot = " + (fromComponent ? 2 : -1) + ";",
        m[0],
        "return helpBackTarget(" + (inDetail ? "true" : "false") + ");",
    ].join("\n");
    try { return new Function(body)(); }
    catch (e) { fail("helpBackTarget behaviour: " + e.message); }
};

const BRAIDS = { title: "Braids" };
const CONTROLS = { title: "Controls" };
const HELP = { title: "Help" };

// A DETAIL returns to the list it was opened from -- the TOP frame, not its
// parent. This is the reported "shows back braids" case: opened from Controls,
// footer said Braids.
if (run([BRAIDS, CONTROLS], true, true) !== "Controls")
    fail("a help DETAIL must name the list it was opened from (Controls), got " +
         run([BRAIDS, CONTROLS], true, true));

// THE MODULE NAME IS RESERVED FOR THE BACK THAT REALLY LEAVES.
//
// The first frame of a Module Help session is TITLED with the module, so before
// this the name meant two destinations on adjacent screens: at the top it left
// for the module, one level in it returned to the topic list -- and the header
// said the module name on the first of those anyway. Reported from hardware:
// "the top level is the module name, so its confusing it stays the same."
if (run([BRAIDS], true, true) !== "List")
    fail("a DETAIL opened from the first frame of a Module Help session must say List " +
         "(the module name means LEAVING), got " + run([BRAIDS], true, true));
if (run([BRAIDS, CONTROLS], true, false) !== "List")
    fail("a nested list whose parent is the first frame of a Module Help session must " +
         "say List, got " + run([BRAIDS, CONTROLS], true, false));

// A nested LIST deeper in still names its real parent.
const DEEP = [BRAIDS, CONTROLS, { title: "Knobs" }];
if (run(DEEP, true, false) !== "Controls")
    fail("a list deeper than one level must name its parent frame, got " + run(DEEP, true, false));
if (run(DEEP, true, true) !== "Knobs")
    fail("a detail deeper than one level must name the list it was opened from, got " +
         run(DEEP, true, true));

// The FIRST frame LEAVES the viewer -- and THAT is where the module name goes.
if (run([BRAIDS], true, false) !== "Braids")
    fail("the first frame of a Module Help session must name the MODULE (Back leaves for " +
         "its knob grid), got " + run([BRAIDS], true, false));
if (run([HELP], false, false) !== "Settings")
    fail("the first frame of a [Help...] session must name Settings, got " + run([HELP], false, false));

// ...and the component return is what tells the two sessions apart, not the
// frame title. A [Help...] session keeps its real titles: "Help" collides with
// nothing and is a truer label than "List".
if (run([HELP], false, true) !== "Help")
    fail("a detail in a [Help...] session must still name its own list, got " + run([HELP], false, true));
if (run([HELP, CONTROLS], false, false) !== "Help")
    fail("a nested list in a [Help...] session must name its parent (Help), got " +
         run([HELP, CONTROLS], false, false));

console.log("  ok  helpBackTarget(): the module name is reserved for the Back that LEAVES; one " +
            "level in says List; deeper frames name their real parent; a [Help...] session " +
            "keeps its own titles and exits to Settings");
' "$UI"

# ============================================================================
# 4. BEHAVIOUR: the header says WHAT the screen is, and is fitted in PIXELS.
# ============================================================================

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { fontWidth4x5 } from "./src/shared/param_pages/font4x5.mjs";

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
const q = (t) => JSON.stringify(t);

/* ---- the rule -------------------------------------------------------- */
const m = src.match(/^function helpHeaderTitle\([^]*?^}/m);
if (!m) { console.log("FAIL: could not lift helpHeaderTitle() out of shadow_ui.js"); process.exit(1); }
const run = (stack, fromComponent) => {
    const body = [
        "let helpNavStack = " + JSON.stringify(stack) + ";",
        "let componentHelpReturnSlot = " + (fromComponent ? 2 : -1) + ";",
        m[0],
        "return helpHeaderTitle(helpNavStack[helpNavStack.length - 1]);",
    ].join("\n");
    return new Function(body)();
};
const BRAIDS = { title: "Braids" }, CONTROLS = { title: "Controls" }, HELP = { title: "Help" };

/* The first frame of a Module Help session is the module TOPIC LIST, and the
   bare module name is the same word the knob grid it was opened from wears --
   so the screen said what it was ABOUT, not what it WAS. */
if (run([BRAIDS], true) !== "Help: Braids")
    fail("the first frame of a Module Help session must be headed " + q("Help: <module>") +
         ", got " + q(run([BRAIDS], true)));
/* A nested frame is headed with its own topic, which is already unambiguous. */
if (run([BRAIDS, CONTROLS], true) !== "Controls")
    fail("a nested help frame must be headed with its own topic, got " + q(run([BRAIDS, CONTROLS], true)));
/* A [Help...] session is literally titled "Help" -- prefixing it says it twice. */
if (run([HELP], false) !== "Help")
    fail("a [Help...] session must keep its own title, got " + q(run([HELP], false)));

/* ---- and the cap that would have eaten it ---------------------------- */
/* NO CHAR CAP. drawHeader fits the left side to the bar in PIXELS
   (fitText/FONT4_MEASURE), measuring the right side first and giving the left
   the remainder; with no right side that is W - 4 = 124px. A truncateText(_, 18)
   in front of it is a second, blinder truncation -- and 12 of the 133 modules
   installed on the device at the time have a "Help: <name>" longer than 18
   characters while every one of them FITS. */
/* Scoped to the two HELP draws: other screens still cap by characters and are
   out of scope here -- a repo-wide grep would have failed on those instead of
   on the thing this pins. */
for (const fn of ["drawHelpList", "drawHelpDetail", "helpHeaderTitle"]) {
    const b = src.match(new RegExp("^function " + fn + "\\([^]*?^}", "m"));
    if (!b) { fail("could not lift " + fn + "() out of shadow_ui.js"); continue; }
    if (/truncateText\(/.test(b[0]))
        fail(fn + "() still pre-truncates to a character count -- the header face is " +
             "proportional and drawHeader already fits it in pixels");
}

const BUDGET = 128 - 4;
/* The widest real names measured across the 133 installed modules. Fixture, not
   a device query: the assertion is about the FONT, which needs no hardware. */
const WIDEST = ["V8 tuneSample Slicer", "Custom MIDI Control", "PulsesArpeggiator", "Junologue Chorus"];
for (const n of WIDEST) {
    const w = fontWidth4x5(("Help: " + n).toUpperCase());
    if (w > BUDGET) fail(q("Help: " + n) + " measures " + w + "px against a " + BUDGET + "px bar");
    if (("Help: " + n).length <= 18)
        fail("fixture " + q(n) + " is not long enough to prove the old 18-char cap was wrong");
}

if (failures) process.exit(1);
console.log("  ok  helpHeaderTitle(): " + q("Help: <module>") + " on a Module Help root, the " +
            "topic name deeper in, " + q("Help") + " for a [Help...] session; and no char cap " +
            "in front of the pixel fit (the widest fleet name is 118px against 124)");
'

echo "PASS"
