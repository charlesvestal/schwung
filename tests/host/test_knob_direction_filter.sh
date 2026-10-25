#!/usr/bin/env bash
# An OPT-IN filter for an encoder that misreports its direction.
#
# Found on a real Move, 2026-10-22: knob 1 reversed on 27% of its counts while
# turned one way (knob 8 on the same param: clean). It read as a software bug
# -- "enums that gate other cells skip and snap back" -- because every gating
# enum in the reporting modules sat on knob 1. The fixture is that knob,
# captured raw in the shim (spi_midi_log_on), one touch per segment, turned ONE
# way per segment: slow cw 131 right / 37 wrong, slow ccw 125 / 26, fast cw
# 179 / 6. The wrong counts arrive in bursts of up to ~14, so a debounce of one
# or two counts does nothing; a majority over the last 12 counts that flips
# only at a margin of 6, committing a new touch only after 8 agree, does.
#
# Measured through the REAL knob engine (4-detent enum gate), not by counting
# raw deltas: the complaint is the enum stepping backwards, so that is the
# observable. The cost is lag on a deliberate reversal, bounded below.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null 2>&1 || { echo "FAIL: node required"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { createKnobDirectionFilter, parseKnobFilterConfig } from "./src/shared/param_pages/knob_direction_filter.mjs";
import { knobInit, knobStep } from "./src/shared/knob_engine.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "  ok  " : "FAIL ") + " -- " + m); if (!c) fail++; };
const cap = JSON.parse(readFileSync("tests/fixtures/knob_jitter_capture.json", "utf8"));
const META = { kind: "enum", type: "enum", options: ["A", "B", "C", "D", "E"], min: 0, max: 4 };

/* Feed a delta stream through (optionally) the filter and the real knob engine;
 * count option moves against the intended direction. */
function run(seq, intended, filter, start = 2) {
  const st = knobInit(start); let t = 0, back = 0, moves = 0, prev = start;
  if (filter) filter.reset(0);
  for (const d of seq) {
    t += 10;
    if (filter && !filter.accept(0, d)) continue;
    const v = knobStep(st, META, d > 0 ? 1 : -1, t);
    if (v !== prev) { moves++; if ((v - prev) * intended < 0) back++; prev = v; }
  }
  return { back, moves, end: prev };
}

const segs = [["slow_cw", 1], ["slow_ccw", -1], ["fast_cw", 1]];
let rawBack = 0;
for (const [k, dir] of segs) rawBack += run(cap[k], dir, null).back;
ok(rawBack > 0, "control: unfiltered, the captured knob steps the enum BACKWARDS (" + rawBack + " moves)");

for (const [k, dir] of segs) {
  const r = run(cap[k], dir, createKnobDirectionFilter());
  ok(r.back === 0, k + ": filtered, no backward enum step (" + r.back + " backward, " + r.moves + " moves)");
  ok(r.moves > 0, k + ": filtered, the turn still moves the enum (" + r.moves + " moves)");
}

/* A HEALTHY knob still reverses: knob 8 swept +4 -12 +12 -12 on the device. */
{
  const clean = [].concat(Array(4).fill(1), Array(12).fill(-1), Array(12).fill(1), Array(12).fill(-1));
  const f = createKnobDirectionFilter();
  let passed = 0, lost = 0, prevSign = 0, sinceRev = 0, worstLag = 0;
  for (const d of clean) {
    const s = Math.sign(d);
    if (s !== prevSign) { sinceRev = 0; prevSign = s; }
    if (f.accept(0, d)) { passed++; worstLag = Math.max(worstLag, sinceRev); sinceRev = -1e9; }
    else { lost++; sinceRev++; }
  }
  ok(lost > 0 && lost <= 30, "a deliberate reversal is delayed, boundedly (" + lost + " of 40 counts held back)");
  ok(worstLag <= 9, "no reversal waits more than 9 counts (" + worstLag + ")");
  /* THE COST, pinned so nobody turns this on by default without seeing it:
   * unfiltered this sweep walks C D C B A B C D C B A; filtered it is C D C.
   * Every reversal pays ~9 counts, and a 12-detent sweep has only ~3 left for
   * a 4-detent gate. It still turns BOTH ways, which is the floor. */
  const r = run(clean, 0, createKnobDirectionFilter());
  ok(r.moves >= 2, "a healthy back-and-forth sweep still moves the enum both ways (" + r.moves + " moves)");
  const raw = run(clean, 0, null);
  ok(raw.moves > r.moves, "...and loses motion doing it, which is why this is opt-in (" + raw.moves + " -> " + r.moves + " moves)");
}

/* Per knob, and a reset starts a new gesture. */
{
  const f = createKnobDirectionFilter();
  for (let i = 0; i < 10; i++) f.accept(0, 1);
  ok(f.accept(0, 1), "a settled knob passes counts in its direction");
  ok(!f.accept(0, -1), "a lone reversed count on a settled knob is held back");
  let firstPassAt = -1;
  for (let i = 0; i < 12 && firstPassAt < 0; i++) if (f.accept(1, -1)) firstPassAt = i + 1;
  ok(firstPassAt === 8, "knobs are independent, and a new gesture commits after 8 agreeing counts (" + firstPassAt + ")");
  f.reset(0);
  ok(!f.accept(0, -1), "after a reset (new touch) a single count does not yet commit");
}

/* Config: off unless the file exists; content optionally names knobs 1-8. */
{
  const off = parseKnobFilterConfig(null);
  ok(off.enabled === false, "no file: filter off");
  const all = parseKnobFilterConfig("");
  ok(all.enabled && all.knobs.size === 8, "empty file: every knob (" + all.knobs.size + ")");
  const one = parseKnobFilterConfig("1\n");
  ok(one.enabled && one.knobs.has(0) && one.knobs.size === 1, "\"1\": knob 1 only (slot 0)");
  const some = parseKnobFilterConfig("1, 4 x 9");
  ok(some.knobs.has(0) && some.knobs.has(3) && some.knobs.size === 2, "\"1, 4 x 9\": knobs 1 and 4, junk ignored");
}

if (fail) { console.log(fail + " failure(s)"); process.exit(1); }
console.log("PASS: knob direction filter");
'

# ---- wiring: the grid's input path consults it, and a held-back count is CONSUMED.
# Returning false instead would fall through to shadow_ui.js's legacy knob
# handler -- a second writer for the very count the filter withheld.
pp="src/shadow/shadow_ui_param_pages.mjs"
body=$(sed -n '/^export function handleParamPagesMidi(data) {/,/^}/p' "$pp")
echo "$body" | grep -q 'knobFilter.accept(intent.slot, intent.direction)) return true;' \
    || { echo "FAIL: handleParamPagesMidi does not consume a count the filter held back"; exit 1; }
echo "$body" | grep -q "intent.type === 'touch' && intent.down) knobFilter.reset(intent.slot)" \
    || { echo "FAIL: a new touch does not reset the filter's gesture"; exit 1; }
grep -q '^    loadKnobFilterConfig();$' "$pp" || { echo "FAIL: the flag file is never read on grid entry"; exit 1; }
echo "  ok  -- the grid input path filters, consumes, and resets per touch"
