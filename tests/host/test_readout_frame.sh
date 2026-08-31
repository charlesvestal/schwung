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
# What this pins is the DIFFERENCE, not the drawing: a readout must render
# differently from the same param without `access`, for every widget a readout
# can currently be. The frame's exact pixels are pinned by the widget sheet
# (tests/host/test_widget_sheet.sh), whose diff is the picture itself.
#
# The other half is INERTNESS, and it is the half a "they differ" assertion
# cannot see on its own: the frame is ADDITIVE, so the readout render must be a
# strict SUPERSET of the editable one. A change that moved or shrank the widget
# to make room would still "differ", and would still be wrong.

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

/*
 * The three widgets a readout is in the fleet today: an enum square
 * (keydetect detected_key, gesture-test detected), an arc knob (4K EQ peak
 * floats), a big number (4K EQ clip, tb3po current_bank).
 */
const CASES = [
    ["enum square", { key: "detected", name: "Detected", type: "enum",
                      options: ["Alpha", "Bravo", "Charlie"] }, "1"],
    ["arc knob",    { key: "in_peak_l", name: "In L", type: "float",
                      min: 0, max: 1, step: 0.01 }, "0.66"],
    ["big number",  { key: "clip", name: "Clip", type: "int", min: 0, max: 9 }, "3"],
];

for (const [what, decl, raw] of CASES) {
    const rw = META(decl);
    const ro = META(Object.assign({}, decl, { access: "read" }));

    if (!ro.readOnly) { fail(what + ": access read did not set readOnly -- the probe is inert"); continue; }
    if (rw.readOnly) { fail(what + ": the control half is read-only too -- nothing is being compared"); continue; }

    /* SAME WIDGET. If the two draw different widgets the comparison below is
     * about the widget, not about the frame. */
    if (RM.widgetKindFor(ro) !== RM.widgetKindFor(rw))
        fail(what + ": readOnly changed the widget kind -- it must only add a frame");

    const a = cell(decl, raw), b = cell(Object.assign({}, decl, { access: "read" }), raw);

    let same = true, lost = 0, gained = 0, outsideBox = 0, onCellEdge = 0;
    for (let y = 0; y < BAND; y++) {
        for (let x = 0; x < CELL_W; x++) {
            const pa = at(a, x, y), pb = at(b, x, y);
            if (pa !== pb) same = false;
            if (pa && !pb) lost++;
            if (!pa && pb) {
                gained++;
                if (y >= RM.BOX_H) outsideBox++;
                if (x === 0 || x === CELL_W - 1) onCellEdge++;
                const onFrame = (y === 0 || y === RM.BOX_H - 1 || x === 1 || x === CELL_W - 2);
                if (!onFrame) fail(what + ": a pixel appeared at " + x + "," + y + ", off the frame rect");
            }
        }
    }

    if (same) fail(what + ": a readout renders IDENTICALLY to the same param without access read");
    else ok(what + " renders differently from its editable twin (" + gained + " pixels added)");

    if (lost) fail(what + ": the frame REMOVED " + lost + " widget pixels -- it must be additive");
    if (outsideBox) fail(what + ": " + outsideBox + " frame pixels fell past BOX_H into the label band");
    if (onCellEdge) fail(what + ": the frame reached the cell edge -- two adjacent readouts would merge");

    /*
     * A READOUT GAINS NO AFFORDANCE. The frame says "look"; nothing about it
     * may promise a turn or a door.
     */
    if (isTurnable(ro)) fail(what + ": a readout is turnable");
    if (isDivable(ro)) fail(what + ": a readout is divable");
    if (alsoOpens(ro)) fail(what + ": a readout wears the corner brackets");
}

/*
 * The frame is on the CHECKER lattice in ABSOLUTE coordinates, which is what
 * lets two neighbouring frames share one phase. Drawn at two x origins one
 * apart, the dots must land on opposite parities -- a rect-relative step would
 * produce the same picture twice.
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

/*
 * AN OPAQUE READOUT IS NOT FRAMED, and the reason is geometric rather than
 * aesthetic: drawOpaqueBox already draws a frame on the IDENTICAL rect, so the
 * dots land invisibly on top of it and show only in the five-row cut where the
 * chevron sits. Marking it degrades the door mark and buys nothing.
 */
{
    const decl = { key: "sample_path", name: "Sample", type: "filepath" };
    const a2 = cell(decl, "/x/kick_01.wav");
    const b2 = cell(Object.assign({}, decl, { access: "read" }), "/x/kick_01.wav");
    if (RM.widgetKindFor(META(Object.assign({}, decl, { access: "read" }))) !== RM.WIDGET_OPAQUE)
        fail("the opaque probe is not drawing an opaque box -- it proves nothing");
    let diff = 0;
    for (let i = 0; i < a2.pixels.length; i++) if (a2.pixels[i] !== b2.pixels[i]) diff++;
    if (diff) fail("a read-only opaque cell was framed (" + diff + " pixels) -- it must not be");
    else ok("an opaque readout keeps its own frame and chevron, unmarked");
}

/* An OPAQUE cell is a door and draws its own notched frame with a chevron; a
 * write-only TRIGGER is a button. Neither is a readout, and access is one
 * field -- so this is a statement about the meta, not about the drawing. */
{
    const trig = META({ key: "clear", name: "Clear", type: "enum",
                        options: ["-", "Go"], access: "write" });
    if (trig.readOnly) fail("access write set readOnly");
    else ok("a trigger is not a readout");
}

if (bad) { console.error(bad + " problem(s)"); process.exit(1); }
console.log("PASS: a readout draws its own frame, and only a frame");
'
