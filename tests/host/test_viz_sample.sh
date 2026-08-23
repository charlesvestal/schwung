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

/* ===================================================================== 7 ==
 * SPRAY FENCES. A granular sampler picks each grain from a random offset
 * around the position -- a REGION on the axis the cursor already lives on, so
 * it draws as a pair of fences rather than as a knob showing a percentage.
 *
 * Two behaviours copied from granny engine rather than guessed:
 *   max_offset = spray * (sample_len - 1)   -> the WHOLE file, not a window
 *   start_idx wraps into [0, len)           -> so the fence wraps too
 * and because the offset is symmetric, +-0.5 already reaches every frame. Past
 * that the region cannot grow, so the fences stop at the file edges instead of
 * drifting on and implying a spread the DSP never applies.
 *
 * THE DECOY IS THE POINT. granny ALSO declares `spread`, which is stereo width
 * between voices. Matching it would draw a region on the sample that no grain
 * is ever read from -- and it would look entirely plausible.
 */
{
  const cp = [
    { key: "position", name: "Position", type: "wav_position", min: 0, max: 1 },
    { key: "spray",    name: "Spray",    type: "float", min: 0, max: 1 },
    { key: "spread",   name: "Spread",   type: "float", min: 0, max: 1 },
  ];
  const keys = ["position", "spray", "spread", null, null, null, null, null];
  const { g, mi } = groupOf(cp, keys);
  ok(g && g.roles.spray === "spray", "spray binds as the spread role");
  ok(g && g.keys.indexOf("spray") < 0,
     "spray does NOT claim a cell -- it is a modifier of the cursor, and it "
     + "keeps its own arc");
  ok(g && g.roles.spray !== "spread",
     "spread does not win over spray when both are present");


  const rect = { x: 0, y: 16, w: 128, h: 16 };
  const render = (vals) => {
    const fb = createFramebuffer();
    drawVizGroup(drawContext(fb), rect, g, vals, mi);
    return fb;
  };
  const colOf = (p) => Math.min(127, Math.floor(p * 128));
  /*
   * A fence is DOTTED: it alternates down the whole column. Counting lit
   * pixels does not distinguish it from the solid waveform body, which is why
   * the first version of this test passed two assertions with no fence code
   * written at all. Count TRANSITIONS instead -- a dotted column flips many
   * times, a solid body flips twice.
   */
  const flips = (fb, col) => {
    let n = 0;
    for (let y = 18; y <= 30; y++) if (at(fb, col, y) !== at(fb, col, y - 1)) n++;
    return n;
  };
  const fenced = (fb, col) => flips(fb, col) >= 6;

  {
    const fb = render({ position: "0.5", spray: "0.2", spread: "1.0" });
    ok(fenced(fb, colOf(0.3)), "lower fence sits at position - spray");
    ok(fenced(fb, colOf(0.7)), "upper fence sits at position + spray");
  }
  {
    const fb = render({ position: "0.9", spray: "0.2", spread: "0" });
    ok(fenced(fb, colOf(0.1)), "the upper fence WRAPS rather than running off the edge");
  }
  {
    const fb = render({ position: "0.5", spray: "0.8", spread: "0" });
    ok(fenced(fb, 0) && fenced(fb, 127),
       "past +-0.5 the fences pin to the file edges -- the region cannot grow");
  }
  {
    const fb = render({ position: "0.5", spray: "0", spread: "1.0" });
    let any = false;
    for (let c = 0; c < 128; c++) if (c !== colOf(0.5) && fenced(fb, c)) any = true;
    ok(!any, "spray 0 draws no fence -- and spread at 1.0 draws nothing either");

    /* Scanning columns is not enough on its own: at spray 0 both fences land
       on the CURSOR column, which the scan above skips, so a missing `> 0`
       guard survives it. Compare against a render with no spray role at all --
       the two must be pixel-identical, which is the actual claim. */
    const bare = createFramebuffer();
    const gNoSpray = JSON.parse(JSON.stringify(g));
    delete gNoSpray.roles.spray;
    drawVizGroup(drawContext(bare), rect, gNoSpray, { position: "0.5" }, mi);
    let diff = 0;
    for (let i = 0; i < fb.pixels.length; i++) if (fb.pixels[i] !== bare.pixels[i]) diff++;
    ok(diff === 0,
       "spray 0 renders identically to no spray at all (" + diff + " pixels differ)");
  }
}

/* THE DECOY, ON ITS OWN. The assertion above is toothless by itself: `spray`
 * comes first in the pool, so a matcher widened to /spray|spread/ still binds
 * the right key and the test passes. fizzik, nusaw and freak declare `spread`
 * and NO spray -- stereo width, which is not a position on the sample at all --
 * so the case that actually matters is a page where spread is the only
 * candidate. Binding it would draw a region no grain is ever read from, and it
 * would look entirely plausible. */
{
  const cp = [
    { key: "position", name: "Position", type: "wav_position", min: 0, max: 1 },
    { key: "spread",   name: "Spread",   type: "float", min: 0, max: 1 },
  ];
  const keys = ["position", "spread", null, null, null, null, null, null];
  const { g, mi } = groupOf(cp, keys);
  ok(g && !g.roles.spray,
     "a page with `spread` and no `spray` binds NO spread role");

  const fb = createFramebuffer();
  drawVizGroup(drawContext(fb), { x: 0, y: 16, w: 128, h: 16 }, g,
    { position: "0.5", spread: "0.3" }, mi);
  let flipsMax = 0;
  for (let c = 0; c < 128; c++) {
    let n = 0;
    for (let y = 18; y <= 30; y++) if (at(fb, c, y) !== at(fb, c, y - 1)) n++;
    if (c !== 64 && n > flipsMax) flipsMax = n;
  }
  ok(flipsMax < 6, "and it draws no fence at all (max column flips " + flipsMax + ")");
}

/* ===================================================================== 9 ==
 * FILE, NO MARKER. Restored once the peaks became real.
 *
 * While the envelope was SYNTHETIC this cell was a fabricated picture of a
 * file -- it looked like the sample shape and was not one -- so anchoring on
 * the marker correctly dropped it. A real waveform with no cursor is genuine
 * information about what is loaded, so it comes back.
 */
{
  const cp = [{ key: "sample_path", name: "Sample", type: "filepath" }];
  const keys = ["sample_path", null, null, null, null, null, null, null];
  const { g } = groupOf(cp, keys);
  ok(!!g, "a filepath with no marker anywhere still produces a sample group");
  ok(g && g.roles.value === "sample_path", "with the file as its only role");
  ok(g && !g.roles.position, "and no cursor invented for it");
}

/* Two samplers side by side each get their own picture -- breakbeat loads
   A_sample_path and B_sample_path on one page. */
{
  const cp = [
    { key: "A_sample_path", name: "A", type: "filepath" },
    { key: "B_sample_path", name: "B", type: "filepath" },
  ];
  const keys = ["A_sample_path", "B_sample_path", null, null, null, null, null, null];
  const mi = idx(cp, keys);
  const { groups } = resolveViz({ keys, metaIndex: mi });
  const gs = groups.filter((x) => x.kind === VIZ_SAMPLE);
  ok(gs.length === 2, "two file params give TWO cells, not one (got " + gs.length + ")");
}

process.exit(fail ? 1 : 0);
'
