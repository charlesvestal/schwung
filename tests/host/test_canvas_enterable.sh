#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN ENTERABLE CANVAS OWNS THE CLICK, AND BACK IS WHAT MAKES THAT SAFE.
#
# A canvas gets the jog wheel and the knobs; the host keeps the jog click and
# Back as the close gesture. That is right for a visualiser and fatal for
# anything nested -- the one gesture that means "enter" is the one spent on
# "leave", so a file browser cannot be built as a canvas at all. Observed on a
# drum module whose sample browser is a canvas: clicking ".." left the browser,
# clicking a folder left the browser, every click was the close gesture.
#
# `enterable: true` hands the click over. The exit contract is what keeps that
# from stranding anyone:
#
#     handleBack() === true   "I went up a level"     -> stay inside
#     anything else           "I am at my top level"  -> the host closes
#
# Two halves, tested two ways. The helpers are EXTRACTED AND RUN, because they
# are small and pure and there is no excuse for pinning their text. The routing
# -- which branch of a 27,000-line MIDI handler sees the press -- is pinned
# against the source, in the house style of test_canvas_overlay_containment.sh:
# weaker than a unit test, and here because the alternative is nothing. Each
# pin asserts a SHAPE that cannot be satisfied by accident.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the canvas enterable checks" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";

const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let fail = 0;
const bad = (m) => { console.error("FAIL: " + m); fail++; };
const ok  = (m) => console.log("  ok   " + m);

/* ---- part 1: run the helpers ------------------------------------------- */

function extract(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { bad(name + " is gone from shadow_ui.js"); return ""; }
  let i = src.indexOf("{", at), depth = 0;
  for (let j = i; j < src.length; j++) {
    if (src[j] === "{") depth++;
    else if (src[j] === "}") { depth--; if (depth === 0) return src.slice(at, j + 1); }
  }
  bad(name + " did not close"); return "";
}

const body = ["getMetaOption", "canvasIsEnterable", "canvasOverlayHookResult",
              "invokeCanvasOverlayHook"].map(extract).join("\n");
const sentinel = "const CANVAS_HOOK_ABSENT = {};";
if (!src.includes(sentinel)) bad("CANVAS_HOOK_ABSENT is gone -- a throw can no longer be told from a value");
if (!fail) {
  const make = new Function("state", `
    let canvasParamMeta = state.meta, canvasRuntime = state.runtime;
    const canvasHookCtx = () => ({}), debugLog = () => {};
    ${sentinel}
    ${body}
    return { canvasIsEnterable, canvasOverlayHookResult, invokeCanvasOverlayHook,
             ABSENT: CANVAS_HOOK_ABSENT };
  `);

  /* the flag, and the two spellings the host accepts */
  const flagOf = (meta) => make({ meta, runtime: null }).canvasIsEnterable();
  if (flagOf(null) !== false)                          bad("no meta must not be enterable");
  else ok("a canvas with no meta is not enterable");
  if (flagOf({ type: "canvas" }) !== false)            bad("default must be false");
  else ok("enterable defaults to false -- a visualiser is unaffected");
  if (flagOf({ enterable: true }) !== true)            bad("enterable: true not read");
  else ok("enterable: true is read");
  if (flagOf({ options: { enterable: true } }) !== true) bad("options.enterable not read");
  else ok("options.enterable is read, as every other canvas flag is");

  /* the exit contract, through the hook-result helper */
  const call = (overlay, disabled = false) => {
    const rt = { overlay, hookDisabled: disabled, error: "" };
    const api = make({ meta: { enterable: true }, runtime: rt });
    return { got: api.canvasOverlayHookResult("handleBack"), rt };
  };
  if (call({ handleBack: () => true }).got !== true)   bad("a true handleBack must be reported");
  else ok("handleBack true is reported -- the module stays inside");
  if (call({ handleBack: () => false }).got === true)  bad("a false handleBack must not consume");
  else ok("handleBack false does not consume -- the host closes");
  if (call({}).got === true)                           bad("a missing hook must not consume");
  else ok("no handleBack at all does not consume");

  /* ⭐ THE ONE THAT MATTERS: a throwing hook must never hold the button. Being
   * stuck on a screen whose script just died is the outcome the whole contract
   * exists to prevent. */
  const thrown = call({ handleBack: () => { throw new Error("boom"); } });
  if (thrown.got === true)          bad("a THROWING handleBack consumed Back -- the user is stuck");
  else ok("a throwing handleBack cannot consume Back");
  if (thrown.rt.hookDisabled !== true) bad("a throw must disable the overlay (one strike)");
  else ok("...and disables the overlay, one strike, as every other hook does");
  if (call({ handleBack: () => true }, true).got === true)
    bad("an already-disabled overlay must not be asked");
  else ok("an already-disabled overlay is never asked again");

  /* ⭐ ONE try/catch, not two. The first cut copied invokeCanvasOverlayHook
   * whole to get at a return value -- fourteen duplicated lines of one-strike
   * error handling that could drift apart. They are one implementation now, and
   * this asserts that the older question is still answered correctly by it. */
  const wrap = (overlay) => {
    const api = make({ meta: {}, runtime: { overlay, hookDisabled: false, error: "" } });
    return api.invokeCanvasOverlayHook("draw");
  };
  if (wrap({ draw: () => undefined }) !== true)
    bad("a draw that returns undefined must still count as HAVING RUN");
  else ok("invokeCanvasOverlayHook still answers did-it-run, through the one primitive");
  if (wrap({}) !== false)      bad("a missing draw must report not-run");
  else ok("a missing hook reports not-run");
  if (wrap({ draw: () => { throw new Error("x"); } }) !== false)
    bad("a throwing draw must report not-run, or the fallback never paints");
  else ok("a throwing draw reports not-run");
}

/* ---- part 2: pin the routing ------------------------------------------- */

/* The click steal must be gated. Without the gate the module never sees a
   click, which is the entire defect. */
if (!/d1 === MoveMainButton && d2 > 0 && !canvasEnterable/.test(src))
  bad("the jog-click steal is not gated on canvasEnterable");
else ok("the jog-click steal declines for an enterable canvas");

/* Back must reach handleBack BEFORE the close, and only when enterable. */
/* ⚠ SCOPED TO THE BACK BRANCH. This compared against the first
   closeCanvasPreview(true) anywhere in the file -- and the Shift+jog escape
   hatch below later added one ABOVE this branch, so a correct ordering read as
   broken. An assertion a nearby edit can flip is worse than none. */
const backBranch = src.slice(src.indexOf("if (d1 === MoveBack && d2 > 0) {",
                                         src.indexOf("SHIFT+JOG IS THE ESCAPE HATCH")));
const backAt  = backBranch.indexOf("canvasOverlayHookResult(\"handleBack\")");
const closeAt = backBranch.indexOf("closeCanvasPreview(true)");
if (backAt < 0)               bad("Back is not offered to handleBack at all");
else if (closeAt < 0)         bad("the Back close path moved -- re-check the ordering");
else if (backAt > closeAt)    bad("handleBack is offered AFTER the close, so it can never run");
else ok("Back is offered to the module before the canvas closes");

/* ⭐⭐ SHIFT+JOG IS THE ESCAPE HATCH: unconditional, not the modules, and NOT
   CONSUMED -- so the turn that gets you out also moves you on. Charles,
   reviewing the PR: "I worry about getting stuck in a page tho if you are more
   than one level in." */
const steal = src.slice(src.indexOf("SHIFT+JOG IS THE ESCAPE HATCH"),
                        src.indexOf("if (d1 === MoveBack && d2 > 0)",
                                    src.indexOf("SHIFT+JOG IS THE ESCAPE HATCH")));
if (!/if \(d1 === 14 && isShiftHeld\(\) && d2 !== 0\) \{/.test(steal))
  bad("Shift+jog is not handled in the canvas steal block");
else ok("Shift+jog is handled while a canvas is up");
if (!/closeCanvasPreview\(true\)/.test(steal)) bad("Shift+jog does not close the canvas");
else ok("...and closes it");

const shiftBranch = steal.slice(steal.indexOf("if (d1 === 14 && isShiftHeld"));
const braceEnd = shiftBranch.indexOf("} else");
/* ⚠ CODE ONLY: the comment inside that branch says "NO return", so matching the
   word anywhere failed on the very text documenting the behaviour. */
const shiftCode = shiftBranch.slice(0, braceEnd < 0 ? 0 : braceEnd)
                             .replace(/\/\*[\s\S]*?\*\//g, "");
if (braceEnd < 0 || /\breturn\b/.test(shiftCode))
  bad("the Shift+jog branch RETURNS -- the turn is swallowed, so exiting costs a second gesture");
else ok("...without consuming the turn, so it pages on the way out");

if (/isShiftHeld\(\)[\s\S]{0,60}canvasEnterable/.test(steal))
  bad("the escape hatch is gated on enterable -- one a module can decline is not one");
else ok("...and is not gated on enterable");

/* Shift is NOT part of this contract. An earlier fork version made Shift+Back
   a failsafe; it is deliberately absent, because a module cannot trap anyone --
   track switch, module swap and leaving the editor all exit without asking. A
   reintroduced Shift gate here would silently change what Back means. */
if (/isShiftHeld\(\)\s*&&\s*canvasOverlayHookResult/.test(src) ||
    /!isShiftHeld\(\)\s*&&\s*canvasOverlayHookResult/.test(src))
  bad("Back is gated on Shift -- the contract has no failsafe gesture");
else ok("Back carries no Shift gate");

/* The select action must not close an enterable canvas either: that is the
   screen readers and the remote UI, not the MIDI path. */
const selAt = src.indexOf("case VIEWS.CANVAS:");
if (selAt < 0 || !/case VIEWS\.CANVAS:[\s\S]{0,600}?if \(canvasIsEnterable\(\)\) break;/.test(src))
  bad("handleSelect closes an enterable canvas");
else ok("select does not close an enterable canvas");

/* ⭐ SHIFT IS EXPOSED AS STATE, and survives the draw-path strip.
   A module draws its own hints, so it needs to know a modifier is held -- and
   it cannot learn that from MIDI, because the host reads Shift from the shim
   shared memory and the CC does not reliably reach a canvas. A module watching
   CC 49 worked under dAVEBOx, which forwards the byte, and silently did nothing
   on stock. */
const ctxFn = src.slice(src.indexOf("function createCanvasRuntimeContext"));
const ctxBody = ctxFn.slice(0, ctxFn.indexOf("\nfunction "));
if (!/shiftHeld\(\)\s*\{\s*return isShiftHeld\(\);/.test(ctxBody))
  bad("the canvas ctx does not expose shiftHeld");
else ok("the canvas ctx exposes shiftHeld as state");

const strip = src.slice(src.indexOf("const { getParam, setParam, getValue, setValue"));
const stripLine = strip.slice(0, strip.indexOf("\n"));
if (/shiftHeld/.test(stripLine))
  bad("shiftHeld is stripped from the draw path -- drawing is exactly where it is wanted");
else ok("...and keeps it on the draw path, unlike the param accessors");

/* The footer must not promise a click that the module now owns. */
if (!/canvasIsEnterable\(\) \? "Back: return" : "Click\/Back: return"/.test(src))
  bad("the fallback footer still says Click/Back for an enterable canvas");
else ok("the fallback footer tells the truth about the way out");

if (fail) { console.error(`test_canvas_enterable: ${fail} FAILURE(S)`); process.exit(1); }
console.log("PASS: an enterable canvas owns the click, and Back is contractual");
'
