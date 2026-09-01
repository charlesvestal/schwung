#!/usr/bin/env bash
# overlay_card.mjs — the one card chrome every overlay on the device uses.
#
# Rendered into the harness framebuffer and inspected pixel by pixel, because
# the thing most likely to be wrong here is invisible in review:
#
#   THE BLACK GAP. The border is white and so is the header band. Where they
#   touch, the border stops existing and the card reads as one fat stripe with
#   no left, right or top. One black row between them is the whole fix.
#   knob_card.mjs carried this rule alone; now that seven overlays share the
#   frame, one regression would take all of them at once.
#
# The vertical centring is asserted too. `print(x, y, ...)` takes y as the
# GLYPH TOP, and baseline arithmetic put a 7px glyph 14px down a 23px box —
# text jammed against the bottom under a band of dead space. Geometry alone
# cannot see that; the gaps have to be compared.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null 2>&1 || { echo "FAIL: node required" >&2; exit 1; }

node --input-type=module -e '
import * as H from "./tools/param-pages/harness.mjs";
import * as OC from "./src/shared/overlay_card.mjs";

let f = 0;
const ok = (what, c) => { if (!c) { console.error("FAIL " + what); f++; } };

function render(o) {
  const fb = H.createFramebuffer();
  const ctx = { fillRect: fb.fillRect, print: fb.print, textWidth: fb.textWidth };
  const r = OC.drawOverlayCard(ctx, o);
  return { fb, r, px: (x, y) => fb.toAscii()[0] !== undefined
      ? (fb.toAscii().split("\n")[y] || "")[x] === "#" : false };
}

const GLYPH_H = 7;

/* ---- the gap ---------------------------------------------------------- */
{
  const { r, px } = render({ title: "Snapshot", titleRight: "saved" });
  const mid = r.x + Math.floor(r.w / 2);

  /* Border, then at least one BLACK row, then the band. Walk down the middle
     column from the top edge and require the sequence. */
  let y = r.y;
  let white = 0;
  while (px(mid, y) && y < r.y + r.h) { white++; y++; }
  /* Against the LITERAL 2, not against OC.BORDER_W — an assertion that reads
     the constant it is checking passes whatever the constant becomes. 2px
     reads as a frame at this size where 1px reads as a hairline, which is the
     actual claim. */
  ok("top border is 2px (got " + white + ")", white === 2);
  ok("BORDER_W says the same", OC.BORDER_W === 2);
  ok("a BLACK row separates the border from the band", !px(mid, y));

  /* The same on the left edge: border column, then a black column, then the
     band fill. A band that ran to the border would erase the frame. */
  const bandY = y + 1;
  let wx = 0, x = r.x;
  while (px(x, bandY) && x < r.x + r.w) { wx++; x++; }
  ok("left border is 2px on the band row (got " + wx + ")", wx === 2);
  ok("a BLACK column separates the border from the band", !px(x, bandY));
}

/* ---- geometry --------------------------------------------------------- */
const one = render({ title: "Snapshot", titleRight: "saved" }).r;
const two = render({ title: "Snapshot", titleRight: "restored", lines: ["2 skipped"] }).r;
const ftr = render({ title: "Delete", lines: ["Sure?"], footer: "Back:No  Jog:Yes" }).r;

for (const [name, r] of [["band only", one], ["band+line", two], ["with footer", ftr]]) {
  ok(name + ": on screen", r.x >= 0 && r.y >= 0 &&
     r.x + r.w <= OC.SCREEN_WIDTH && r.y + r.h <= OC.SCREEN_HEIGHT);
  ok(name + ": nothing clipped", r.clipped === 0);
  ok(name + ": horizontally centred",
     Math.abs(r.x - (OC.SCREEN_WIDTH - r.w - r.x)) <= 1);
  ok(name + ": vertically centred",
     Math.abs(r.y - (OC.SCREEN_HEIGHT - r.h - r.y)) <= 1);
}
ok("a body line makes the card taller", two.h > one.h);
ok("a footer makes the card taller", ftr.h > one.h);

/* EVERY text row is inside the card, and clear of the band.
   A row past the bottom border is drawn straight over it with no error; a row
   on the band is white on white. Neither shows up in the height arithmetic. */
for (const [name, r] of [["band+line", two], ["with footer", ftr]]) {
  for (const row of r.textRows) {
    ok(name + ": row \"" + row.text + "\" is inside the card",
       row.y >= r.y && row.y + GLYPH_H <= r.y + r.h);
    /* Strictly BELOW the band bottom, by the gap. Touching is not enough:
       the band is a white fill and a glyph whose top row abuts it reads as
       part of it. GAP_W is the constant that says so. */
    ok(name + ": row \"" + row.text + "\" clears the band by the gap",
       row.y >= r.bandBottom + OC.GAP_W);
  }
}

/* Width is FIXED — it must not follow the message, or the card changes shape
   depending on which thing happened. */
ok("width does not follow the message", one.w === two.w && two.w === ftr.w);

/* cardHeight is the single source of the sizes above. A caller adding up
   constants by hand is how one overlay ends up a pixel out from the rest. */
ok("cardHeight agrees with band only", OC.cardHeight({ band: true }) === one.h);
ok("cardHeight agrees with band+line",
   OC.cardHeight({ band: true, lines: 1 }) === two.h);

/* ---- overflow --------------------------------------------------------- */
{
  const wide = render({ title: "x", lines: ["y".repeat(60)] }).r;
  ok("an over-long line is REPORTED", wide.clipped === 1);
  ok("the box stays on screen anyway",
     wide.x >= 0 && wide.x + wide.w <= OC.SCREEN_WIDTH);
}

/* ---- the band drops the NAME, never the value ------------------------- */
{
  const fb = H.createFramebuffer();
  const ctx = { fillRect: fb.fillRect, print: fb.print, textWidth: fb.textWidth };
  OC.drawCardBand(ctx, 0, 0, 60, "AnExtremelyLongParameterName", "62.4 %");
  ok("the value survives a collision", fb.missingGlyphs.size === 0);
}

if (f) { console.error(f + " assertion(s) failed"); process.exit(1); }
console.log("PASS test_overlay_card");
'
