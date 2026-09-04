#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE GENERATOR FAILS ON THE AUTHORS MACHINE, NOT THE USERS DEVICE.
#
# That is the whole reason the declarative tier is a BUILD step rather than a
# runtime format: a malformed sprite config is a red build for the person who
# wrote it, instead of a broken widget for whoever installed the module. The
# nominal-dimension check is the load-bearing part, because a mismatched frame
# CANNOT be corrected at runtime -- 1-bit art is never rescaled.
#
# The round trip is compared BY CONTENT. zlib is not byte-stable, and more to
# the point a byte comparison of generated artifacts passes only on the machine
# that wrote them.
#
# The generated file is also driven through the REAL frame ctx, so "it emits
# plausible-looking code" is not what is being checked -- what is checked is
# that the code it emits actually paints the source pixels and stays in frame.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the generator tests" >&2
  exit 1
fi

node --input-type=module -e '
import { generate } from "./tools/param-pages/widget_gen.mjs";
import { frameCtx } from "./src/shared/param_pages/frame_ctx.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const ROWS = ["##..##", "..##..", "######", "#....#", ".####."];
const SPEC = { kind: "custom:demo", nominal: { w: 6, h: 5 },
               frames: [{ atMost: 1.0, rows: ROWS }] };

const a = generate(SPEC);
const b = generate(SPEC);
ok(a === b, "generation is deterministic");

ok(/export const widgetKind = "custom:demo"/.test(a), "the generated file exports widgetKind");
ok(/export const widgetNominal = \{ w: 6, h: 5 \}/.test(a), "the generated file exports widgetNominal");
ok(/export function drawCell/.test(a), "the generated file exports drawCell");
/* A CALL, not the word -- the generated header comment legitimately explains
 * that the ctx carries no getParam, and a bare substring check would flag its
 * own documentation. */
ok(!/\bgetParam\s*\(/.test(a) && !/\bsetParam\s*\(/.test(a),
   "generated code never CALLS a param accessor");
ok(!/\bhost_[a-z_]*\s*\(/.test(a), "generated code calls no host binding");
ok(/GENERATED/.test(a) && /do not edit/.test(a), "the generated file says it is generated");

/* ---- Run the generated code for real. ---- */
const mod = await import("data:text/javascript," + encodeURIComponent(a));
ok(mod.widgetKind === "custom:demo", "the generated module actually loads");

const painted = new Set();
const parent = {
  fillRect(x, y, w, h) { for (let i = 0; i < w; i++) painted.add((x + i) + "," + y); },
  print() {}, textWidth(t) { return String(t).length * 4; },
};
const ctx = frameCtx(parent, { x: 0, y: 0, w: 6, h: 5 });
mod.drawCell(ctx, { values: { drive: "0.5" }, group: { keys: ["drive"] } });

const want = new Set();
ROWS.forEach((r, y) => [...r].forEach((c, x) => { if (c === "#") want.add(x + "," + y); }));
ok(painted.size === want.size && [...want].every((k) => painted.has(k)),
   "round trip: the generated drawCell paints exactly the source pixels");
ok(ctx.clipped() === 0, "the generated widget clips nothing at its nominal frame");

/* Refuses rather than rescaling when the frame is too small. */
const small = [];
const sctx = frameCtx({ fillRect: (...v) => small.push(v), print() {},
                        textWidth: (t) => String(t).length * 4 },
                      { x: 0, y: 0, w: 4, h: 3 });
mod.drawCell(sctx, { values: {}, group: { keys: ["drive"] } });
ok(small.length === 0, "the generated widget draws nothing in a frame smaller than nominal");

/* Centres in a larger frame, and stays inside it. */
const big = [];
const bctx = frameCtx({ fillRect: (...v) => big.push(v), print() {},
                        textWidth: (t) => String(t).length * 4 },
                      { x: 20, y: 30, w: 32, h: 15 });
mod.drawCell(bctx, { values: {}, group: { keys: ["drive"] } });
ok(big.length > 0 && big.every(([x, y, w, h]) =>
     x >= 20 && y >= 30 && x + w <= 52 && y + h <= 45),
   "the generated widget centres inside a larger frame and stays in it");
ok(bctx.clipped() === 0, "and clips nothing there either");

/* ---- Build-time refusals. ---- */
const throws = (fn, re) => {
  try { fn(); return false; } catch (e) { return re.test(String(e.message)); }
};

ok(throws(() => generate({ kind: "custom:bad", nominal: { w: 6, h: 5 },
                           frames: [{ atMost: 1, rows: ["###", "###"] }] }), /nominal/i),
   "a frame that does not match nominal fails the build with a nominal error");
ok(throws(() => generate({ kind: "demo", nominal: { w: 6, h: 5 },
                           frames: [{ atMost: 1, rows: ROWS }] }), /custom:/),
   "a kind without the custom: prefix is rejected");
ok(throws(() => generate({ kind: "custom:x", nominal: { w: 0, h: 5 },
                           frames: [{ atMost: 1, rows: ROWS }] }), /nominal/i),
   "a zero nominal dimension is rejected");
ok(throws(() => generate({ kind: "custom:x", nominal: { w: 6, h: 5 }, frames: [] }), /no frames/),
   "a spec with no frames is rejected");
ok(throws(() => generate({ kind: "custom:x", nominal: { w: 6, h: 5 },
                           frames: [{ atMost: 1, rows: ["######", "###"] }] }), /ragged/),
   "ragged rows are rejected");
ok(throws(() => generate({ kind: "custom:x", nominal: { w: 6, h: 5 },
                           frames: [{ rows: ROWS }] }), /atMost/),
   "a frame with no atMost threshold is rejected");

/* Frame order must not change the output, or two authors writing the same
 * widget differently would get different files. */
const R2 = ["######", "######", "######", "######", "######"];
const s1 = generate({ kind: "custom:x", nominal: { w: 6, h: 5 },
                      frames: [{ atMost: 0.5, rows: ROWS }, { atMost: 1, rows: R2 }] });
const s2 = generate({ kind: "custom:x", nominal: { w: 6, h: 5 },
                      frames: [{ atMost: 1, rows: R2 }, { atMost: 0.5, rows: ROWS }] });
ok(s1 === s2, "frame declaration order does not change the generated file");

process.exit(fail ? 1 : 0);
'
