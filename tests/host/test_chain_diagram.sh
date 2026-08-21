#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The chain diagram, drawn against the real 128x64 framebuffer.
#
# The diagram is the only part of the variable-length chain the user actually
# looks at, and every failure mode it has is a PIXEL failure: a box past the
# right edge, a synth that looks like an FX, a `+` that looks like a module.
# None of those are visible from the source, so this renders and inspects.
#
# The clipping check is the load-bearing one. On the device, pixels drawn
# outside the display are silently discarded — a 12-FX chain that "works" would
# simply lose its last box with no error anywhere.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import fs from "node:fs";
const R = process.cwd();
const M = await import(R + "/src/shared/chain_model.mjs");
const H = await import(R + "/tools/param-pages/harness.mjs");
const D = await import(R + "/src/shared/chain_diagram.mjs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const build = (nfx, nmidi = 0, mod = "sf2") => {
  let c = M.emptyChain();
  c.synth = { module: mod };
  for (let i = 0; i < nmidi; i++) c = M.appendTo(c, "midiFx", { module: "arp" });
  for (let i = 0; i < nfx; i++) c = M.appendTo(c, "fx", { module: "fv" + i });
  return c;
};

const render = (comps, sel) => {
  const fb = H.createFramebuffer();
  D.drawChainDiagram(H.drawContext(fb), comps, sel);
  return fb;
};

/* ---- 0. settings is LAST, which is what lets it share a column with the rail --
 *
 * SETTINGS_GAP pushes the settings box one column past the strip, onto the
 * column the right scroll rail uses. That is safe only because the two can
 * never both be drawn: the rail means there is more to the right, and settings
 * is the last position, so when it is on screen there is nothing beyond it.
 *
 * Add a position after settings and that stops being true silently -- the rail
 * would draw over the box, or the box past the display. So the ordering is
 * pinned here rather than assumed by the layout. */
for (const nfx of [0, 3, 12]) {
  const comps = M.chainComponents(build(nfx, nfx ? 1 : 0));
  const last = comps[comps.length - 1];
  if (!last || last.kind !== "settings")
    fail(nfx + " fx: the last chain position is " + (last && last.kind) +
         ", not settings -- the settings box shares the right rail column and " +
         "may only do so while nothing can follow it");
  const strays = comps.filter((c, i) => c.kind === "settings" && i !== comps.length - 1);
  if (strays.length) fail(nfx + " fx: a settings position is not last");
}

/* ---- 1. nothing is drawn off-screen, at either end of any length ---- */
for (const nfx of [0, 3, 12]) {
  const comps = M.chainComponents(build(nfx, nfx ? 1 : 0));
  for (const sel of [-1, 0, Math.floor(comps.length / 2), comps.length - 1]) {
    const fb = render(comps, sel);
    if (fb.clipped() > 0)
      fail(nfx + " fx, selection " + sel + ": drew " + fb.clipped() + " px off-screen");
    if (fb.missingGlyphs.size)
      fail(nfx + " fx: characters the device cannot draw: " + [...fb.missingGlyphs].join(""));
  }
}

/* ---- 2. the scroll keeps the selection on screen ---- */
{
  const comps = M.chainComponents(build(12, 4));
  for (let sel = 0; sel < comps.length; sel++) {
    const lay = D.layoutChainDiagram(comps, sel);
    if (sel < lay.first || sel >= lay.first + lay.count)
      fail("selection " + sel + " of " + comps.length + " fell outside the window " +
           lay.first + "+" + lay.count);
  }
  /* ...and while everything fits, nothing scrolls: the short chain is drawn
   * from its first component, so the synth stays where the user drew it.
   * An empty chain is five positions — patch, `+`, synth, `+`, settings —
   * which is exactly the capacity. (The editor drops `patch`, whose seat is
   * the selection at -1, so on the device one more position fits than this.) */
  const shortComps = M.chainComponents(build(0));
  if (D.layoutChainDiagram(shortComps, shortComps.length - 1).first !== 0)
    fail("a chain that fits must not scroll");
}

/* ---- 3. the synth is distinguishable from an FX ----
 * Same module id in both positions, so the ABBREVIATION is identical and the
 * only thing that can differ is the box. The synth is the landmark the whole
 * scroll design leans on: past the fold it is the only orientation left. */
{
  const cfg = build(1, 0, "sf2");
  cfg.fx = [{ module: "sf2" }];
  const comps = M.chainComponents(cfg);
  const synthAt = comps.findIndex((p) => p.id === "synth");
  const fxAt = comps.findIndex((p) => p.id === "fx1");
  const boxPixels = (fb, lay, idx) => {
    const out = [];
    const x0 = lay.boxX(idx);
    for (let y = lay.y; y < lay.y + lay.boxH; y++)
      for (let x = x0; x < x0 + lay.boxW; x++) out.push(fb.pixels[y * fb.width + x]);
    return out.join("");
  };
  for (const sel of [-1, synthAt, fxAt]) {
    const fb = render(comps, sel);
    const lay = D.layoutChainDiagram(comps, sel);
    /* Compare each box against itself under the SAME selection state, so a
     * difference can only come from the kind of box, not from the highlight. */
    const a = boxPixels(fb, lay, synthAt), b = boxPixels(fb, lay, fxAt);
    if (a === b) fail("selection " + sel + ": the synth box draws identically to an FX box");
  }
}

/* ---- 4. the `+` boxes are at both ends, and are dotted ----
 * Dotted is what says "nothing lives here yet". A solid outline would read as
 * an empty module position, which is a different thing entirely. */
{
  const comps = M.chainComponents(build(1));
  const ends = [comps.findIndex((p) => p.id === "add_midi"),
                comps.findIndex((p) => p.id === "add_fx")];
  if (ends.some((i) => i < 0)) fail("the model stopped offering both `+` positions");
  const fb = render(comps, -1);
  const lay = D.layoutChainDiagram(comps, -1);
  const topEdge = (idx) => {
    const x0 = lay.boxX(idx);
    let lit = 0;
    for (let x = x0; x < x0 + lay.boxW; x++) lit += fb.pixels[lay.y * fb.width + x];
    return lit;
  };
  const fxAt = comps.findIndex((p) => p.id === "fx1");
  if (topEdge(fxAt) !== lay.boxW) fail("a module box should have a solid top edge");
  for (const i of ends) {
    if (topEdge(i) >= lay.boxW) fail("the `+` box at " + i + " is not dotted");
    if (topEdge(i) === 0) fail("the `+` box at " + i + " has no top edge at all");
  }
}

/* ---- 5. the editor does not probe the whole cap every frame ----
 * loadChainConfigFromSlot runs from drawChainEdit on EVERY frame and one IPC
 * read is ~2.8ms, so reading eight FX positions to find out there are none
 * would cost more than the entire page render. The DSP publishes the length;
 * this pins that we ask for it rather than hunting for it. */
{
  const src = fs.readFileSync(R + "/src/shadow/shadow_ui.js", "utf8");
  const body = /function loadChainConfigFromSlot\([\s\S]*?\n}\n/.exec(src);
  if (!body) fail("could not find loadChainConfigFromSlot to inspect");
  else {
    const b = body[0];
    if (!/fx_count/.test(b) || !/midi_fx_count/.test(b))
      fail("loadChainConfigFromSlot must read fx_count / midi_fx_count, not probe positions");
    if (/MAX_FX|MAX_MIDI_FX/.test(b))
      fail("loadChainConfigFromSlot must not loop to the cap — that is 8 IPC reads a frame");
  }
  if (/CHAIN_FX_POSITIONS|CHAIN_MIDI_FX_POSITIONS/.test(src))
    fail("the fixed-shape position constants should be gone");
}

if (failures) process.exit(1);
console.log("PASS: chain diagram — 0/3/12 FX draw unclipped, the scroll follows the selection, the synth and the `+` boxes are distinct");
'
