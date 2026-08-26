/**
 * styles/index.mjs — the SCH-50 option catalog registry.
 *
 * Each SET is ten alternative treatments of one control, ordered on a shared
 * minimal -> radical axis so that "option 7" means a comparable distance in
 * every set. The axis is a HYPOTHESIS, not a conclusion: the pairwise
 * preference data collected by tools/param-pages/ab_server.mjs is allowed to
 * contradict it, and that disagreement is a finding rather than an error.
 *
 * Nothing here ships to the device. Production draw code imports nothing from
 * this directory.
 */

export const OPTIONS_PER_SET = 10;

/** Payload shapes. A set declares one; every option in it must match. */
export const KIND_DRAW = "draw";     /* draw(ctx, ...) — same signature as the fn it could replace */
export const KIND_FONT = "font";     /* { glyphs } — [advance, yOff, w, h, ...rowBits], CHARS order */
export const KIND_MOTION = "motion"; /* { behaviour, frames(from, to, n) } */

export const KINDS = [KIND_DRAW, KIND_FONT, KIND_MOTION];

/* Sets are registered here as they are authored. Order is the catalog order. */
export const SETS = [];

export function registerSet(set) {
    SETS.push(set);
    return set;
}

export function setById(id) {
    return SETS.find((s) => s.id === id) || null;
}

/**
 * Structural problems with a set, as readable strings. Empty array = valid.
 * Deliberately returns ALL problems rather than throwing on the first: when a
 * set is being authored, seeing every complaint at once is faster.
 */
export function validateSet(set) {
    const bad = [];
    const where = set && set.id ? set.id : "<no id>";

    if (!set || typeof set !== "object") return ["set is not an object"];
    if (!set.id) bad.push("set has no id");
    if (!set.title) bad.push(where + ": set has no title");
    if (!KINDS.includes(set.kind)) bad.push(where + ": kind must be one of " + KINDS.join("/"));
    if (!Array.isArray(set.options)) return bad.concat(where + ": options is not an array");

    if (set.options.length !== OPTIONS_PER_SET)
        bad.push(where + ": has " + set.options.length + " options, want " + OPTIONS_PER_SET);

    const seenId = new Set();
    const seenPos = new Set();
    for (const o of set.options) {
        const oid = o && o.id ? o.id : "<no id>";
        if (!o || typeof o !== "object") { bad.push(where + ": an option is not an object"); continue; }
        if (!o.id) bad.push(where + ": an option has no id");
        else if (seenId.has(o.id)) bad.push(where + ": duplicate option id " + o.id);
        else seenId.add(o.id);

        if (!o.name) bad.push(where + "/" + oid + ": no name");
        if (!o.note) bad.push(where + "/" + oid + ": no note (the catalog rationale line)");

        if (!Number.isInteger(o.position) || o.position < 1 || o.position > OPTIONS_PER_SET)
            bad.push(where + "/" + oid + ": position must be an integer 1.." + OPTIONS_PER_SET);
        else if (seenPos.has(o.position)) bad.push(where + ": duplicate position " + o.position);
        else seenPos.add(o.position);

        if (set.kind === KIND_DRAW && typeof o.draw !== "function")
            bad.push(where + "/" + oid + ": kind draw needs a draw function");
        if (set.kind === KIND_FONT && !Array.isArray(o.glyphs))
            bad.push(where + "/" + oid + ": kind font needs a glyphs array");
        if (set.kind === KIND_MOTION && (typeof o.frames !== "function" || !o.behaviour))
            bad.push(where + "/" + oid + ": kind motion needs frames() and behaviour");
    }

    if (set.options.length === OPTIONS_PER_SET && seenPos.size === OPTIONS_PER_SET) {
        for (let p = 1; p <= OPTIONS_PER_SET; p++)
            if (!seenPos.has(p)) bad.push(where + ": position " + p + " is missing");
    }
    return bad;
}

/**
 * Every set, plus the one problem that only exists BETWEEN sets.
 *
 * Thirteen sets are registered from six different authoring tasks, so an id
 * collision is plausible. Per-set validation cannot see it, and the symptom is
 * silent: both sets validate clean, `setById` returns whichever registered
 * first, and the other never renders. A missing contact sheet is easy to
 * mistake for a set that was not written yet.
 */
export function validateAll() {
    const bad = SETS.flatMap(validateSet);
    const seen = new Set();
    for (const s of SETS) {
        if (!s || !s.id) continue;
        if (seen.has(s.id)) bad.push("duplicate set id " + s.id + " — one of them will never render");
        else seen.add(s.id);
    }
    return bad;
}

/*
 * ------------------------------------------------------------- the sets --
 *
 * Each set lives in its own file and is registered HERE, by a call, at the
 * bottom of this module. Call order is catalog order. New sets join this
 * block; do not invent a second way in.
 *
 * A set imports `registerSet` and the KIND_* constants from this file, so the
 * two modules are a cycle, and the obvious two spellings of "self-register on
 * import" both fail:
 *
 *   import "./knob.mjs";         Import declarations are HOISTED, so this is
 *                                not evaluated at the bottom of the file. The
 *                                set runs before this module's body and finds
 *                                `SETS` and `KIND_DRAW` still in the temporal
 *                                dead zone — ReferenceError at import time,
 *                                nothing registered. Only `registerSet`
 *                                survives, because a function declaration is
 *                                hoisted too, which is what makes this easy to
 *                                talk yourself out of.
 *
 *   await import("./knob.mjs");  Deadlocks. The set statically imports this
 *                                module, so its evaluation waits on ours while
 *                                ours waits on its — "Detected unsettled
 *                                top-level await", and the process exits 13.
 *
 * A named export called after the body has run has neither problem: the set
 * file only DECLARES functions at evaluation time, so the hoisted import of it
 * is harmless, and by the time `register()` runs every binding above is
 * initialised.
 */
import { register as registerKnob } from "./knob.mjs";
import { register as registerFader } from "./fader.mjs";
import { register as registerFills } from "./fills.mjs";
import { register as registerOpaqueBox } from "./opaque_box.mjs";
import { register as registerVizSwitch } from "./viz_switch.mjs";
import { register as registerFont } from "./font/index.mjs";
import { register as registerVizEnvelope } from "./viz_envelope.mjs";
import { register as registerVizFilter } from "./viz_filter.mjs";
import { register as registerVizLfo } from "./viz_lfo.mjs";
import { register as registerVizSample } from "./viz_sample.mjs";
import { register as registerEnumSquare } from "./enum_square.mjs";
import { register as registerLabelCell } from "./label_cell.mjs";

registerKnob();
registerFader();
registerFills();
registerOpaqueBox();
registerVizSwitch();
registerFont();
registerVizEnvelope();
registerVizFilter();
registerVizLfo();
registerVizSample();
registerEnumSquare();
registerLabelCell();
