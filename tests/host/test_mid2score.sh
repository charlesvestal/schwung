#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE SCORE CONVERTER, and the two rules that decide whether a preview sounds
# like the module or like mush.
#
#   - `frames` must OUTLAST the last note. A reverb or a long release is still
#     sounding when the score ends, and cutting the file at the final note-off
#     is exactly what makes an effect preview sound like it does nothing.
#
#   - Every sounding note must be CLOSED inside the window. A held note with no
#     note-off drones through the tail. (A real one was found the other way
#     round: a note-off scheduled at sample 44100 never fired, because the
#     render loop steps by 128 and 44100 is not a multiple of it. That made
#     every module look like it ignored note-offs.)
#
# Also pinned: a note-on with velocity 0 IS a note-off. Files in the wild use
# only that form, and reading it as a note-on leaves every note held forever.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { parseMidi } from "./tools/probe/mid2score.mjs";
import fs from "node:fs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* A hand-built one-track SMF: note 60 on at tick 0, off at 480 (one beat at
 * 480 ppq = 0.5 s at the default 120 bpm), and note 64 left HELD. */
const be32 = (n) => [n>>>24 & 255, n>>>16 & 255, n>>>8 & 255, n & 255];
const trk = [
  0x00, 0x90, 60, 100,
  0x83, 0x60, 0x80, 60, 0,
  0x00, 0x90, 64, 100,
  0x00, 0xff, 0x2f, 0x00,
];
const bytes = Buffer.from([
  0x4d,0x54,0x68,0x64, ...be32(6), 0,0, 0,1, 0x01,0xe0,
  0x4d,0x54,0x72,0x6b, ...be32(trk.length), ...trk,
]);

const sc = parseMidi(bytes, 4, { tailSeconds: 3 });

// 1. Tempo and division are honoured: 480 ticks at 480 ppq / 120 bpm = 0.5 s.
const off60 = sc.events.find((e) => e[1] === 0x80 && e[2] === 60);
if (!off60) fail("note 60 has no note-off at all");
else if (Math.abs(off60[0] - 22050) > 64) fail("note 60 off at sample " + off60[0] + ", want ~22050 (0.5 s)");

// 2. frames OUTLASTS the score, or a tail is cut off.
if (sc.frames <= 4 * 44100) fail("frames (" + sc.frames + ") does not outlast the 4 s score");
if (Math.abs(sc.frames - 7 * 44100) > 64) fail("frames " + sc.frames + ", want ~" + 7*44100 + " (4 s + 3 s tail)");

// 3. The HELD note is closed inside the window.
const on64 = sc.events.filter((e) => e[1] === 0x90 && e[2] === 64);
const off64 = sc.events.filter((e) => e[1] === 0x80 && e[2] === 64);
if (on64.length !== 1) fail("expected one note-on for 64, got " + on64.length);
if (off64.length !== 1) fail("a held note must be closed inside the window; got " + off64.length + " offs for 64");
else if (off64[0][0] <= on64[0][0]) fail("the synthesised note-off precedes its note-on");

// 4. A note-on with velocity 0 is a note-OFF.
const trk2 = [0x00,0x90,60,100, 0x81,0x00,0x90,60,0, 0x00,0xff,0x2f,0x00];
const b2 = Buffer.from([0x4d,0x54,0x68,0x64,...be32(6),0,0,0,1,0x01,0xe0,
                        0x4d,0x54,0x72,0x6b,...be32(trk2.length),...trk2]);
const sc2 = parseMidi(b2, 4, { tailSeconds: 1 });
const on60 = sc2.events.filter((e) => e[1] === 0x90 && e[2] === 60);
const of60 = sc2.events.filter((e) => e[1] === 0x80 && e[2] === 60);
/* COUNTING the offs is not enough: misread as a note-on, the held-note pass
 * synthesises an off at the end of the window and the count still comes to
 * one. Assert there is exactly ONE on, and that the off lands at the vel-0
 * event (0x81 0x00 = 128 ticks at 480 ppq = 1/3.75 s) rather than at the far
 * end of the score. */
if (on60.length !== 1) fail("velocity 0 must not be a second note-ON; got " + on60.length + " ons");
if (of60.length !== 1) fail("velocity 0 must produce ONE note-off, got " + of60.length);
else {
  const want = Math.round((128 / 480) * 0.5 * 44100);
  if (Math.abs(of60[0][0] - want) > 64)
    fail("velocity-0 note-off at sample " + of60[0][0] + ", want ~" + want + " (its own event, not the window end)");
}

// 5. Transpose shifts notes and clamps rather than wrapping.
const up = parseMidi(bytes, 4, { transpose: 12 });
if (!up.events.some((e) => e[2] === 72)) fail("transpose +12 did not move note 60 to 72");
const far = parseMidi(bytes, 4, { transpose: 90 });
if (far.events.some((e) => e[2] > 127 || e[2] < 0)) fail("transpose must clamp into 0..127");

// 6. The committed Bach parses and is what the default score expects.
const bach = parseMidi(fs.readFileSync("tools/probe/wtk1-prelude1.mid"), 27);
const notes = bach.events.filter((e) => e[1] === 0x90);
if (notes.length < 50) fail("Bach: only " + notes.length + " notes in 27 s");
const lo = Math.min(...notes.map((e) => e[2])), hi = Math.max(...notes.map((e) => e[2]));
if (lo < 36 || hi > 96) fail("Bach: range " + lo + "-" + hi + " is outside a generally playable register");
for (const e of bach.events) {
  if (e[0] < 0 || e[0] > bach.frames) { fail("Bach: event at sample " + e[0] + " is outside frames"); break; }
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: mid2score");
'
