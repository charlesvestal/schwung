#!/usr/bin/env node
/**
 * movie.mjs — render a page over TIME and encode it.
 *
 * The SCH-50 animation catalog could only offer still frame strips, and a
 * strip renders duration as a frame count: comparing two easings meant reading
 * a number off a chart rather than feeling a motion. Six of ten options were
 * withdrawn on exactly that ground. This is the tool that makes the question
 * answerable — the same renderer, driven by a clock, encoded to something that
 * actually plays.
 *
 * Deterministic: the renderer takes `nowMs` and never reads a clock, so a
 * movie is reproducible frame for frame and a diff between two runs means a
 * real change.
 *
 *   node tools/param-pages/movie.mjs --list
 *   node tools/param-pages/movie.mjs --scene enum
 *   node tools/param-pages/movie.mjs --all
 *
 * Output: catalog-out/movies/<scene>.gif, plus the PNG frames beside it.
 * Node-only. Nothing here ships to the device.
 */

import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createFramebuffer, drawContext } from "./harness.mjs";
import * as RM from "../../src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";
import { createAnimState } from "../../src/shared/param_pages/anim_state.mjs";
import { drawSlide, slideOffsets, scrollFrame, advanceLinear, advanceEased }
    from "../../src/shared/param_pages/page_transition.mjs";

const ROOT = path.dirname(path.dirname(path.dirname(fileURLToPath(import.meta.url))));
const OUT = path.join(ROOT, "catalog-out", "movies");
const FIXTURE = path.join(ROOT, "tools", "param-pages", "catalog-page.json");

const FPS = 30;
const MS_PER_FRAME = 1000 / FPS;

/* The page every scene is filmed on, with a trigger and two switches present
 * so one fixture serves every widget. */
function loadPage(overrides = {}) {
    const j = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
    let cp = j.chain_params;
    if (typeof cp === "string") cp = JSON.parse(cp);
    for (const p of cp) {
        if (p.key === "lfo1_env_mode") { p.type = "enum"; p.options = ["off", "on"]; }
        if (p.key === "lfo1_keytrigger") { p.type = "enum"; p.options = ["off", "on"]; }
        if (p.key === "lfo1_keyfollow") { p.access = "write"; }
        if (overrides[p.key]) { p.type = "enum"; p.options = overrides[p.key]; }
    }
    const metaIndex = buildMetaIndex({ hierarchy: j.hierarchy, chainParams: cp });
    const { groups } = resolveViz({ keys: j.page.keys, metaIndex });
    return { j, metaIndex, groups };
}

/*
 * A scene is a duration and a function from time to values. Keeping it that
 * way rather than a list of frames means the same scene can be re-filmed at a
 * different frame rate without being rewritten, and that a value is always a
 * function of the clock the renderer is given — never of the frame index.
 */
const SCENES = {
    enum: {
        ms: 3200,
        caption: "enum square — the frame sizes itself to the value",
        /*
         * `lfo1_mode` is the only real enum SQUARE on this page — lfo1_shape is
         * an enum too but the viz layer claims it and draws a waveform, so a
         * scene driving that filmed the wrong widget entirely. Its own options
         * are Poly/Mono, two values 2px apart, which demonstrates nothing.
         *
         * So the options are OVERRIDDEN for this scene to span the range the
         * widget actually has to cover: the 15px floor to the 28px cap. That is
         * a synthetic value list and is called out here rather than left to be
         * mistaken for something a module declares.
         */
        options: { lfo1_mode: ["ON", "TRI", "POLY", "MONO", "MMMM"] },
        at: (t, base) => {
            const n = 5;
            const i = Math.min(n - 1, Math.floor(t / 640));
            return { ...base, lfo1_mode: String(i) };
        },
    },
    shape: {
        ms: 2600,
        caption: "waveform silhouette — shape morph",
        at: (t, base) => {
            const i = Math.min(4, Math.floor(t / 520));
            return { ...base, lfo1_shape: String(i % 4) };
        },
    },
    switch: {
        ms: 2400,
        caption: "switch — slug flips on the frame, fill wipes after it",
        at: (t, base) => ({
            ...base,
            lfo1_env_mode: t > 1800 ? "1" : (t > 1200 ? "0" : (t > 600 ? "1" : "0")),
            lfo1_keytrigger: t > 1500 ? "0" : (t > 900 ? "1" : "0"),
        }),
    },
    knob: {
        ms: 2400,
        caption: "arc knob — no easing, the pointer IS the animation",
        at: (t, base) => ({
            ...base,
            lfo1_rate: String(Math.round(50 + 45 * Math.sin(t / 380))),
        }),
    },
    trigger: {
        ms: 2000,
        caption: "momentary — press travel and burst",
        at: (t, base) => base,
        fired: (t) => (t > 1400 ? 1400 : (t > 700 ? 700 : (t > 200 ? 200 : 0))),
    },
};

/* ffmpeg with an explicit palette: a 1-bit page has two colours and the
 * default 256-colour quantiser dithers the edges into grey mush.
 *
 * Shared by the single-page scenes and the slide variants because they differ
 * only in FPS — and the slide's FPS is NOT 30 (see below), so a second copy of
 * this would be a second place for the two cadences to be confused. */
function encodeGif(dir, gif, fps) {
    execFileSync("ffmpeg", ["-y", "-loglevel", "error", "-framerate", String(fps),
        "-i", path.join(dir, "%04d.png"),
        "-vf", "palettegen=max_colors=2:reserve_transparent=0:stats_mode=single",
        "-frames:v", "1", path.join(dir, "pal.png")]);
    execFileSync("ffmpeg", ["-y", "-loglevel", "error", "-framerate", String(fps),
        "-i", path.join(dir, "%04d.png"), "-i", path.join(dir, "pal.png"),
        "-lavfi", "paletteuse=dither=none", "-loop", "0", gif]);
}

/*
 * THE DEVICE CANNOT PRODUCE A SMOOTH 30FPS SLIDE, SO FILMING ONE IS THE WRONG
 * PROBE.
 *
 * Two hardware facts, both recorded in shadow_ui_param_pages.mjs: this
 * device's clock is quantized to roughly 11-12ms (proven there by 20
 * back-to-back Date.now() calls returning the identical value), and the grid
 * ticks at ~55Hz. A 128px slide over 200ms is therefore about eleven real
 * frames of ~12px each, and the animation reads the clock through that
 * quantum. A film at a continuous 30fps would flatter the motion into
 * something the panel cannot show, and would report green on a slide that
 * stutters in the hand.
 *
 * So: sample at DEVICE_TICK_MS and quantize the clock the advance is handed to
 * DEVICE_CLOCK_QUANTUM_MS. The GIF is re-timed to the sampled cadence for
 * playback, so what plays back is what the panel would show.
 */
const DEVICE_TICK_MS = 1000 / 55;
const DEVICE_CLOCK_QUANTUM_MS = 12;

/*
 * THIS SCENE ONCE CARRIED A LOCAL RETUNE OF advanceEased, AND IT IS GONE
 * BECAUSE THE MODULE WAS FIXED INSTEAD.
 *
 * The first round of filming found that advanceEased ran at tau = ms/3 -- 95%
 * of the travel -- so a nominal 200ms actually settled at 364ms, and `ms` was
 * incomparable between the two advances. The tool briefly compensated by
 * passing a scaled duration. That compensation is now WRONG, not merely
 * redundant: advanceEased derives its tau from SNAP_PAGES and `ms` is the
 * settle time, so scaling here would double-apply and film something no device
 * will ever do.
 *
 * The general rule this is an instance of: a filming tool that corrects for a
 * defect in the thing it films stops being evidence about the thing it films.
 *
 * Every line this scene prints still reports MEASURED moving frames and
 * measured settle time rather than the nominal `ms` -- that is what caught it,
 * and it is cheap to keep.
 */

/*
 * CUBIC EASE-IN-OUT -- THE SIBLING PRODUCT'S CURVE, AND IT IS NOT SHIPPABLE AS
 * AN ADVANCE.
 *
 * vimana2-rust does this same transition with the `easer` crate's cubic
 * ease-in-out over 10 frames. That curve ACCELERATES as well as decelerates,
 * and acceleration is a statement about where the motion STARTED -- which our
 * (pos, target, dtMs, ms) advance signature cannot see. So this cannot be an
 * advance function; it is a stateful driver carrying `from` and a progress `t`,
 * and it lives here in the filming tool purely so the two feels can be compared
 * side by side. See the report for what adopting it would cost.
 */
function makeCubicInOut() {
    let from = 0, t = 0, tgt = 0;
    return (pos, target, dtMs, ms) => {
        if (!(ms > 0) || !(dtMs >= 0) || !isFinite(pos)) return target;
        /* A retarget restarts the curve FROM WHERE WE ARE -- this is the chase
         * case, and it is why `from` is set to `pos` and not to an index. */
        if (target !== tgt) { tgt = target; from = pos; t = 0; }
        if (pos === target) return target;
        t = Math.min(1, t + dtMs / ms);
        const e = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
        return t >= 1 ? target : from + (target - from) * e;
    };
}

/*
 * `adv` is a plain function; `makeAdv` is a factory called once per variant for
 * the drivers that carry state. Only the cubic comparison needs the latter --
 * both shipping advances are pure, which is the property that lets the
 * controller store nothing but `pos`.
 */
const SLIDE_VARIANTS = [
    { name: "slide-160-linear", ms: 160, adv: advanceLinear },
    { name: "slide-200-linear", ms: 200, adv: advanceLinear },
    { name: "slide-280-linear", ms: 280, adv: advanceLinear },
    { name: "slide-160-eased", ms: 160, adv: advanceEased },
    { name: "slide-200-eased", ms: 200, adv: advanceEased },
    { name: "slide-280-eased", ms: 280, adv: advanceEased },

    /* Round two: the user asked for shorter and snappier, "just enough to get
     * a feel for what's happening".
     *
     * slide-090-eased IS THE SHIPPING BEHAVIOUR -- SLIDE_MS is 90 and
     * advanceScroll is advanceEased -- so it is filmed through the module's own
     * function with no arithmetic of its own. There is exactly one 90ms eased
     * variant on purpose: while the retune was local there were two, differing
     * only in a scale factor, and a folder holding both invites comparing the
     * shipping curve against a tool-only one. */
    { name: "slide-090-eased", ms: 90, adv: advanceEased },
    { name: "slide-090-linear", ms: 90, adv: advanceLinear },
    { name: "slide-120-eased", ms: 120, adv: advanceEased },

    /* Not shippable in this form -- see makeCubicInOut. 167ms is the sibling's
     * 10 frames at 60Hz; at our 55Hz it is ten of ours too. */
    { name: "slide-167-cubicinout", ms: 167, makeAdv: makeCubicInOut },
];

/* A beat of stillness at each end. Motion is judged against rest, and a GIF
 * that loops straight from the last frame back to the first reads as faster
 * than the transition actually is. */
const SLIDE_HOLD_MS = 260;

/* A transition films TWO pages, so it does not fit the single-page `SCENES`
 * shape (one `at(t)` returning one set of values) and gets its own renderer. */
function renderSlideVariant(v) {
    const { j, metaIndex } = loadPage({});
    const dir = path.join(OUT, v.name);
    fs.mkdirSync(dir, { recursive: true });
    for (const f of fs.readdirSync(dir)) fs.unlinkSync(path.join(dir, f));

    /*
     * Two visibly different pages, so the motion is legible rather than one
     * identical grid sliding over another.
     *
     * The viz groups are resolved PER PAGE rather than shared. A group is a
     * run of COLUMN indices, so reusing page A's groups on a page whose keys
     * are in a different order draws the right graphic over the wrong knobs —
     * which would look like a compositing bug and is not one.
     */
    const pages = [
        { page: j.page, label: j.page.name },
        { page: { ...j.page, name: "AMP", keys: j.page.keys.slice().reverse() }, label: "AMP" },
    ];
    for (const p of pages) p.viz = resolveViz({ keys: p.page.keys, metaIndex }).groups;
    /* pos only ever travels 0 -> 1 here, so base is 0 or 1; clamped anyway so
     * a future backwards variant cannot index off the end. */
    const at = (b) => pages[Math.max(0, Math.min(pages.length - 1, b))];

    const total = SLIDE_HOLD_MS + v.ms + SLIDE_HOLD_MS;
    const nFrames = Math.round(total / DEVICE_TICK_MS);

    /* The sliding pass, with both suppressions the device makes: no bank bar
     * (the compositor redraws it fixed) and no footer (it does not travel, and
     * the body's ink stops at row 54 so there is room for it). */
    const drawOne = (p) => (c) => RM.renderPageMovy(c, {
        page: p.page, metaIndex, values: j.values, title: j.title,
        pageIndex: j.pageIndex, pageCount: j.pageCount, touched: -1,
        viz: p.viz, pageLabel: p.label,
        bankBar: false, footer: null,
        nowMs: 0, anim: createAnimState(),
    });

    /* Driven exactly as the controller will drive it: a POSITION advanced by
     * the elapsed time each tick, not a progress computed from a start stamp.
     * A film that computed t = (now - start)/ms would be testing a different
     * mechanism than the one that ships — in particular it could not show a
     * retarget chasing, which is why the position model exists. */
    const adv = v.makeAdv ? v.makeAdv() : v.adv;
    let pos = 0;
    let prevClock = 0;
    let slideFrames = 0;
    let lastFrom = 0;
    const steps = [];

    for (let i = 0; i < nFrames; i++) {
        const wall = i * DEVICE_TICK_MS;
        const clock = Math.floor(wall / DEVICE_CLOCK_QUANTUM_MS) * DEVICE_CLOCK_QUANTUM_MS;
        const dt = clock - prevClock;
        prevClock = clock;
        const target = clock < SLIDE_HOLD_MS ? 0 : 1;
        pos = adv(pos, target, dt, v.ms);

        const fb = createFramebuffer();
        const ctx = drawContext(fb);
        const { base, frac } = scrollFrame(pos);

        if (frac === 0) {
            /* At rest: the ordinary un-proxied draw, chrome and all. The first
             * and last frames of every variant are this. */
            RM.renderPageMovy(ctx, {
                page: at(base).page, metaIndex, values: j.values, title: j.title,
                pageIndex: j.pageIndex + base, pageCount: j.pageCount,
                touched: -1, viz: at(base).viz, pageLabel: at(base).label,
                footer: j.footer, nowMs: clock, anim: createAnimState(),
            });
        } else {
            slideFrames++;
            const off = slideOffsets(frac, 128);
            /* The per-frame TRAVEL, in real pixels. The mean is what a frame
             * count implies; for an eased curve it is the wrong number to
             * judge by, because the first step is the one the eye reads as
             * speed and the last few are sub-pixel. Report the range. */
            steps.push(-off.from - lastFrom);
            lastFrom = -off.from;
            drawSlide(ctx, {
                fromDx: off.from, toDx: off.to,
                drawFrom: drawOne(at(base)),
                drawTo: drawOne(at(base + 1)),
                drawChrome: (c) => {
                    /* The indicator reports the TARGET, which is where input
                     * already is — it does not travel and it does not lag. */
                    RM.drawBankBar(c, j.pageIndex + target, j.pageCount, undefined);
                    if (j.footer) RM.drawFooter(c, j.footer);
                },
            });
        }
        /* NO clipped() ASSERTION HERE, AND NOT AN OVERSIGHT. A transition
         * frame produces out-of-bounds writes BY DESIGN — that is exactly how
         * the two pages get clipped at the screen edges without a clip rect.
         * The probe would fire on every composited frame, so it measures the
         * wrong thing on this scene and is deliberately not consulted. The
         * cost is real: this scene has no check against genuine overflow. */
        fs.writeFileSync(path.join(dir, String(i).padStart(4, "0") + ".png"), fb.toPng(4));
    }

    /* The film must END SETTLED, or the GIF loops out of a moving frame and
     * the motion reads as faster than it is. It is not hypothetical: an eased
     * advance settles at ~1.85x its nominal ms, so a hold sized to the nominal
     * would truncate every eased variant. Loud, because a truncated film is
     * still a plausible-looking GIF. */
    if (pos !== 1) {
        console.error("  !! " + v.name + " did not settle within " + total +
                      "ms (pos " + pos.toFixed(4) + ") — lengthen SLIDE_HOLD_MS");
    }

    const fps = Math.round(1000 / DEVICE_TICK_MS);
    const gif = path.join(OUT, v.name + ".gif");
    encodeGif(dir, gif, fps);
    /* MEASURED, never nominal — see EASED_SETTLE_FACTOR. */
    const settleMs = Math.round(slideFrames * DEVICE_TICK_MS);
    const px = steps.map((s) => Math.round(s));
    console.log(v.name.padEnd(21) + String(slideFrames).padStart(2) + " moving frames" +
        ", settle " + String(settleMs).padStart(3) + "ms" +
        " (nominal " + String(v.ms).padStart(3) + ")" +
        ", px/frame " + Math.min(...px) + ".." + Math.max(...px) +
        " mean " + Math.round(128 / Math.max(1, slideFrames)) +
        /* A zero-travel frame is a DUPLICATE picture that still costs two full
         * page renders. The eased tail is made of them, which is the cost side
         * of the settle overrun and is invisible in a frame count. */
        ", " + px.filter((s) => s === 0).length + " still" +
        "  -> " + path.relative(ROOT, gif));
    return gif;
}

function renderScene(name, scene) {
    const { j, metaIndex, groups } = loadPage(scene.options || {});
    const dir = path.join(OUT, name);
    fs.mkdirSync(dir, { recursive: true });
    for (const f of fs.readdirSync(dir)) fs.unlinkSync(path.join(dir, f));

    const anim = createAnimState();
    const nFrames = Math.round(scene.ms / MS_PER_FRAME);
    let clipped = 0;

    for (let i = 0; i < nFrames; i++) {
        const t = i * MS_PER_FRAME;
        const values = scene.at(t, j.values);
        const fb = createFramebuffer();
        RM.renderPageMovy(drawContext(fb), {
            page: j.page, metaIndex, values,
            title: j.title, pageIndex: j.pageIndex, pageCount: j.pageCount,
            touched: -1, viz: groups, footer: j.footer,
            nowMs: t, anim,
            triggerFiredAt: scene.fired ? { lfo1_keyfollow: scene.fired(t) } : {},
        });
        clipped += fb.clipped();
        fs.writeFileSync(path.join(dir, String(i).padStart(4, "0") + ".png"), fb.toPng(4));
    }

    const gif = path.join(OUT, name + ".gif");
    encodeGif(dir, gif, FPS);

    console.log(name.padEnd(9) + " " + nFrames + " frames, " + scene.ms + "ms" +
        (clipped ? "  CLIPPED " + clipped : "") + "  -> " + path.relative(ROOT, gif));
    return gif;
}

function main() {
    const argv = process.argv.slice(2);
    if (argv.includes("--list") || argv.length === 0) {
        for (const [k, s] of Object.entries(SCENES)) console.log(k.padEnd(9) + s.caption);
        /* `slide` is not in SCENES — it films two pages, not one — so it is
         * listed by hand rather than left invisible to --list. */
        console.log("slide".padEnd(9) + "page slide transition — " +
                    SLIDE_VARIANTS.length + " duration/advance variants");
        return;
    }
    fs.mkdirSync(OUT, { recursive: true });
    const only = argv.includes("--scene") ? argv[argv.indexOf("--scene") + 1] : null;
    const names = only ? [only] : Object.keys(SCENES).concat("slide");
    for (const n of names) {
        if (n === "slide") { for (const v of SLIDE_VARIANTS) renderSlideVariant(v); continue; }
        if (!SCENES[n]) { console.error("no such scene: " + n); process.exit(1); }
        renderScene(n, SCENES[n]);
    }
}

main();
