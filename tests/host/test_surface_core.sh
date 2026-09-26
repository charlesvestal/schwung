#!/usr/bin/env bash
# surface_core.mjs -- what every external surface (E16, EC4) shares.
#
#   - ONE focus: going back is going back (a module remembers its page, a
#     slot its module), and Follow parks the surface focus and gives it back
#   - a null from the follow source is not a plan
#   - the binding writes a foreign parameter write into the cache, only for
#     the focused slot and only for cells on screen
#   - knob feel: pulses scaled to Move detents once, choices by angle, the
#     Mixer through the knob engine
#   - presence: an owed goodbye survives a refused send; no goodbye, none owed
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { createFocus, createBinding, createKnobFeel, createPresence, TURN_IDLE_MS }
    from "./src/shared/surface_core.mjs";
import { E16_PULSES_PER_DETENT, E16_SELECTOR, MOVE_DETENTS_PER_ROTATION, e16Curve }
    from "./src/shared/e16_surface.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };

const chain = { slots: [
  { midiFx: ["arp"], synth: "obxd", fx: ["verb"] },
  { midiFx: [], synth: null, fx: ["delay"] },
  { midiFx: [], synth: "dx7", fx: [] },
  {},
] };

/* ---- the focus ---- */
{
  const f = createFocus({ chainOf: () => chain });
  eq("starts on slot 1, synth, page 1", [f.slot, f.component, f.pageIndex], [0, "synth", 0]);
  f.setPage(3);
  f.set(0, "fx1");
  eq("another module starts on its own page", f.pageIndex, 0);
  f.setPage(1);
  f.set(0, "synth");
  eq("going back to a module lands on the page it was left on", f.pageIndex, 3);
  f.set(0, "fx1");
  f.enterSlot(2);
  eq("a slot never visited is entered at its synth", [f.slot, f.component], [2, "synth"]);
  f.enterSlot(0);
  eq("a slot is entered at the module last edited there, on its page", [f.component, f.pageIndex], ["fx1", 1]);
  f.enterSlot(1);
  eq("a slot with no synth is entered at its first module", f.component, "fx1");
  f.enterSlot(3);
  eq("an empty slot falls back to the synth position", f.component, "synth");
  eq("components in chain order", f.components(0).map((c) => c.component), ["midi_fx1", "synth", "fx1"]);
}

/* ---- follow ---- */
{
  let src = null;
  const f = createFocus({ chainOf: () => chain, followFocusOf: () => src });
  f.set(2, "synth"); f.setPage(2);
  src = { slot: 0, component: "fx1" };
  f.setFollow(true);
  eq("follow takes Move screen at once", [f.slot, f.component], [0, "fx1"]);
  src = null;
  eq("a null source is not a plan", [f.poll(), f.slot, f.component], [false, 0, "fx1"]);
  src = { slot: 1, component: "fx1" };
  eq("a change is followed", [f.poll(), f.slot], [true, 1]);
  eq("a second setFollow(true) does not re-park", f.setFollow(true), false);
  f.setFollow(false);
  eq("follow off gives the surface focus back, page and all",
     [f.slot, f.component, f.pageIndex], [2, "synth", 2]);
}

/* ---- the binding ---- */
{
  const f = createFocus({ chainOf: () => chain });
  const loads = [];
  const ctl = { pages: [], state: { values: { cutoff: "0.1" } }, load: (x) => loads.push(x.slot + ":" + x.component),
                tick() {}, metaIndex: { getOrGuess: () => null } };
  const b = createBinding({ makeController: () => ctl, focus: f });
  eq("sync loads the focus", [b.sync(), loads], [true, ["0:synth"]]);
  eq("...once", [b.sync(), loads.length], [false, 1]);
  f.set(2, "synth");
  eq("a jump reloads", [b.sync(), loads[1]], [true, "2:synth"]);
  const view = { cells: [{ key: "cutoff", enc: 0 }, null] };
  eq("a write to another slot touches nothing", b.noteWrite(0, "synth:cutoff", "0.9", view).length, 0);
  eq("a prefixed write to the focused slot lands in the cache",
     [b.noteWrite(2, "synth:cutoff", "0.9", view).length, ctl.state.values.cutoff], [1, "0.9"]);
  eq("a key not on screen is left alone", b.noteWrite(2, "reso", "1", view).length, 0);
}

/* ---- knob feel ---- */
{
  const one = createKnobFeel();
  one.begin(0, 0);
  eq("a device at Move resolution passes pulses through", one.detents(0, 3), 3);
  const ec4 = createKnobFeel({ pulsesPerDetentOf: () => 0.5 });
  ec4.begin(0, 0);
  eq("coarser pulses are scaled to detents", ec4.detents(0, 3), 6);
  const slow = createKnobFeel({ pulsesPerDetentOf: () => 2 });
  slow.begin(0, 0);
  eq("a fraction carries", [slow.detents(0, 1), slow.detents(0, 1)], [0, 1]);
  slow.detents(0, 1);
  eq("a reversal starts over", slow.detents(0, -1), 0);
  slow.begin(0, 10000);
  slow.detents(0, 1);
  slow.begin(0, 10000 + TURN_IDLE_MS + 1);
  eq("a pause starts over", slow.detents(0, 1), 0);
  eq("choices step by angle", [slow.steps(1, 5, 6), slow.steps(1, 1, 6)], [0, 1]);

  const turns = [];
  const mixer = { turn: (enc, ticks, alt) => { turns.push([enc, ticks, alt]); return true; } };
  let t = 0, moved = 0;
  for (let i = 0; i < 20; i++) { t += 100; if (one.mixerTurn(mixer, 0, 1, false, t)) moved++; }
  eq("the Mixer moves through the engine, not a fixed step a detent", moved > 0 && moved < 20, true);
  eq("...in the Mixer own ticks, on the level", turns.every((x) => x[0] === 0 && x[1] >= 1 && x[2] === false), true);
  eq("capture has no travel", one.mixerTurn(mixer, 14, 1, false, t), false);
}

/* ---- the E16, measured: one slow turn is ~46 ticks, and must be ~one Move turn ---- */
{
  const e16 = createKnobFeel({ pulsesPerDetentOf: () => E16_PULSES_PER_DETENT });
  let d = 0, tt = 0;
  for (let i = 0; i < 46; i++) { tt += 100; e16.begin(0, tt); d += e16.detents(0, 1); }
  eq("one slow E16 rotation is about one Move rotation of detents (a full sweep)",
     Math.abs(d - MOVE_DETENTS_PER_ROTATION) <= 6, true);
  eq("E16 choices step by angle, not per tick", E16_SELECTOR.choice > 1, true);
  /* The E16 own x8 at speed: compressed, or one quick turn slams to max. */
  const fast = createKnobFeel({ pulsesPerDetentOf: () => E16_PULSES_PER_DETENT, pulseCurve: (p) => e16Curve(p) });
  fast.begin(1, 0); const one = fast.detents(1, 1);
  fast.begin(1, 1000); const eight = fast.detents(1, 8);
  eq("a fast E16 message (x8) moves at most ~3 slow clicks, not 8", eight > one && eight <= 3 * (one + 1), true);
}

/* ---- presence ---- */
{
  let refuse = true; const sent = [];
  const send = (p) => { if (refuse) return false; sent.push(p); return true; };
  const p = createPresence({ probeMs: 1000, keepaliveMs: 2000, lossMs: 5000, probeMsg: () => [1],
    exitMsg: () => [2], isReply: (b) => b[0] === 9, packetize: (b) => b });
  p.setEnabled(true, 0, send);
  p.setEnabled(false, 10, send);
  eq("a refused goodbye is owed", sent.length, 0);
  refuse = false;
  p.tick(20, send);
  eq("...and paid on the next tick", sent, [[2]]);
  const q = createPresence({ probeMs: 1000, keepaliveMs: 2000, lossMs: 5000, probeMsg: () => [1],
    isReply: (b) => b[0] === 9, packetize: (b) => b });
  const s2 = [];
  q.setEnabled(true, 0, (x) => { s2.push(x); return true; });
  q.tick(0, (x) => { s2.push(x); return true; });
  eq("probes on the first tick", s2, [[1]]);
  eq("another device is not an answer", [q.onSysex([7], 5), q.present], [false, false]);
  eq("the device is", [q.onSysex([9], 5), q.present], [true, true]);
  q.tick(5100, () => true);
  eq("silence past lossMs: gone", q.present, false);
  q.setEnabled(false, 6000, (x) => { s2.push(x); return true; });
  eq("no exitMsg, no goodbye owed", s2.length, 1);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
