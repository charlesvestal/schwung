#!/usr/bin/env bash
# The E16 map view and its navigation gestures.
#
# Five rules, and the first one is the reason this test exists at all:
#
#   1. A LOST SHIFT NOTE-OFF CANNOT STRAND THE MAP. CLAUDE.md records a field
#      device with `pad_block` stuck for THIRTEEN HOURS across two shim inits,
#      and that enumerating the ways a modifier can end failed twice on
#      hardware. The map must therefore come down with NO event at all -- so
#      this test drives a shift-down, never sends the note-off, and asserts
#      that the surface is back on parameters and that the next turn edits a
#      parameter rather than paging.
#   2. THE MAP IS DRAWN ON THE PRESS. Asserted in PIXELS against a canvas
#      rendered straight from buildMap(), because "the state says map" and
#      "the framebuffer that went out is the map" are two different claims and
#      only the second one reaches the device.
#   3. A LOWER PUSH DROPS BACK TO PARAMETERS. The jump is the end of the
#      gesture; leaving the map up would mean the surface you land on is the
#      one you cannot see.
#   4. NAVIGATION SENDS ONE FRAMEBUFFER PER DISCRETE ACTION, AND NOTHING WHEN
#      NOTHING CHANGED. Measured outbound 2026-09-09: 31 packets alone arrived
#      byte-perfect, 34 amid other traffic lost 8 whole packets. An unbounded
#      repaint measures fine in isolation and drops packets in use, so the
#      count is asserted EXACTLY, including the idle ticks that must add none.
#   5. HOLES ARE NOT DESTINATIONS. An empty lower cell is not a jump target,
#      and pressing one must not consume the hold or move focus.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { createNav, createDisplay, MAP_MAX_HOLD_MS }
    from "./src/shared/e16_surface.mjs";
import { renderMap, pageStep, HALVES } from "./src/shared/e16_view.mjs";
import { buildMap } from "./src/shared/e16_map.mjs";
import { createCanvas } from "./src/shared/e16_canvas.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };

/* ---------------------------------------------------------------------------
 * Fixture.
 *
 * Deliberately ASYMMETRIC between slots and deliberately LONG in slot 1. A
 * fixture whose slots hold the same components cannot tell "switched slot"
 * from "did not switch", and a fixture with three components per slot cannot
 * tell "paginated" from "truncated" -- both are mutations this file has to be
 * able to fail on. Slot 1 carries thirteen occupied components against twelve
 * cells, so overflow is real rather than theoretical.
 * ------------------------------------------------------------------------- */
const many = (prefix, n) => Array.from({ length: n }, (_, i) => prefix + (i + 1));
const CHAIN = { slots: [
  /* 0: a plain rig -- one synth, two FX, one bus */
  { midiFx: [], synth: "Braids", fx: ["Freeverb", "Tapescam"], buses: ["Drums"] },
  /* 1: thirteen occupied components against twelve cells, and a HOLE at
     midiFx position 2 -- so overflow AND the compaction rule are both real */
  { midiFx: ["Arp", null, "Chord"], synth: "Surge", fx: many("fx", 10),
    buses: ["BusA", "BusB"] },
  /* 2: a synth only */
  { synth: "DX7" },
  /* 3: empty */
  {},
] };

/* The parameter view stands in for whatever the caller draws; all this test
 * needs of it is that it is DISTINGUISHABLE from the map in pixels. */
const PARAM_INK = (ctx) => { ctx.clear(); ctx.fillRect(0, 0, 128, 3, 1); };

const mkSend = () => { const log = [];
  const fn = (p) => { log.push(p); return true; }; fn.log = log; return fn; };

const unpack = (p) => { const out = [];
  for (let i = 0; i < p.length; i += 4) {
    const cin = p[i];
    const n = cin === 0x04 ? 3 : (cin - 0x04);
    for (let k = 0; k < n; k++) out.push(p[i + 1 + k]);
  }
  return out; };
const kindOf = (packets) => { const b = unpack(packets);
  if (b[0] !== 0xF0) return "?";
  const id = (b[6] << 8) | b[7];
  return id === 0x0602 ? "framebuffer" : id === 0x0604 ? "ring" : "other"; };

/* One rig: a nav, a display, a canvas, and a clock the test advances by hand.
 * The frame producer is the nav`s own render, which is the whole point --
 * "what is on screen" and "what the state says" must be the same answer. */
function rig(opts) {
  const cv = createCanvas();
  const display = createDisplay();
  const focused = [];
  const pages = (opts && opts.pageCount) || 6;
  const nav = createNav({
    display,
    chainOf: () => CHAIN,
    renderParams: PARAM_INK,
    pageCountOf: () => pages,
    onFocus: (s, c) => focused.push([s, c]),
    ...(opts || {}),
  });
  const send = mkSend();
  let now = 1000;
  const at = (t) => { now = t; };
  /* One surface frame: restate the invariant, then let the paced sender do at
   * most one message. Exactly the shape shadow_ui.js will call it in. */
  const frame = () => { nav.tick(now); return display.tick(send, () => {
    nav.render(cv, now); return cv.toBuffer(); }); };
  const ev = (e) => { const r = nav.handle(e, now); frame(); return r; };
  return { nav, display, cv, send, focused, at, frame, ev,
           now: () => now, fbCount: () => send.log.filter(
             (p) => kindOf(p) === "framebuffer").length };
}

/* ---- 1. THE SCRIPTED SEQUENCE, AND ITS EXACT FRAMEBUFFER COUNT ---------- */
{
  const r = rig();
  eq("starts on parameters", r.nav.mapVisible(r.now()), false);

  eq("shift down raises the map",
     r.ev({ type: "shift", down: true }).action, "map");
  eq("...and the map is up", r.nav.mapVisible(r.now()), true);

  eq("a top-row push switches slot",
     r.ev({ type: "push", enc: 1 }), { action: "slot", slot: 1 });
  eq("...and leaves the map up", r.nav.mapVisible(r.now()), true);

  /* Cell 4 of slot 1 is its first occupied component -- the Arp at MIDI FX
   * position 1. The HOLE at position 2 means position 3 (Chord) is the
   * SECOND cell, which is the off-by-one a compaction bug would produce. */
  eq("a lower push jumps to that component",
     r.ev({ type: "push", enc: 5 }),
     { action: "focus", slot: 1, component: "midi_fx3" });
  eq("...and drops back to parameters", r.nav.mapVisible(r.now()), false);
  eq("...telling the caller once", r.focused, [[1, "midi_fx3"]]);

  eq("shift up after a jump changes nothing",
     r.ev({ type: "shift", down: false }), null);

  eq("three actions, three framebuffers", r.fbCount(), 3);

  /* Rule 4 in its sharpest form: a surface that repaints per tick passes every
   * assertion above and fails this one. */
  for (let i = 0; i < 20; i++) r.frame();
  eq("twenty idle ticks add no framebuffer", r.fbCount(), 3);
  eq("...and nothing is owed", r.display.framebufferOwed, false);
}

/* ---- 2. THE MAP IS DRAWN ON THE PRESS, IN PIXELS ------------------------ */
{
  const r = rig();
  const params = createCanvas(); PARAM_INK(params);
  const paramsBytes = Array.from(params.toBuffer());

  r.ev({ type: "shift", down: true });
  const want = createCanvas();
  renderMap(want, buildMap(CHAIN, { slot: 0, page: 0, showBuses: false }));
  eq("the frame drawn on the PRESS is the map",
     Array.from(r.cv.toBuffer()), Array.from(want.toBuffer()));
  eq("...and is not the parameter view",
     Array.from(r.cv.toBuffer()).join() === paramsBytes.join(), false);

  r.ev({ type: "shift", down: false });
  eq("the frame drawn on the RELEASE is the parameter view",
     Array.from(r.cv.toBuffer()), paramsBytes);
}

/* ---- 3. A LOST NOTE-OFF CANNOT STRAND THE MAP --------------------------- */
{
  const r = rig();
  r.ev({ type: "shift", down: true });
  eq("map up while the hold is fresh", r.nav.mapVisible(r.now()), true);

  /* No note-off is EVER sent below. The only thing that happens is time. */
  r.at(1000 + MAP_MAX_HOLD_MS - 1);
  r.frame();
  eq("still up one millisecond before the budget", r.nav.mapVisible(r.now()), true);

  r.at(1000 + MAP_MAX_HOLD_MS);
  r.frame();
  eq("DOWN with no event at all", r.nav.mapVisible(r.now()), false);

  const params = createCanvas(); PARAM_INK(params);
  eq("...and the parameter view is what went out",
     Array.from(r.cv.toBuffer()), Array.from(params.toBuffer()));

  /* The literal acceptance criterion: the next interaction is a PARAMETER
   * interaction, handed back to the caller rather than eaten as a map
   * gesture. */
  eq("a turn now edits a parameter",
     r.nav.handle({ type: "turn", enc: 9, ticks: 1 }, r.now()),
     { action: "turn", enc: 9, ticks: 1 });
  eq("a push now clicks a parameter",
     r.nav.handle({ type: "push", enc: 9 }, r.now()),
     { action: "click", enc: 9 });
  eq("...and no focus was moved by any of it", r.focused, []);

  /* And the escape is not a one-shot: a fresh press works afterwards. */
  r.at(1000 + MAP_MAX_HOLD_MS + 50);
  eq("a fresh press raises the map again",
     r.ev({ type: "shift", down: true }).action, "map");
  eq("...and it is up", r.nav.mapVisible(r.now()), true);
}

/* ---- 4. SHIFT + TURN ---------------------------------------------------- */
{
  /* pageStep owns the pairing: the parameter view shows N and N+1, so a
   * detent moves by TWO or the two halves overlap. */
  eq("one detent steps a whole pair", pageStep(0, 1, 6), HALVES);
  eq("a detent back steps a pair back", pageStep(4, -1, 6), 2);
  eq("clamped at zero", pageStep(0, -1, 6), 0);
  eq("clamped at the last page", pageStep(4, 1, 6), 4);
  eq("a three-page component can still reach its last page", pageStep(0, 1, 3), 2);

  const r = rig({ pageCount: 6 });
  r.ev({ type: "shift", down: true });
  eq("shift + turn on a lower encoder pages the pair",
     r.ev({ type: "turn", enc: 9, ticks: 1 }), { action: "page", pageIndex: 2 });
  eq("...and the map is still up", r.nav.mapVisible(r.now()), true);
  eq("a big turn back clamps at page 0",
     r.ev({ type: "turn", enc: 9, ticks: -5 }), { action: "page", pageIndex: 0 });
  const before = r.fbCount();
  r.ev({ type: "turn", enc: 9, ticks: -1 });   /* already at 0 */
  eq("...no repaint for a clamped detent", r.fbCount(), before);
}

/* ---- 5. THE MAP`S OWN OVERFLOW ------------------------------------------ */
{
  const r = rig();
  r.ev({ type: "shift", down: true });
  r.ev({ type: "push", enc: 1 });              /* slot 1: thirteen components */
  eq("slot 1 overflows twelve cells",
     buildMap(CHAIN, { slot: 1, page: 0 }).pageCount, 2);
  eq("a turn on the SLOT ROW pages the map",
     r.ev({ type: "turn", enc: 0, ticks: 1 }), { action: "mapPage", mapPage: 1 });
  eq("...clamped at the last map page",
     r.ev({ type: "turn", enc: 0, ticks: 1 }), { action: "mapPage", mapPage: 1 });
  eq("the tail is reachable",
     buildMap(CHAIN, { slot: 1, page: 1 }).cells[4].label, "fx10");
  eq("a slot with one page cannot be paged off it",
     rigMapPage(0), 0);
}
function rigMapPage(slot) {
  const r = rig();
  r.ev({ type: "shift", down: true });
  r.ev({ type: "push", enc: slot });
  r.ev({ type: "turn", enc: 0, ticks: 3 });
  return r.nav.mapPage;
}

/* ---- 6. THE BUS CELL ---------------------------------------------------- */
{
  const r = rig();
  r.ev({ type: "shift", down: true });
  eq("pressing the CURRENT slot swaps the lower 12 to buses",
     r.ev({ type: "push", enc: 0 }), { action: "buses", showBuses: true });
  const want = createCanvas();
  renderMap(want, buildMap(CHAIN, { slot: 0, page: 0, showBuses: true }));
  eq("...in pixels", Array.from(r.cv.toBuffer()), Array.from(want.toBuffer()));
  eq("...and a bus is a jump target",
     r.ev({ type: "push", enc: 4 }),
     { action: "focus", slot: 0, component: "bus1" });

  /* Switching slot must leave the bus view, or the new slot`s components are
   * hidden behind a mode the user set on a different slot. */
  const r2 = rig();
  r2.ev({ type: "shift", down: true });
  r2.ev({ type: "push", enc: 0 });             /* buses on */
  r2.ev({ type: "push", enc: 2 });             /* switch slot */
  eq("switching slot returns to components", r2.nav.showBuses, false);
}

/* ---- 7. HOLES ARE NOT DESTINATIONS -------------------------------------- */
{
  const r = rig();
  r.ev({ type: "shift", down: true });
  r.ev({ type: "push", enc: 2 });              /* slot 2: one synth only */
  const before = r.fbCount();
  eq("an empty lower cell is not a jump target",
     r.ev({ type: "push", enc: 9 }), null);
  eq("...it does not consume the hold", r.nav.mapVisible(r.now()), true);
  eq("...it moves no focus", r.focused, []);
  eq("...and it repaints nothing", r.fbCount(), before);
}

/* ---- 8. A RELEASE IS NOT A SECOND PUSH ---------------------------------- */
{
  const r = rig();
  r.ev({ type: "shift", down: true });
  r.ev({ type: "push", enc: 4 });              /* jumps, drops the map */
  const before = r.fbCount();
  eq("the button release is inert", r.ev({ type: "release", enc: 4 }), null);
  eq("...and repaints nothing", r.fbCount(), before);
  eq("...one focus move, not two", r.focused.length, 1);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'

# The map must not become a second place that writes parameters. Every jump is
# reported to the caller through onFocus; a nav that set a param itself would
# pass every assertion above and still be the second value-application path
# e16_view.mjs is explicitly written not to have.
if grep -nE 'setParam|host_module_set_param|shadow_set_param' src/shared/e16_surface.mjs; then
    echo "FAIL: e16_surface.mjs writes parameters itself -- navigation reports"
    echo "      focus to the caller, it does not apply values."
    exit 1
fi
echo "ok   navigation writes no parameters"
