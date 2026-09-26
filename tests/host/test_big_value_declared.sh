#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A PARAM MAY ASK FOR THE BIG FACE, WHATEVER ITS VALUE LOOKS LIKE.
#
# isCountedQuantity already concedes the principle -- some numbers are read,
# not aimed -- but the only way in is a name this file happens to recognise,
# enums are refused outright, and the face could only spell integers. So a
# swing percentage, a clip length and a 2:4 trig condition, which are all the
# same shape, could not ask.
#
# The guard moves with it: three digits was a proxy for "does it fit", and once
# the text is arbitrary the honest test is the measured width of the WIDEST
# thing the cell can show, so a cell cannot change widget as it is turned.
#
# Defaults first and last: nothing undeclared moves (the fleet baseline says so
# for the whole fixture; this says it for the two gates by name).
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the display:big test" >&2
  exit 1
fi

node --input-type=module -e '
import { shouldDrawBigNumber, bigValueText, widgetKindFor, drawKnobWidget,
         WIDGET_BIGNUM, WIDGET_ENUM, WIDGET_KNOB, CELL_W }
    from "./src/shared/param_pages/render_page_movy.mjs";
import { fontWidth, missingGlyphs } from "./src/shared/param_pages/font_big_num.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

/* DEFAULTS: the span cap and the enum refusal are unchanged. */
const wideInt = { type: "int", kind: "number", key: "cutoff", name: "Cutoff", min: 0, max: 127, step: 1 };
if (shouldDrawBigNumber(wideInt)) bad("a wide int started drawing big without asking");
if (widgetKindFor(wideInt) !== WIDGET_KNOB) bad("a wide int stopped being a knob");
const plainEnum = { type: "enum", kind: "enum", key: "shape", name: "Shape", options: ["1:1", "1:2"] };
if (shouldDrawBigNumber(plainEnum)) bad("an enum started drawing big without asking");
if (widgetKindFor(plainEnum) !== WIDGET_ENUM) bad("an undeclared enum stopped being an enum square");

/* DECLARED: both gates yield. */
const swing = { ...wideInt, key: "swing", name: "Swing", min: 50, max: 80, display: "big" };
const swingWide = { ...swing, min: 0, max: 100 };
if (!shouldDrawBigNumber(swingWide)) bad("display:big did not lift the span cap");
if (widgetKindFor(swingWide) !== WIDGET_BIGNUM) bad("display:big did not reach the widget");
const cond = { ...plainEnum, key: "cond", name: "Condition", options: ["1:1", "1:2", "2:2", "3:4"], display: "big" };
if (!shouldDrawBigNumber(cond)) bad("display:big did not lift the enum refusal");
if (widgetKindFor(cond) !== WIDGET_BIGNUM) bad("a declared enum did not draw big");

/* A WRITE-ONLY or OPAQUE param is never a number, declared or not. */
if (widgetKindFor({ ...cond, writeOnly: true }) === WIDGET_BIGNUM) bad("a trigger drew big");
if (widgetKindFor({ ...cond, kind: "opaque" }) === WIDGET_BIGNUM) bad("an opaque param drew big");

/* THE TEXT: the host reading first, then the option, then the number. */
if (bigValueText(cond, "3", null) !== "3:4") bad("a declared enum drew " + bigValueText(cond, "3", null) + ", not its option");
if (bigValueText(cond, "2:2", null) !== "2:2") bad("an enum reported by NAME did not resolve");
if (bigValueText(swing, "54", "54%") !== "54%") bad("a declared int dropped the host reading it was given");
if (bigValueText(swing, "54", null) !== "54") bad("a declared int with no reading stopped drawing its number");
if (bigValueText(swing, null, null) !== "--") bad("an unread value stopped drawing --");
if (bigValueText(cond, null, "3:4") !== "--") bad("an unread value drew a host reading");
/* A reading the face cannot spell, or that does not fit, falls back rather
 * than drawing a hole or a smear. */
if (bigValueText(swing, "54", "54 pct") !== "54") bad("an unspellable host reading was drawn");
if (bigValueText(swing, "54", "5555%") !== "54") bad("a too-wide host reading was drawn");
const shortCond = { ...cond, short_options: ["1:1", "1:2", "2:2", "3/4"] };
if (bigValueText(shortCond, "3", null) !== "3/4") bad("short_options did not win for the big cell");

/* THE FIT is asked of the WIDEST thing the cell can show, so a declaration
 * that cannot fit keeps the widget it would otherwise have had. */
const longEnum = { ...cond, options: ["1:1", "Mixolydian"] };
if (widgetKindFor(longEnum) === WIDGET_BIGNUM) bad("an option the face cannot spell was still drawn big");
const wideEnum = { ...cond, options: ["1:1", "88:88"] };
if (fontWidth("88:88") <= CELL_W - 2) bad("fixture is not actually too wide");
if (widgetKindFor(wideEnum) !== WIDGET_ENUM) bad("an option too wide for the cell was still drawn big");
const wideNum = { ...swing, min: -1000, max: 1000 };
if (widgetKindFor(wideNum) === WIDGET_BIGNUM) bad("a declared range too wide for the cell was still drawn big");

/* Anything a declared value typically emits exists in the face. */
for (const t of ["2:4", "54%", "1/4", "0.5"]) {
    const miss = [...missingGlyphs(t)];
    if (miss.length) bad("the face cannot spell " + t + ", missing: " + miss.join(""));
}

/* AND IT IS DRAWN: the widget draws the option, not the index. */
function drawn(meta, raw, cellText) {
    const px = [];
    const ctx = { fillRect: (x, y, w, h) => { px.push([x, y, w, h].join(",")); } };
    const g = { cellW: CELL_W, x0: 0 };
    drawKnobWidget(ctx, g, 0, 20, meta, raw, undefined, undefined, cellText);
    return px.join(" ");
}
const idxOnly = { type: "int", kind: "number", key: "x", name: "X", min: 0, max: 8, step: 1 };
if (drawn(cond, "3", null) === drawn(idxOnly, "3", null)) bad("the big cell drew the index 3, not 3:4");

if (fail) process.exit(1);
console.log("PASS: display:big is honoured, the text is the resolved one, and undeclared params are unchanged");
'
