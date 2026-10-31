#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# LIVE VALUES SHARE ONE READ PER TICK, HOWEVER MANY THERE ARE.
#
# A live (or modulated) key has its `:effective` value re-read on the fast path
# so its picture can animate. Every one of those reads is an IPC round-trip
# (~2.8 ms -- more than redrawing the whole page) on a mailbox the rest of the
# shadow UI shares, inside a 16.7 ms frame. So the fast path is a ROUND-ROBIN
# capped at MOD_FAST_READS_PER_TICK, and a page with eight live knobs costs the
# same per tick as a page with one: each key just refreshes less often.
#
# Nothing pinned that cap. Raising it left every test green, while the frame
# budget of every page with live values quietly multiplied -- and live extra
# keys (a widget meter read every tick) are built on the assumption that it
# holds. This asserts the per-tick cost directly, from the reads a settled page
# makes, and that the round-robin still reaches every live key.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
const { createController, LAYOUT_MOVY } = await import(R + "/src/shared/param_pages/page_controller.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

/* Eight knobs, every one of them live: the worst case for the fast path. */
const keys = ["a", "b", "c", "d", "e", "f", "g", "h"];
const HIER = { levels: { root: { label: "M", knobs: keys, params: keys.map((k) => ({ key: k })) } } };
const CP = keys.map((k) => ({ key: k, name: k.toUpperCase(), type: "float",
                              min: 0, max: 1, step: 0.01, live: true }));
const store = { ui_hierarchy: JSON.stringify(HIER), chain_params: JSON.stringify(CP) };
for (const k of keys) { store[k] = "0.1"; store[k + ":base"] = "0.1"; store[k + ":effective"] = "0.9"; }

let tickReads = [];
let t = 0;
const ctrl = createController({
  getParam: (k) => { const b = String(k).replace(/^synth:/, ""); tickReads.push(b);
                     return store[b] === undefined ? null : store[b]; },
  setParam: () => true, announce: () => {}, now: () => (t += 16),
});
ctrl.load({ prefix: "synth" });
ctrl.setLayout(LAYOUT_MOVY);
for (let i = 0; i < 80; i++) ctrl.tick();          /* past the entry warm */

const TICKS = 24;
let maxFast = 0, maxTotal = 0;
const reached = new Set();
for (let i = 0; i < TICKS; i++) {
  tickReads = [];
  ctrl.tick();
  const fast = tickReads.filter((k) => k.endsWith(":effective"));
  for (const k of fast) reached.add(k);
  maxFast = Math.max(maxFast, fast.length);
  maxTotal = Math.max(maxTotal, tickReads.length);
}

ok(maxFast <= 1,
   "eight live keys made " + maxFast + " :effective reads in one tick -- the fast path must stay at one");
ok(maxTotal <= 2,
   "a settled page with eight live keys made " + maxTotal + " reads in one tick -- one live read plus one rotation stop is the budget");
ok(reached.size === keys.length,
   "the round-robin reached " + reached.size + " of " + keys.length + " live keys in " + TICKS + " ticks");

if (fails) process.exit(1);
console.log("PASS: live values share one fast read per tick and every live key is still reached");
'
