#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A WIDGET DRAWS INTO A FRAME AND CANNOT SAY "SCREEN".
#
# The rect a widget receives is unstable three ways:
#
#   render_page.mjs:619   cellW = floor(rect.w / COLS), caller-dependent
#   render_page.mjs:116   rowH is DYNAMIC, and computeGeom picks the whole
#                         render mode from it (dial -> shrinking radius ->
#                         bar-value -> bar-label -> bar-only)
#   render_page_movy      a fixed 32x15, whose own comment warns 15 is only
#                         right because both grid gaps happen to be 15px
#   render_page.mjs:671   Math.min(g.slotSpan, COLS - col) silently CLAMPS a
#                         two-slot group near the right edge
#
# So clipping here is not a safety net, it IS the coordinate system: there is
# no accessor that reaches absolute space, and drawing outside your frame stops
# being a rule an author must follow and becomes something they cannot write
# down.
#
# clipped() exists so a widget that TRIES to overflow is a red test rather than
# something quietly absorbed -- the same bargain as
# test_master_fx_diagram_fit.sh, which exists because a fixed-width row cannot
# report that it overflowed, and nine Master FX boxes were drawn 86px
# off-screen with no error at all.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the frame ctx tests" >&2
  exit 1
fi

node --input-type=module -e '
import { frameCtx } from "./src/shared/param_pages/frame_ctx.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const recorder = () => {
  const calls = [];
  return {
    calls,
    fillRect(x, y, w, h, c) { calls.push([x, y, w, h, c]); },
    print(x, y, t, c) { calls.push(["print", x, y, t, c]); },
    textWidth(t) { return String(t).length * 4; },
  };
};

/* Inside the frame: translated, not clipped. */
let p = recorder();
let f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(2, 3, 4, 5, 1);
ok(JSON.stringify(p.calls[0]) === JSON.stringify([12, 23, 4, 5, 1]),
   "an in-frame fillRect is translated by the frame origin");
ok(f.clipped() === 0, "an in-frame fillRect does not count as clipped");

/* Frame dimensions are the frames, not the screens. */
ok(f.width === 16 && f.height === 15, "width and height are the frame dimensions");

/* Overhanging: clipped to the frame, and counted. */
p = recorder();
f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(12, 0, 40, 4, 1);
ok(JSON.stringify(p.calls[0]) === JSON.stringify([22, 20, 4, 4, 1]),
   "an overhanging fillRect is clipped to the frame width");
ok(f.clipped() === 1, "an overhanging fillRect increments clipped()");

/* Fully outside: nothing reaches the parent. */
p = recorder();
f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(100, 100, 4, 4, 1);
ok(p.calls.length === 0, "a fully-outside fillRect draws nothing");
ok(f.clipped() === 1, "a fully-outside fillRect increments clipped()");

/* Negative coordinates cannot reach above or left of the frame. */
p = recorder();
f = frameCtx(p, { x: 10, y: 20, w: 16, h: 15 });
f.fillRect(-8, -8, 12, 12, 1);
ok(JSON.stringify(p.calls[0]) === JSON.stringify([10, 20, 4, 4, 1]),
   "negative coordinates clamp to the frame origin, never above or left of it");

/* Text truncates rather than overflowing. */
p = recorder();
f = frameCtx(p, { x: 0, y: 0, w: 16, h: 15 });
f.print(0, 0, "ABCDEFGH", 1);
const printed = p.calls[0][3];
ok(printed.length === 4, "print truncates to what the frame width fits");
ok(f.clipped() === 1, "a truncated print increments clipped()");

/* A print that fits is untouched and uncounted. */
p = recorder();
f = frameCtx(p, { x: 5, y: 6, w: 16, h: 15 });
f.print(0, 0, "AB", 1);
ok(JSON.stringify(p.calls[0]) === JSON.stringify(["print", 5, 6, "AB", 1]),
   "a fitting print is translated and left intact");
ok(f.clipped() === 0, "a fitting print is not counted as clipped");

/* No escape hatch, and no reads. */
f = frameCtx(recorder(), { x: 0, y: 0, w: 16, h: 15 });
ok(typeof f.getParam === "undefined", "the frame ctx exposes no getParam");
ok(typeof f.setParam === "undefined", "the frame ctx exposes no setParam");
ok(!Object.values(f).some((v) => v && typeof v === "object" && typeof v.fillRect === "function"),
   "no property of the frame ctx is the parent ctx itself");

/* A degenerate frame must be inert rather than throwing. */
p = recorder();
f = frameCtx(p, { x: 0, y: 0, w: 0, h: 15 });
f.fillRect(0, 0, 4, 4, 1);
f.print(0, 0, "X", 1);
ok(p.calls.length === 0, "a zero-width frame draws nothing and does not throw");

process.exit(fail ? 1 : 0);
'
