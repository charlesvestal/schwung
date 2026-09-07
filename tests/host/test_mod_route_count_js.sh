#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE TWO SIDES OF THE MOD-ROUTE COUNT.
#
# src/modules/chain/dsp/chain_internal.h owns the real array; the shadow UI
# holds a MIRROR of the count because it only ever addresses routes by
# "mod<N>:" key. Drift between them has no error path in either direction:
#
#   UI count too HIGH  -> the grid draws pages for routes the DSP does not
#                         have. Every read answers "" and every knob looks
#                         dead, which reads as a broken module, not a mismatch.
#   UI count too LOW   -> routes the DSP is happily running are invisible and
#                         unreachable, including in the target picker.
#
# So this derives the count from source on BOTH sides and fails if they
# disagree -- it never restates the number itself. Modelled directly on
# test_master_fx_slots_js.sh, which exists for the same reason: a
# _Static_assert cannot span the two languages.

node - <<'NODE'
const fs = require("fs");

const MJS = "src/shadow/shadow_ui_slot_grid.mjs";
const JS  = "src/shadow/shadow_ui.js";
const HDR = "src/modules/chain/dsp/chain_internal.h";

const mjsSrc = fs.readFileSync(MJS, "utf8");
const jsSrc  = fs.readFileSync(JS, "utf8");
const hdrSrc = fs.readFileSync(HDR, "utf8");

const fails = [];
const check = (ok, msg) => { if (!ok) fails.push(msg); };

/* ---- 1. the two counts, read out of source, must agree ----------------- */

const mjsM = mjsSrc.match(/^export const MOD_ROUTE_COUNT = (\d+);/m);
if (!mjsM) {
    console.error("FAIL: could not read MOD_ROUTE_COUNT from " + MJS);
    process.exit(1);
}
const hdrM = hdrSrc.match(/^#define MOD_ROUTE_COUNT\s+(\d+)/m);
if (!hdrM) {
    console.error("FAIL: could not read MOD_ROUTE_COUNT from " + HDR);
    process.exit(1);
}
const jsCount = parseInt(mjsM[1], 10);
const cCount = parseInt(hdrM[1], 10);
check(jsCount === cCount,
      "MOD_ROUTE_COUNT is " + jsCount + " in " + MJS + " but " + cCount +
      " in " + HDR + " -- the UI would draw pages the DSP does not serve, or " +
      "hide routes it is running");

/* ---- 2. ONE declaration on the JS side --------------------------------- */
/* shadow_ui.js must IMPORT it, not re-declare it: a second literal is the
   drift this test exists to catch, one file further along. */
check(!/^\s*(const|let|var)\s+MOD_ROUTE_COUNT\s*=/m.test(jsSrc),
      "shadow_ui.js declares its own MOD_ROUTE_COUNT -- import it from " + MJS);
check(/MOD_ROUTE_COUNT,\s*MOD_ROUTE_INDICES\s*\}\s*from\s*'\.\/shadow_ui_slot_grid\.mjs'/.test(jsSrc),
      "shadow_ui.js does not import MOD_ROUTE_COUNT/MOD_ROUTE_INDICES from " + MJS);

/* ---- 3. the count is USED, not decorative ------------------------------ */
/* A constant nothing reads is a promise, not a mechanism. These are the two
   places a hand-written list of eight would otherwise sit. */
check(/MOD_ROUTE_INDICES\.map/.test(mjsSrc),
      MJS + " does not generate its route levels from MOD_ROUTE_INDICES");
check(/MOD_ROUTE_INDICES\.map/.test(jsSrc),
      JS + " does not generate its settings-list rows from MOD_ROUTE_INDICES");
check(/i < MOD_ROUTE_COUNT/.test(jsSrc),
      JS + " does not bound its mod-to-mod target list by MOD_ROUTE_COUNT");

/* ---- 4. no surviving two-route literals, IN THE SLOT CONTEXT ------------ */
/*
 * Scoped to makeSlotLfoCtx, because the identical shapes are CORRECT in
 * makeMasterFxLfoCtx: Master FX really does have exactly two LFOs, so flipping
 * an index there offers the only other one. Banning them file-wide would fail
 * on code that is right, which is how a pin gets deleted rather than fixed.
 */
const slotCtx = (() => {
  const m = jsSrc.match(/^function makeSlotLfoCtx\([^]*?^}/m);
  if (!m) { console.error("FAIL: could not lift makeSlotLfoCtx"); process.exit(1); }
  return m[0];
})();
const banned = [
  [/const otherIdx = lfoIdx === 0 \? 1 : 0;/, slotCtx,
   "the slot mod-to-mod target list still flips an index instead of listing every other route"],
  [/key === "lfo1" \|\| key === "lfo2"/, jsSrc,
   "the settings-list handler still tests two literal route keys"],
  [/\{ key: "lfo1", label: "LFO 1", type: "action" \}/, jsSrc,
   "the settings list still hardcodes two LFO rows"],
];
for (const [re, hay, msg] of banned) check(!re.test(hay), msg);

/* ---- 5. the KEY SPELLINGS the two screens use -------------------------- */
/* A slot route is "modN:"; a Master FX route keeps "lfoN:" because its params
   are parsed in the shim by a literal strncmp on "lfo1:"/"lfo2:". Renaming the
   master keys leaves every Master FX LFO control writing a key nothing reads,
   silently, with the page still drawing. */
check(/const prefix = "mod" \+ \(lfoIdx \+ 1\) \+ ":";/.test(jsSrc),
      "makeSlotLfoCtx no longer addresses a slot route as modN:");
check(/strncmp\(fx_key, "lfo1:", 5\) == 0/.test(
        fs.readFileSync("src/host/shadow_chain_mgmt.c", "utf8")),
      "the shim no longer parses master_fx lfo1:/lfo2: -- if that moved, the " +
      "master key stem in shadow_ui_slot_grid.mjs must move with it");
check(/keyPrefix === "" \? "mod" : "lfo"/.test(mjsSrc),
      MJS + " no longer picks its key stem per screen");

if (fails.length) {
    for (const f of fails) console.error("FAIL: " + f);
    process.exit(1);
}
console.log("PASS: mod route count agrees across chain_internal.h, " + MJS +
            " and " + JS + " (" + cCount + " routes), and the per-screen key " +
            "stems are intact");
NODE
