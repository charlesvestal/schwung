#!/usr/bin/env node
/**
 * preview_knob_card.mjs — render the chain editor's knob card over a real
 * chain diagram, as half-block art or a PNG. No hardware required: text goes
 * through the device font atlas, so this is what the OLED shows.
 *
 *   node tools/param-pages/preview_knob_card.mjs cloudseed --knob 2
 *   node tools/param-pages/preview_knob_card.mjs obxd --knob 5 --png /tmp/out --scale 4
 *   node tools/param-pages/preview_knob_card.mjs --short
 *
 * The frame exists to survive being drawn over the diagram, so the diagram is
 * not optional scenery here — a card judged against a blank screen is a card
 * judged against the one background it never has.
 */
import fs from "node:fs";
import path from "node:path";
import { createFramebuffer, drawContext } from "./harness.mjs";
import { FIXTURE } from "./cases.mjs";
import { planPages, PAGE_KNOBS } from "../../src/shared/param_pages/page_plan.mjs";
import { buildMetaIndex } from "../../src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "../../src/shared/param_pages/viz.mjs";
import { drawHeader, drawFooter, RULE_Y } from "../../src/shared/param_pages/render_page_movy.mjs";
import { drawKnobCard } from "../../src/shared/param_pages/knob_card.mjs";
import { drawChainDiagram, DEFAULT_Y, BOX_H } from "../../src/shared/chain_diagram.mjs";

const argv = process.argv.slice(2);
const flag = (n, d = null) => {
    const i = argv.indexOf("--" + n);
    return i >= 0 ? (argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[i + 1] : true) : d;
};
const modId = argv.find((a) => !a.startsWith("--")) || null;
const knob = parseInt(flag("knob", "2"), 10) || 0;
const short = !!flag("short");
const pngDir = flag("png");
const scale = parseInt(flag("scale", "4"), 10) || 4;

/* A representative chain: two MIDI FX, a synth, three FX, FX 1 selected. */
const comps = [
    { key: "patch", kind: "patch", label: "Patch" },
    { key: "add_midi", kind: "add", section: "midiFx", label: "+" },
    { key: "midiFx", kind: "module", section: "midiFx", label: "MIDI FX 1" },
    { key: "midi_fx2", kind: "module", section: "midiFx", label: "MIDI FX 2" },
    { key: "synth", kind: "synth", label: "Synth" },
    { key: "fx1", kind: "module", section: "fx", label: "FX 1" },
    { key: "fx2", kind: "module", section: "fx", label: "FX 2" },
    { key: "fx3", kind: "module", section: "fx", label: "FX 3" },
    { key: "add_fx", kind: "add", section: "fx", label: "+" },
    { key: "settings", kind: "settings", label: "Settings" },
];
const ABBREV = { patch: "PA", add_midi: "+", midiFx: "AR", midi_fx2: "CH",
                 synth: "OB", fx1: "RV", fx2: "DL", fx3: "CH", add_fx: "+", settings: "*" };

const fb = createFramebuffer();
const ctx = drawContext(fb);

drawHeader(ctx, "Slot 1", "OB-Xd", false);
{
    const GAP = 1, BH = Math.floor((RULE_Y - DEFAULT_Y - 3 * GAP) / 4);
    for (let s = 0; s < 4; s++) {
        const iy = DEFAULT_Y + s * (BH + GAP);
        if (s === 0) ctx.fillRect(0, iy, 4, BH, 1);
        else {
            ctx.fillRect(0, iy, 4, 1, 1); ctx.fillRect(0, iy + BH - 1, 4, 1, 1);
            ctx.fillRect(0, iy, 1, BH, 1); ctx.fillRect(3, iy, 1, BH, 1);
        }
    }
}
drawChainDiagram(ctx, comps, 5, { abbrev: (c) => ABBREV[c.key] || "--" });
{
    const ly = DEFAULT_Y + BOX_H + 3;
    const centre = (y, s) => ctx.print(Math.floor((128 - s.length * 5) / 2), y, s, 1);
    centre(ly, "FX 1"); centre(ly + 11, "CloudSeed");
}
drawFooter(ctx, [["MUTE", "DFLT"], ["SHFT", "FINE"]]);

if (short) {
    drawKnobCard(ctx, { name: "S1: CUTOFF", value: "72" });
} else {
    const fx = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
    const mod = fx.modules.find((m) => m.id === modId) || fx.modules[0];
    const { pages } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const page = pages.find((p) => p.kind === PAGE_KNOBS && (p.keys || []).some(Boolean));
    if (!page) { console.error("no knob page in " + mod.id); process.exit(1); }
    const metaIndex = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const values = {};
    for (const k of page.keys) if (k) {
        const m = metaIndex.getOrGuess(k);
        const min = typeof m.min === "number" ? m.min : 0;
        const max = typeof m.max === "number" ? m.max : 1;
        values[k] = min + (max - min) * 0.62;
    }
    const { groups } = resolveViz({ keys: page.keys, metaIndex });

    /*
     * Not every knobs page fills all eight slots (obxd's pages, for example,
     * run short). `--knob 7` on a four-key page is a real input the on-device
     * gesture can produce too -- a physical knob with nothing mapped to it --
     * so this must draw a deliberate, empty-cell card rather than crash or
     * print "undefined"/"NaN". Falling back to the first populated key would
     * silently show knob 0's data under a "knob 7" label, which is worse: it
     * looks like a right answer. So an out-of-range or unmapped knob index
     * draws the row with that slot genuinely empty (drawKnobRow already
     * renders a blank cell for a null key) and the header reads "not mapped",
     * matching the real showKnobOverlay behaviour in shadow_ui.js.
     */
    const key = page.keys[knob];
    const meta = key ? metaIndex.getOrGuess(key) : null;
    const rawValue = key ? values[key] : undefined;
    const name = meta ? String(meta.label || meta.key).toUpperCase() : `KNOB ${knob + 1}`;
    const value = (rawValue === undefined || rawValue === null) ? "not mapped" : Number(rawValue).toFixed(2);

    drawKnobCard(ctx, {
        name, value,
        page, metaIndex, values, touched: knob, row: knob >> 2, viz: groups,
    });
    console.log(mod.id + " knob " + knob + ": " + (key || "(empty)"));
}

if (pngDir && pngDir !== true) {
    fs.mkdirSync(pngDir, { recursive: true });
    const file = path.join(pngDir, "knob-card" + (short ? "-short" : "-" + knob) + ".png");
    fs.writeFileSync(file, fb.toPng(scale));
    console.log(file + "  clipped=" + fb.clipped());
} else {
    console.log(fb.toBlocks());
    console.log("clipped=" + fb.clipped());
}
