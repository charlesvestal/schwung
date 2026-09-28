#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE LIST ROW FOLLOWS THE FONT THE CTX DRAWS WITH.
#
# drawMenuList's 9px row is the device's 7px glyphs plus a pixel either side.
# A host drawing its own smaller face through the ctx (movy's is 5px) kept the
# 9px rows, so the text sat at the top of the highlight, off-centre. A ctx may
# now declare `fontHeight`; the row is then the glyphs plus one pixel of
# highlight either side. Without it -- every existing caller -- nothing moves,
# and an explicit lineHeight still wins.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
globalThis.fill_rect = () => {}; globalThis.print = () => {};
globalThis.set_pixel = () => {}; globalThis.text_width = (t) => String(t).length * 5;
const { drawMenuList } = await import(process.cwd() + "/src/shared/menu_layout.mjs");

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

function draw(fontHeight, extra) {
  const prints = [], fills = [];
  const ctx = {
    fillRect: (x, y, w, h, c) => { if (c) fills.push([y, h]); },
    print: (x, y, t, c) => prints.push([y, String(t)]),
    textWidth: (t) => String(t).length * 4,
  };
  if (fontHeight) ctx.fontHeight = fontHeight;
  drawMenuList(Object.assign({ ctx, items: ["A", "B", "C", "D", "E", "F", "G", "H"], selectedIndex: 1,
    listArea: { topY: 9, bottomY: 54 }, getLabel: (i) => i, announce: false }, extra || {}));
  const ys = prints.filter((p) => /^[A-H]$/.test(p[1])).map((p) => p[0]);
  const hl = fills.find(([y, h]) => h > 3);
  const selY = prints.find((p) => p[1] === "B")[0];
  return { ys, hl, selY, rows: ys.length };
}

const dev = draw(0);
if (dev.ys[1] - dev.ys[0] !== 9) bad("without fontHeight the row must stay 9px, got " + (dev.ys[1] - dev.ys[0]));

const small = draw(5);
if (small.ys[1] - small.ys[0] !== 7) bad("a 5px font should get 7px rows, got " + (small.ys[1] - small.ys[0]));
if (!small.hl) bad("no highlight drawn");
else {
  const above = small.selY - small.hl[0];
  const below = (small.hl[0] + small.hl[1]) - (small.selY + 5);
  if (above !== 1 || below !== 1)
    bad("5px glyphs should sit centred in the highlight (1 above, 1 below), got " + above + "/" + below);
}
if (small.rows <= dev.rows) bad("smaller rows should show more options: " + small.rows + " vs " + dev.rows);

const pinned = draw(5, { lineHeight: 9 });
if (pinned.ys[1] - pinned.ys[0] !== 9) bad("an explicit lineHeight must win over the ctx font");

if (fail) process.exit(1);
console.log("PASS: list rows follow the ctx font height; the device default and explicit geometry are unchanged");
'
