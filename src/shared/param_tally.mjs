/**
 * param_tally.mjs — who is reading parameters, and how often.
 *
 * Tracing established that param IPC is ~98% of shadow_ui tick time, that a
 * round-trip costs ~2.9ms of which the shim spends ~10us, and that the
 * param-pages controller behaves exactly as designed at ONE read per tick.
 * It also showed ~7.7 reads per tick actually happening — so ~6.7 come from
 * somewhere else in shadow_ui.js. Spans cannot say where: they carry a name,
 * not a key or a caller.
 *
 * This does, from the JS side, where both are already in hand. It wraps the
 * `shadow_get_param` / `shadow_set_param` globals — every call site, including
 * the ~37 that bypass the getSlotParam helper — and reports per second: calls
 * per tick, the keys by frequency, and a sampled caller for each.
 *
 * Why not put the key in the span name: the trace name table is 256 entries
 * and silently buckets everything past that into one id, and interning does a
 * linear strcmp scan per call. Both would corrupt the measurement being taken.
 *
 * Opt-in via /data/UserData/schwung/param_tally_on, reports to the unified log
 * and to param_tally.txt. Costs one host_file_exists at init when absent. When
 * armed it adds a map increment (~250ns) to a 2.9ms round-trip — 0.01%, which
 * is small enough not to move the thing it measures.
 */

export const TALLY_OUT = "/data/UserData/schwung/param_tally.txt";
export const TALLY_FLAG = "/data/UserData/schwung/param_tally_on";

/* 1-in-N calls pays for a stack capture. A key says WHAT is read; only the
 * caller says who to go and delete. Sampling keeps the cost off every call
 * while still naming every regular caller within a second. */
const STACK_SAMPLE = 20;
const REPORT_MS = 1000;
const TOP_KEYS = 14;

let armed = false;
let gets = null, sets = null, callers = null;
let tickCount = 0, sampleCount = 0, windowStart = 0, reportIndex = 0;
let logFn = null;
const lines = [];

function say(s) {
    lines.push(s);
    if (logFn) logFn(s); else console.log(s);
    if (typeof host_write_file === "function") {
        /* Keep only the recent window on disk: this runs for as long as the
         * flag is present and the interesting part is the end, not the boot. */
        const keep = lines.length > 400 ? lines.slice(lines.length - 400) : lines;
        try { host_write_file(TALLY_OUT, keep.join("\n") + "\n"); } catch (e) { /* ignore */ }
    }
}

/** Best-effort "who called" — the first frame outside this module. */
function callerOf() {
    let stack;
    try { stack = new Error().stack || ""; } catch (e) { return "?"; }
    for (const raw of stack.split("\n")) {
        const l = raw.trim();
        if (!l || l.indexOf("param_tally") !== -1) continue;
        if (l.indexOf("callerOf") !== -1 || l.indexOf("Error") === 0) continue;
        /* QuickJS frames read "    at name (file:line)". Keep it short — the
         * function name is the actionable part. */
        const m = l.match(/at\s+([^\s(]+)/);
        if (m && m[1] && m[1] !== "<anonymous>") return m[1];
        return l.slice(0, 60);
    }
    return "?";
}

function note(map, key, sampleStack) {
    map.set(key, (map.get(key) || 0) + 1);
    if (sampleStack) {
        const who = callerOf();
        let byKey = callers.get(key);
        if (!byKey) { byKey = new Map(); callers.set(key, byKey); }
        byKey.set(who, (byKey.get(who) || 0) + 1);
    }
}

/**
 * Wrap the param globals. Safe to call when the flag is absent — it returns
 * without touching anything.
 */
export function installParamTally(log) {
    if (armed) return false;
    if (typeof host_file_exists !== "function" || !host_file_exists(TALLY_FLAG)) return false;
    if (typeof shadow_get_param !== "function") return false;

    logFn = log || null;
    gets = new Map(); sets = new Map(); callers = new Map();
    windowStart = Date.now();

    const realGet = globalThis.shadow_get_param;
    globalThis.shadow_get_param = function (slot, key) {
        sampleCount++;
        note(gets, String(key), sampleCount % STACK_SAMPLE === 0);
        return realGet(slot, key);
    };
    if (typeof shadow_set_param === "function") {
        const realSet = globalThis.shadow_set_param;
        globalThis.shadow_set_param = function (slot, key, value) {
            sampleCount++;
            note(sets, String(key), sampleCount % STACK_SAMPLE === 0);
            return realSet(slot, key, value);
        };
    }

    armed = true;
    say("param_tally: armed (reads and writes wrapped; reporting every " + REPORT_MS + "ms)");
    return true;
}

export function paramTallyArmed() { return armed; }

/** Call once per shadow_ui tick. No-op unless armed. */
export function paramTallyTick() {
    if (!armed) return;
    tickCount++;
    const now = Date.now();
    const dt = now - windowStart;
    if (dt < REPORT_MS) return;

    const total = (m) => { let n = 0; for (const v of m.values()) n += v; return n; };
    const nGet = total(gets), nSet = total(sets);
    const secs = dt / 1000;
    reportIndex++;

    say("param_tally: --- window " + reportIndex + " (" + dt + "ms, " + tickCount + " ticks) ---");
    say("param_tally:   reads " + nGet + " (" + (nGet / secs).toFixed(0) + "/s, " +
        (nGet / Math.max(1, tickCount)).toFixed(1) + "/tick)   writes " + nSet +
        " (" + (nSet / secs).toFixed(0) + "/s, " + (nSet / Math.max(1, tickCount)).toFixed(1) + "/tick)");

    const dump = (label, map) => {
        const rows = [...map.entries()].sort((a, b) => b[1] - a[1]).slice(0, TOP_KEYS);
        for (const [key, n] of rows) {
            const byKey = callers.get(key);
            let who = "";
            if (byKey && byKey.size) {
                who = "  <- " + [...byKey.entries()].sort((a, b) => b[1] - a[1])
                    .slice(0, 2).map(([c]) => c).join(", ");
            }
            say("param_tally:     " + label + " " + String(n).padStart(5) + "  " +
                (n / Math.max(1, tickCount)).toFixed(2).padStart(6) + "/tick  " + key + who);
        }
    };
    dump("R", gets);
    dump("W", sets);

    gets.clear(); sets.clear(); callers.clear();
    tickCount = 0; windowStart = now;
}
