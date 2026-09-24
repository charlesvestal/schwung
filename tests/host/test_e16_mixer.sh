#!/usr/bin/env bash
# THE E16 MIXER (src/shared/e16_mixer.mjs): double-tap Shift for four tracks
# by four rows -- level (push mute, Shift+push solo), Send A, Send B (push to
# 0 and back, Shift+push 100%), and returns / capture / filter.
#
# NOTE: this whole file is one single-quoted node script. No apostrophes in it.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { createMixer, renderMixer, SEND_MAX, VOLUME_MAX, MIXER_ROW_RGB } from "./src/shared/e16_mixer.mjs";
import { createNav, createDisplay, createSurface, DOUBLE_TAP_MS, MAP_SHOW_DELAY_MS } from "./src/shared/e16_surface.mjs";
import { createCanvas } from "./src/shared/e16_canvas.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; } else console.log("ok   " + n); };

/* A fake host: the slot params and the global send returns. */
function fakeIo() {
  const slots = [0, 1, 2, 3].map(() => ({ "slot:volume": "1", "slot:muted": "0", "slot:soloed": "0", "slot:pan": "0.000",
    "buses:main_send1": "0", "buses:main_send2": "64" }));
  const glob = { "send1:return": "100", "send2:return": "127", "master_fx:filter": "0.000" };
  const io = { slots, glob, reads: 0, writes: [], skipbacks: 0,
    getSlot: (s, k) => { io.reads++; return slots[s][k] === undefined ? null : slots[s][k]; },
    setSlot: (s, k, v) => { io.writes.push([s, k, v]); slots[s][k] = String(v); return true; },
    getGlobal: (k) => { io.reads++; return glob[k] === undefined ? null : glob[k]; },
    setGlobal: (k, v) => { io.writes.push(["g", k, v]); glob[k] = String(v); return true; },
    skipback: () => { io.skipbacks++; return true; },
    nameOf: (s) => ["Braids", "Surge", "Empty", "Dx7"][s] };
  return io;
}

/* ---- the model ---- */
{
  const io = fakeIo(); const m = createMixer(io);
  m.load();
  eq("load reads every value once", io.reads, 4 * 6 + 3);
  eq("level cell prints dB", m.cell(0), { label: "Vol", value: "0.0" });
  m.turn(0, 2, false);
  eq("a turn steps the level half a dB per detent", m.cell(0).value, "+1.0");
  eq("...written as the slot volume", io.slots[0]["slot:volume"], String((10 ** (1 / 20)).toFixed(4)));
  for (let i = 0; i < 40; i++) m.turn(0, 1, false);
  eq("the level stops at +6 dB (volume 2)", Number(io.slots[0]["slot:volume"]), VOLUME_MAX);
  for (let i = 0; i < 400; i++) m.turn(1, -1, false);
  eq("...and bottoms out at -inf", [m.cell(1).value, Number(io.slots[1]["slot:volume"])], ["-inf", 0]);

  m.turn(1, -15, true);
  eq("Shift+turn on a level pans", [io.slots[1]["slot:pan"], m.cell(1).label], ["-0.30", "L 30"]);
  m.turn(1, 15, true);
  eq("...back to centre reads Vol again", m.cell(1).label, "Vol");
  m.push(2, false);
  eq("push on a level mutes", [io.slots[2]["slot:muted"], m.cell(2).label], ["1", "MUTE"]);
  m.push(2, false);
  eq("...and pushing again unmutes", io.slots[2]["slot:muted"], "0");
  m.push(3, true);
  eq("Shift+push on a level solos", [io.slots[3]["slot:soloed"], m.cell(3).label], ["1", "SOLO"]);

  /* Send B of track 1 is 64. SWITCHED OFF IS A STATE, like mute. */
  m.push(8, false);
  eq("push switches a send off: Move sees 0", io.slots[0]["buses:main_send2"], "0");
  eq("...the cell is inverted and still shows its level", m.cell(8), { label: "SndB", value: "50%", off: true });
  m.turn(8, 30, false);
  eq("turning while off sets the level it comes back at, with no effect yet",
     [m.cell(8), io.slots[0]["buses:main_send2"]], [{ label: "SndB", value: "98%", off: true }, "0"]);
  m.push(8, false);
  eq("push switches it on at that level", [m.cell(8).off, io.slots[0]["buses:main_send2"]], [undefined, "124"]);
  m.push(8, false); io.slots[0]["buses:main_send2"] = "20";
  for (let k = 0; k < 40; k++) m.refreshNext();
  eq("a send switched off here but turned up elsewhere is on again", [m.cell(8).off, m.cell(8).value], [undefined, "16%"]);
  m.push(4, true);
  eq("Shift+push on a send throws it to 100%", [io.slots[0]["buses:main_send1"], m.cell(4).value], [String(SEND_MAX), "100%"]);
  m.turn(5, 3, false);
  eq("a send turns in steps", io.slots[1]["buses:main_send1"], "6");

  m.push(12, false);
  eq("push switches Return A off", [io.glob["send1:return"], m.cell(12)], ["0", { label: "RtnA", value: "79%", off: true }]);
  m.push(12, false);
  eq("...and back on at its level", io.glob["send1:return"], "100");
  m.turn(13, -5, false);
  eq("Return B turns", io.glob["send2:return"], "117");
  m.push(14, false);
  eq("the capture knob saves Skipback on a push", io.skipbacks, 1);
  eq("the filter starts off", m.cell(15).value, "off");
  eq("a push on an off filter does nothing", m.push(15, false), false);
  m.turn(15, -20, false);
  eq("turning left is a low-pass", [m.cell(15).value, io.glob["master_fx:filter"]], ["LP 40", "-0.400"]);
  m.push(15, false);
  eq("push switches the filter off", io.glob["master_fx:filter"], "0.000");
  eq("...inverted, showing where it comes back", m.cell(15), { label: "Filt", value: "LP 40", off: true });
  m.turn(15, -10, false);
  eq("turning while off moves where it comes back, silently", [m.cell(15).value, io.glob["master_fx:filter"]], ["LP 60", "0.000"]);
  m.push(15, false);
  eq("...push switches it on there", io.glob["master_fx:filter"], "-0.600");
  m.push(15, true);
  eq("Shift+push resets it to off", m.cell(15).value, "off");
  eq("...and forgets it (a push does not bring it back)", m.push(15, false), false);
  eq("the filter ring is bipolar, centred when off", [m.ringFor(15).bipolar, m.ringFor(15).amount], [true, Math.round(0.5 * 16383)]);

  const r = m.rings();
  eq("all sixteen rings", r.length, 16);
  eq("each row wears its colour", [r[5], r[9], r[13]].map((x) => [x.r, x.g, x.b]),
     [MIXER_ROW_RGB[1], MIXER_ROW_RGB[2], MIXER_ROW_RGB[3]].map((c) => [c.r, c.g, c.b]));
  m.push(2, false);
  eq("a muted track ring goes dim", m.ringFor(2).r < MIXER_ROW_RGB[0].r, true);

  /* A failed read is blank, never zero. */
  const io2 = fakeIo(); io2.getSlot = () => null; const m2 = createMixer(io2); m2.load();
  eq("an unread level draws nothing", m2.cell(0).value, "");
  eq("...and a turn on it writes nothing", [m2.turn(0, 1, false), io2.writes.length], [false, 0]);

  const cv = createCanvas(); renderMixer(cv, m);
  eq("the mixer draws", Array.from(cv.toBuffer()).some((b) => b), true);
}

/* ---- the gesture: double-tap Shift ---- */
{
  const display = createDisplay();
  const mk = () => createNav({ display, chainOf: () => ({ slots: [{ synth: "A" }, {}, {}, {}] }),
    pageCountOf: () => 4, renderParams: () => {}, renderMixer: () => {}, onFocus: () => {} });
  let nav = mk();
  const tap = (t) => { nav.handle({ type: "shift", down: true }, t); nav.handle({ type: "shift", down: false }, t + 60); };
  tap(1000);
  eq("one tap is not the mixer", nav.mixer, false);
  const r = nav.handle({ type: "shift", down: true }, 1000 + 60 + DOUBLE_TAP_MS - 10);
  eq("a second tap within DOUBLE_TAP_MS toggles it on", [r, nav.mixer], [{ action: "mixer", on: true }, true]);
  nav.handle({ type: "shift", down: false }, 1500);
  eq("...and it stays on after the release (a view, not a hold)", nav.mixer, true);
  eq("a turn in the mixer is a mixer turn", nav.handle({ type: "turn", enc: 5, ticks: 1 }, 3000),
     { action: "mixerTurn", enc: 5, ticks: 1, shift: false });
  nav.handle({ type: "shift", down: true }, 4000);
  eq("Shift+turn in the mixer is a mixer turn with shift, not a page",
     nav.handle({ type: "turn", enc: 0, ticks: 1 }, 4100), { action: "mixerTurn", enc: 0, ticks: 1, shift: true });
  eq("...and the map stays hidden", nav.mapVisible(4100 + MAP_SHOW_DELAY_MS * 3), false);
  nav.handle({ type: "shift", down: false }, 4200);
  eq("Shift+push in the mixer is a mixer push with shift",
     (nav.handle({ type: "shift", down: true }, 5000), nav.handle({ type: "push", enc: 3 }, 5050)),
     { action: "mixerPush", enc: 3, shift: true });
  nav.handle({ type: "shift", down: false }, 5100);
  nav.handle({ type: "shift", down: true }, 6000);
  eq("holding Shift in the mixer never opens the slot map", nav.mapVisible(6000 + MAP_SHOW_DELAY_MS * 5), false);
  eq("...and the mixer stays up", nav.mixer, true);
  nav.handle({ type: "shift", down: false }, 6000 + MAP_SHOW_DELAY_MS * 5);
  tap(7000); nav.handle({ type: "shift", down: true }, 7200);
  eq("double-tap again leaves the mixer", nav.mixer, false);
  nav.handle({ type: "shift", down: false }, 7260);

  nav = mk();
  nav.handle({ type: "shift", down: true }, 1000); nav.handle({ type: "shift", down: false }, 1000 + DOUBLE_TAP_MS + 50);
  nav.handle({ type: "shift", down: true }, 1000 + DOUBLE_TAP_MS + 100);
  eq("a slow press is not a tap", nav.mixer, false);
  nav = mk();
  nav.handle({ type: "shift", down: true }, 1000); nav.handle({ type: "turn", enc: 5, ticks: 1 }, 1020);
  nav.handle({ type: "shift", down: false }, 1060); nav.handle({ type: "shift", down: true }, 1100);
  eq("a press that paged is not a tap", nav.mixer, false);
  nav = mk();
  tap(1000); nav.handle({ type: "shift", down: true }, 1000 + 60 + DOUBLE_TAP_MS + 20);
  eq("a second tap too late is not a double tap", nav.mixer, false);
}

/* ---- through the surface: MIDI in, parameter writes out ---- */
{
  const io = fakeIo();
  let t = 0;
  const s = createSurface({ now: () => t, send: () => true, chainOf: () => ({ slots: [{ synth: "A" }, {}, {}, {}] }), mixer: io });
  s.setEnabled(true);
  const shift = (down) => s.feedMidi(down ? [0x90, 0x10, 0x7F] : [0x80, 0x10, 0x00]);
  t = 1000; shift(true); t = 1060; shift(false); t = 1120; shift(true); t = 1180; shift(false);
  eq("double-tap Shift over the wire opens the mixer", s.nav ? s.nav.mixer : "no nav getter", true);
  const before = io.writes.length;
  s.feedMidi([0xB0, 0x01, 0x41]);          /* encoder 0 turned one detent up */
  eq("turning encoder 1 writes the track 1 slot volume",
     io.writes.slice(before).map((w) => w[1]), ["slot:volume"]);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
