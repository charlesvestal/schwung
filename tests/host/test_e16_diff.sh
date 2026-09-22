#!/usr/bin/env bash
# Row-run clustering, not a single bounding box -- see the header of
# e16_diff.mjs for why a global bbox degrades badly on two far-apart
# changes (the motivating case this file exists to test: opposite corners
# of the E16 map).
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { diffFramebuffers, MAX_REGIONS, FULL_REPAINT_THRESHOLD } from "./src/shared/e16_diff.mjs";
import { WIDTH, HEIGHT } from "./src/shared/e16_canvas.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; }
  else console.log("ok   " + n); };

const blank = () => new Uint8Array(1024);
const setPx = (buf, x, y) => { buf[(y >> 3) * WIDTH + x] |= (1 << (y & 7)); };

eq("null prev -> full", diffFramebuffers(null, blank()), { kind: "full" });

const same = blank();
eq("identical -> none", diffFramebuffers(same, same.slice()), { kind: "none" });

/* One pixel at (10,10) -> a single small rect region. */
{
  const prev = blank(), next = blank();
  setPx(next, 10, 10);
  const d = diffFramebuffers(prev, next);
  eq("single pixel -> one region", d.kind, "regions");
  eq("single pixel region count", d.regions.length, 1);
  eq("single pixel rect", d.regions[0], { kind: "rect", x: 10, y: 10, w: 1, h: 1 });
}

/* A full-width single row -> scanline. */
{
  const prev = blank(), next = blank();
  for (let x = 0; x < WIDTH; x++) setPx(next, x, 5);
  const d = diffFramebuffers(prev, next);
  eq("full row -> scanline", d, { kind: "regions", regions: [{ kind: "scanline", y: 5 }] });
}

/* THE MOTIVATING CASE: top-left corner and bottom-right corner both change.
 * A single global bounding box would span nearly the whole screen; row-run
 * clustering must produce two SMALL regions instead. */
{
  const prev = blank(), next = blank();
  setPx(next, 0, 0);
  setPx(next, WIDTH - 1, HEIGHT - 1);
  const d = diffFramebuffers(prev, next);
  eq("opposite corners -> two regions", d.kind, "regions");
  eq("opposite corners region count", d.regions.length, 2);
  eq("opposite corners top region", d.regions[0], { kind: "rect", x: 0, y: 0, w: 1, h: 1 });
  eq("opposite corners bottom region", d.regions[1],
     { kind: "rect", x: WIDTH - 1, y: HEIGHT - 1, w: 1, h: 1 });
}

/* More than MAX_REGIONS scattered single-row changes -> full. */
{
  const prev = blank(), next = blank();
  for (let i = 0; i <= MAX_REGIONS; i++) setPx(next, 0, i * 2);   /* MAX_REGIONS+1 runs */
  eq("more than MAX_REGIONS runs -> full", diffFramebuffers(prev, next), { kind: "full" });
}

/* A region whose area exceeds FULL_REPAINT_THRESHOLD -> full, even though
 * it is a single contiguous run (not a region-count problem). */
{
  const prev = blank(), next = blank();
  const bigH = Math.ceil((FULL_REPAINT_THRESHOLD * WIDTH * HEIGHT) / WIDTH) + 2;
  for (let y = 0; y < bigH; y++) for (let x = 0; x < WIDTH; x++) setPx(next, x, y);
  eq("area over threshold -> full", diffFramebuffers(prev, next), { kind: "full" });
}

/* Overridable via opts. */
{
  const prev = blank(), next = blank();
  setPx(next, 0, 0);
  setPx(next, WIDTH - 1, HEIGHT - 1);
  eq("maxRegions override forces full",
     diffFramebuffers(prev, next, { maxRegions: 1 }), { kind: "full" });
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
