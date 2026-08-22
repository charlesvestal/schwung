#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The JS side of the Master FX cap.
#
# src/host/shadow_chain_mgmt.h owns the real array; src/shadow/shadow_ui.js
# holds a MIRROR of the cap because it only ever addresses the slots by
# "master_fx:fx<N>:" key. Drift between the two is the whole failure mode: the
# UI silently stops seeing the slots past its own idea of the cap, with no
# error anywhere. So this test derives the cap from source on BOTH sides and
# fails if they disagree — it never restates the number itself.
#
# It then asserts the things a 4 -> 8 raise would silently break, and scans the
# Master FX code paths for the hand-enumerated literals that the constant is
# supposed to have replaced.

node - <<'NODE'
const fs = require("fs");

const JS = "src/shadow/shadow_ui.js";
const MJS = "src/shadow/shadow_ui_master_fx.mjs";
const HDR = "src/host/shadow_chain_mgmt.h";

const jsSrc = fs.readFileSync(JS, "utf8");
const mjsSrc = fs.readFileSync(MJS, "utf8");
const hdrSrc = fs.readFileSync(HDR, "utf8");

const fails = [];
function check(ok, msg) { if (!ok) fails.push(msg); }

/* ---- 1. the two caps, read out of source, must agree ------------------- */

const jsCapM = jsSrc.match(/^const MASTER_FX_SLOTS = (\d+);/m);
if (!jsCapM) {
    console.error("FAIL: could not read MASTER_FX_SLOTS from " + JS);
    process.exit(1);
}
const hdrCapM = hdrSrc.match(/^#define MASTER_FX_SLOTS\s+(\d+)/m);
if (!hdrCapM) {
    console.error("FAIL: could not read MASTER_FX_SLOTS from " + HDR);
    process.exit(1);
}
const cap = parseInt(jsCapM[1], 10);
const hdrCap = parseInt(hdrCapM[1], 10);

check(cap === hdrCap,
    "cap drift: " + JS + " says MASTER_FX_SLOTS=" + cap + " but " + HDR +
    " says " + hdrCap + " — the UI would address only " + Math.min(cap, hdrCap) +
    " of the shim slots");

if (fails.length) {
    for (const f of fails) console.error("FAIL: " + f);
    process.exit(1);
}

/* ---- 2. evaluate the declaration block for real ------------------------ */

/* Slice the contiguous block that declares the cap and everything derived
 * from it, and run it. Structure is asserted against the running values, not
 * against a grep of their source text. */
const startIdx = jsSrc.indexOf("const MASTER_FX_SLOTS = ");
const endIdx = jsSrc.indexOf("let selectingMasterFxModule");
check(startIdx >= 0 && endIdx > startIdx, "could not slice the Master FX declaration block");
if (fails.length) {
    for (const f of fails) console.error("FAIL: " + f);
    process.exit(1);
}
const block = jsSrc.slice(startIdx, endIdx);

/* The chain MODEL, evaluated whole: MASTER_CHAIN_TARGET.key parses a position
 * id with parseId, and the Master FX component list is now DERIVED from
 * chainComponents through the same chainEditorComponents the slot chain uses.
 * Both are taken from source rather than restated here — a second copy of
 * "what fx3 means" is the drift this file exists to catch. */
const modelSrc = fs.readFileSync("src/shared/chain_model.mjs", "utf8");
const model = new Function(modelSrc.replace(/^export /gm, "") +
    "\nreturn { parseId, chainComponents };")();
const parseChainId = model.parseId;

/* chainEditorComponents is in shadow_ui.js, not in the block, so it is lifted
 * separately and handed in — the block calls it to build the Master FX list. */
const ceAt = jsSrc.indexOf("function chainEditorComponents(");
check(ceAt >= 0, "chainEditorComponents is gone from " + JS);
const chainEditorComponents = new Function("chainComponents",
    jsSrc.slice(ceAt, jsSrc.indexOf("\n}\n", ceAt) + 2) +
    "\nreturn chainEditorComponents;")(model.chainComponents);

const decls = new Function("parseChainId", "chainEditorComponents", block + `
    return { MASTER_FX_SLOTS, masterFxChainComponents, masterFxChainConfig,
             setMasterFxChainConfig, masterFxConfig, makeEmptyMasterFxConfig,
             masterFxComponentKey, MASTER_CHAIN_TARGET,
             setConfig: (c) => { masterFxConfig = c; } };
`)(parseChainId, chainEditorComponents);

check(decls.MASTER_FX_SLOTS === cap, "evaluated MASTER_FX_SLOTS !== parsed cap");

/* ---- 3. the component list is the CHAIN, not the cap ------------------ */

/* THE POINT OF STEP 4e. A Master FX holding nothing must show ONE `+` box and
 * a Settings box, not `cap` empty ones: a fixed array of empty positions
 * communicates nothing, and at the 8 cap it is eight boxes of nothing. */
{
    const comps = decls.masterFxChainComponents();
    check(comps.length === 2,
        "an EMPTY Master FX draws " + comps.length + " boxes; it must draw two — " +
        "the `+` and Settings");
    check(comps[0] && comps[0].kind === "add",
        "the first box of an empty Master FX is " + JSON.stringify(comps[0] && comps[0].kind) +
        ", expected the `+`");
    check(comps[0] && comps[0].label === "Add FX",
        "the `+` announces as " + JSON.stringify(comps[0] && comps[0].label) +
        " — `+` read aloud is nothing at all");
    check(comps[1] && comps[1].key === "settings", "the second box is not Settings");
}

/* And a FULL one is cap modules then Settings, with NO `+`.
 *
 * This used to assert the `+` was still there, on the grounds that the slot
 * chain drew one at its cap too. The invariant worth keeping is that the two
 * chains AGREE -- and they still do; both hide it now. Offering "New effect"
 * with every slot taken was reported from the device, and clicking it either
 * did nothing or aimed at a position that cannot exist. */
decls.setConfig((() => {
    const c = decls.makeEmptyMasterFxConfig();
    for (let i = 1; i <= cap; i++) c["fx" + i] = { module: "freeverb" };
    return c;
})());
const comps = decls.masterFxChainComponents();
check(comps.length === cap + 1,
    "a full Master FX has " + comps.length + " boxes, expected cap+1 = " + (cap + 1) +
    " (the modules and Settings, with no `+`)");

for (let i = 0; i < cap; i++) {
    const c = comps[i] || {};
    check(c.key === "fx" + (i + 1),
        "component " + i + " key is " + JSON.stringify(c.key) + ", expected fx" + (i + 1));
    check(c.label === "FX " + (i + 1),
        "component " + i + " label is " + JSON.stringify(c.label));
    check(c.position === i, "component " + i + " position is " + c.position);
    check(c.kind === "module", "component " + i + " kind is " + JSON.stringify(c.kind));
    /* NEVER "synth": the diagram paints a filled band across the top of a synth
     * box as the landmark its scroll leans on, and Master FX has none. */
    check(c.kind !== "synth", "component " + i + " claims to be a synth");
}

const last = comps[comps.length - 1] || {};
check(last.key === "settings", "last component key is " + JSON.stringify(last.key) + ", expected settings");
check(last.position === cap, "settings component position is " + last.position);
check(!comps.some((c) => c && c.kind === "add"),
    "a full Master FX still draws a `+` — there is nowhere for it to add to");

/* One BELOW the cap it must come back, or the guard has turned into "never". */
decls.setConfig((() => {
    const c = decls.makeEmptyMasterFxConfig();
    for (let i = 1; i <= cap - 1; i++) c["fx" + i] = { module: "freeverb" };
    return c;
})());
const nearly = decls.masterFxChainComponents();
check(nearly.some((c) => c && c.kind === "add"),
    "one below the cap there is no `+` — the guard is hiding it always");

check(!comps.some(c => c.key === "fx" + (cap + 1)),
    "a component is keyed fx" + (cap + 1) + " — one past the cap");

/* A hole in FRONT of a loaded module is KEPT, and trailing empties are dropped:
 * position i of this list IS fx(i+1) in the DSP, so compacting a hole away on
 * READ would leave the editor addressing fx1 params while the audio ran through
 * fx2. Same rule loadChainConfigFromSlot follows for a slot chain. */
{
    const c = decls.makeEmptyMasterFxConfig();
    c.fx1 = { module: "freeverb" };
    c.fx3 = { module: "cloudseed" };
    decls.setConfig(c);
    const held = decls.masterFxChainComponents();
    check(held.length === 5,
        "a chain with a hole at fx2 draws " + held.length + " boxes, expected 5");
    check(held[1] && !held[1].module, "the hole at fx2 was compacted away on READ");
    check(held[2] && held[2].key === "fx3", "the module after the hole is not fx3");
}
decls.setConfig(decls.makeEmptyMasterFxConfig());

/* ---- 4. masterFxConfig keys ------------------------------------------- */

const cfgKeys = Object.keys(decls.makeEmptyMasterFxConfig());
const wantKeys = [];
for (let i = 1; i <= cap; i++) wantKeys.push("fx" + i);
check(cfgKeys.join(",") === wantKeys.join(","),
    "makeEmptyMasterFxConfig keys are [" + cfgKeys.join(",") + "], expected [" + wantKeys.join(",") + "]");
check(Object.keys(decls.masterFxConfig).join(",") === wantKeys.join(","),
    "the initial masterFxConfig does not cover fx1.." + ("fx" + cap));

/* ---- 5. the bounds guards reject the cap and accept cap-1 -------------- */

/* 5a. The ONE guard. getMasterFxChainParams / getMasterFxHierarchy /
 * getMasterFxParam no longer each carry a copy of `i < 0 || i >= cap`; they
 * turn an index into a component key through masterFxComponentKey and the
 * chain target rejects what that returns null for. So the guard is asserted
 * where it now lives, by RUNNING it rather than by matching its text. */
const mfxKey = decls.masterFxComponentKey;
check(typeof mfxKey === "function", "masterFxComponentKey is gone — the Master FX bounds guard has no single home");
if (typeof mfxKey === "function") {
    check(mfxKey(-1) === null, "masterFxComponentKey accepts -1");
    check(mfxKey(cap) === null,
        "masterFxComponentKey ACCEPTS slot " + cap + ", one past the cap");
    check(mfxKey(cap + 1) === null, "masterFxComponentKey accepts slot " + (cap + 1));
    check(mfxKey(cap - 1) === "fx" + cap,
        "masterFxComponentKey REJECTS slot " + (cap - 1) + ", the last real slot");
    check(mfxKey(0) === "fx1", "masterFxComponentKey rejects slot 0");
}

/* And the target itself refuses a key past the cap, refuses the settings box,
 * and addresses everything under "master_fx:" at slot 0. */
const mfxTarget = decls.MASTER_CHAIN_TARGET;
check(mfxTarget && mfxTarget.slot === 0, "MASTER_CHAIN_TARGET is not addressed at slot 0");
check(mfxTarget && mfxTarget.hasSynth === false && mfxTarget.hasMidiFx === false,
    "MASTER_CHAIN_TARGET claims a synth or MIDI FX section");
if (mfxTarget) {
    check(mfxTarget.key("fx1", "cutoff") === "master_fx:fx1:cutoff",
        "MASTER_CHAIN_TARGET.key(fx1, cutoff) is " + mfxTarget.key("fx1", "cutoff"));
    check(mfxTarget.key("fx" + cap, "cutoff") === "master_fx:fx" + cap + ":cutoff",
        "MASTER_CHAIN_TARGET refuses fx" + cap + ", the last real slot");
    check(mfxTarget.key("fx" + (cap + 1), "cutoff") === null,
        "MASTER_CHAIN_TARGET ACCEPTS fx" + (cap + 1) + ", one past the cap");
    check(mfxTarget.key("settings", "cutoff") === null,
        "MASTER_CHAIN_TARGET builds a param key for the settings box");
    check(mfxTarget.key("synth", "cutoff") === null,
        "MASTER_CHAIN_TARGET builds a param key for a synth it does not have");
    check(mfxTarget.key("midi_fx1", "cutoff") === null,
        "MASTER_CHAIN_TARGET builds a param key for a MIDI FX it does not have");
    check(mfxTarget.components().length === decls.masterFxChainComponents().length,
        "MASTER_CHAIN_TARGET.components() is not the Master FX component list");
    check(typeof mfxTarget.cap === "function" && mfxTarget.cap("fx") === cap,
        "MASTER_CHAIN_TARGET.cap does not report the shim array size");
}

/* 5b. Non-vacuity: the index-taking accessors must actually route through it,
 * or 5a would be asserting a guard nothing calls. */
for (const fn of ["getMasterFxChainParams", "getMasterFxHierarchy", "getMasterFxParam"]) {
    const body = jsSrc.match(new RegExp("function\\s+" + fn + "\\s*\\([^)]*\\)\\s*\\{([\\s\\S]*?)\\n\\}"));
    check(!!body, fn + " is gone");
    check(!!body && body[1].indexOf("masterFxComponentKey") >= 0,
        fn + " does not go through masterFxComponentKey, so it has an unguarded index");
}

const guarded = ["enterMasterFxHierarchyEditor"];
for (const fn of guarded) {
    const m = jsSrc.match(new RegExp("function\\s+" + fn + "\\s*\\(fxSlot\\)\\s*\\{\\s*if\\s*\\(([^)]*)\\)"));
    if (!m) { check(false, fn + ": no `if (...)` bounds guard found on fxSlot"); continue; }
    const cond = m[1];
    let rejects;
    try {
        rejects = new Function("fxSlot", "MASTER_FX_SLOTS", "return !!(" + cond + ");");
    } catch (e) {
        check(false, fn + ": bounds guard did not compile: " + cond);
        continue;
    }
    check(rejects(-1, cap) === true, fn + " accepts -1");
    check(rejects(cap, cap) === true,
        fn + " ACCEPTS slot " + cap + ", one past the cap — guard is `" + cond.trim() + "`");
    check(rejects(cap + 1, cap) === true, fn + " accepts slot " + (cap + 1));
    check(rejects(cap - 1, cap) === false,
        fn + " REJECTS slot " + (cap - 1) + ", the last real slot — guard is `" + cond.trim() + "`");
    check(rejects(0, cap) === false, fn + " rejects slot 0");
}

/* ---- 6. source scan: no hand-enumerated literals survive --------------- */

/* Comments are stripped first. The per-slot chain code legitimately discusses
 * "fx2" in prose, and this test is about executable literals. */
function stripComments(src) {
    return src
        .replace(/\/\*[\s\S]*?\*\//g, m => m.replace(/[^\n]/g, " "))
        .replace(/(^|[^:])\/\/[^\n]*/g, (m, p1) => p1 + " ".repeat(m.length - p1.length));
}

const files = [[JS, stripComments(jsSrc)], [MJS, stripComments(mjsSrc)]];

/* 6a. A quoted or backticked literal `fx<N>` for N >= 2 is a hand-written
 * table entry: the generated forms are all `fx${i + 1}` / "fx" + (i + 1). */
for (const [name, src] of files) {
    const lines = src.split("\n");
    for (let i = 0; i < lines.length; i++) {
        const hits = lines[i].match(/["'`]fx([2-9])["'`:]/g);
        if (hits) {
            check(false, name + ":" + (i + 1) + " hand-enumerated FX key literal " +
                hits.join(" ") + " — generate it from MASTER_FX_SLOTS instead");
        }
    }
}

/* 6b. ANY bare numeric bound within three lines of something that mentions
 * Master FX is a duplicate of the cap that a raise will not move. Deliberately
 * NOT restricted to the current cap value: after a 4 -> 8 raise, the literal
 * this is hunting for is a stale `< 4`, which a cap-valued regex would stop
 * matching exactly when it started mattering. The declaration line is the one
 * legitimate occurrence; the LFO pair (`li <= 2`) is a genuinely separate
 * fixed-size thing and is exempted by name. */
const boundRe = /(?:[<>]=?|[!=]==?)\s*(\d+)\b/g;
for (const [name, src] of files) {
    const lines = src.split("\n");
    for (let i = 0; i < lines.length; i++) {
        if (/const MASTER_FX_SLOTS =/.test(lines[i])) continue;
        if (/\bli\b/.test(lines[i])) continue;   /* the 2 Master FX LFOs */
        /* 0 and 1 are emptiness/off-by-one tests, never a slot cap. Every
         * match on the line is considered, not just the first. */
        const bm = [...lines[i].matchAll(boundRe)].filter(m => parseInt(m[1], 10) >= 2);
        if (!bm.length) continue;
        const window = lines.slice(i, i + 4).join("\n");
        if (!/master_?fx/i.test(window)) continue;
        check(false, name + ":" + (i + 1) + " bare numeric bound `" + lines[i].trim() +
            "` in Master FX code — bound it by MASTER_FX_SLOTS (or by the " +
            "component list, if it means `not the settings box`)");
    }
}

/* ---- report ----------------------------------------------------------- */

if (fails.length) {
    for (const f of fails) console.error("FAIL: " + f);
    process.exit(1);
}
console.log("PASS: Master FX JS cap derived (" + cap + "), the component list is the CHAIN " +
    "rather than the cap (empty draws one `+`, full draws " + (cap + 2) + " boxes), " +
    "config/bounds consistent, no hand-enumerated literals remain");
NODE
