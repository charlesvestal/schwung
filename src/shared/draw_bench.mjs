/**
 * draw_bench.mjs — measure what a draw primitive actually costs on device.
 *
 * The whole param-pages draw design rests on one number in a comment: that a
 * QuickJS->C binding call costs "roughly 90-100us". Every decision downstream
 * — sampling curves instead of drawing them per pixel, a draw-call budget on
 * the LFO, preferring one native call over forty fillRects — follows from it,
 * and nothing in the tree re-measures it. Another comment in the same file
 * says "every draw primitive measures near-zero", which cannot also be true.
 *
 * Opt-in and one-shot: runs from shadow_ui init when
 * /data/UserData/schwung/draw_bench_on exists, prints to the unified log, and
 * does nothing otherwise. It blocks the UI for a second or so while it runs,
 * which is why it is not on a timer.
 *
 * Reading the results: `text_width` is the control. It marshals one string,
 * walks it, and returns an int — no framebuffer writes at all — so its cost
 * is essentially the boundary crossing on its own. Subtracting it from the
 * others gives the actual rasterisation cost, and comparing it to the pure-JS
 * baselines says whether the boundary or the interpreter is the constraint.
 */

import { renderPageMovy } from "/data/UserData/schwung/shared/param_pages/render_page_movy.mjs";
import { PAGE_KNOBS } from "/data/UserData/schwung/shared/param_pages/page_plan.mjs";

const BENCH_KEYS = ["level", "tune", "drift", "width", "glide", "noise", "spread", "depth"];
const BENCH_META = {};
for (const k of BENCH_KEYS) {
    BENCH_META[k] = { key: k, label: k, type: "float", kind: "number", min: 0, max: 1, step: 0.01 };
}
const BENCH_PAGE = {
    page: { kind: PAGE_KNOBS, name: "Bench", level: "root", keys: BENCH_KEYS },
    metaIndex: { getOrGuess: (k) => BENCH_META[k] },
    values: Object.fromEntries(BENCH_KEYS.map((k, i) => [k, String(i / 7)])),
    title: "T1 > BENCH", pageIndex: 0, pageCount: 6, touched: -1, viz: [],
};
let DEV_CTX = null;

const TARGET_MS = 120;      /* per case; long enough for a 1ms clock to bite */
const MAX_ITERS = 400000;   /* hard stop, so a fast binding cannot spin */

/** Time `fn` over enough iterations to clear the clock, return ns per call. */
function timeOne(fn) {
    /* Warm up: let QuickJS settle its inline caches before the clock starts. */
    for (let i = 0; i < 200; i++) fn(i);

    let iters = 1000;
    for (;;) {
        const t0 = Date.now();
        for (let i = 0; i < iters; i++) fn(i);
        const dt = Date.now() - t0;
        if (dt >= TARGET_MS || iters >= MAX_ITERS) {
            return { ns: dt > 0 ? (dt * 1e6) / iters : 0, iters, ms: dt };
        }
        /* Scale towards the target rather than doubling blindly. */
        iters = Math.min(MAX_ITERS, Math.max(iters * 2, Math.ceil(iters * (TARGET_MS + 20) / Math.max(dt, 1))));
    }
}

export const BENCH_OUT = "/data/UserData/schwung/draw_bench.txt";

export function runDrawBench(log) {
    /* Written straight to a file as well as the log: this is a one-shot
     * diagnostic and the whole point is to come back with numbers, so it must
     * not depend on the unified logger being armed and rotating correctly. */
    const lines = [];
    const say = (s) => {
        lines.push(s);
        if (log) log(s); else console.log(s);
        if (typeof host_write_file === "function") {
            try { host_write_file(BENCH_OUT, lines.join("\n") + "\n"); } catch (e) { /* ignore */ }
        }
    };
    const has = (n) => typeof globalThis[n] === "function";
    DEV_CTX = {
        fillRect: fill_rect, print, textWidth: text_width,
        line: (typeof draw_line === "function") ? draw_line : undefined,
        fillCircle: (typeof fill_circle === "function") ? fill_circle : undefined,
        drawCircle: (typeof draw_circle === "function") ? draw_circle : undefined,
        drawArc: (typeof draw_arc === "function") ? draw_arc : undefined,
    };

    const cases = [];
    const add = (name, note, fn) => cases.push({ name, note, fn });

    /* --- controls: no C boundary at all ---------------------------------- */
    const scratch = new Uint8Array(128 * 64);
    let sink = 0;
    add("js: empty loop body", "interpreter floor", (i) => { sink += i; });
    add("js: fn call", "one JS call", (i) => { sink += jsNoop(i); });
    add("js: typed-array write", "what a JS-side framebuffer would cost per pixel",
        (i) => { scratch[i & 8191] = 1; });

    /* --- the boundary, with as little work behind it as possible ---------- */
    if (has("text_width")) add("text_width('MMMM')", "CONTROL: crossing, no framebuffer write",
        () => { sink += text_width("MMMM"); });

    /* --- real primitives -------------------------------------------------- */
    if (has("fill_rect")) {
        add("fill_rect 1x1", "crossing + 1 pixel", (i) => fill_rect(i & 63, (i >> 6) & 31, 1, 1, 1));
        add("fill_rect 32x8", "crossing + 256 pixels", (i) => fill_rect(0, (i & 7) * 8, 32, 8, 1));
    }
    if (has("draw_line")) add("draw_line 40px diag", "crossing + Bresenham",
        (i) => draw_line(0, i & 15, 40, 24 + (i & 15), 1));
    if (has("draw_arc")) add("draw_arc r=7", "crossing + the knob ring scan",
        (i) => draw_arc(20 + (i & 31), 30, 7, 230, 260, 1));
    if (has("fill_circle")) add("fill_circle r=7", "crossing + a disk",
        (i) => fill_circle(20 + (i & 31), 30, 7, 1));
    if (has("print")) add("print 'MMMM'", "crossing + 4 glyph blits",
        (i) => print(2, (i & 7) * 8, "MMMM", 1));

    say("draw_bench: measuring " + cases.length + " cases");
    const out = [];
    for (const c of cases) {
        const r = timeOne(c.fn);
        out.push({ name: c.name, note: c.note, ns: r.ns, iters: r.iters, ms: r.ms });
        say("draw_bench:   " + pad(c.name, 22) + pad(fmt(r.ns), 12) + " (" + r.iters + " iters / " + r.ms + "ms)  " + c.note);
    }

    /* Derived: how much of a primitive is boundary and how much is work. */
    const ctrl = out.find((o) => o.name.indexOf("text_width") === 0);
    if (ctrl && ctrl.ns > 0) {
        say("draw_bench: --- crossing overhead = " + fmt(ctrl.ns) + " (text_width) ---");
        for (const o of out) {
            if (o.name.indexOf("js:") === 0 || o === ctrl) continue;
            const work = o.ns - ctrl.ns;
            say("draw_bench:   " + pad(o.name, 22) + "work beyond the crossing: " + fmt(work > 0 ? work : 0));
        }
        const perPage = 475;
        say("draw_bench: a worst-case " + perPage + "-call page = " +
            (perPage * ctrl.ns / 1e6).toFixed(1) + "ms of pure crossing overhead");
    }
    /* The number that actually decides anything: a whole page, drawn by the
     * real renderer through the real bindings. */
    try {
        const r = timeOne(() => {
            if (typeof clear_screen === "function") clear_screen();
            renderPageMovy(DEV_CTX, BENCH_PAGE);
        });
        say("draw_bench: --- FULL PAGE (renderPageMovy, 8 knobs) = " +
            (r.ns / 1000).toFixed(1) + "us  -> " + (100 * (r.ns / 1e9) / (1 / 44)).toFixed(2) +
            "% of a 44Hz frame ---");
    } catch (e) { say("draw_bench: full-page case failed: " + e); }

    if (typeof clear_screen === "function") clear_screen();
    say("draw_bench: done");
    return out;
}

function jsNoop(i) { return i & 1; }
function pad(s, n) { s = String(s); while (s.length < n) s += " "; return s; }
function fmt(ns) {
    if (ns >= 1000) return (ns / 1000).toFixed(2) + "us/call";
    return ns.toFixed(0) + "ns/call";
}
