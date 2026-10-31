#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A preset name too long for its row must be readable in full: it scrolls while
# it is selected (a pause first, a pause at the end, then back to the start).
# A name that fits must be drawn exactly as before, and an unselected row never
# moves. Two screens carry preset names:
#
#   - the My Presets browser (shadow_ui_presets.mjs drawPresets), which used to
#     draw through renderPicker -- a renderer that truncates and cannot scroll --
#     and now draws through drawListScreen like every other chain-editor list;
#   - the module-provided preset browser (shadow_ui.js, hierarchy preset level),
#     which cut the name to 22 glyphs with an ellipsis, for good.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const { drawListScreen } = await import(R + "/src/shared/chain_editor_chrome.mjs");
const { createFramebuffer } = await import(R + "/tools/param-pages/harness.mjs");
const { createTextScroller } = await import(R + "/src/shared/text_scroll.mjs");

const LONG = "Evolving Pad With A Very Long Name Indeed";
const SHORT = "Short Name";

/* Draw the My Presets list once, the way drawPresets calls it; return the text
 * printed on each list row (top to bottom), the cursor and scroll chrome removed. */
function frame(entries, index) {
  const fb = createFramebuffer();
  const cells = [];
  const ctx = {
    fillRect: fb.fillRect,
    print: (x, y, t, c) => { cells.push({ x, y, t: String(t) }); fb.print(x, y, t, c); },
    setPixel: fb.setPixel,
    textWidth: fb.textWidth,
  };
  drawListScreen(ctx, {
    headerLeft: "Module", headerRight: "PRESETS",
    entries: entries.map((n) => ({ name: n, value: "" })), index,
    emptyMessage: "No presets saved", emptyFooter: [["BACK", "EXIT"]],
    footer: [["JOG", "SEL"], ["CLK", "LOAD"], ["BACK", "EXIT"]],
  });
  const rows = new Map();
  for (const c of cells) {
    if (c.y < 9 || c.y > 53) continue;           /* header band and footer are not the list */
    if (!rows.has(c.y)) rows.set(c.y, []);
    rows.get(c.y).push(c);
  }
  return [...rows.entries()].sort((a, b) => a[0] - b[0]).map(([, cs]) => {
    cs.sort((a, b) => a.x - b.x);
    return cs.map((c) => c.t).filter((t) => !/^[>*]$/.test(t.trim())).join("").replace(/^[^A-Za-z0-9]+/, "");
  });
}

/* ---- 1. a long SELECTED name scrolls, and wraps back ------------------------ */
{
  const seen = [];
  for (let i = 0; i < 600; i++) seen.push(frame([SHORT, LONG], 1)[1]);
  const first = seen[0];
  if (!LONG.startsWith(first.replace(/\.+$/, ""))) fail("the long name starts at its head: got " + first);
  if (seen.slice(0, 40).some((t) => t !== first)) fail("the name must hold still for the initial pause");
  const distinct = new Set(seen);
  if (distinct.size < 4) fail("the long name never scrolled (" + distinct.size + " distinct frames)");
  if (![...distinct].some((t) => t.endsWith(LONG.slice(-6)))) fail("the scroll never reached the end of the name");
  const lastIdx = seen.map((t, i) => t.endsWith(LONG.slice(-6)) ? i : -1).filter((i) => i >= 0).pop();
  if (!seen.slice(lastIdx).some((t) => t === first)) fail("after the end pause the name must return to its head");
}

/* ---- 2. a name that fits never moves --------------------------------------- */
{
  const seen = new Set();
  for (let i = 0; i < 300; i++) seen.add(frame([SHORT, LONG], 0)[0]);
  if (seen.size !== 1) fail("a name that fits changed between frames: " + [...seen].join(" | "));
  if (![...seen][0].startsWith(SHORT)) fail("a name that fits must be drawn whole: " + [...seen][0]);
}

/* ---- 3. an UNSELECTED long name is truncated, never scrolled ----------------- */
{
  const seen = new Set();
  for (let i = 0; i < 300; i++) seen.add(frame([SHORT, LONG], 0)[1]);
  if (seen.size !== 1) fail("an unselected long name moved: " + [...seen].join(" | "));
}

/* ---- 4. nothing saved: the same message and footer as before ----------------- */
{
  const fb = createFramebuffer();
  const said = [];
  drawListScreen({ fillRect: fb.fillRect, print: (x, y, t) => said.push(String(t)), setPixel: fb.setPixel, textWidth: fb.textWidth },
    { headerLeft: "Module", headerRight: "PRESETS", entries: [], emptyMessage: "No presets saved",
      emptyFooter: [["BACK", "EXIT"]], footer: [["JOG", "SEL"]] });
  if (!said.some((t) => t.includes("No presets saved"))) fail("empty list must say so");
}

/* ---- 5. wiring: the two screens use the scrolling path ---------------------- */
const presets = readFileSync(R + "/src/shadow/shadow_ui_presets.mjs", "utf8");
const ui = readFileSync(R + "/src/shadow/shadow_ui.js", "utf8");
const at = presets.indexOf("export function drawPresets(");
const body = presets.slice(at, presets.indexOf("\n}\n", at));
if (/renderMovyPicker/.test(body)) fail("drawPresets must not draw its list through renderPicker (it cannot scroll)");
if (!/drawListScreen\(/.test(body)) fail("drawPresets must draw its list through drawListScreen");

if (!/const hierPresetNameScroller\s*=\s*createTextScroller\(\)/.test(ui)) fail("the hierarchy preset browser needs its own scroller");
if (/truncateText\(presetNameForDraw/.test(ui)) fail("the hierarchy preset browser must not cut the name for good");
if (!/hierPresetNameScroller\.getScrolledText\(fullPresetName, hierPresetNameWindow\.chars\)/.test(ui)) {
  fail("the hierarchy preset browser must scroll a name wider than the line");
}
if (!/fitScrollWindow\(fullPresetName, HIER_PRESET_NAME_WIDTH, text_width\)/.test(ui)) fail("the browser window must be measured in pixels");
{
  const a = ui.indexOf("const fullPresetName"), b = ui.indexOf("print(nameX", a);
  if (!/const nameX = [^;]*text_width\(name\)/.test(ui.slice(a, b))) fail("the name must be centred on its measured width");
}

/* ---- 5b. every frame of the browser marquee fits the screen ----------------- */
{
  const { fitScrollWindow } = await import(R + "/src/shared/text_scroll.mjs");
  const fb = createFramebuffer();
  const WIDTH = 128 - 4;
  for (const name of [LONG, "WWWWWWWWWWWWWWWWWWWWWWWWWWWW", "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii Wide End MMMMMM"]) {
    const k = fitScrollWindow(name, WIDTH, fb.textWidth);
    if (k < 1) fail("window must hold at least one character: " + name);
    const s = createTextScroller();
    let reachedEnd = false;
    for (let i = 0; i < 1500; i++) {
      s.setSelected("k"); s.tick();
      const shown = s.getScrolledText(name, k);
      const w = fb.textWidth(shown);
      if (w > WIDTH) { fail("frame overflows (" + w + "px > " + WIDTH + "): \"" + shown + "\""); break; }
      if (name.endsWith(shown)) reachedEnd = true;
    }
    if (!reachedEnd) fail("the marquee never showed the end of " + name);
  }
}

/* ---- 6. the scroller the browser relies on: pause, step, end pause, wrap ----- */
{
  const s = createTextScroller();
  const W = 22;
  const frames = [];
  for (let i = 0; i < 400; i++) {
    s.setSelected("k");
    s.tick();
    frames.push(s.getScrolledText(LONG, W));
  }
  if (frames[0] !== LONG.slice(0, W)) fail("scroller must begin at the head");
  if (frames.slice(0, 40).some((f) => f !== frames[0])) fail("scroller must pause before moving");
  if (frames.every((f) => f === frames[0])) fail("scroller never moved");
  if (!frames.includes(LONG.slice(-W))) fail("scroller must reach the final window");
  if (frames.some((f) => f.length !== W)) fail("every window is exactly the line width");
  const s2 = createTextScroller();
  s2.setSelected("a"); s2.tick(); s2.getScrolledText(LONG, W);
  for (let i = 0; i < 80; i++) { s2.setSelected("a"); s2.tick(); s2.getScrolledText(LONG, W); }
  s2.setSelected("b");
  if (s2.getScrolledText(LONG, W) !== LONG.slice(0, W)) fail("changing the preset must restart from the head");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.error("PASS: test_preset_name_marquee");
'
