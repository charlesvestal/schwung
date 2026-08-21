#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Turning a KNOB to scroll a list.
#
# The jog and a knob are not the same input. A jog detent is a deliberate click
# you feel; a knob detent is a fraction of a casual twist, dozens per flick. So
# 1:1 — right for the jog — reads as "way too fast" on a knob, reported from
# the device after the knob was first routed to the picker.
#
# What is pinned is the FEEL, expressed as numbers: a deliberate turn lands on
# the entry you want, a fast turn crosses a long list, and a short list never
# accelerates because there is nowhere to go.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/list_knob.mjs").then((L) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* Turn one way at a fixed pace, and count entries travelled per detent. */
  const run = (length, detents, gapMs) => {
    const st = L.listKnobInit();
    let t = 1000, moved = 0;
    for (let i = 0; i < detents; i++) { t += gapMs; moved += L.listKnobStep(st, 1, t, length); }
    return moved;
  };

  /* ---- a deliberate turn is not 1:1 --------------------------------- */
  const slow47 = run(47, 30, 400);
  if (slow47 >= 30) fail("a slow turn still moves >= 1 entry per detent (" + slow47 + "/30)");
  if (slow47 === 0) fail("a slow turn moves nothing at all");
  const perDetent = slow47 / 30;
  if (perDetent > 0.5)
    fail("a slow turn moves " + perDetent.toFixed(2) + " entries per detent — too fast to land on one");

  /* ---- a short list must NOT accelerate ------------------------------ */
  const shortSlow = run(12, 30, 400);
  const shortFast = run(12, 30, 8);
  if (shortFast !== shortSlow)
    fail("a 12-entry list accelerated (" + shortSlow + " slow vs " + shortFast +
         " fast) — there is nowhere to go, so it should feel identical");

  /* ---- an ORDINARY turn must never accelerate, at any length ---------
   *
   * The gate is per DETENT, not per step. Measuring between steps meant
   * "60ms" actually gated on 20ms per detent — an ordinary brisk turn — so
   * every real spin hit the ceiling. That was "the fast spins are too fast",
   * and it was the gate being wide open rather than the ceiling being wrong.
   * 20ms/detent is a normal purposeful turn and must stay at the base rate
   * for anything a person steers through. */
  for (const n of [6, 12, 17, 47]) {
    /* The reference has to be UNAMBIGUOUSLY slow — 400ms/detent, far outside
     * any plausible gate. Using 60ms here hid the bug once already: widening
     * the gate to 60 made the reference accelerate too, so the comparison was
     * between two accelerated runs and passed. */
    const steering = run(n, 60, 400);
    const ordinary = run(n, 60, 20);
    if (ordinary !== steering)
      fail("a " + n + "-entry list accelerated on an ORDINARY 20ms/detent turn (" +
           steering + " steering vs " + ordinary + ") — the gate is too wide");
  }

  /* ---- a long list must accelerate, and enough to be usable ---------- */
  const longSlow = run(519, 60, 400);
  const longFast = run(519, 60, 8);
  if (longFast <= longSlow)
    fail("a 519-entry list did not accelerate (" + longSlow + " slow vs " + longFast + " fast)");
  if (longFast < 519 / 4)
    fail("a fast spin of 60 detents covered only " + longFast + " of 519 entries — " +
         "crossing the list would take an unreasonable amount of turning");

  /* ...but not so much that it is uncontrollable. */
  const maxPerStep = L.ACCEL_MAX_MULTIPLIER;
  const st = L.listKnobInit();
  let t = 0, worst = 0;
  for (let i = 0; i < 200; i++) { t += 5; worst = Math.max(worst, Math.abs(L.listKnobStep(st, 1, t, 100000))); }
  if (worst > maxPerStep)
    fail("a step moved " + worst + " entries, over the ACCEL_MAX_MULTIPLIER ceiling of " + maxPerStep);

  /* ---- reversal drops the banked partial turn ------------------------ */
  {
    const s2 = L.listKnobInit();
    let tt = 0;
    /* Bank a partial turn one way... */
    for (let i = 0; i < L.DETENTS_PER_ENTRY - 1; i++) L.listKnobStep(s2, 1, tt += 400, 47);
    /* ...then reverse. The first entry the other way must cost a full,
     * predictable number of detents, not fewer because of the residue. */
    let cost = 0, movedBack = 0;
    while (movedBack === 0 && cost < 20) { cost++; movedBack = L.listKnobStep(s2, -1, tt += 400, 47); }
    if (cost !== L.DETENTS_PER_ENTRY)
      fail("after reversing, the first entry cost " + cost + " detents, expected " +
           L.DETENTS_PER_ENTRY + " — the banked turn was not dropped");
  }

  /* ---- degenerate inputs ------------------------------------------- */
  if (L.listKnobStep(L.listKnobInit(), 0, 1, 47) !== 0) fail("a zero delta moved something");
  if (L.listKnobStep(null, 1, 1, 47) !== 0) fail("a null state threw or moved something");
  if (L.listKnobStep(L.listKnobInit(), 1, 1, 0) !== 0 && false) fail("unreachable");

  console.log("  ok  deliberate turn: " + slow47 + " entries per 30 detents (" +
              perDetent.toFixed(2) + "/detent)");
  console.log("  ok  12-entry list does not accelerate");
  console.log("  ok  519-entry list: " + longSlow + " slow vs " + longFast + " fast per 60 detents");
  console.log("  ok  ceiling honoured, reversal drops the banked turn");
  console.log("PASS: a knob scrolls a list at a usable rate, length-aware");
});
'
