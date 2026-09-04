#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# 1-BIT ART IS NEVER FRACTIONALLY SCALED, AND THE FORMAT IS RUNS.
#
# Scaling: 1-bit art on a 128x64 mono display cannot be resampled -- it dithers
# into mush. So a sprite declares the nominal frame it was drawn for, anchors
# 1:1 with integer scale only on an exact multiple, and is REFUSED when it does
# not fit. A refusal is not a failure: the caller falls back to the built-in
# widget, which is a correct picture rather than a smeared one.
#
# Runs: a QuickJS binding is ~490ns. A 17x15 knob box blitted per pixel is 255
# calls ~= 125us, which against a 1.68ms page render sounds survivable -- until
# you notice a page holds EIGHT knob boxes, and a module shipping one custom
# widget ships eight. That is ~1ms of the render gone before anything else is
# drawn. Row runs are ~45 calls ~= 22us, so a page is ~180us.
#
# CHECK THE FULL-PAGE FIGURE, NOT THE PER-SPRITE ONE. That is what makes runs
# non-optional rather than a nicety.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the sprite tests" >&2
  exit 1
fi

node --input-type=module -e '
import { drawSprite, drawSpriteAnchored, anchorSprite, spriteFromRows }
  from "./src/shared/param_pages/sprite_rle.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const recorder = (w = 32, h = 15) => {
  const calls = [];
  return { calls, width: w, height: h,
    fillRect(x, y, rw, rh, c) { calls.push([x, y, rw, rh, c]); },
    print() {}, textWidth(t) { return String(t).length * 4; },
    clipped() { return 0; } };
};

/* ---- decode ---- */
const s = spriteFromRows(["##..##", "..##..", "######"]);
ok(s.w === 6 && s.h === 3, "sprite dimensions come from the rows");

let p = recorder();
drawSprite(p, s, 0, 0, 1);
ok(p.calls.length === 4,
   "runs, not pixels: 2+1+1 = 4 runs for that sprite, not 12 set pixels");
ok(JSON.stringify(p.calls[0]) === JSON.stringify([0, 0, 2, 1, 1]),
   "the first run is a 2-wide fillRect");

/* Every set pixel is painted, and no clear one -- BY CONTENT. */
const paint = (sprite, ox, oy) => {
  const seen = new Set();
  drawSprite({ fillRect(x, y, w, h) { for (let i = 0; i < w; i++) seen.add((x + i) + "," + y); } },
             sprite, ox, oy, 1);
  return seen;
};
const ROWS = ["##..##", "..##..", "######"];
const want = new Set();
ROWS.forEach((r, y) => [...r].forEach((c, x) => { if (c === "#") want.add(x + "," + y); }));
const got = paint(s, 0, 0);
ok(got.size === want.size && [...want].every((k) => got.has(k)),
   "every set pixel is painted and no clear one is");

/* ---- run economy, at the size that actually matters ---- */
const rows = [];
for (let i = 0; i < 15; i++) rows.push("#####........####");
const box = spriteFromRows(rows);
p = recorder();
drawSprite(p, box, 0, 0, 1);
ok(p.calls.length <= 15 * 3, "a 17x15 striped sprite stays within 3 runs per row");
ok(p.calls.length < 17 * 15, "it is emphatically fewer calls than per-pixel");
/* A page holds eight of these. That total is the number that decides the
 * format, so assert the total rather than the per-sprite figure. */
ok(p.calls.length * 8 < 17 * 15,
   "EIGHT of them still cost less than ONE per-pixel blit");

/* ---- anchoring ---- */
let a = anchorSprite({ w: 17, h: 15 }, { width: 17, height: 15 });
ok(a && a.scale === 1 && a.x === 0 && a.y === 0, "an exact fit anchors at 1:1");

a = anchorSprite({ w: 17, h: 15 }, { width: 32, height: 15 });
ok(a && a.scale === 1 && a.x === 7 && Number.isInteger(a.x),
   "a smaller sprite is centred at an integer offset");

a = anchorSprite({ w: 8, h: 7 }, { width: 16, height: 14 });
ok(a && a.scale === 2, "an exact double fits at integer scale 2");

a = anchorSprite({ w: 8, h: 7 }, { width: 12, height: 11 });
ok(a && a.scale === 1, "a non-integer multiple stays at 1:1 rather than 1.5x");

/* REFUSAL rather than shrinking. */
ok(anchorSprite({ w: 40, h: 15 }, { width: 17, height: 15 }) === null,
   "a sprite too wide for the frame is refused, not scaled down");
ok(anchorSprite({ w: 17, h: 40 }, { width: 17, height: 15 }) === null,
   "a sprite too tall for the frame is refused, not scaled down");
ok(anchorSprite({ w: 0, h: 0 }, { width: 17, height: 15 }) === null,
   "an empty sprite is refused");

/* A refused sprite draws nothing at all. */
p = recorder(2, 1);
const drew = drawSprite(p, spriteFromRows(["####"]), 0, 0, 1, { width: 2, height: 1 });
ok(drew === false && p.calls.length === 0,
   "a refused sprite draws nothing and reports the refusal");

/* drawSpriteAnchored refuses through the ctx it was given. */
p = recorder(4, 2);
ok(drawSpriteAnchored(p, spriteFromRows(["########"]), 1) === false && p.calls.length === 0,
   "drawSpriteAnchored refuses a sprite wider than its ctx");

/* And scales the runs when it legitimately can. */
p = recorder(4, 2);
ok(drawSpriteAnchored(p, spriteFromRows(["##"]), 1) === true,
   "drawSpriteAnchored draws a sprite that fits");
ok(p.calls.every(([x, y, w, h]) => x >= 0 && y >= 0 && x + w <= 4 && y + h <= 2),
   "an integer-scaled sprite stays inside its ctx");

process.exit(fail ? 1 : 0);
'
