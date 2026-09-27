#!/usr/bin/env bash
# Global Settings -> Surfaces: Follow Focus and Surface Nav exist only for a
# surface with a screen. Under "CC Only" they are HIDDEN -- a Surface Nav row
# there once refused every choice and read back Map (hardware, 2026-09-26).
# Driven through the real Global Settings io and the real page controller,
# including the live re-plan when the Surface row is turned.
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
const G = await import("./src/shadow/shadow_ui_global_grid.mjs");
const { createController } = await import("./src/shared/param_pages/page_controller.mjs");
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };
const state = { external_surface: "0", follow_focus: "0", surface_nav: "0" };
let ctl = null;
/* cachedValue: the controller own value, as shadow_ui.js wires paramPagesCachedValue. */
const io = { readParam: (k) => (k in state ? state[k] : "0"), writeParam: (k, v) => { state[k] = String(v); },
             cachedValue: (k) => (ctl && ctl.state && ctl.state.values ? ctl.state.values[k] : undefined) };
const gio = G.createGlobalGridIo(io);
let t = 0;
ctl = createController({ getParam: (k) => gio.getParam(k), setParam: (k, v) => gio.setParam(k, v), now: () => t });
ctl.load({ slot: 0, component: "global", prefix: "global", visible: gio.visible, paginate: false });
for (let i = 0; i < 30; i++) { t += 16; ctl.tick(); }
const surfaces = () => (ctl.pages.find((p) => p.name === "Surfaces") || {}).keys || [];
eq("the Surface row names CC Only first", G.SURFACES_PARAMS[0].options, ["CC Only", "E16", "EC4"]);
eq("under CC Only: no Follow Focus, no Surface Nav", surfaces().filter(Boolean), ["external_surface", "ec4_setup"]);
ctl.goToPage(ctl.pages.findIndex((p) => p.name === "Surfaces"), { remember: false });
/* An enum steps one option per ENUM_DELTA_DIV (4) detents. */
for (let d = 0; d < 4; d++) { t += 50; ctl.onKnobTurn(0, 1, t); }
for (let i = 0; i < 30; i++) { t += 16; ctl.tick(); }
eq("turning Surface to E16 brings both rows back, on the page, with no re-entry",
   surfaces().filter(Boolean), ["external_surface", "follow_focus", "surface_nav", "ec4_setup"]);
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
