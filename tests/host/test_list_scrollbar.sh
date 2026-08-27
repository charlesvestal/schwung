#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A SCROLLBAR REPORTS POSITION AND EXTENT; THE ARROWS REPORTED NEITHER.
#
# The arrows said "there is more, that way". A 47-model list raises the other
# two questions -- how much more, and where am I -- and those are what the
# thumb's SIZE and POSITION answer. So that is what is asserted: not that a bar
# is drawn, but that its geometry tracks the list. A test that only checked for
# ink at the edge would pass with a fixed stripe.
#
# THE 2px FLOOR IS PINNED SEPARATELY. At 47 items in 5 rows the proportional
# thumb is 1.4px, and a 1px thumb is indistinguishable from a tick of the dotted
# track -- it would report position and lose extent, which is half the point.
#
# AND THE BAR MUST NOT APPEAR WHEN THE LIST FITS. That is the case the arrows
# also got right and the easiest one to break, because a track drawn
# unconditionally looks like a design decision rather than a bug.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the scrollbar test" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
const H = await import(R + "/tools/param-pages/harness.mjs");
const { drawMenuList } = await import(R + "/src/shared/menu_layout.mjs");
const LG = await import(R + "/src/shared/list_geometry.mjs");

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const TOP = LG.MENU_LIST_Y, BOT = LG.RULE_Y;
const TRACK_X = LG.SCREEN_WIDTH - 2;

/*
 * MEASURED FROM THE DRAW CALLS, NOT THE FRAMEBUFFER.
 *
 * The selection highlight is a full-width lit fill, so it lights the track
 * column too -- reading pixels at x=126 finds a 9-row solid run wherever the
 * highlight is and reports THAT as the thumb. The first version of this test
 * did exactly that and produced three failures that all looked like scrollbar
 * bugs and were none of them.
 *
 * The bar is the only thing that draws a 1px-wide fill, so the calls identify
 * it exactly and nothing else can be mistaken for it.
 */
function render(n, sel) {
    const fb = H.createFramebuffer();
    const fills = [];
    globalThis.fill_rect = (x, y, w, h, c) => { fills.push({ x, y, w, h, c }); return fb.fillRect(x, y, w, h, c); };
    globalThis.set_pixel = (x, y, c) => { fills.push({ x, y, w: 1, h: 1, c }); return fb.setPixel(x, y, c); };
    globalThis.text_width = fb.textWidth;
    globalThis.print = fb.print;
    drawMenuList({
        items: Array.from({ length: n }, (_, i) => ({ label: "Item " + (i + 1) })),
        selectedIndex: sel,
        listArea: { topY: TOP, bottomY: BOT },
        getLabel: (it) => it.label,
        getValue: () => "",
        announce: false,
    });
    const bar = fills.filter((f) => f.x === TRACK_X && f.w === 1 && f.c);
    /* The dotted track is 1-row fills; the thumb is the taller one. */
    const t = bar.filter((f) => f.h >= 2).sort((a, b) => b.h - a.h)[0] || null;
    return { fb, bar, thumb: t };
}
const thumb = (r) => r.thumb;
const anyInk = (r) => r.bar.length > 0;

/* 1. A list that FITS draws no bar at all. */
if (anyInk(render(4, 0)))
    bad("a 4-item list fits the window and must draw no scrollbar");

/* 2. A long list draws one, and the thumb MOVES from top to bottom. */
const N = 47;
const top = thumb(render(N, 0));
const mid = thumb(render(N, Math.floor(N / 2)));
const end = thumb(render(N, N - 1));
if (!top || !mid || !end) {
    bad("no thumb found on a " + N + "-item list at one or more positions");
} else {
    if (!(top.y < mid.y && mid.y < end.y))
        bad("the thumb does not advance with the selection: top y=" + top.y +
            ", middle y=" + mid.y + ", end y=" + end.y);
    if (top.y !== TOP)
        bad("at the start of a list the thumb should sit at the top of the track (" +
            TOP + "), got " + top.y);
    if (end.y + end.h !== BOT)
        bad("at the end of a list the thumb should reach the bottom of the track (" +
            BOT + "), got " + (end.y + end.h));

    /* 3. The floor. Proportionally this thumb is ~1.4px. */
    if (top.h < 2)
        bad("the thumb is " + top.h + "px -- below the 2px floor it is a tick on the " +
            "track and reports position without extent");

    /* 4. EXTENT IS REAL: a shorter list must give a TALLER thumb. Without this
     *    a fixed-height marker satisfies everything above. */
    const shortT = thumb(render(9, 0));
    if (!shortT || !(shortT.h > top.h))
        bad("a 9-item list should give a taller thumb than a " + N + "-item one, got " +
            (shortT ? shortT.h : "none") + " vs " + top.h +
            " -- the thumb is not reporting extent");
}

/* 5. Nothing is drawn off-screen, which is how the first attempt failed. */
{
    const { fb } = render(N, 20);
    if (fb.clipped() !== 0)
        bad(fb.clipped() + " pixel(s) drawn outside the display");
}

if (fail === 0) {
    console.log("PASS: the scrollbar appears only when the list scrolls, and its thumb " +
        "tracks both position and extent");
}
process.exit(fail ? 1 : 0);
'
