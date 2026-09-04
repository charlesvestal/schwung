#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A THROWING OVERLAY MUST THROW ONCE, MUST SAY SO, AND draw() MUST NOT READ.
#
# Three defects, all live in shipped code, all in one ten-line function.
#
# 1. The try/catch recorded canvasRuntime.error and carried on, so a broken
#    overlay threw on EVERY FRAME, forever, at 60Hz. The error was visible in
#    the log and the flood was not.
#
# 2. It then returned TRUE. A hook that threw reported success to its caller,
#    which is the same defect class as move_midi_internal_send returning true
#    on a discarded write -- the failure is erased at the boundary, so nothing
#    upstream can react to it.
#
# 3. ctx.getParam is a synchronous param round-trip (~2.8ms) and it was
#    reachable from draw(), where a WHOLE PAGE RENDER costs 1.68ms. One read
#    cost more than redrawing the entire screen, so an overlay that read a
#    couple of values per frame halved its own frame rate and everything
#    drawn with it.
#
# These are pinned against the SOURCE because the canvas runtime lives inside
# shadow_ui.js, which cannot be imported without the host bindings. Source pins
# are weaker than unit tests and they are here because the alternative is
# nothing -- so each one below asserts a SHAPE that cannot be satisfied by
# accident, not merely that a word appears somewhere in the file.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the canvas containment checks" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const src = readFileSync("./src/shadow/shadow_ui.js", "utf8");

/* Isolate the hook invoker so a match cannot come from elsewhere in a
 * 20k-line file. */
const hookStart = src.indexOf("function invokeCanvasOverlayHook");
ok(hookStart > 0, "invokeCanvasOverlayHook exists");
const hookBody = src.slice(hookStart, src.indexOf("\nfunction ", hookStart + 1));

/* 1. ONE STRIKE -- both halves: it must SET the flag, and it must CHECK it. */
ok(/hookDisabled\s*=\s*true/.test(hookBody),
   "a throwing hook sets canvasRuntime.hookDisabled");
ok(/if\s*\(\s*canvasRuntime\.hookDisabled\s*\)\s*return\s+false/.test(hookBody),
   "a disabled runtime refuses further hook invocations");

/* 2. A hook that threw must report failure. The catch block has to end in
 *    return false -- checked inside the catch, because a bare "return false"
 *    anywhere in the function would pass a laxer test. */
const catchBlock = hookBody.slice(hookBody.indexOf("catch"));
ok(/return\s+false/.test(catchBlock.slice(0, catchBlock.indexOf("}\n") + 2)),
   "the catch block returns false rather than reporting success");

/* 3. The draw path gets a ctx with no reads. */
const ctxStart = src.indexOf("function createCanvasRuntimeContext");
const ctxBody = src.slice(ctxStart, src.indexOf("\nfunction ", ctxStart + 1));

ok(/DRAW_PATH_HOOKS/.test(src),
   "a named set declares which hooks run on the draw path");
ok(/DRAW_PATH_HOOKS[\s\S]{0,120}"draw"[\s\S]{0,40}"tick"|DRAW_PATH_HOOKS[\s\S]{0,120}"tick"[\s\S]{0,40}"draw"/.test(src),
   "the draw-path set names both draw and tick");
ok(/canvasHookCtx/.test(hookBody),
   "the hook invoker resolves its ctx through canvasHookCtx, not canvasRuntime.ctx directly");
ok(!/fn\(\s*canvasRuntime\.ctx/.test(hookBody),
   "the hook invoker no longer hands every hook the unrestricted ctx");

/* The stripping must remove all four accessors, not just getParam. */
const stripFn = src.slice(src.indexOf("function canvasHookCtx"));
const stripBody = stripFn.slice(0, stripFn.indexOf("\nfunction ", 1));
for (const acc of ["getParam", "setParam", "getValue", "setValue"]) {
  ok(new RegExp("\\b" + acc + "\\b").test(stripBody),
     `canvasHookCtx strips ${acc} from the draw-path ctx`);
}

/* The cost that motivates the split has to be written down where the next
 * person will edit this, or it grows back. */
ok(/2\.8\s*ms/.test(src.slice(ctxStart - 1400, ctxStart + ctxBody.length)),
   "the read cost that motivates the split is stated at the ctx");

process.exit(fail ? 1 : 0);
'
