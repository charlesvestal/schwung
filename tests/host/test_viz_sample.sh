#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE SAMPLE CELL IS ANCHORED ON THE MARKER, NOT ON THE FILE.
#
# It used to require a filepath param on the same page, which meant a page of
# nothing but Start / Loop Start / Loop End -- the page most in NEED of the
# picture, because three knobs cannot show that a loop sits inside the region
# that plays -- got no picture at all. The marker is the thing that indexes into
# a sample, so the marker is what the graphic is about.
#
# BRACKETS FACE INWARD. That is what tells a start from an end with no label,
# and it is invisible in code review: getting the direction backwards still
# draws two brackets, still passes any "are there brackets" assertion, and reads
# as a loop that excludes the region it actually plays. Pinned on pixels.
#
# THE CURSOR IS DRAWN LAST. It is the thing that moves and the thing you are
# looking for; a loop bound drawn over it hides it exactly when it matters.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the sample viz tests" >&2
  exit 1
fi

node --input-type=module -e '
import { resolveViz, VIZ_SAMPLE } from "./src/shared/param_pages/viz.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { drawVizGroup } from "./src/shared/param_pages/viz_draw.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* The harness exposes a flat pixel array, not a getter. */
const at = (fb, x, y) => fb.pixels[y * fb.width + x];

const idx = (chainParams, keys) => buildMetaIndex({
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: keys.filter(Boolean),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams,
});
const groupOf = (cp, keys) => {
  const mi = idx(cp, keys);
  const { groups } = resolveViz({ keys, metaIndex: mi });
  return { g: groups.find((x) => x.kind === VIZ_SAMPLE), mi };
};

/* ===================================================================== 1 ==
 * A marker with no file on the page still draws.
 */
{
  const cp = [{ key: "position", name: "Position", type: "wav_position", min: 0, max: 1 }];
  const keys = ["position", null, null, null, null, null, null, null];
  const { g } = groupOf(cp, keys);
  ok(!!g, "a lone wav_position produces a sample group");
  ok(g && g.roles.position === "position", "its position role is the marker");
}

/* ===================================================================== 2 ==
 * A declared filepath_param makes a plain float a marker. mrsample types its
 * Start and Loop Start as floats and says filepath_param: sample_path -- that
 * declaration is the module telling us the knob is a position into that
 * sample, and it is a stronger signal than the type string.
 */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "start", name: "Start", type: "float", min: 0, max: 1, filepath_param: "sample_path" },
  ];
  const keys = ["sample_path", "start", null, null, null, null, null, null];
  const { g } = groupOf(cp, keys);
  ok(!!g, "a float declaring filepath_param is a marker");
  ok(g && g.roles.value === "sample_path", "the declared file becomes the group file");
}

/* ===================================================================== 3 ==
 * An ALL-LOOP page. No playback cursor at all, and it still deserves the
 * picture -- this is the case the file-anchored version could not draw.
 */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "loop_start", name: "Loop Start", type: "float", min: 0, max: 1, filepath_param: "sample_path" },
    { key: "loop_end",   name: "Loop End",   type: "float", min: 0, max: 1, filepath_param: "sample_path" },
  ];
  const keys = ["sample_path", "loop_start", "loop_end", null, null, null, null, null];
  const { g } = groupOf(cp, keys);
  ok(!!g, "an all-loop page still produces a sample group");
  ok(g && g.roles.loopStart === "loop_start", "loop_start is the opening bracket");
  ok(g && g.roles.loopEnd === "loop_end", "loop_end is the closing bracket");
  ok(g && !g.roles.position, "and it has no playback cursor to invent");
}

/* ===================================================================== 4 ==
 * A full sampler page: file, cursor and both bounds, all one graphic.
 */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "position",   name: "Position",   type: "wav_position", min: 0, max: 1 },
    { key: "loop_start", name: "Loop Start", type: "float", min: 0, max: 1, filepath_param: "sample_path" },
    { key: "loop_end",   name: "Loop End",   type: "float", min: 0, max: 1, filepath_param: "sample_path" },
  ];
  const keys = ["sample_path", "position", "loop_start", "loop_end", null, null, null, null];
  const { g, mi } = groupOf(cp, keys);
  ok(!!g, "a full sampler page produces one sample group");
  ok(g && g.roles.position === "position", "the cursor is the position");
  ok(g && g.roles.loopStart === "loop_start" && g.roles.loopEnd === "loop_end",
     "both bounds join it");
  ok(g && g.keys.length === 4, "all four keys are claimed by the one graphic");
  ok(g && g.slotStart === 0 && g.slotSpan === 4, "it spans the whole run");

  /* ---------------------------------------------------------- the pixels */
  const fb = createFramebuffer();
  const rect = { x: 0, y: 16, w: 128, h: 16 };
  drawVizGroup(drawContext(fb), rect, g, {
    sample_path: "/x.wav", position: "0.5", loop_start: "0.25", loop_end: "0.75",
  }, mi);

  const topY = rect.y + 1;
  const colOf = (p) => Math.min(rect.w - 1, Math.floor(p * rect.w));
  const sx = colOf(0.25), ex = colOf(0.75);

  ok(at(fb, sx, topY) === 1 && at(fb, ex, topY) === 1,
     "both loop bounds draw a stem at the top row");
  /* THE DIRECTION. Opening bracket tip to the RIGHT (into the loop), closing
     bracket tip to the LEFT. Backwards still draws two brackets. */
  ok(at(fb, sx + 1, topY) === 1 && at(fb, sx - 1, topY) === 0,
     "the opening bracket tip points INTO the loop (right)");
  ok(at(fb, ex - 1, topY) === 1 && at(fb, ex + 1, topY) === 0,
     "the closing bracket tip points INTO the loop (left)");

  /* The cursor is the envelope COMPLEMENT: cleared inside the body. */
  const midY = topY + 7;
  const mx = colOf(0.5);
  ok(at(fb, mx, midY) === 0, "the playback cursor cuts the sample out at its column");

  ok(fb.clipped() === 0, "nothing was drawn off-screen");
}

/* ===================================================================== 5 ==
 * THE CURSOR WINS. Put a loop bound on the same column as the cursor: the
 * cursor must still be legible there, which means it is drawn last.
 */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "position",   name: "Position",   type: "wav_position", min: 0, max: 1 },
    { key: "loop_start", name: "Loop Start", type: "float", min: 0, max: 1, filepath_param: "sample_path" },
  ];
  const keys = ["sample_path", "position", "loop_start", null, null, null, null, null];
  const { g, mi } = groupOf(cp, keys);
  const fb = createFramebuffer();
  const rect = { x: 0, y: 16, w: 128, h: 16 };
  drawVizGroup(drawContext(fb), rect, g,
    { sample_path: "/x.wav", position: "0.5", loop_start: "0.5" }, mi);
  const midY = rect.y + 8, mx = Math.floor(0.5 * rect.w);
  ok(at(fb, mx, midY) === 0,
     "with a bound on the SAME column, the cursor is still cut in -- it is drawn last");
}

/* ===================================================================== 6 ==
 * A marker naming a DIFFERENT file does not join. A page holding two samplers
 * would otherwise draw one of them a loop it does not have.
 */
{
  const cp = [
    { key: "a_path", name: "A", type: "filepath" },
    { key: "a_pos",  name: "A Pos",  type: "float", min: 0, max: 1, filepath_param: "a_path" },
    { key: "b_loop", name: "B Loop", type: "float", min: 0, max: 1, filepath_param: "b_path" },
    { key: "b_path", name: "B", type: "filepath" },
  ];
  const keys = ["a_path", "a_pos", "b_loop", "b_path", null, null, null, null];
  const { g } = groupOf(cp, keys);
  ok(!!g, "the first sampler still draws");
  ok(g && g.roles.loopStart !== "b_loop",
     "a marker naming ANOTHER file does not join this graphic");
}

process.exit(fail ? 1 : 0);
'
