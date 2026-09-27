#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# EACH KEY AGES AGAINST ITS OWN DURATION, AND A STREAM IS NOT A TRANSITION.
#
# settled() asks "was anything stamped in the last 120 ms" of every key, with
# one number for all of them. Two consequences a caller cannot see past:
#   - a 100 ms wave morph is held "moving" for 120 ms (SP-38 recorded the gap);
#   - a key re-stamped every tick (an LFO on an enum) never settles, so the page
#     redraws at the tick rate forever.
# activity() answers per key, against the duration observe() was given for it,
# and names the second case STREAMING rather than moving, so the host can
# throttle it instead of either redrawing every tick or freezing the last
# tween.
#
# settled() is unchanged -- that is the assertion that matters here, and it is
# made first.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the anim activity test" >&2
  exit 1
fi

node --input-type=module -e '
import { createAnimState, observe, settled, activity } from "./src/shared/param_pages/anim_state.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

/* DEFAULT: settled() and observe() answer exactly as before. */
{
  const st = createAnimState();
  observe(st, "k", "a", 0);
  const r = observe(st, "k", "b", 1000);
  if (r.from !== "a" || r.t !== 0 || !r.moving) bad("observe changed its answer for a plain change");
  if (settled(st, 1119)) bad("settled() stopped holding a 120 ms transition");
  if (!settled(st, 1120)) bad("settled() stopped releasing at 120 ms");
  /* settled() still uses ITS argument, not the recorded duration. */
  const w = createAnimState();
  observe(w, "wave", 0, 0, 100);
  observe(w, "wave", 1, 1000, 100);
  if (settled(w, 1110)) bad("settled() started using the recorded duration -- that is a behaviour change");
}

/* PER KEY: a 100 ms morph is done at 100 ms, whatever the 120 ms default says. */
{
  const st = createAnimState();
  observe(st, "wave", 0, 0, 100);
  observe(st, "wave", 1, 1000, 100);
  observe(st, "enum", "a", 0, 120);
  let a = activity(st, 1050);
  if (!a.moving || a.streaming) bad("a fresh 100 ms morph did not read as moving at 50 ms");
  a = activity(st, 1100);
  if (a.moving || a.streaming) bad("a 100 ms morph was still active at 100 ms");
  observe(st, "enum", "b", 1100, 120);
  if (!activity(st, 1219).moving) bad("the 120 ms key aged out early");
  a = activity(st, 1220);
  if (a.moving || a.streaming) bad("the 120 ms key did not age out at its own duration");
}

/* A STREAM: re-stamped every 20 ms for longer than its own duration. */
{
  const st = createAnimState();
  observe(st, "lfo", 0, 0, 120);
  let t = 1000;
  for (let i = 0; i < 5; i++) { observe(st, "lfo", i + 1, t, 120); t += 20; }
  let a = activity(st, t);
  if (!a.moving || a.streaming) bad("a run shorter than its duration was not a plain transition");
  for (let i = 0; i < 10; i++) { observe(st, "lfo", i + 10, t, 120); t += 20; }
  a = activity(st, t);
  if (a.moving || !a.streaming) bad("a run past its duration was not reported as streaming");
  /* ...and when it stops, it settles on its own schedule. */
  a = activity(st, t + 120);
  if (a.moving || a.streaming) bad("a stopped stream did not settle");
  /* A later single change is a transition again, not a stream. */
  observe(st, "lfo", 99, t + 500, 120);
  a = activity(st, t + 510);
  if (!a.moving || a.streaming) bad("a change after a stream was not a plain transition");
}

/* ARRIVAL is not a change. */
{
  const st = createAnimState();
  observe(st, "k", "a", 5000);
  const a = activity(st, 5000);
  if (a.moving || a.streaming) bad("a first sighting read as activity");
  if (activity(null, 0).moving) bad("no store read as moving");
}

if (fail) process.exit(1);
console.log("PASS: activity() ages each key by its own duration and names a stream; settled() unchanged");
'
