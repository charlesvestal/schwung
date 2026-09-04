#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE PRIMITIVES ARE CLIPPED BECAUSE THEY ARE BUILT ON THE CLIPPED fillRect.
#
# frame_ctx offered only fillRect / print / textWidth, so a module drawer that
# wanted a line built Bresenham on rects while the built-in widget beside it
# called the host. The gap was real for BOTH module hooks -- a cell widget and a
# card drawer see the same context.
#
# They are NOT delegated to the parent, and that is the whole design: a
# delegated call draws in the parent's coordinates with the parent's own
# implementation, so nothing could clip it, and one `line` would end the
# guarantee this file exists for. Built on the frame's own fillRect, clipping is
# not a rule they follow -- it is a thing they cannot avoid. So the assertions
# below drive every primitive far outside the frame and demand zero escapes.
#
# The algorithms are ports of js_display.c, not reinventions: a widget's arc
# sits beside the grid's own arc knobs, and two circles drawn by different maths
# on a 1-bit display do not read as the same object.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the frame ctx primitive tests" >&2
  exit 1
fi

node --input-type=module -e '
import { frameCtx } from "./src/shared/param_pages/frame_ctx.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const FRAME = { x: 10, y: 20, w: 17, h: 15 };
const mk = () => {
  const calls = [];
  const parent = {
    fillRect: (...a) => calls.push(a),
    print() {}, textWidth: (t) => String(t).length * 4,
  };
  return { calls, ctx: frameCtx(parent, FRAME) };
};
const contained = (calls) => calls.every(([x, y, w, h]) =>
  x >= FRAME.x && y >= FRAME.y &&
  x + w <= FRAME.x + FRAME.w && y + h <= FRAME.y + FRAME.h);

/* All five exist, on every frame ctx, with no host and no feature detection. */
{
  const { ctx } = mk();
  for (const n of ["setPixel", "line", "fillCircle", "drawCircle", "drawArc"]) {
    ok(typeof ctx[n] === "function", `${n} is present without a host`);
  }
}

/* setPixel translates, and is clipped. */
{
  const a = mk();
  a.ctx.setPixel(2, 3, 1);
  ok(JSON.stringify(a.calls[0]) === JSON.stringify([12, 23, 1, 1, 1]),
     "setPixel is translated by the frame origin");
  const b = mk();
  b.ctx.setPixel(-5, -5, 1);
  b.ctx.setPixel(500, 500, 1);
  ok(b.calls.length === 0, "an out-of-frame setPixel draws nothing");
  ok(b.ctx.clipped() === 2, "and both attempts are counted");
}

/* Every primitive, driven far outside, lands nothing outside. */
{
  const cases = {
    line: (c) => c.line(-40, -40, 400, 400, 1),
    "line (vertical)": (c) => c.line(8, -100, 8, 100, 1),
    fillCircle: (c) => c.fillCircle(8, 7, 60, 1),
    drawCircle: (c) => c.drawCircle(8, 7, 60, 1),
    drawArc: (c) => c.drawArc(8, 7, 60, 210, 300, 1),
    "drawArc (full)": (c) => c.drawArc(-20, -20, 90, 0, 360, 1),
  };
  for (const [name, fn] of Object.entries(cases)) {
    const { calls, ctx } = mk();
    fn(ctx);
    ok(contained(calls), `${name} cannot draw outside the frame`);
    ok(ctx.clipped() > 0, `${name} COUNTS the overflow rather than hiding it`);
  }
}

/* A primitive that fits draws, and clips nothing. */
{
  const { calls, ctx } = mk();
  ctx.line(1, 1, 5, 5, 1);
  ok(calls.length > 0 && contained(calls), "an in-frame line draws and stays in");
  ok(ctx.clipped() === 0, "and clips nothing");
}

/* fillCircle is emitted per ROW, not per pixel -- the budget is bindings. */
{
  const { calls, ctx } = mk();
  ctx.fillCircle(8, 7, 6, 1);
  ok(calls.length <= 13 + 2,
     "fillCircle costs about one call per row, not one per pixel, got " + calls.length);
}

/* A degenerate or hostile argument must not hang or throw. */
{
  const { ctx } = mk();
  let threw = false;
  try {
    ctx.line(NaN, NaN, NaN, NaN, 1);
    ctx.fillCircle(8, 7, -3, 1);
    ctx.drawArc(8, 7, 5, 0, 0, 1);
    ctx.drawArc(8, 7, 5, NaN, NaN, 1);
    ctx.drawCircle(8, 7, 0, 1);
  } catch (e) { threw = true; }
  ok(!threw, "NaN, a negative radius and a zero sweep neither throw nor hang");
}

/* drawArc reads like a knob: 0 at twelve oclock, increasing CLOCKWISE. */
{
  const { calls, ctx } = mk();
  ctx.drawArc(8, 7, 5, 45, 90, 1);
  const pts = calls.map(([x, y]) => [x - FRAME.x - 8, y - FRAME.y - 7]);
  ok(pts.length > 0, "a quarter arc draws something");
  ok(pts.every(([dx, dy]) => dx >= -1),
     "a 45..135 sweep stays on the RIGHT of centre -- clockwise from twelve");
}

/* A CIRCLE MUST BE A CIRCLE, NOT MERELY CONTAINED.
 *
 * Containment and a quadrant check are not a shape: deleting the mirrored plot
 * from drawArc row pass wipes the entire LEFT half of every circle, and the
 * first version of this file went green on it. Symmetry and no-gaps are what
 * actually pin the port. */
{
  const R = 9, C = 40;
  const lit = new Set();
  const c = frameCtx({
    fillRect(x, y, w, h, col) {
      for (let i = 0; i < w; i++) if (col) lit.add((x + i - C) + "," + (y - C));
    },
    print() {}, textWidth: (t) => String(t).length * 4,
  }, { x: 0, y: 0, w: 200, h: 200 });
  c.drawCircle(C, C, R, 1);

  ok(lit.size > 0, "a full circle draws pixels");
  const mirroredX = [...lit].every((k) => {
    const a = k.split(","); return lit.has((-Number(a[0])) + "," + a[1]);
  });
  const mirroredY = [...lit].every((k) => {
    const a = k.split(","); return lit.has(a[0] + "," + (-Number(a[1])));
  });
  ok(mirroredX, "a full circle is symmetric left-to-right");
  ok(mirroredY, "and top-to-bottom");

  let gaps = 0;
  for (let dy = -R; dy <= R; dy++) {
    if (![...lit].some((k) => Number(k.split(",")[1]) === dy)) gaps++;
  }
  ok(gaps === 0, "every row of the circle has at least one pixel -- no gaps");
}

/* PORTED, NOT REINVENTED.
 *
 * A widget circle sits beside the grid own arc knobs, and two circles drawn by
 * different maths on a 1-bit display do not read as the same object. So this
 * asserts pixel identity against js_display.c fill_circle predicate rather than
 * against a picture someone liked -- including the single-pixel poles, which
 * are the host shape and not a bug in the port. */
{
  const cRef = (r) => {
    const set = new Set();
    for (let dy = -r; dy <= r; dy++)
      for (let dx = -r; dx <= r; dx++)
        if (dx * dx + dy * dy <= r * r) set.add(dx + "," + dy);
    return set;
  };
  let bad = 0;
  for (const r of [0, 1, 2, 3, 5, 7, 8, 12]) {
    const got = new Set();
    const c = frameCtx({
      fillRect(x, y, w, h, col) {
        for (let i = 0; i < w; i++) if (col) got.add((x + i - 40) + "," + (y - 40));
      },
      print() {}, textWidth: (t) => String(t).length * 4,
    }, { x: 0, y: 0, w: 200, h: 200 });
    c.fillCircle(40, 40, r, 1);
    const want = cRef(r);
    if (!(got.size === want.size && [...want].every((k) => got.has(k)))) bad++;
  }
  ok(bad === 0, "fillCircle is pixel-identical to js_display.c at every radius tested");
}

process.exit(fail ? 1 : 0);
'
