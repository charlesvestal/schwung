#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A CANVAS THAT ASKS FOR PADS HEARS THEM.
#
# The knob grid reconciles `pad_observe` from the module's contract, and opening
# a canvas LEAVES the grid -- so a module-drawn browser could not tell which pad
# you pressed, on the one screen where filling a pad is the entire job.
#
# Measured on the device before this existed: knob-touch notes reached the
# canvas and pad notes did not. Every part of the mechanism was already there --
# the shim publishes 68..99 when the flag is set, shadow_ui.c registers
# host_pad_observe -- and nothing raised it while a canvas was up.
#
# Three properties, and each one has already gone wrong somewhere in this tree:
#
#   OPT-IN     only an overlay declaring `wantsPads` gets them, so no existing
#              canvas changes behaviour
#   RESTATED   every tick, not raised once on open. The shim drops the flag on
#              its own authority when the shadow display closes, so a JS mirror
#              goes stale and the feature dies silently for the session.
#   LOWERED    in the close path, because the tick that would restate it is the
#              very thing that stops there -- leaving it raised strands pads
#              pointing at a screen that is gone.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
const shim = readFileSync("src/schwung_shim.c", "utf8");
const bind = readFileSync("src/shadow/shadow_ui.c", "utf8");
let fail = 0;
const ok = (c, m) => { console.log((c ? "  ok   " : "  FAIL ") + m); if (!c) fail++; };

const fn = (name, text) => {
  const at = text.indexOf("function " + name);
  return at < 0 ? "" : text.slice(at, text.indexOf("\nfunction ", at + 1));
};

const tick = fn("tickCanvasPreview", src);
ok(!!tick, "tickCanvasPreview exists");
ok(/host_pad_observe\(/.test(tick),
   "the canvas tick reconciles pad_observe -- raised once on open would go stale when the shim drops it");
ok(/wantsPads/.test(tick),
   "...and only for an overlay that ASKS, so no existing canvas changes");

const close = fn("closeCanvasPreview", src);
ok(/host_pad_observe\(0\)/.test(close),
   "the close path lowers it -- the tick that would restate it stops here");

/* The flag must be lowered BEFORE the hooks run: onClose is a module hook and
   a module must never be able to keep the pads by throwing in it. */
const lowerAt = close.indexOf("host_pad_observe(0)");
const hookAt = close.indexOf("invokeCanvasOverlayHook");
ok(lowerAt >= 0 && hookAt >= 0 && lowerAt < hookAt,
   "...before the module hooks, so a throwing onClose cannot strand the pads");

/* The two halves this depends on, pinned so a rename elsewhere fails HERE
   rather than as pads that silently stop arriving. */
ok(/js_host_pad_observe/.test(bind) && /"host_pad_observe"/.test(bind),
   "shadow_ui.c still registers host_pad_observe");
ok(/pad_observe\s*&&[\s\S]{0,80}d1 >= 68 && d1 <= 99/.test(shim),
   "the shim still publishes pads 68..99 when the flag is set");
ok(!/pad_observe[\s\S]{0,200}continue;/.test(shim.slice(shim.indexOf("pad_observe &&"))),
   "...and does NOT continue, so the pad still plays -- observe, never block");

if (fail) { console.error("test_canvas_wants_pads: " + fail + " FAILURE(S)"); process.exit(1); }
console.log("PASS: a canvas that declares wantsPads is told about the pads, passively");
'
