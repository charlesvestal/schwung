#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A READOUT MUST NOT LOOK LIKE A CONTROL.
#
# `access: "read"` is telemetry. The INPUT layer has honoured it for a while
# (shadow_ui.js isReadoutParam writes nothing on a turn and refuses to open a
# picker on a click; param_meta isDivable / isTurnable both exclude it), but the
# DRAW layer did not -- so a readout was pixel-identical to the same parameter
# you can change. Reported from the device as a control that "does not seem to
# do anything", which was an accurate reading of the picture.
#
# THE RULE IS "A READOUT IS DOTTED", and where the stroke lives is the widget's
# business. A dial and a big number have no frame, so one is added around the
# cell; the enum square already has one, so it dots that. One dotted rectangle
# per cell, never two.
#
# What this pins is the DIFFERENCE, not the drawing: the frame's exact pixels
# are pinned by the widget sheet (tests/host/test_widget_sheet.sh), whose diff
# is the picture itself. What a "they differ" assertion cannot see on its own is
# three things, so each is checked separately:
#
#   INERTNESS   an added frame must be strictly ADDITIVE, and a dotted stroke
#               strictly SUBTRACTIVE. Either way the value may not move: a
#               change that shrank the widget to make room would still "differ",
#               and would still be wrong.
#   STRENGTH    the enum square's first implementation put the frame OUTSIDE,
#               on the cell rect, where the square's own frame absorbed it --
#               17 differing pixels at full width against 27 at the narrow one,
#               and side by side the two cells were indistinguishable. It failed
#               exactly where the feature is for: keydetect's values are musical
#               keys, always full width. Dotting the stroke inverts that
#               gradient, so the check is that a WIDER box differs MORE.
#   SINGULARITY a full-width square must not ALSO wear the outer frame.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import * as RM from "./src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex, isDivable, isTurnable, alsoOpens }
    from "./src/shared/param_pages/param_meta.mjs";

let bad = 0;
const fail = (m) => { console.error("FAIL: " + m); bad++; };
const ok = (m) => console.log("  ok  " + m);

const CELL_W = RM.CELL_W;
const BAND = RM.LBL0_Y - RM.ROW0_Y + RM.LBL_H;   /* widget row + its label */
const META = (o) => buildMetaIndex({ chainParams: [o] }).getOrGuess(o.key);

/*
 * THROUGH drawKnobRow, NOT through drawKnobWidget plus a frame call.
 *
 * Calling drawReadoutFrame here would be the probe drawing the thing it is
 * meant to be checking for: delete the branch in drawKnobRow and this file
 * would still pass, green, describing a device that no longer draws a frame.
 * That failure mode has cost this codebase more time than any other, so the
 * row -- the real entry point, with the real cascade in it -- is what is
 * driven.
 */
function cell(decl, raw) {
    const metaIndex = buildMetaIndex({ chainParams: [decl] });
    const fb = createFramebuffer(CELL_W, BAND);
    RM.drawKnobRow(drawContext(fb), {
        page: { keys: [decl.key] },
        metaIndex,
        values: { [decl.key]: raw },
    }, 0, 0, RM.LBL0_Y - RM.ROW0_Y, { x0: 0, cellW: CELL_W });
    if (fb.clipped && fb.clipped() !== 0) fail("a cell drew outside its own band");
    return fb;
}

const at = (fb, x, y) => fb.pixels[y * fb.width + x];
const pair = (decl, raw) =>
    [cell(decl, raw), cell(Object.assign({}, decl, { access: "read" }), raw)];

/* Every pixel that changed between a control and its readout twin. */
function changes(a, b) {
    const out = { gained: [], lost: [] };
    for (let y = 0; y < BAND; y++)
        for (let x = 0; x < CELL_W; x++) {
            const pa = at(a, x, y), pb = at(b, x, y);
            if (!pa && pb) out.gained.push([x, y]);
            if (pa && !pb) out.lost.push([x, y]);
        }
    return out;
}

/* --- 1. a frameless widget gains a frame ---------------------------------- */
/*
 * The two widgets a readout is in the fleet that draw no stroke of their own:
 * an arc knob (4K EQ peak floats) and a big number (its clip flag, tb3po
 * current_bank).
 */
const FRAMELESS = [
    ["arc knob",   { key: "in_peak_l", name: "In L", type: "float",
                     min: 0, max: 1, step: 0.01 }, "0.66"],
    ["big number", { key: "clip", name: "Clip", type: "int", min: 0, max: 9 }, "3"],
];

for (const [what, decl, raw] of FRAMELESS) {
    const rw = META(decl), ro = META(Object.assign({}, decl, { access: "read" }));
    if (!ro.readOnly) { fail(what + ": access read did not set readOnly -- the probe is inert"); continue; }
    if (rw.readOnly) { fail(what + ": the control half is read-only too -- nothing is compared"); continue; }
    if (RM.widgetKindFor(ro) !== RM.widgetKindFor(rw))
        fail(what + ": readOnly changed the widget kind -- it must only change the stroke");

    const [a, b] = pair(decl, raw);
    const { gained, lost } = changes(a, b);

    if (!gained.length && !lost.length)
        fail(what + ": a readout renders IDENTICALLY to the same param without access read");
    else ok(what + " gains a frame (+" + gained.length + " / -" + lost.length + ")");

    if (lost.length) fail(what + ": the frame REMOVED " + lost.length + " widget pixels -- it must be additive");
    for (const [x, y] of gained) {
        if (y >= RM.BOX_H) fail(what + ": a frame pixel at " + x + "," + y + " fell past BOX_H into the label band");
        if (x === 0 || x === CELL_W - 1)
            fail(what + ": the frame reached the cell edge -- two adjacent readouts would merge");
        if (!(y === 0 || y === RM.BOX_H - 1 || x === 1 || x === CELL_W - 2))
            fail(what + ": a pixel appeared at " + x + "," + y + ", off the frame rect");
    }
}

/* --- 2. the enum square dots the stroke it already has --------------------- */
/*
 * Three widths, because the failure this replaced was width-dependent and
 * invisible at one of them.
 */
const ENUM = (opt) => ({ key: "k", name: "Key", type: "enum", options: [opt] });
const WIDTHS = [["G MAJ", "full width"], ["SAW", "three glyphs"], ["ON", "narrow"]];
const strength = {};

for (const [opt, what] of WIDTHS) {
    const decl = ENUM(opt);
    const ro = META(Object.assign({}, decl, { access: "read" }));
    if (RM.widgetKindFor(ro) !== RM.WIDGET_ENUM) { fail(opt + ": not drawing an enum square"); continue; }

    const [a, b] = pair(decl, "0");
    const { gained, lost } = changes(a, b);
    strength[opt] = gained.length + lost.length;

    if (!lost.length) fail("enum " + opt + ": the readout is IDENTICAL to its editable twin");
    else ok("enum square " + JSON.stringify(opt) + " (" + what + ") is dotted, not solid ("
            + strength[opt] + " pixels differ)");

    /* SUBTRACTIVE: a dotted stroke is a SUBSET of the solid one it replaces.
     * Anything gained means the box moved, resized, or grew a second frame. */
    if (gained.length)
        fail("enum " + opt + ": " + gained.length + " pixels APPEARED -- the dotted stroke must be a subset of the solid one");

    /* Confined to the square is otherwise the whole cell width. */
    const w = RM.enumSquareWidth(opt);
    const bx = Math.floor((CELL_W - RM.ENUM_W) / 2) + Math.floor((RM.ENUM_W - w) / 2);
    for (const [x, y] of lost) {
        const onBox = (x >= bx && x < bx + w && y < RM.BOX_H)
                      && (y === 0 || y === RM.BOX_H - 1 || x === bx || x === bx + w - 1);
        if (!onBox) fail("enum " + opt + ": a pixel changed at " + x + "," + y + ", off the square s own stroke");
    }
}

/*
 * ONE DOTTED RECTANGLE, NEVER TWO. At full width the square spans the cell all
 * but two columns, so an outer frame would land one pixel outside it. Those two
 * columns must be untouched.
 */
{
    const [a, b] = pair(ENUM("G MAJ"), "0");
    const { gained } = changes(a, b);
    const outer = gained.filter(([x]) => x === 1 || x === CELL_W - 2);
    if (outer.length) fail("a full-width enum readout ALSO wears the outer frame -- two rectangles on one cell");
    else ok("a full-width enum readout wears one rectangle, not two");
}

/*
 * A WIDER BOX MUST DIFFER MORE. This is the regression check: the outer-frame
 * version was absorbed by the square, so it got WEAKER as the value got wider
 * (17 pixels at full width against 27 at the narrow one) and was illegible on
 * keydetect, whose values are always full width. Dotting the stroke means more
 * perimeter to dot, so the gradient runs the other way.
 */
if (strength["G MAJ"] !== undefined && strength["ON"] !== undefined) {
    if (strength["G MAJ"] <= strength["ON"])
        fail("a full-width enum readout differs by " + strength["G MAJ"] + " pixels and a narrow one by "
             + strength["ON"] + " -- the mark gets WEAKER as the box gets wider, which is the bug this replaced");
    else ok("the mark strengthens with the box (" + strength["ON"] + " narrow -> "
            + strength["G MAJ"] + " full width)");
}

/* --- 3. what must NOT be marked ------------------------------------------- */
/*
 * AN OPAQUE READOUT IS NOT FRAMED, and the reason is geometric rather than
 * aesthetic: drawOpaqueBox already draws a frame on the IDENTICAL rect, so the
 * dots land invisibly on top of it and show only in the five-row cut where the
 * chevron sits. Marking it degrades the door mark and buys nothing.
 */
{
    const decl = { key: "sample_path", name: "Sample", type: "filepath" };
    if (RM.widgetKindFor(META(Object.assign({}, decl, { access: "read" }))) !== RM.WIDGET_OPAQUE)
        fail("the opaque probe is not drawing an opaque box -- it proves nothing");
    const [a, b] = pair(decl, "/x/kick_01.wav");
    const { gained, lost } = changes(a, b);
    if (gained.length || lost.length)
        fail("a read-only opaque cell was marked (" + (gained.length + lost.length) + " pixels) -- it must not be");
    else ok("an opaque readout keeps its own frame and chevron, unmarked");
}

/* --- 4. a readout gains no AFFORDANCE ------------------------------------- */
for (const decl of [{ key: "a", name: "A", type: "float", min: 0, max: 1, step: 0.01 },
                    { key: "b", name: "B", type: "enum", options: ["X", "Y", "Z"] },
                    { key: "c", name: "C", type: "float", ui_type: "wav_position",
                      min: 0, max: 1, step: 0.01 }]) {
    const ro = META(Object.assign({}, decl, { access: "read" }));
    if (isTurnable(ro)) fail(decl.key + ": a readout is turnable");
    if (isDivable(ro)) fail(decl.key + ": a readout is divable");
    if (alsoOpens(ro)) fail(decl.key + ": a readout wears the corner brackets");
}
ok("a readout is never turnable, divable, or bracketed");

/*
 * The dots sit on the CHECKER lattice in ABSOLUTE coordinates, which is what
 * lets a dotted square and a dotted added frame in adjacent cells share one
 * phase. Drawn at two x origins one apart, the dots must land on opposite
 * parities -- a rect-relative step would produce the same picture twice.
 */
{
    const w = 30, h = RM.BOX_H;
    const p = (x0) => {
        const fb = createFramebuffer(64, h);
        RM.drawReadoutFrame(drawContext(fb), x0, 0, w, h);
        return fb;
    };
    const f0 = p(1), f1 = p(2);
    let shifted = 0;
    for (let i = 0; i < w; i++) if (at(f0, 1 + i, 0) !== at(f1, 2 + i, 0)) shifted++;
    if (shifted === 0) fail("the dot phase follows the rect, not the screen -- neighbouring frames will not line up");
    else ok("the dots are phased on absolute coordinates (" + shifted + " of " + w + " differ when shifted 1px)");
}

/* access is ONE field, so a trigger can never also be a readout. */
{
    const trig = META({ key: "clear", name: "Clear", type: "enum",
                        options: ["-", "Go"], access: "write" });
    if (trig.readOnly) fail("access write set readOnly");
    else ok("a trigger is not a readout");
}

if (bad) { console.error(bad + " problem(s)"); process.exit(1); }
console.log("PASS: a readout is dotted, once, wherever its stroke lives");
'
