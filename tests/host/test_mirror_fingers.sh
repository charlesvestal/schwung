#!/usr/bin/env bash
# /mirror draws a fading finger on every control as it is used, so a recording
# shows what the hands did. What the page knows about the hands comes from two
# places in the surface snapshot, and the overlay is only right if it reads
# both: the HELD state (a control is down now) and the EVENT ring (one was
# pressed) -- a tap that begins and ends between two snapshots exists only in
# the ring. Turns are in neither; they are read off the encoder positions.
#
# This lifts the tracker (layout constants, the control map, trackInput) out of
# the page and runs it under node against snapshots built here.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null || { echo "SKIP: node not installed"; exit 0; }
PAGE=schwung-manager/static/mirror.html
T=$(mktemp -d "${TMPDIR:-/tmp}/mirror_fingers.XXXXXX"); trap 'rm -rf "$T"' EXIT
# `const`/`let` inside eval are scoped to the eval, so the lifted ones become vars.
{
  awk '/^const OFF = \{/{on=1} on{print} /^const N_EVENTS/{exit}' "$PAGE"
  awk '/^const W = 920/{on=1} on{print} /^const rowCy/{exit}' "$PAGE"
  awk '/^const FUNCS = \[/{on=1} on{print} /^const NAV/{exit}' "$PAGE"
  awk '/^const FINGER_FADE_MS/{on=1} on{print} /^const fadeOf/{exit}' "$PAGE"
} | sed -E 's/^(const|let) /var /' > "$T/f.js"
for want in 'function trackInput' 'var SPOT = ' 'var fadeOf' 'var OFF = '; do
  grep -q "$want" "$T/f.js" || { echo "FAIL: could not lift '$want' out of $PAGE"; exit 1; }
done
cat > "$T/run.js" <<'JS'
const fs = require('fs');
const localStorage = { getItem: () => null, setItem: () => {} };
let surf = null, surfView = null;
const u8 = (o, i) => surf ? surf[o + i] : 0;
const noteDown = n => u8(1560, n), ccVal = c => u8(1816, c);
const encPos = i => surfView ? surfView.getInt16(1944 + i * 2, true) : 0;
eval(fs.readFileSync(process.argv[2], 'utf8'));
let fails = 0;
const check = (ok, m) => { if (ok) console.log('ok   ' + m); else { console.log('FAIL ' + m); fails++; } };

/* A device: the struct the page decodes, and the writes the shim makes. */
const buf = new Uint8Array(OFF.size), dv = new DataView(buf.buffer);
let nEv = 0;
const ev = (st, d1, d2) => { const o = OFF.events + (nEv % N_EVENTS) * 8; dv.setUint32(o, 0, true);
  buf[o + 4] = st; buf[o + 5] = d1; buf[o + 6] = d2; nEv++; dv.setUint32(OFF.evCount, nEv, true); };
const note = (n, v) => { buf[OFF.noteDown + n] = v; ev(v ? 0x90 : 0x80, n, v); };
const cc = (c, v) => { buf[OFF.ccVal + c] = v; ev(0xB0, c, v); };
const turn = (i, d) => dv.setInt16(OFF.enc + i * 2, dv.getInt16(OFF.enc + i * 2, true) + d, true);
const snap = now => { surf = buf.slice(); surfView = new DataView(surf.buffer); trackInput(now); };

/* History is not a press: whatever happened before the page connected. */
note(70, 100); note(70, 0); turn(3, 5);
snap(1000);
check(seenAt.n70 === undefined && !turns[3], 'a first snapshot seeds the tracker: old presses and turns draw nothing');

/* A hold, and the fade starting when it ENDS (snapshots only come on change). */
note(92, 100); snap(2000);
check(isHeld.n92 === true, 'a held pad is held');
note(92, 0); snap(9000);
check(isHeld.n92 === false && seenAt.n92 === 9000, 'released after a long hold: the fade starts at the release, not the press');
check(fadeOf(seenAt.n92, 9250, FINGER_FADE_MS) === 0.5 && fadeOf(seenAt.n92, 9500, FINGER_FADE_MS) === 0,
      'the finger is half gone halfway through the fade and gone at its end');

/* A tap inside one snapshot: only the event ring has it. */
note(68, 100); note(68, 0); snap(10000);
check(isHeld.n68 === false && seenAt.n68 === 10000, 'a tap that began and ended between two snapshots still shows');

/* A chord, and a modifier with a pad. */
note(68, 100); note(77, 100); cc(49, 127); snap(11000);
check(isHeld.n68 && isHeld.n77 && isHeld.c49, 'two pads and Shift held together are three fingers');
note(68, 0); note(77, 0); cc(49, 0); snap(11100);

/* Turns: direction from the encoder position, across the int16 wrap. */
turn(2, 1); snap(12000);
check(turns[2] && turns[2].dir === 1 && turns[2].at === 12000, 'a knob turned clockwise shows a clockwise arrow');
check(seenAt.n2 === 12000, '...and a finger on it, touch reported or not');
turn(2, -3); snap(12100);
check(turns[2].dir === -1, 'turned back: the arrow reverses');
dv.setInt16(OFF.enc + 9 * 2, 32767, true); snap(12200);
turn(9, 1); snap(12300);          /* 32767 + 1 wraps to -32768 */
check(turns[9].dir === 1, 'the wheel keeps its direction across the position counter wrapping');
snap(12400);
check(turns[9].at === 12300, 'no movement, no new arrow');

/* The wheel: a touch is not a click. */
note(9, 127); snap(13000);
check(isHeld.n9 && clickAt < 13000, 'touching the wheel is a finger and no click');
cc(3, 127); snap(13100);
check(isHeld.c3 && clickAt === 13100, 'pressing it is a click, stamped once');
snap(13200);
check(clickAt === 13100, '...and held down, it does not click again');
cc(3, 0); cc(3, 127); cc(3, 0); snap(13300);
check(clickAt === 13300 && !isHeld.c3, 'a click shorter than a snapshot still rings');

/* Things with no place on the drawing. */
ev(0xB0, 14, 1); ev(0x90, 120, 100); snap(14000);
check(seenAt.c14 === undefined && seenAt.n120 === undefined, 'an event for a control the page does not draw is ignored');

/* Every control the page draws has a place for a finger. */
const want = [];
for (let n = 0; n <= 9; n++) want.push('n' + n);
for (let n = 16; n <= 31; n++) want.push('n' + n);
for (let n = 68; n <= 99; n++) want.push('n' + n);
for (const c of [3, 40, 41, 42, 43, 49, 50, 51, 52, 54, 55, 56, 58, 60, 62, 63, 85, 86, 88, 118, 119]) want.push('c' + c);
const missing = want.filter(k => !SPOT[k] || !isFinite(SPOT[k][0]) || !isFinite(SPOT[k][1]));
check(missing.length === 0, 'pads, steps, knobs, wheel and every button have a spot' + (missing.length ? ' (missing ' + missing.join(' ') + ')' : ''));
check(SPOT.c3 === SPOT.n9, 'the wheel click and the wheel touch share one spot');

process.exit(fails ? 1 : 0);
JS
node "$T/run.js" "$T/f.js"
