#!/usr/bin/env bash
# The EC4 surface, end to end against an emulated device.
#
# The emulator below decodes what the surface SENDS (USB-MIDI packets ->
# SysEx -> page/offset/character commands) onto a model of the EC4 screen, so
# every assertion is about what the device would show, not about what the
# surface believes it showed. The properties, each silent on hardware:
#
#   - nothing is written to a setup that is not Schwung's: its '----' cells
#     would show our text, and its encoders are the user's own
#   - no message is longer than one SPI frame places whole (12 packets):
#     a longer one is spliced by Move's own MIDI and lands damaged
#   - leaving Schwung's setup and coming back rewrites the screen, since the
#     device redrew its own names while we were away
#   - one page at a time, navigated by labelled knobs (pages, slot, module),
#     with VOL and PAN for the focused slot
#   - Shift is SysEx on the EC4: a TAP switches Module <-> Mixer, a HOLD is
#     the alternate layer, and Shift + push arrives as its own report
#   - turning the setting off gives the device its "----" names back
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { createEc4Surface, DEFAULT_SETUP, OVERLAY_HOLD_MS, LOSS_MS, barRow, headline, SELECTOR_PULSES,
         SLOT_PULSES, SHIFT_TAP_MS, listRow,
         DEFAULT_PULSES_PER_DETENT } from "./src/shared/ec4_surface.mjs";
import { BLOCK, schwungSetupMsg } from "./src/shared/ec4_protocol.mjs";
import { ENUM_DELTA_DIV } from "./src/shared/knob_engine.mjs";
import { PAGE_KNOBS } from "./src/shared/param_pages/page_plan.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };
const ok = (n, c) => eq(n, !!c, true);

/* ---- the emulated EC4 ---- */
/* Like the real one (docs/EC4_SURFACE.md): it answers a request with its
 * setup, and acknowledges every other message with a bare header. Replies
 * are queued and delivered by run(), never inside the send that caused them. */
const dev = { names: "----".repeat(16), total: " ".repeat(80), overlay: false, queries: 0, msgs: [], maxPackets: 0,
              setup: null, replies: [], dumps: [] };
const ROM_TO_ASCII = { 0xFA: "[", 0xFB: "\\", 0xFC: "]", 0xFE: "|" };
function devMessage(m) {
  dev.msgs.push(m);
  /* A setup download (41 = download start), not a live message. */
  if (m[4] === 0x41) { dev.dumps.push(m); return; }
  if (m.length === 8 && m[4] === 0x4E && m[5] === 0x20) {
    dev.queries++;
    if (dev.setup !== null) dev.replies.push(report(dev.setup, 0));
    return;
  }
  if (dev.setup !== null) dev.replies.push(HDR.concat([0xF7]));
  let page = 0, off = 0;
  for (let i = 7; i + 2 < m.length; i += 3) {
    const cmd = m[i], v = ((m[i + 1] & 0x0F) << 4) | (m[i + 2] & 0x0F);
    if (cmd === 0x4E && v === 0x20 + 0x00) page = 0;
    if (cmd === 0x4E && (m[i + 1] & 0x0F) === 2) {
      const f = m[i + 2] & 0x0F;
      if (f <= 3) page = f; else if (f === 4) dev.overlay = true; else if (f === 5) dev.overlay = false;
    } else if (cmd === 0x4A) off = v;
    else if (cmd === 0x4D) {
      const ch = ROM_TO_ASCII[v] || String.fromCharCode(v);
      if (page === 0) { dev.names = dev.names.slice(0, off) + ch + dev.names.slice(off + 1); off++; }
      if (page === 3) { dev.total = dev.total.slice(0, off) + ch + dev.total.slice(off + 1); off++; }
    }
  }
}
let asm = null;
function send(pkts) {
  let msgBytes = 0;
  for (let i = 0; i < pkts.length; i += 4) {
    const cin = pkts[i] & 0x0F, n = cin === 0x05 ? 1 : cin === 0x06 ? 2 : 3;
    for (let k = 1; k <= n; k++) {
      const b = pkts[i + k];
      if (b === 0xF0) asm = [b]; else if (asm) { asm.push(b); if (b === 0xF7) { devMessage(asm); asm = null; } }
    }
  }
  dev.maxPackets = Math.max(dev.maxPackets, pkts.length / 4);
  return true;
}
const HDR = [0xF0, 0x00, 0x00, 0x00, 0x4E, 0x2C, 0x1B];
const report = (setup, group) => HDR.concat([0x4E, 0x28, 0x10 | setup, 0x4E, 0x24, 0x10 | group, 0xF7]);
const key = (k, down) => HDR.concat([0x4E, 0x26, 0x10 | k, 0x4E, 0x2E, down ? 0x11 : 0x10, 0xF7]);
const shiftedPush = (n, down) => HDR.concat([0x4E, 0x2A, 0x10 | n, 0x4E, 0x2E, down ? 0x11 : 0x10, 0xF7]);

/* ---- a fake page controller, as test_e16_skipped_pages.sh builds one ---- */
const page = (name, keys) => ({ kind: PAGE_KNOBS, name, level: name, keys });
const writes = [];
const ctl = {
  pages: [page("Main", ["cutoff", "reso"]), page("Env", ["attack", "decay"]), page("Mod", ["rate"])],
  pageIndex: 0,
  state: { values: { cutoff: "0.5", reso: "0.1", attack: "0.2", decay: "0.3", rate: "0.4" } },
  load(f) { this.loaded = f; }, tick() {},
  goToPage(i) { this.pageIndex = i; },
  onKnobTurn(slot, dir) { writes.push([this.pages[this.pageIndex].name, slot, dir]); },
};
/* ---- fake slot params for the Mixer model ---- */
const slotParams = {};
const mixerIo = {
  getSlot: (s, k) => (slotParams[s + ":" + k] !== undefined ? slotParams[s + ":" + k]
                     : ({ "slot:volume": "1", "slot:muted": "0", "slot:soloed": "0", "slot:pan": "0",
                          "buses:main_send1": "0", "buses:main_send2": "0" })[k]),
  setSlot: (s, k, v) => { slotParams[s + ":" + k] = v; return true; },
  getGlobal: () => "0", setGlobal: () => true, skipback: () => true,
  nameOf: (s) => "Track " + (s + 1),
};

let t = 1000;
let installed = null;
const s = createEc4Surface({ now: () => t, send, onInstalled: (n) => { installed = n; },
  chainOf: () => ({ slots: [{ synth: "obxd", fx: ["freeverb"] }, { synth: "dx7" }, {}, {}] }),
  makeController: () => ctl, mixer: mixerIo, pulsesPerDetentOf: () => 3 });
const run = (ms) => { for (let i = 0; i < ms / 16; i++) {
  t += 16; s.tick();
  const r = dev.replies; dev.replies = []; for (const m of r) s.feedMidi(m);
} };
/* The user switches the EC4 to a setup: it reports it unasked. */
const toSetup = (n) => { dev.setup = n; s.feedMidi(report(n, 0)); };
const row = (r) => dev.names.slice(r * 16, r * 16 + 16);
const CC = (enc, v) => [0xB0, enc + 1, v];
const NOTE = (enc) => [0x90, enc, 0x7F];
/* The EC4 sends PULSES; the surface scales them to Move detents
 * (pulsesPerDetentOf, 3 here); selectors and enums step by rotation. */
const P = 3;
const pulses = (enc, n) => { for (let i = 0; i < Math.abs(n); i++) s.feedMidi(CC(enc, n > 0 ? 1 : 127)); };
const spin = (enc, detents) => pulses(enc, detents * P);
/* Selectors and enums step by ROTATION, not detents: one per SELECTOR_PULSES. */
const choose = (enc, steps) => pulses(enc, steps * (enc === 12 ? SLOT_PULSES : SELECTOR_PULSES));

run(500);
eq("off: nothing is sent", dev.msgs.length, 0);

s.setEnabled(true);
run(2500);
ok("on, unanswered: it asks which setup", dev.queries >= 2);
eq("on, unanswered: no text is written", dev.names, "----".repeat(16));

toSetup(10);   /* setup 11, the user one */
run(1000);
eq("a setup that is not ours is never written", dev.names, "----".repeat(16));
eq("...and is not active", s.present, false);
s.feedMidi(CC(0, 1));
eq("...and its encoders are ignored", writes.length, 0);

toSetup(DEFAULT_SETUP);
run(1000);
eq("setup 13: active", s.present, true);
eq("MODULE view, row 1: the page knobs (ONE page)", row(0), "CUTORESO        ");
eq("...row 2: the rest of that page, not the next page", row(1), " ".repeat(16));
eq("...row 3: page navigation", row(2), "<PG MAIN1/3 PG> ");
eq("...row 4: slot, module, volume, pan", row(3), "SL 1OBXDVOL PAN ");
ok("every message fits one SPI frame (12 packets)", dev.maxPackets <= 12);

s.feedMidi(CC(0, 1)); s.feedMidi(CC(0, 1));
eq("two pulses are less than a detent: nothing moves", writes, []);
s.feedMidi(CC(0, 1));   /* the third pulse is one Move detent */
eq("a page knob reaches the controller, one detent per three pulses", writes, [["Main", 0, 1]]);
run(200);
eq("...and raises the overlay", dev.overlay, true);
eq("...row 1: [page] >> parameter, centred", dev.total.slice(0, 20), "  [Main] >> cutoff  ");
eq("...row 2 blank", dev.total.slice(20, 40), " ".repeat(20));
eq("...row 3: the value in brackets", dev.total.slice(40, 60).trim().startsWith("["), true);
eq("...row 4: a value bar, 0.5 is ten of twenty", dev.total.slice(60, 80), "\x1F".repeat(10) + " ".repeat(10));
run(OVERLAY_HOLD_MS + 500);
eq("the overlay goes after the hold", dev.overlay, false);

s.feedMidi(NOTE(11));   /* PG> */
run(300);
eq("PG> steps ONE page", row(0), "ATTADECA        ");
eq("...quietly: the page buttons raise no overlay", dev.overlay, false);
eq("...and the count follows", row(2), "<PG ENV 2/3 PG> ");
pulses(9, SELECTOR_PULSES - 1);
eq("a page selector does not move before SELECTOR_PULSES", row(0).slice(0, 4), "ATTA");
pulses(9, 1);   /* turning the page name scrolls */
run(300);
eq("turning a page cell scrolls pages", row(0).slice(0, 4), "RATE");
eq("...and lists the pages, current one centred", dev.total.slice(60, 80), "in Env [Mod]        ");
s.feedMidi(NOTE(8)); s.feedMidi(NOTE(8));
run(300);
eq("<PG steps back", row(0).slice(0, 8), "CUTORESO");
choose(8, -1);
run(300);
eq("...and stops at the first page", row(0).slice(0, 8), "CUTORESO");

choose(13, 1);   /* module: synth -> fx1 */
run(300);
eq("turning the module cell moves along the slot", [s.slot, s.component], [0, "fx1"]);
eq("...and its name is on the cell", row(3).slice(4, 8), "FREE");
choose(12, 1);   /* slot 1 -> 2 */
run(300);
eq("turning the slot cell enters that slot at its synth", [s.slot, s.component], [1, "synth"]);
eq("...with no overlay: the cells already say where you are", dev.overlay, false);
eq("...named", row(3).slice(0, 8), "SL 2DX7 ");
choose(12, -1);
run(300);
eq("going back to a slot returns to the module left there", [s.slot, s.component], [0, "fx1"]);

spin(14, 1);   /* VOL: one detent is 0.33 dB -- under one Mixer tick */
eq("VOL turns through the knob engine: one detent is less than a Mixer tick", slotParams["0:slot:volume"], undefined);
spin(14, 1);
ok("VOL writes this slot level", slotParams["0:slot:volume"] !== undefined);
s.feedMidi(NOTE(14));
run(300);
eq("VOL push mutes, and the cell says so", [slotParams["0:slot:muted"], row(3).slice(8, 12)], ["1", "MUTE"]);
spin(15, 2);   /* PAN: 0.5% of -1..1 a detent, so two detents are one 0.02 tick */
run(300);
eq("PAN turns this slot pan, and its cell still says PAN", [slotParams["0:slot:pan"], row(3).slice(12, 16)], ["0.02", "PAN "]);
s.feedMidi(NOTE(15));
run(300);
eq("PAN push centres", [slotParams["0:slot:pan"], row(3).slice(12, 16)], ["0.00", "PAN "]);

s.feedMidi(key(1, true)); s.feedMidi(key(1, false));   /* a Shift TAP */
run(300);
eq("a Shift tap switches to the MIXER", [s.mixerOn, row(1)], [true, "SndASndASndASndA"]);
s.feedMidi(key(1, true));
run(100);
eq("inside the tap window the names do not flash the alternate layer (slot 1 is muted)", row(0), "MUTEVOL VOL VOL ");
run(SHIFT_TAP_MS + 100);
eq("held past the tap window: the alternate layer", row(0), "PAN PAN PAN PAN ");
spin(1, 2);   /* Shift + turn track 2 level = pan */
s.feedMidi(key(1, false));
run(300);
eq("Shift + turn is the alternate (pan), and is not a tap", [slotParams["1:slot:pan"], s.mixerOn], ["0.02", true]);
eq("letting go restores the names", row(1), "SndASndASndASndA");
s.feedMidi(key(1, true)); s.feedMidi(shiftedPush(2, true)); s.feedMidi(shiftedPush(2, false)); s.feedMidi(key(1, false));
eq("Shift + push (SysEx) is the alternate (solo)", slotParams["2:slot:soloed"], "1");
s.feedMidi(key(1, true)); run(SHIFT_TAP_MS + 200); s.feedMidi(key(1, false));
run(300);
eq("a HOLD with nothing touched does not switch view", s.mixerOn, true);
eq("a panned track still reads VOL on the Mixer", row(0).slice(4, 8), "VOL ");
s.feedMidi(key(1, true)); s.feedMidi(key(1, false));
run(300);
eq("another tap goes back to MODULE", [s.mixerOn, row(3).slice(0, 4)], [false, "SL 1"]);

/* An ENUM parameter is a choice: it steps by rotation like a selector, and
 * each step hands the engine the ENUM_DELTA_DIV detents it gates an option at. */
ctl.metaIndex = { getOrGuess: (k) => (k === "reso" ? { type: "enum", options: ["a", "b", "c"], label: "reso" } : null) };
writes.length = 0;
pulses(1, SELECTOR_PULSES - 1);
eq("an enum does not move before SELECTOR_PULSES", writes.length, 0);
pulses(1, 1);
eq("...then moves exactly one option (ENUM_DELTA_DIV detents to the engine)",
   writes, new Array(ENUM_DELTA_DIV).fill(["Main", 1, 1]));
run(100);
eq("...and its overlay shows the options, not a bar", dev.total.slice(60, 80).includes("\x1F"), false);
delete ctl.metaIndex;

toSetup(10);
run(300);
eq("leaving for another setup: inactive", s.present, false);
const before = dev.msgs.length;
run(1000);
ok("...and only queries go out", dev.msgs.slice(before).every((m) => m.length === 8));
dev.names = "----".repeat(16);    /* the device redrew its own setup */
toSetup(DEFAULT_SETUP);
run(1000);
ok("coming back rewrites the screen", dev.names !== "----".repeat(16));

const was = dev.setup; dev.setup = null;   /* unplugged: nothing answers */
run(LOSS_MS + 500);
eq("silent for LOSS_MS: gone", s.present, false);
toSetup(was);
run(500);
eq("...and back on the next answer", s.present, true);

/* ---- installing the Schwung setup from Move ---- */
toSetup(4);                      /* the user picks setup 5 on the EC4 */
s.installBegin();
run(300);
eq("install: the screen can name the EC4s current setup", s.installState.current, 4);
eq("install: arming takes it as the target", [s.installArm(), s.installState.phase, s.installState.setup], [true, "ready", 4]);
dev.setup = null;                /* the user enters receive mode: it stops answering */
const quiet = dev.msgs.length;
run(3000);
eq("install: once armed, NOTHING goes out (a stray message would join the dump)", dev.msgs.length, quiet);
eq("install: send", s.installSend(), true);
run(20000);
eq("install: the whole message arrives as ONE download", dev.dumps.length, 1);
eq("install: ...and it is exactly the single-setup message for that setup",
   JSON.stringify(dev.dumps[0]) === JSON.stringify(schwungSetupMsg(4, 0)), true);
eq("install: the host is told which setup holds the map", [s.installState.phase, installed], ["done", 4]);
toSetup(DEFAULT_SETUP);          /* the user leaves setup mode, on the Schwung setup */
run(1000);
eq("install done: the surface resumes WITHOUT waiting for the screen to close", s.present, true);
s.installEnd();

s.setEnabled(false);
run(2000);
eq("off: the device gets its ---- names back", dev.names, "----".repeat(16));
eq("...and no overlay", dev.overlay, false);
ok("no message was ever longer than one frame", dev.maxPackets <= 12);

eq("bar: empty, full", [barRow(0, false), barRow(1, false)], [" ".repeat(20), BLOCK.repeat(20)]);
eq("bar: bipolar fills from the centre", [barRow(0.75, true), barRow(0.25, true)],
   [" ".repeat(10) + BLOCK.repeat(5) + " ".repeat(5), " ".repeat(5) + BLOCK.repeat(5) + " ".repeat(10)]);
eq("bar: bipolar at the centre is marked, not empty", barRow(0.5, true), " ".repeat(10) + "|" + " ".repeat(9));
eq("headline: [page] >> name, centred", headline("Filter", "Cutoff", "CUTO"), " [Filter] >> Cutoff");
eq("headline: the abbreviation when it fits", headline("Amp", "Gain", "GAIN"), "[Amp] >> Gain (GAIN)");
eq("headline: then the page shortens", headline("Oscillators", "Waveform", "WAVE"), " [OSCI] >> Waveform");
eq("headline: the name is cut last", headline("Oscillators", "Oscillator 2 Waveform", "OSC2"), "[OSCI] >> Oscillator");

eq("default scale: 72 EC4 pulses cover the 210 detents of a Move rotation",
   Math.round(72 / DEFAULT_PULSES_PER_DETENT), 210);
eq("list: current centred in brackets", listRow(["a", "b", "c"], 1), "      a [b] c       ");
eq("list: neighbours run off the edges", listRow(["alpha", "beta", "gamma", "delta", "epsilon"], 2),
   " beta [gamma] delta ");
eq("list: nothing to the left of the first", listRow(["Off", "On"], 0), "       [Off] On     ");
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
