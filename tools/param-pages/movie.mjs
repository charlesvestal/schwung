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

    /* ffmpeg with an explicit palette: a 1-bit page has two colours and the
     * default 256-colour quantiser dithers the edges into grey mush. */
    const gif = path.join(OUT, name + ".gif");
    execFileSync("ffmpeg", ["-y", "-loglevel", "error", "-framerate", String(FPS),
        "-i", path.join(dir, "%04d.png"),
        "-vf", "palettegen=max_colors=2:reserve_transparent=0:stats_mode=single", "-frames:v", "1",
        path.join(dir, "pal.png")]);
    execFileSync("ffmpeg", ["-y", "-loglevel", "error", "-framerate", String(FPS),
        "-i", path.join(dir, "%04d.png"), "-i", path.join(dir, "pal.png"),
        "-lavfi", "paletteuse=dither=none", "-loop", "0", gif]);

    console.log(name.padEnd(9) + " " + nFrames + " frames, " + scene.ms + "ms" +
        (clipped ? "  CLIPPED " + clipped : "") + "  -> " + path.relative(ROOT, gif));
    return gif;
}

function main() {
    const argv = process.argv.slice(2);
    if (argv.includes("--list") || argv.length === 0) {
        for (const [k, s] of Object.entries(SCENES)) console.log(k.padEnd(9) + s.caption);
        return;
    }
    fs.mkdirSync(OUT, { recursive: true });
    const only = argv.includes("--scene") ? argv[argv.indexOf("--scene") + 1] : null;
    const names = only ? [only] : Object.keys(SCENES);
    for (const n of names) {
        if (!SCENES[n]) { console.error("no such scene: " + n); process.exit(1); }
        renderScene(n, SCENES[n]);
    }
}

main();
