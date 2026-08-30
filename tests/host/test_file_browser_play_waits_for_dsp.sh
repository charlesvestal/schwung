#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Playing a file from the File Browser must not lose the first Play (#344).
#
# The wav-player's DSP load runs on the shim worker, so its instance is still
# NULL for a few frames after "load" is requested. An overtake_dsp set_param
# with no instance is NOT queued -- it is refused outright (error 13, the final
# else in schwung_shim.c's overtake_dsp SET branch). The browser wrote
# file_path immediately after the load, so the first Play switched to the
# playback screen and played nothing; the second worked only because the DSP
# had come up in between. Reported against 0.12.1 and 1.0.0.
#
# This RUNS the module rather than pinning its text. ui.js is a QuickJS ES
# module importing 'os' and 'std', which node cannot resolve, so the imports
# are stripped and every remaining free identifier is auto-stubbed through a
# Proxy scope. That is sound here because the unit under test is the playback
# state machine, which touches none of the imported helpers -- and it means
# this test fails on BEHAVIOUR, not on wording.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
const fs = require("fs"), vm = require("vm");

const raw = fs.readFileSync("src/modules/tools/file-browser/ui.js", "utf8");
/* import statements contain no ";" of their own, so a non-greedy cut to the
 * first semicolon removes each whole (possibly multi-line) statement. */
const src = raw.replace(/\bimport\b[\s\S]*?;/g, "");

let failures = 0;
const check = (cond, what) => {
    if (cond) { console.log("  ok  " + what); }
    else { console.log("FAIL: " + what); failures++; }
};

/* ---- one run of the module, with a scripted DSP load ---------------------- */
function run(opts) {
    const calls = [];
    const real = Object.create(null);
    /* `has: () => true` below claims EVERY bare identifier, built-ins
     * included, so the language intrinsics have to be seeded or the module
     * gets a no-op stub for Array.isArray and friends. */
    for (const k of ["Array", "Object", "String", "Number", "Boolean", "Math",
                     "JSON", "Date", "RegExp", "Error", "TypeError", "parseInt",
                     "parseFloat", "isNaN", "isFinite", "Symbol", "Map", "Set",
                     "Promise", "console", "undefined", "NaN", "Infinity"]) {
        real[k] = globalThis[k];
    }
    let ready = opts.readyAfter === 0 ? "1" : "0";
    let ticks = 0;

    real.host_module_set_param = (k, v) => { calls.push(["set", k, String(v)]); };
    real.host_module_set_param_blocking = (k, v) => { calls.push(["setb", k, String(v)]); };
    real.host_module_get_param = (k) => {
        if (k === "__ready") { calls.push(["get", "__ready"]); return opts.noReadyKey ? "" : ready; }
        return "";
    };

    let sandbox;
    sandbox = new Proxy(real, {
        has: () => true,
        get(t, k) {
            if (k === Symbol.unscopables) return undefined;
            /* The module assigns its entry points onto globalThis; without
             * this the Proxy auto-stubs the name and `globalThis.tick = ...`
             * lands on a throwaway. */
            if (k === "globalThis") return sandbox;
            if (k in t) return t[k];
            const f = function () {};
            t[k] = f;
            return f;
        },
        set(t, k, v) { t[k] = v; return true; },
    });
    const ctx = vm.createContext(sandbox);
    vm.runInContext(src, ctx, { filename: "ui.js" });

    real.startPlayback("/data/UserData/Samples/kick.wav");
    for (let i = 0; i < (opts.ticks || 0); i++) {
        ticks++;
        if (opts.readyAfter !== undefined && ticks >= opts.readyAfter) ready = "1";
        if (opts.stopAtTick === ticks) real.stopPlayback();
        real.tick();
    }
    return { calls, real };
}

const filePaths = (calls) => calls.filter(c => c[0] === "set" && c[1] === "file_path");

/* 1. THE BUG: DSP not ready on the first Play -- file_path must not be sent
 *    into the void, and must still arrive once the load completes. */
{
    const { calls } = run({ readyAfter: 3, ticks: 10 });
    const sent = filePaths(calls);
    const loadIdx = calls.findIndex(c => (c[0] === "setb" || c[0] === "set") && c[1] === "load");
    check(loadIdx >= 0, "the wav-player DSP load is requested");

    /* The pre-fix module sent file_path inside startPlayback, before any tick
     * and while __ready was still \"0\". */
    const readyGetsBeforeFirstSend = calls
        .slice(0, calls.indexOf(sent[0] ? sent[0] : null) === -1 ? calls.length : calls.indexOf(sent[0]))
        .filter(c => c[0] === "get" && c[1] === "__ready").length;

    check(sent.length === 1, "file_path is sent exactly once (was: once, but discarded)");
    check(readyGetsBeforeFirstSend > 0,
          "file_path is only sent after __ready has been consulted -- the pre-fix "
          + "module sent it immediately, and the shim refused it (error 13)");
}

/* 2. It is not sent WHILE the host still says loading. */
{
    const { calls } = run({ readyAfter: 5, ticks: 4 });   /* never becomes ready */
    check(filePaths(calls).length === 0,
          "nothing is sent while __ready still answers \"0\"");
}

/* 3. Already loaded: the common case must still start on the spot, not a
 *    frame later -- a deferred-only fix would make every later Play laggy. */
{
    const { calls } = run({ readyAfter: 0, ticks: 0 });
    check(filePaths(calls).length === 1,
          "with the DSP ready, file_path goes out on the Play itself (no tick needed)");
}

/* 4. A host that does not serve __ready (older shim, or a read that did not
 *    complete) must not hang: "" is not "0", so it sends. */
{
    const { calls } = run({ noReadyKey: true, readyAfter: 99, ticks: 3 });
    check(filePaths(calls).length === 1,
          "an unserved __ready sends immediately rather than waiting forever");
}

/* 5. The wait is BOUNDED. A host stuck at "0" forever still plays. */
{
    const { calls } = run({ readyAfter: 100000, ticks: 60 });
    check(filePaths(calls).length === 1,
          "a host stuck at \"0\" still sends once the tick budget expires");
}

/* 6. Stop cancels a start that is still waiting, or Play-then-Stop begins
 *    playing after the user stopped. */
{
    const { calls } = run({ readyAfter: 6, ticks: 12, stopAtTick: 2 });
    check(filePaths(calls).length === 0,
          "Stop while the load is pending cancels it -- no playback after the stop");
}

if (failures) { console.log("FAILURES: " + failures); process.exit(1); }
console.log("PASS: File Browser Play waits for the wav-player DSP");
'
