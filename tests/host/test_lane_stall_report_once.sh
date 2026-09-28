#!/usr/bin/env bash
# A STUCK TAKE IS ANNOUNCED ONCE, NOT EVERY AUTOSAVE PASS.
#
# The autosave runs every ~5 s and a stuck take stays stuck, so a report on
# the CONDITION repeats until the user clears it -- which is how a useful
# sentence becomes noise the user learns to ignore. It latches on the
# transition INTO the state and resets when it leaves, so it is said when it
# becomes true and a second episode is still heard.
#
# Pinned as source shape because the behaviour is a JS latch inside the
# autosave pass, which the host units do not drive. Comments and strings are
# stripped first: this file explains the latch at length, and a pin that
# reads prose measures the prose (a mistake already made once in this suite).
set -euo pipefail
cd "$(dirname "$0")/../.."

node - <<'JS'
const fs = require('fs');
const src = fs.readFileSync('src/shadow/shadow_ui.js', 'utf8');

// Blank comments and string/template literals, keeping line structure.
let code = src
  .replace(/\/\*[\s\S]*?\*\//g, m => m.replace(/\S/g, ' '))
  .replace(/\/\/[^\n]*/g, m => ' '.repeat(m.length))
  .replace(/"(?:[^"\\\n]|\\.)*"/g, m => ' '.repeat(m.length))
  .replace(/'(?:[^'\\\n]|\\.)*'/g, m => ' '.repeat(m.length))
  .replace(/`(?:[^`\\]|\\.)*`/g, m => ' '.repeat(m.length));

function fail(msg) { console.log('FAIL: ' + msg); process.exit(1); }

if (!/let\s+laneStallAnnounced\s*=\s*\[\s*false\s*,\s*false\s*,\s*false\s*,\s*false\s*\]/.test(code))
  fail('laneStallAnnounced is not declared per slot -- a latch shared across slots reports the first stuck take and silences the other three');

// The announcement must be guarded by the latch, not by the condition alone.
const guard = /if\s*\(\s*stalledLanes\s*>\s*0\s*&&\s*!\s*laneStallAnnounced\s*\[\s*i\s*\]\s*\)/;
if (!guard.test(code))
  fail('the stall report is not latched: it fires on the condition, so it repeats every autosave pass (~5 s) for as long as the take is stuck');

// ...and it must clear, or a second episode is silent.
if (!/laneStallAnnounced\s*\[\s*i\s*\]\s*=\s*false/.test(code))
  fail('the latch is never cleared -- a take that resolves and a LATER one that sticks would be reported only once per session');

// A set change must reset it, or the incoming set's first stuck take is eaten.
const inval = code.slice(code.indexOf('function invalidateAutosaveWriteCache'));
const body = inval.slice(0, inval.indexOf('}') + 1);
if (!/laneStallAnnounced\s*=\s*\[/.test(body))
  fail('invalidateAutosaveWriteCache does not reset laneStallAnnounced -- the latch survives a set change and swallows the new set\'s first stuck take');

// And the report has to come from the branch that DROPS the take: the empty
// document is the only place the autosave learns the slot holds nothing
// storable. Reporting anywhere else describes a state nobody is acting on.
const persist = code.slice(code.indexOf('function persistSlotLanes'));
const emptyBranch = persist.slice(0, persist.indexOf('lastWrittenLaneJson[i] = '));
if (!/lanes:pending|getSlotParam\s*\(\s*i\s*,/.test(persist.slice(0, 4000)))
  fail('persistSlotLanes never reads the pending report -- the take is dropped as silently as before');
if (emptyBranch.indexOf('stalledLanes') < 0)
  fail('the stall check is not in the empty-document branch, which is the one that deletes the file and drops the take');

console.log('PASS: a stuck take is reported once per episode, from the branch that drops it');
JS
