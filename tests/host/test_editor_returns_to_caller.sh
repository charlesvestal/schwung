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

if (bad === 0) console.log("PASS: own-view editors return to their caller");
process.exit(bad === 0 ? 0 : 1);
' || FAIL=1

if [ $FAIL -ne 0 ]; then
    echo "FAIL: test_editor_returns_to_caller"
    exit 1
fi
echo "PASS: test_editor_returns_to_caller"
