#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A MODULE THAT BINDS THE CONTROLLER ITSELF MUST SEE ITS OWN MODULATION.
#
# `isModulated` is an injected io callback and it used to default to a flat
# "no". The host supplies one (isHierarchyParamModulated); a module that binds
# the controller from its own ui_chain.js supplies getParam/setParam/announce
# and nothing else -- so for 9W9 the controller believed NOTHING was ever
# modulated: `modCache` stayed empty, `refreshModulatedValues` had nothing to
# read, and the knob showed the BASE while an LFO or an automation lane drove
# the parameter underneath it. No error, no missing page -- the knob simply
# never moved, which reads as "automation isn't playing".
#
# Driven against 9W9's own captured contract, because it is the module the
# defect was found on. The assertion is the SCREEN: render the page at a
# series of clip positions and require the picture to change. Anything less
# (a flag, a cache) can be true while the user still sees a frozen knob.

fail() { echo "FAIL: $1"; exit 1; }
command -v node >/dev/null || { echo "SKIP: node not available"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/t.mjs" <<EOF
const REPO = "$PWD";
EOF
cat >> "$tmp/t.mjs" <<'EOF'
import fs from "fs";
/* Dynamic import: the repo path is injected above, and a static import
 * cannot be built from a variable. */
const { createController, LAYOUT_MOVY } = await import(REPO + "/src/shared/param_pages/page_controller.mjs");
const { createFramebuffer, drawContext } = await import(REPO + "/tools/param-pages/harness.mjs");

const mods = JSON.parse(fs.readFileSync(`${REPO}/tests/fixtures/module-contracts.json`, "utf8")).modules;
const m = mods.find(x => x.id === "9w9");
if (!m) { console.log("SKIP: no 9w9 contract in the fixture"); process.exit(0); }
const str = (v) => (typeof v === "string" ? v : JSON.stringify(v));

const DRIVEN = "bd_c_tune";
let t = 0;
function getParam(key) {
    if (key === "synth:ui_hierarchy") return str(m.ui_hierarchy);
    if (key === "synth:chain_params") return str(m.chain_params);
    if (key === `synth:${DRIVEN}:modulated`) return "1";            /* a lane is driving it */
    if (key === `synth:${DRIVEN}:effective`) return String(40 + (t % 80));
    if (key.endsWith(":modulated")) return "0";
    if (key.endsWith(":base")) return "74";
    if (key.endsWith(":effective")) return "";
    return "74";
}

function run(old) {
    /* `old` reproduces what a module-built io used to get. */
    const ctrl = createController(Object.assign(
        { getParam, setParam: () => {}, announce: () => {} },
        old ? { isModulated: () => false } : {}));
    ctrl.load({ slot: 0, component: "synth", prefix: "synth" });
    ctrl.setLayout(LAYOUT_MOVY);
    const page = ctrl.pages.findIndex(p => (p.keys || []).includes(DRIVEN));
    if (page < 0) { console.log("FAIL: " + DRIVEN + " is on no page of the captured contract"); process.exit(1); }
    ctrl.goToPage(page, { remember: false });
    const frames = new Set();
    t = 0;
    for (let i = 0; i < 40; i++) {
        t += 3;
        ctrl.tick();
        const fb = createFramebuffer();
        ctrl.render(drawContext(fb), { title: "9W9" });
        frames.add(fb.toBlocks());
    }
    return frames.size;
}

const now = run(false);
const before = run(true);
if (now <= 1) {
    console.log("FAIL: a module-built io shows a FROZEN screen while a lane drives the parameter");
    process.exit(1);
}
/* The negative control. Without it this test would still pass if the picture
 * moved for some unrelated reason, which is how a probe reports green while
 * measuring nothing. */
if (before !== 1) {
    console.log("FAIL: the negative control moved too (" + before + " frames) -- this test is not measuring the modulation path");
    process.exit(1);
}
console.log("PASS: module-built io follows modulation (" + now + " distinct frames; flat-no control: " + before + ")");
EOF

node "$tmp/t.mjs" || fail "module-built io does not follow modulation"

# ...and the default must stay the DEVICE's answer, not "no".
grep -q 'io.isModulated ||' src/shared/param_pages/page_controller.mjs \
  || fail "isModulated must keep a default -- an io without one is every module-drawn grid"
grep -q "MOD_PROBE_EVERY" src/shared/param_pages/page_controller.mjs \
  || fail "the isModulated default must ASK the device; a flat false is the defect this pins"

# ...and the probe must SPEND a rotation stop, never add a read. The cursor's
# one-read-per-tick budget is a frame-rate fact (~2.8 ms per round trip against
# a 1.68 ms whole-page render), so probing alongside the value would have
# doubled it for exactly the consumers that draw their own screen.
probe=$(awk '/THE MODULATION PROBE/,/^        }$/' src/shared/param_pages/page_controller.mjs)
echo "$probe" | grep -q 'return null;' \
  || fail "the probe must TAKE the stop (return) rather than fall through to the value read"
