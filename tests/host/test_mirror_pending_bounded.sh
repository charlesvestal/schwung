#!/usr/bin/env bash
# /mirror delays every visual update by the sound's buffer (later()), and only
# requestAnimationFrame drains that queue -- which a browser pauses in a
# background tab while the EventSource keeps delivering. The queue then grew
# without bound (~100 MB/hour) and ran all at once on return.
#
# This lifts later()/runLater()/flushLater() out of the page and runs them
# under node with rAF never firing: hidden, updates must apply at once; and
# even visible, the queue must not exceed its cap -- while every update still
# runs, once, in arrival order.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v node >/dev/null || { echo "SKIP: node not installed"; exit 0; }
PAGE=schwung-manager/static/mirror.html
T=$(mktemp -d "${TMPDIR:-/tmp}/mirror_pending.XXXXXX"); trap 'rm -rf "$T"' EXIT
# `const` inside eval is scoped to the eval, so the lifted consts become vars.
awk '/^const pending = \[\];/{on=1} on{print} /^function flushLater\(\)/{exit}' "$PAGE" \
  | sed 's/^const /var /' > "$T/q.js"
grep -q 'function later' "$T/q.js" || { echo "FAIL: could not lift later() out of $PAGE"; exit 1; }
cat > "$T/run.js" <<'JS'
const fs = require('fs');
let audioOn = true, audioPrimed = true, nextTime = 5;
const actx = { currentTime: 0, baseLatency: 0, outputLatency: 0 };
const document = { hidden: false };
const performance = { now: () => 0 };
eval(fs.readFileSync(process.argv[2], 'utf8'));
let fails = 0;
const fail = m => { console.log('FAIL: ' + m); fails++; };
let ran = [];

/* Visible, rAF paused (a tab mid-switch, a throttled window): bounded. */
for (let i = 0; i < 10000; i++) later(() => ran.push(i));
if (pending.length > 1024) fail('visible queue grew to ' + pending.length + ' with no rAF');
flushLater();
if (ran.length !== 10000) fail('updates were lost: ran ' + ran.length + ' of 10000');
if (ran.some((v, i) => v !== i)) fail('updates ran out of order');

/* Hidden: applied at once, nothing queued. */
ran = []; document.hidden = true;
later(() => ran.push('a'));
if (pending.length !== 0 || ran.length !== 1) fail('a hidden tab still queues updates');

/* Going hidden with a queue: the queue is applied first, order kept. */
document.hidden = false; ran = [];
later(() => ran.push(1)); later(() => ran.push(2));
document.hidden = true; later(() => ran.push(3));
if (ran.join() !== '1,2,3') fail('queued updates did not run before a hidden one: ' + ran.join());

if (!fails) console.log('test_mirror_pending_bounded: all passed');
process.exit(fails ? 1 : 0);
JS
node "$T/run.js" "$T/q.js"
# and the page must flush when it goes hidden, since rAF stops then
grep -q "if (document.hidden) flushLater();" "$PAGE" || { echo "FAIL: visibilitychange no longer flushes the queue"; exit 1; }
