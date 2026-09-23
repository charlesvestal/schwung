#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Fullscreen canvas live values (fullscreen_live_ms + extra_keys -> onValues).
#
# tickCanvasLiveValues is lifted out of shadow_ui.js and RUN against a stub
# runtime, because what matters is behaviour a grep cannot see:
#   - ONE read per tick. A read is ~2.8 ms; four in one tick is an ~11 ms stall
#     landing on the frame every interval.
#   - onValues fires once per cycle, with every key, after the last read.
#   - the interval is honoured between cycles.
#   - nothing is read for an overlay that cannot take the answer (no onValues,
#     disabled after a throw, no overlay).
# The open-time clamps (four keys, 50 ms floor) and the draw-path rule are
# pinned at source level.

node --input-type=module -e '
import { readFileSync } from "node:fs";
let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");

const m = src.match(/function tickCanvasLiveValues\(\) \{[\s\S]*?\n\}\n/);
ok(!!m, "tickCanvasLiveValues exists");
if (!m) process.exit(1);

function harness(opts = {}) {
    const reads = [], delivered = [];
    let now = 1000;
    const rt = {
        liveKeys: opts.keys || ["a", "b", "c", "d"],
        liveIntervalMs: opts.interval ?? 50,
        lastLiveReadMs: 0, liveCursor: 0, liveValues: {},
        hookDisabled: !!opts.disabled,
        overlay: opts.noOverlay ? null : (opts.noHook ? {} : { onValues() {} }),
        ctx: { getParam(k) { reads.push({ k, now }); return "v:" + k; } },
    };
    const invokeCanvasOverlayHook = (name, payload) => { delivered.push({ name, payload, now }); return true; };
    const DateStub = { now: () => now };
    const tick = new Function("canvasRuntime", "invokeCanvasOverlayHook", "Date",
        m[0] + "\nreturn tickCanvasLiveValues;")(rt, invokeCanvasOverlayHook, DateStub);
    return { rt, reads, delivered, tick, advance(ms) { now += ms; } };
}

{
    const h = harness();
    const perTick = [];
    for (let i = 0; i < 4; i++) { const b = h.reads.length; h.tick(); perTick.push(h.reads.length - b); h.advance(16); }
    ok(perTick.every(n => n === 1), "one read per tick, never the set at once (" + perTick + ")");
    ok(h.delivered.length === 1 && h.delivered[0].name === "onValues", "onValues fires once per cycle");
    const v = h.delivered[0] && h.delivered[0].payload.values;
    ok(v && v.a === "v:a" && v.b === "v:b" && v.c === "v:c" && v.d === "v:d", "onValues carries every key");
    // Next cycle must wait for the interval measured from the cycle START.
    h.tick();
    ok(h.reads.length === 5, "a new cycle starts once the interval has passed since the last one began");
}
{
    const h = harness({ keys: ["only"], interval: 100 });
    h.tick(); h.advance(16); h.tick(); h.advance(16); h.tick();
    ok(h.reads.length === 1, "the interval is honoured between cycles");
    h.advance(100); h.tick();
    ok(h.reads.length === 2 && h.delivered.length === 2, "and resumes after it");
}
for (const [opts, what] of [[{ noHook: true }, "no onValues hook"], [{ disabled: true }, "overlay disabled after a throw"], [{ noOverlay: true }, "no overlay loaded"], [{ interval: 0 }, "fullscreen_live_ms absent"]]) {
    const h = harness(opts);
    for (let i = 0; i < 8; i++) { h.tick(); h.advance(60); }
    ok(h.reads.length === 0, "no reads when " + what);
}

ok(/extra_keys\) \? meta\.extra_keys\.slice\(0, 4\)/.test(src), "live keys are capped at four at open");
ok(/Math\.max\(50, Number\(meta\.fullscreen_live_ms\)\)/.test(src), "live interval floor is 50 ms");
ok(/const DRAW_PATH_HOOKS = new Set\(\["draw", "tick"\]\)/.test(src), "draw and tick still lose param accessors");
process.exit(fail ? 1 : 0);
'
