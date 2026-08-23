#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Does enterParamPages actually HAND the caller's io to the controller?
#
# This test exists because both ends of that seam were tested and the seam was
# not. createSlotGridIo produced a `formatValue`, page_controller consumed one,
# a pixel test proved the renderer honoured one — and the single line between
# them listed the io fields by hand and did not mention it. An LFO target went
# on reading "FX1" on the device with every test green.
#
# So: drive the real view module with a real io and assert the capability is
# CONSULTED, rather than asserting that some line of source looks right.
#
# The view imports from the deployed /data/UserData/schwung/ paths, so the tree
# is staged into a scratch copy with those rewritten — same trick as
# test_shadow_param_editor_routing.sh.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "FAIL: python3 is required to stage the scratch tree" >&2; exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cp -R src "$TMP/src"
TMP="$TMP" python3 - <<'PY'
import os
root = os.environ["TMP"] + "/src"
for base, _, files in os.walk(root):
    for f in files:
        if not f.endswith((".mjs", ".js")):
            continue
        p = os.path.join(base, f)
        try:
            s = open(p, encoding="utf8").read()
        except Exception:
            continue
        o = s
        s = s.replace("/data/UserData/schwung/", root + "/")
        if s != o:
            open(p, "w", encoding="utf8").write(s)
PY

TREE="$TMP/src" node --input-type=module -e '
const TREE = process.env.TREE;
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

for (const n of ["print","fill_rect","clear_screen","text_width","draw_line",
                 "draw_circle","fill_circle","draw_arc","flush_display",
                 /* the knob indicator ring LEDs go out through this one */
                 "move_midi_internal_send"]) {
  globalThis[n] = () => 0;
}
globalThis.param_view_get_mode = () => 1;      /* Knobs */
globalThis.tts_get_enabled = () => false;
globalThis.shadow_get_shift_held = () => 0;

const PP = await import(TREE + "/shadow/shadow_ui_param_pages.mjs");
const { ctx } = await import(TREE + "/shadow/shadow_ui_ctx.mjs");

/* The minimum of shadow_ui.js the view touches. */
ctx.VIEWS = { PARAM_PAGES: "parampages", CHAIN_EDIT: "chainedit" };
ctx.setView = () => {};
ctx.getSlotParam = () => "";
ctx.setSlotParam = () => {};
ctx.getModuleAbbrev = () => "XX";
ctx.evaluateVisibilityCondition = () => true;

/*
 * A one-knob contract holding a single opaque param — the shape of an LFO
 * target: a stored key that only the host can turn into a name.
 */
const CHAIN_PARAMS = [{ key: "target", name: "Targ", type: "string" }];
const HIERARCHY = {
  modes: null,
  levels: { root: { label: "Slot", knobs: ["target"], params: [{ key: "target" }] } },
};

const calls = { format: [], modulated: [], get: [] };
const io = {
  getParam(fullKey) {
    calls.get.push(fullKey);
    const bare = String(fullKey).replace(/^[^:]+:/, "");
    if (bare === "ui_hierarchy") return JSON.stringify(HIERARCHY);
    if (bare === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    if (bare === "target")       return "fx1";
    return "";
  },
  setParam() {},
  isModulated(fullKey) { calls.modulated.push(fullKey); return false; },
  formatValue(fullKey, raw, surface) {
    calls.format.push([fullKey, raw, surface]);
    return surface === "header" ? "FX 1: Regen" : "Regen";
  },
};

PP.enterParamPages(0, "slot", "slot", undefined, io);
for (let i = 0; i < 12; i++) PP.tickParamPages();
PP.drawParamPages();

/* ---- 1. the io is the one being read ---------------------------------- */
if (!calls.get.length) fail("the caller io getParam was never used");
if (!calls.get.some((k) => /(^|:)chain_params$/.test(k)))
  fail("the contract was not read through the caller io");

/* ---- 2. formatValue is FORWARDED and consulted ------------------------- *
 *
 * The regression itself. Field-by-field forwarding dropped it silently. */
if (!calls.format.length) {
  fail("io.formatValue was never called — the view is not forwarding it to the " +
       "controller, so a host-resolved value (an LFO target) renders as its stored key");
}
if (!calls.format.some(([k]) => /target$/.test(String(k))))
  fail("formatValue was called, but never for the opaque key: " + JSON.stringify(calls.format));

/* Given the FULL key, like getParam — the io strips a prefix it can rely on. */
for (const [k] of calls.format) {
  if (!/^slot:/.test(String(k)))
    fail("formatValue should receive the full prefixed key, got " + JSON.stringify(k));
}

/* ---- 3. isModulated is forwarded too ----------------------------------- */
if (!calls.modulated.length)
  fail("io.isModulated was never called — the view fell back to the generic oracle, " +
       "which for a synthesised contract is both wrong and three IPC reads a tick");

if (failures) process.exit(1);
console.log("PASS: param pages io forwarding — getParam, isModulated and formatValue " +
            "all reach the controller from a caller-supplied io");
'
