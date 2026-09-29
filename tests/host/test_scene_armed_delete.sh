#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# DELETE + KNOB MEANS ONE THING AT A TIME.
#
# With no scene armed, Delete + a knob TOUCH clears that knob's clip
# automation (`lanes:clear_param`). With a scene snapshot ARMED, Delete + a
# knob TURN removes the knob from the snapshot -- decided in the shim, which
# turns the armed write into an unlock. The touch comes first, so before this
# the one gesture did both: the knob left the scene AND its automation was
# erased, silently. The armed edit owns Delete.

fail() { echo "FAIL: $1"; exit 1; }
command -v node >/dev/null || { echo "SKIP: node not available"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/t.mjs" <<EOF
const REPO = "$PWD";
EOF
cat >> "$tmp/t.mjs" <<'EOF'
import fs from "fs";
const { createController, LAYOUT_MOVY } = await import(REPO + "/src/shared/param_pages/page_controller.mjs");
const mods = JSON.parse(fs.readFileSync(REPO + "/tests/fixtures/module-contracts.json", "utf8")).modules;
const m = mods.find((x) => x.id === "9w9");
if (!m) { console.log("SKIP: no 9w9 contract"); process.exit(0); }
const str = (v) => (typeof v === "string" ? v : JSON.stringify(v));

let armed = false;
const writes = [];
function getParam(key) {
    if (key === "synth:ui_hierarchy") return str(m.ui_hierarchy);
    if (key === "synth:chain_params") return str(m.chain_params);
    if (key === "lanes:cleared") return "3";
    if (key.endsWith(":held")) return "";
    if (key.endsWith(":modulated")) return "0";
    return "74";
}
function run(useDefault) {
    const io = { getParam, announce: () => {}, heldStep: () => -1,
                 setParam: (k, v) => { writes.push(k); return true; },
                 deleteHeld: () => true };
    if (useDefault) {
        /* The SHM default, as on a module-drawn screen (9W9 binds no io). */
        globalThis.shadow_get_scene_state = () => ({ edit: armed ? 2 : -1 });
    } else {
        io.sceneArmed = () => armed;
    }
    const ctrl = createController(io);
    ctrl.load({ slot: 0, component: "synth", prefix: "synth" });
    ctrl.setLayout(LAYOUT_MOVY);
    ctrl.dismissHint && ctrl.dismissHint();
    for (let i = 0; i < 14; i++) ctrl.tick();
    const cleared = () => writes.filter((k) => k === "lanes:clear_param").length;

    armed = false; writes.length = 0;
    ctrl.onKnobTouch(1, true); ctrl.onKnobTouch(1, false);
    if (cleared() !== 1) throw new Error("unarmed Delete+touch must clear the lane (" + cleared() + ")");

    armed = true; writes.length = 0;
    ctrl.onKnobTouch(1, true); ctrl.onKnobTouch(1, false);
    if (cleared() !== 0) throw new Error("ARMED Delete+touch cleared automation (" + (useDefault ? "shm default" : "io") + ")");
}
run(false);
run(true);
console.log("PASS: armed scene owns Delete + knob; unarmed it clears automation");
EOF
node "$tmp/t.mjs" || fail "see above"
