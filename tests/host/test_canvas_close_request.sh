#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# ctx.close() RECORDS A WISH; THE HOST LEAVES, ONCE, THE USER'S WAY.
#
# It used to call closeCanvasPreview() from inside the module's hook, which took
# the wrong exit twice:
#   - co-run: unwrapped, so setView() moved the OUTER view (the running tool)
#     to the list editor and left coRunView pointing at a dead canvas;
#   - handleBack: closed once in close(), then AGAIN in the Back branch when
#     the hook returned anything but true -- the second close overwrote the
#     grid return with the list editor.
# And a canvas PAGE's hook state never reached drawPage, and was shared by
# every slot running the same module.
#
# The helpers are EXTRACTED AND RUN against stubs; the Back ordering is pinned.

command -v node >/dev/null 2>&1 || { echo "FAIL: node required" >&2; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let fail = 0;
const bad = (m) => { console.error("FAIL: " + m); fail++; };
const ok  = (m) => console.log("  ok   " + m);

function extract(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { bad(name + " is gone"); return ""; }
  let depth = 0;
  for (let j = src.indexOf("{", at); j < src.length; j++) {
    if (src[j] === "{") depth++;
    else if (src[j] === "}" && --depth === 0) return src.slice(at, j + 1);
  }
  bad(name + " did not close"); return "";
}

/* ---- consumeCanvasCloseRequest ---------------------------------------- */
const consume = extract("consumeCanvasCloseRequest");
function world(o) {
  const log = [];
  const w = new Function("o", "log", `
    const VIEWS = { CANVAS: "canvas", OVERTAKE: "overtake", HIER: "hier" };
    let view = o.view, coRunView = o.coRunView, needsRedraw = false;
    let canvasRuntime = o.runtime;
    const coRunUiActive = () => o.corun;
    function runCoRunChainEdit(fn) {
      const saved = view; view = coRunView;
      try { fn(); } finally { coRunView = view; view = saved; }
    }
    function closeCanvasPreview(c) { log.push("close"); canvasRuntime = null; view = VIEWS.HIER; }
    function announce(t) { log.push("announce:" + t); }
    ${consume}
    const r = consumeCanvasCloseRequest();
    return { r, view, coRunView };
  `);
  return { ...w(o, log), log };
}

let t = world({ view: "canvas", coRunView: null, corun: false, runtime: {} });
if (t.r !== false || t.log.length) bad("no request must do nothing"); else ok("no request: nothing");

t = world({ view: "canvas", coRunView: null, corun: false, runtime: { closeRequested: true } });
if (t.r !== true || t.log.filter(x => x === "close").length !== 1) bad("plain request must close once");
else if (!t.log.includes("announce:Hierarchy Editor")) bad("close() must announce like every other exit");
else ok("plain canvas: closes once, announced");

t = world({ view: "overtake", coRunView: "canvas", corun: true, runtime: { closeRequested: true } });
if (t.view !== "overtake") bad("co-run close moved the OUTER view to " + t.view);
else if (t.coRunView !== "hier") bad("co-run close left coRunView at " + t.coRunView);
else ok("co-run: outer view kept, overlay closed");

t = world({ view: "canvas", coRunView: "canvas", corun: true, runtime: { closeRequested: true } });
if (t.view !== "hier") bad("inside a wrapper it must close directly, not nest a second wrapper");
else ok("already wrapped: no nested wrapper");

/* ---- ctx.close() does not close from inside the hook -------------------- */
const ctxFn = extract("createCanvasRuntimeContext");
if (/close\(\)\s*\{[^}]*closeCanvasPreview/.test(ctxFn)) bad("ctx.close() calls closeCanvasPreview inside the hook again");
else if (!/close\(\)\s*\{\s*if \(rt\) rt\.closeRequested = true;/.test(ctxFn)) bad("ctx.close() no longer records the request");
else ok("ctx.close() records, never closes");

/* ---- every hook site that can carry close() consumes it ---------------- */
for (const fn of ["dispatchCanvasMidi", "openCanvasPreview", "tickCanvasPreview"]) {
  if (!extract(fn).includes("consumeCanvasCloseRequest()")) bad(fn + " drops a close() made in its hook");
}
ok("onMidi / onOpen / onValues sites consume the request");

/* ---- Back: the request is consumed BEFORE the fall-through close -------- */
const back = src.indexOf("canvasOverlayHookResult(\"handleBack\")");
const seg = src.slice(back, back + 600);
const iConsume = seg.indexOf("if (consumeCanvasCloseRequest()) return;");
const iUp = seg.indexOf("up === true");
if (back < 0 || iConsume < 0 || iUp < 0 || iConsume > iUp) bad("Back branch must consume a close() from handleBack before anything else");
else ok("Back: close() from handleBack closes exactly once");

/* ---- page state: per slot, and reaches drawPage ------------------------- */
const pst = extract("canvasPageState");
const f = new Function(`const canvasPageStates = {}; ${pst} return canvasPageState;`)();
const a = f(0, "k"), b = f(1, "k");
if (a === b) bad("two slots running one module share a page state");
else if (f(0, "k") !== a) bad("page state is not stable per slot");
else ok("page state is per slot and stable");
if (!/state: canvasPageState\(slot, cacheKey\)/.test(extract("drawCanvasPageBody"))) bad("drawPage never receives the hooks state");
else ok("drawPage receives the same state the hooks write");
const hook = extract("canvasPageHook");
for (const m of ["shiftHeld", "measureText", "getValue", "setValue", "random"])
  if (!hook.includes(m + ":")) bad("page hook ctx lacks " + m + " -- a dive script throws on a page");
ok("page hook ctx matches the dive event surface");

if (fail) process.exit(1);
'
