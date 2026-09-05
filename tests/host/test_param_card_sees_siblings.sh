#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A CARD DRAWER CAN READ THE PAGE, THE SAME WAY A CELL WIDGET ALWAYS COULD.
#
# The card payload was { w, h, name, value, raw } -- this parameter and nothing
# else. That is enough for a card that pictures its own number, and it was
# assumed to be enough for all of them. It is not: a card whose MEANING depends
# on a sibling (the vowel of which character, a ratio against which base) had no
# route to that fact at all. There is no getParam on this path, and the card
# script is loaded into its own closure, so it cannot see a variable the
# module's own drawCell set either.
#
# The first module to hit this reached for globalThis plus a staleness
# timestamp. It worked, and it was a hidden side channel between two files the
# contract said were unrelated -- which is the thing to prevent, not to
# document.
#
# What must hold:
#   - `values` reaches the drawer, and is the SAME map drawCell is handed
#   - `nowMs` reaches it too, so a card can animate during the gesture that
#     raised it
#   - the null contract is unchanged: a missing sibling is absent, and a drawer
#     that throws still leaves an empty card rather than a hole
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string, and one apostrophe ends it early with an error pointing nowhere
# near the real line.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param card tests" >&2
  exit 1
fi

node --input-type=module -e '
import { drawParamCard } from "./src/shared/param_pages/param_card.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { readFileSync } from "node:fs";

let fails = 0;
const ok = (cond, what) => { if (!cond) { console.error("FAIL: " + what); fails++; } };

const surface = () => {
  const fb = createFramebuffer(128, 64);
  return { fb, ctx: drawContext(fb) };
};

/* ---- values and nowMs reach the drawer ---- */
{
  const s = surface();
  const pageValues = { vowel: 0.5, face: 3 };
  let got = null;
  drawParamCard(s.ctx, {
    meta: { name: "Vowel" },
    draw: (ctx, o) => { got = o; ctx.fillRect(0, 0, 4, 4, 1); },
    name: "Vowel", value: "0.50", raw: 0.5,
    values: pageValues,
    nowMs: 1234,
  });
  ok(got !== null, "the drawer ran");
  ok(got && got.values === pageValues, "values is passed through, by reference");
  ok(got && got.values.face === 3, "a sibling key is readable from the card");
  ok(got && got.nowMs === 1234, "nowMs is passed through");
}

/* ---- absent values must be null, never invented ---- */
{
  const s = surface();
  let got = null;
  drawParamCard(s.ctx, {
    meta: { name: "Vowel" },
    draw: (ctx, o) => { got = o; },
    name: "Vowel", value: "", raw: null,
  });
  ok(got && got.values === null, "no values supplied reads as null, not as {}");
  ok(got && got.raw === null, "the raw null contract is unchanged");
  ok(got && got.nowMs === 0, "nowMs defaults rather than being undefined");
}

/* ---- a drawer that throws still leaves an EMPTY card, not a hole ---- */
{
  const s = surface();
  let errored = false;
  drawParamCard(s.ctx, {
    meta: { name: "Vowel" },
    draw: (ctx) => { ctx.fillRect(0, 0, 40, 20, 1); throw new Error("boom"); },
    name: "Vowel", value: "0.50", raw: 0.5,
    values: { face: 1 },
    onError: () => { errored = true; },
  });
  ok(errored, "onError fires so the caller can retire the drawer");

  /* The real invariant, rather than a threshold: a drawer that painted and THEN
   * threw must leave EXACTLY what a drawer that painted nothing leaves. An
   * eyeballed pixel budget would pass for a card that kept half its picture. */
  const empty = surface();
  drawParamCard(empty.ctx, {
    meta: { name: "Vowel" },
    draw: () => {},
    name: "Vowel", value: "0.50", raw: 0.5,
  });
  ok(s.fb.countLit() > 0, "the card frame is still on screen");
  ok(s.fb.countLit() === empty.fb.countLit(),
     "a thrower leaves exactly an empty card, not half a picture");
}

/* ---- the controller actually supplies them ---- */
{
  const pc = readFileSync("./src/shared/param_pages/page_controller.mjs", "utf8");
  const call = pc.slice(pc.indexOf("function drawDeclaredCard"),
                        pc.indexOf("function drawDeclaredCard") + 2000);
  /* `liveValues()`, not `s.values`: the card is handed the base with any
   * live/modulated values merged over it, so a card picturing a parameter
   * something else is driving shows what it is DOING rather than where its knob
   * was left. Pinned on the helper rather than on the raw field, because the
   * merge is the point. */
  ok(/values:\s*liveValues\(\)/.test(call),
     "page_controller passes its value map to the card");
  ok(/function liveValues\(\)/.test(pc),
     "and that map merges the live values over the base");
  ok(/nowMs:\s*now\(\)/.test(call), "page_controller passes a clock to the card");
}

if (fails) { console.error(fails + " failure(s)"); process.exit(1); }
console.log("PASS: a card sees the page values and a clock, and the null contract is intact");
'
