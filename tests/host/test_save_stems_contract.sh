#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Save Stems spans four files that cannot see each other, and every rule below
# is one where breaking it produces AUDIO rather than an error — which is the
# worst possible failure mode for a recording feature, because the take is
# already gone by the time anybody plays it back.
#
#   src/host/shadow_constants.h   the register + the SAVE_STEMS_* values
#   src/host/shadow_sampler.{c,h} the rings, the files, the ordering rules
#   src/schwung_shim.c            the per-stem taps and the dispatch order
#   src/shadow/shadow_ui*         the setting and its persistence
#
# Nothing here restates a number that source already owns: the counts are read
# out and compared, so a change moves the test with it or fails it.

node - <<'NODE'
const fs = require("fs");

const CONST = "src/host/shadow_constants.h";
const SMPH  = "src/host/shadow_sampler.h";
const SMPC  = "src/host/shadow_sampler.c";
const SHIM  = "src/schwung_shim.c";
const UIC   = "src/shadow/shadow_ui.c";
const UIJS  = "src/shadow/shadow_ui.js";
const GRID  = "src/shadow/shadow_ui_global_grid.mjs";

const src = {};
for (const [k, p] of Object.entries({ CONST, SMPH, SMPC, SHIM, UIC, UIJS, GRID }))
    src[k] = fs.readFileSync(p, "utf8");

const fails = [];
function check(ok, msg) { if (!ok) fails.push(msg); }

/* Comments carry the reasoning and legitimately discuss the very literals this
 * file hunts for, so executable text is what gets scanned. */
function stripComments(s) {
    return s.replace(/\/\*[\s\S]*?\*\//g, m => m.replace(/[^\n]/g, " "))
            .replace(/(^|[^:])\/\/[^\n]*/g, (m, p1) => p1 + " ".repeat(m.length - p1.length));
}

/* ---- 1. the stem count and the names are ONE list ---------------------- */

const countM = src.SMPH.match(/^#define SAMPLER_STEM_COUNT\s+(\d+)/m);
check(!!countM, "SAMPLER_STEM_COUNT is gone from " + SMPH);
const count = countM ? parseInt(countM[1], 10) : 0;

const namesM = src.SMPC.match(/sampler_stem_names\[SAMPLER_STEM_COUNT\]\s*=\s*\{([^}]*)\}/);
check(!!namesM, "sampler_stem_names[] is gone from " + SMPC);
const names = namesM ? namesM[1].match(/"[^"]+"/g) || [] : [];
check(names.length === count,
    "SAMPLER_STEM_COUNT is " + count + " but sampler_stem_names[] has " + names.length +
    " entries — the extra stem would be written to a file named from garbage, " +
    "or the last one would have no name at all");

/* ---- 2. the Move stem is the LAST index, after the four slots ---------- */

const moveM = src.SMPH.match(/^#define SAMPLER_STEM_MOVE\s+(\d+)/m);
check(!!moveM, "SAMPLER_STEM_MOVE is gone from " + SMPH);
const moveIdx = moveM ? parseInt(moveM[1], 10) : -1;
check(moveIdx === count - 1,
    "SAMPLER_STEM_MOVE is " + moveIdx + ", not the last index (" + (count - 1) + ")");

/* Indices 0..3 are addressed directly as chain slots by the shim's taps
 * (shadow_stem_store(s, ...) inside the per-slot loop), so the slot count and
 * the stem count are not independent: another chain slot without another stem
 * would silently overwrite the Move stem with slot 4's audio. */
const slotsM = src.CONST.match(/^#define SHADOW_CHAIN_INSTANCES\s+(\d+)/m);
check(!!slotsM, "SHADOW_CHAIN_INSTANCES is gone from " + CONST);
const slots = slotsM ? parseInt(slotsM[1], 10) : 0;
check(moveIdx === slots,
    "SAMPLER_STEM_MOVE is " + moveIdx + " but there are " + slots + " chain slots — " +
    "the shim indexes stems by slot number, so slot " + moveIdx + " would write over " +
    "the Move stem");

/* ---- 3. the register is APPENDED at the end of shadow_control_t -------- */

/* schwung-manager reads stay_in_shadow at a RAW OFFSET (shmconfig.go) and
 * sizeof is a contract between two binaries: a field INSERTED rather than
 * appended moves every field behind it and the two halves disagree silently. */
{
    const body = src.CONST.match(/\n([\s\S]*?)\}\s*shadow_control_t;/);
    check(!!body, "could not find the shadow_control_t body");
    if (body) {
        const fields = [...body[1].matchAll(/volatile\s+\w+\s+(\w+)\s*(\[[^\]]*\])?\s*;/g)]
            .map(m => m[1]);
        check(fields.includes("save_stems"), "save_stems is not a field of shadow_control_t");
        /* "Appended" means nothing was put in FRONT of it. It does not mean it
         * stays last forever: the next register appended after it (the first
         * was pad_observe) is exactly the discipline this pins, and a
         * last-field check would fail on every correct append from here on. So
         * the invariant is its PREDECESSOR -- the field it was appended after --
         * which an insertion anywhere before it would change. */
        const at = fields.indexOf("save_stems");
        check(at > 0 && fields[at - 1] === "metronome_beats_per_bar",
            "save_stems no longer directly follows metronome_beats_per_bar (preceded by " +
            JSON.stringify(fields[at - 1]) + ") — a field was INSERTED before it, which " +
            "moves every field behind it and schwung-manager reads one at a raw offset; " +
            "new registers are APPENDED after the last field");
    }
}

/* ---- 4. the three mode values, and the two predicates ------------------ */

for (const [name, want] of [["SAVE_STEMS_MASTER", 0], ["SAVE_STEMS_STEMS", 1], ["SAVE_STEMS_BOTH", 2]]) {
    const m = src.CONST.match(new RegExp("^#define " + name + "\\s+(\\d+)", "m"));
    check(!!m && parseInt(m[1], 10) === want,
        name + " is " + (m ? m[1] : "missing") + ", expected " + want +
        " — the value is persisted in features.json and mirrored into SHM, so " +
        "renumbering silently reinterprets every device's stored setting");
}

/* Run the predicates rather than reading them: "Both" belonging to exactly one
 * of the two is the bug that would drop half of every take. */
{
    const wm = src.CONST.match(/#define SAVE_STEMS_WANTS_MASTER\(v\)\s*(.+)/);
    const ws = src.CONST.match(/#define SAVE_STEMS_WANTS_STEMS\(v\)\s*(.+)/);
    check(!!wm && !!ws, "the SAVE_STEMS_WANTS_* predicates are gone");
    if (wm && ws) {
        const j = (expr) => expr.replace(/SAVE_STEMS_MASTER/g, "0")
                                .replace(/SAVE_STEMS_STEMS/g, "1")
                                .replace(/SAVE_STEMS_BOTH/g, "2");
        const wantsMaster = new Function("v", "return !!(" + j(wm[1]) + ");");
        const wantsStems  = new Function("v", "return !!(" + j(ws[1]) + ");");
        const table = [
            [0, true,  false, "Master"],
            [1, false, true,  "Stems"],
            [2, true,  true,  "Both"],
        ];
        for (const [v, m, st, label] of table) {
            check(wantsMaster(v) === m,
                label + " (" + v + "): WANTS_MASTER is " + wantsMaster(v) + ", expected " + m);
            check(wantsStems(v) === st,
                label + " (" + v + "): WANTS_STEMS is " + wantsStems(v) + ", expected " + st);
        }
    }
}

/* ---- 5. stems are captured BEFORE the master, in the shim -------------- */

/* Both apply the start-of-recording fade-in ramp and the MASTER half is the
 * one that consumes the counter (sampler_capture_stems only snapshots it), so
 * reversing these two lines ramps the stems by a block already spent — a
 * fade-in that is wrong on exactly the first 3 ms of every take, which is
 * audible as a click and attributable to nothing. */
{
    const shim = stripComments(src.SHIM);
    const stemAt = shim.indexOf("shadow_stem_dispatch(sampler_capture_stems)");
    const masterAt = shim.indexOf("sampler_capture_audio_from_buffer(unity_view)");
    check(stemAt >= 0, "the shim no longer dispatches stems to the sampler");
    check(masterAt >= 0, "the shim no longer captures the master from unity_view");
    check(stemAt >= 0 && masterAt >= 0 && stemAt < masterAt,
        "sampler_capture_stems runs AFTER sampler_capture_audio_from_buffer — " +
        "the master consumes the fade-in counter, so the stems would be ramped " +
        "by a block that has already been spent");
}

/* ---- 6. the stem capture gate is opened on the RT ARM, not by the worker */

/* sampler_state is RECORDING the moment sampler_arm_common returns, so the
 * master ring starts filling immediately. sampler_worker_prepare runs up to
 * ~200 ms later; opening the gate there put a few hundred milliseconds into
 * the master that were missing from the stems, and the stems are trimmed by
 * the MASTER's preroll count, so they stayed offset for the whole take. */
{
    const c = stripComments(src.SMPC);
    function bodyOf(fn) {
        const at = c.indexOf("\n" + fn.replace("()", "(") );
        if (at < 0) return null;
        const open = c.indexOf("{", at);
        let depth = 0;
        for (let i = open; i < c.length; i++) {
            if (c[i] === "{") depth++;
            else if (c[i] === "}") { depth--; if (!depth) return c.slice(open, i); }
        }
        return null;
    }
    const arm = bodyOf("static int sampler_arm_common(");
    check(!!arm, "sampler_arm_common is gone from " + SMPC);
    check(!!arm && /sampler_stems_capturing\s*=/.test(arm),
        "sampler_arm_common does not open the stem capture gate — opening it in " +
        "the worker instead loses the first ~200 ms of every take from the stems");
    check(!!arm && /sampler_take_stem_mode\s*=/.test(arm),
        "sampler_arm_common does not LATCH sampler_take_stem_mode — reading the " +
        "live setting later lets a mid-take change produce half a take of each shape");

    const prep = bodyOf("void sampler_worker_prepare(");
    check(!!prep, "sampler_worker_prepare is gone from " + SMPC);
    /* The worker may only CLOSE the gate (no stem file opened). An unconditional
     * assignment there is the regression this pins. */
    if (prep) {
        const opens = [...prep.matchAll(/sampler_stems_capturing\s*=\s*([^;]+);/g)]
            .map(m => m[1].trim());
        for (const rhs of opens) {
            check(rhs === "0",
                "sampler_worker_prepare assigns sampler_stems_capturing = " + rhs +
                "; the worker may only CLOSE the gate (assign 0) — the RT arm opens it");
        }
    }
}

/* ---- 7. skipback stem buffers are bounded ----------------------------- */

/* Five rolling buffers at the master's maximum would be ~265 MB. The cap is
 * what keeps the feature from being an out-of-memory condition you discover by
 * turning a setting on. */
{
    const maxM   = src.SMPH.match(/^#define SKIPBACK_MAX_SECONDS\s+(\d+)/m);
    const stemM  = src.SMPH.match(/^#define SKIPBACK_STEM_MAX_SECONDS\s+(\d+)/m);
    check(!!maxM && !!stemM, "the skipback second caps are gone from " + SMPH);
    if (maxM && stemM) {
        const stemSec = parseInt(stemM[1], 10);
        check(stemSec <= parseInt(maxM[1], 10),
            "SKIPBACK_STEM_MAX_SECONDS (" + stemSec + ") exceeds SKIPBACK_MAX_SECONDS");
        const bytes = stemSec * 44100 * 2 * 2 * count;
        const BUDGET = 64 * 1024 * 1024;
        check(bytes <= BUDGET,
            "the skipback stem buffers would take " + (bytes / 1048576).toFixed(1) +
            " MB (" + count + " x " + stemSec + "s) — over the " +
            (BUDGET / 1048576) + " MB this device can spend on a feature that is " +
            "off by default");
        /* And it must cover the DEFAULT length, or the common case is already
         * truncated and the cap is doing harm rather than bounding it. */
        const defM = src.SMPH.match(/^#define SKIPBACK_DEFAULT_SECONDS\s+(\d+)/m);
        check(!!defM && stemSec >= parseInt(defM[1], 10),
            "SKIPBACK_STEM_MAX_SECONDS (" + stemSec + ") is below SKIPBACK_DEFAULT_SECONDS — " +
            "the stems would be shorter than the master even at the default length");
    }
}

/* ---- 8. the setting exists, is routed, and persists -------------------- */

{
    const grid = src.GRID;
    const decl = grid.match(/\{\s*key:\s*"save_stems",[\s\S]*?\}/);
    check(!!decl, "save_stems is not declared in " + GRID);
    if (decl) {
        const opts = decl[0].match(/options:\s*\[([^\]]*)\]/);
        const n = opts ? (opts[1].match(/"[^"]*"/g) || []).length : 0;
        check(n === 3,
            "save_stems declares " + n + " options, expected 3 (Master / Stems / Both) — " +
            "\"Both\" is the one that answers \"record song mode as master AND stems\"");
        check(/default:\s*0/.test(decl[0]),
            "save_stems does not default to 0 (Master) — this changes what Record " +
            "leaves on disk, and the default must stay what every device already has");
    }
    /* Declared in AUDIO, which is where the user was told to look. */
    const audioAt = grid.indexOf("export const AUDIO_PARAMS");
    const nextAt = grid.indexOf("export const", audioAt + 10);
    check(audioAt >= 0 && grid.slice(audioAt, nextAt).includes('key: "save_stems"'),
        "save_stems is not in AUDIO_PARAMS");

    check(/save_stems:\s*\[0,\s*1,\s*2\]/.test(grid),
        "save_stems is missing from GLOBAL_ENUM_VALUES, so the grid cannot round-trip it");
    check(/save_stems:\s*\{[^}]*read:/.test(grid),
        "save_stems has no entry in the read/write routing map");

    /* The C setter is what writes features.json, exactly as recall_quantize and
     * metronome_mode do — the SHM register does not survive a reboot. */
    check(/features_json_set\("save_stems"/.test(src.UIC),
        "shadow_ui.c does not persist save_stems to features.json — the setting " +
        "would be lost on every reboot, silently, because SHM is not storage");
    check(/shadow_control->save_stems\s*=/.test(src.UIC),
        "shadow_ui.c does not write the save_stems register");
    check(/"shadow_save_stems_set"/.test(src.UIC),
        "shadow_save_stems_set is not bound into the JS global object");

    /* And JS reads it back at startup and pushes it down again. */
    check(/function loadSaveStems\(/.test(src.UIJS), "loadSaveStems is gone from " + UIJS);
    check(/loadSaveStems\(\)/.test(src.UIJS.replace(/function loadSaveStems\(/, "")),
        "loadSaveStems is never CALLED — the persisted setting would never reach the shim");
}

/* ---- 9. the shim mirrors the register into the sampler ---------------- */

check(/sampler_set_stem_mode\(/.test(stripComments(src.SHIM)),
    "the shim never calls sampler_set_stem_mode — the setting would reach SHM and " +
    "stop there");

/* ---- report ----------------------------------------------------------- */

if (fails.length) {
    for (const f of fails) console.error("FAIL: " + f);
    process.exit(1);
}
console.log("PASS: save stems contract — " + count + " stems (" +
    names.map(s => s.replace(/"/g, "")).join(", ") + ") with Move last and one per " +
    "chain slot, save_stems appended at the end of shadow_control_t, Both wanting " +
    "master AND stems, stems captured before the master, the capture gate opened on " +
    "the RT arm, skipback stem buffers bounded, and the setting declared in Audio, " +
    "routed, persisted to features.json and pushed back down at startup");
NODE
