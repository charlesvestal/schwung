#!/bin/bash
# An own-view parameter editor must return to whoever OPENED it.
#
# The grid can dive into a parameter whose editor is a whole VIEW rather than an
# overlay. Those close by calling setView() themselves, so unless each one asks
# where it came from it defaults to the hierarchy list -- which from the grid
# reads as "I came back somewhere else" and costs a second Back.
#
# The filepath browser was fixed for this once; the canvas/wave editor was then
# missed, and granny's position editor returned to the menu instead of the
# knobs. This test LIFTS both close functions and drives them, so it fails on
# the behaviour rather than on a spelling -- deleting the helper call from
# either one, or inverting its sense, fails here.
set -u
cd "$(dirname "$0")/../.."
FAIL=0

node --input-type=module -e '
import { readFileSync } from "fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");

function lift(name) {
    const start = src.indexOf("function " + name + "(");
    if (start < 0) throw new Error("missing function: " + name);
    /* brace-match from the signature */
    let i = src.indexOf("{", start), depth = 0, end = -1;
    for (let j = i; j < src.length; j++) {
        if (src[j] === "{") depth++;
        else if (src[j] === "}" && --depth === 0) { end = j + 1; break; }
    }
    if (end < 0) throw new Error("unbalanced: " + name);
    return src.slice(start, end);
}

/* The helper is the real one; everything it touches is a spy. */
const body = [lift("closeOwnViewEditorToCaller"),
              lift("closeCanvasPreview"),
              lift("closeHierarchyFilepathBrowser")].join("\n");

function run(fnName, openedFromGrid) {
    const log = [];
    const deps = {
        paramEditorOpenedFromGrid: openedFromGrid,
        returnToParamPagesFromEditor: () => log.push("grid"),
        setView: (v) => log.push("view:" + v),
        VIEWS: { HIERARCHY_EDITOR: "hierarch" },
        invokeCanvasOverlayHook: () => {},
        resetCanvasState: () => {},
        resetDynamicParamPickerState: () => {},
        commitFilepathSelection: () => {},
        filepathBrowserState: null,
        filepathBrowserParamKey: "",
        needsRedraw: false,
    };
    const names = Object.keys(deps);
    /* `let` for the mutated module-scope vars, so assignments inside the lifted
     * bodies do not throw on a const. */
    const preamble = names.map((n, i) => "let " + n + " = __d[" + i + "];").join("\n");
    const f = new Function("__d", "__call",
        preamble + "\n" + body + "\n" +
        "if (__call === \"canvas\") closeCanvasPreview(false);" +
        "else closeHierarchyFilepathBrowser();");
    f(names.map(n => deps[n]), fnName);
    return log;
}

let bad = 0;
for (const fn of ["canvas", "filepath"]) {
    const fromGrid = run(fn, true);
    if (!fromGrid.includes("grid")) {
        console.log("FAIL: " + fn + " opened from the grid did not return to it: " +
                    JSON.stringify(fromGrid));
        bad++;
    }
    if (fromGrid.includes("view:hierarch")) {
        console.log("FAIL: " + fn + " opened from the grid ALSO fell into the list: " +
                    JSON.stringify(fromGrid));
        bad++;
    }

    const fromList = run(fn, false);
    if (!fromList.includes("view:hierarch")) {
        console.log("FAIL: " + fn + " opened from the list did not return to it: " +
                    JSON.stringify(fromList));
        bad++;
    }
    if (fromList.includes("grid")) {
        console.log("FAIL: " + fn + " opened from the list teleported to the grid: " +
                    JSON.stringify(fromList));
        bad++;
    }
}

/*
 * The THIRD door: edit mode is closed by a CLICK as well as by Back, and the
 * click is the gesture that opened it. Edit mode is not its own view -- it is
 * the hierarchy editor with the row opened (granny position''s waveform strip)
 * -- so it is driven through openHierarchyParamEditor''s toggle branch rather
 * than a close function. Identifiers past the early return are deliberately
 * left undeclared: reaching them throws, and a throw is a failure here.
 */
function runEditModeClick(openedFromGrid) {
    const log = [];
    const deps = {
        hierEditorEditMode: true,
        paramEditorOpenedFromGrid: openedFromGrid,
        resetHierarchyEditState: () => {},
        invalidateKnobContextCache: () => {},
        returnToParamPagesFromEditor: () => log.push("grid"),
    };
    const names = Object.keys(deps);
    const preamble = names.map((n, i) => "let " + n + " = __d[" + i + "];").join("\n");
    const f = new Function("__d",
        preamble + "\n" + lift("closeOwnViewEditorToCaller") + "\n" +
        lift("openHierarchyParamEditor") + "\n" +
        "openHierarchyParamEditor(\"position\", { type: \"float\" }, false);");
    try { f(names.map(n => deps[n])); }
    catch (e) { log.push("threw:" + e.message); }
    return log;
}

const clickFromGrid = runEditModeClick(true);
if (!clickFromGrid.includes("grid")) {
    console.log("FAIL: clicking out of edit mode from the grid did not return to it: " +
                JSON.stringify(clickFromGrid));
    bad++;
}
const clickFromList = runEditModeClick(false);
if (clickFromList.length !== 0) {
    console.log("FAIL: clicking out of edit mode from the list did not stay put: " +
                JSON.stringify(clickFromList));
    bad++;
}

/*
 * The FOURTH door: a STRING cell opens the on-screen keyboard, and the keyboard
 * is not a view either -- it is an overlay whose onConfirm / onCancel ARE its
 * two exits. So there is no close function to drive; the test reaches in
 * through openTextEntry, keeps the options object it was handed, and fires the
 * callbacks itself.
 *
 * Both exits return, including CANCEL: backing out of the keyboard undoes the
 * hand-off rather than completing it, so it lands where confirm lands.
 *
 * setView is spied even though the helper never calls it, so that a future
 * rewrite which sends the keyboard to the hierarchy list directly fails here
 * rather than passing for want of an assertion.
 */
function runStringEditor(which, openedFromGrid) {
    const log = [];
    let opts = null;
    const deps = {
        hierEditorEditMode: false,
        hierEditorSlot: 0,
        paramEditorOpenedFromGrid: openedFromGrid,
        buildHierarchyParamKey: (k) => "synth:" + k,
        getSlotParam: () => "before",
        setSlotParam: () => log.push("write"),
        refreshHierarchyVisibility: () => {},
        announceParameter: () => {},
        announce: () => {},
        openTextEntry: (o) => { opts = o; },
        setView: (v) => log.push("view:" + v),
        VIEWS: { HIERARCHY_EDITOR: "hierarch" },
        returnToParamPagesFromEditor: () => log.push("grid"),
        needsRedraw: false,
    };
    const names = Object.keys(deps);
    const preamble = names.map((n, i) => "let " + n + " = __d[" + i + "];").join("\n");
    const f = new Function("__d", "__box",
        preamble + "\n" + lift("closeOwnViewEditorToCaller") + "\n" +
        lift("openHierarchyParamEditor") + "\n" +
        "openHierarchyParamEditor(\"preset_name\", { type: \"string\", name: \"Name\" }, true);");
    try {
        f(names.map(n => deps[n]), null);
        if (!opts) { log.push("no-keyboard"); return log; }
        if (which === "confirm") opts.onConfirm("typed");
        else opts.onCancel();
    } catch (e) { log.push("threw:" + e.message); }
    return log;
}

for (const which of ["confirm", "cancel"]) {
    const fromGrid = runStringEditor(which, true);
    if (!fromGrid.includes("grid")) {
        console.log("FAIL: string " + which + " from the grid did not return to it: " +
                    JSON.stringify(fromGrid));
        bad++;
    }
    if (fromGrid.includes("view:hierarch")) {
        console.log("FAIL: string " + which + " from the grid ALSO fell into the list: " +
                    JSON.stringify(fromGrid));
        bad++;
    }
    const fromList = runStringEditor(which, false);
    if (fromList.includes("grid")) {
        console.log("FAIL: string " + which + " from the list teleported to the grid: " +
                    JSON.stringify(fromList));
        bad++;
    }
}

/* Confirm still WRITES -- a return that skipped the write would satisfy every
 * assertion above and lose the text the user typed. */
if (!runStringEditor("confirm", true).includes("write")) {
    console.log("FAIL: string confirm returned to the grid without writing the value");
    bad++;
}
if (runStringEditor("cancel", true).includes("write")) {
    console.log("FAIL: string cancel wrote a value");
    bad++;
}

if (bad === 0) console.log("PASS: own-view editors return to their caller");
process.exit(bad === 0 ? 0 : 1);
' || FAIL=1

if [ $FAIL -ne 0 ]; then
    echo "FAIL: test_editor_returns_to_caller"
    exit 1
fi
echo "PASS: test_editor_returns_to_caller"
