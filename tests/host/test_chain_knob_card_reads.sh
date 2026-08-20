#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The knob card must cost NOTHING per frame.
#
# The chain editor sits at 60Hz and an IPC round trip is ~2.8ms against a
# 1.68ms whole-page render, so a single read added to the draw path costs more
# than redrawing the entire screen. Every value the card shows is read once on
# touch-down (knobCardOpen in shadow_ui.js); this pins the other half of that
# claim -- that the RENDERER reads nothing at all.
#
# Two assertions, because they fail differently. The static one catches a read
# helper being imported into knob_card.mjs. The dynamic one catches a read
# reaching the draw path through the ctx or the data.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob card read-budget test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/harness.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/knob_card.mjs"),
  import("node:fs"),
]).then(([H, M, KC, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  /* ---- 1. static: the module must not reach for a param at all ---- */
  const src = fs.readFileSync("src/shared/param_pages/knob_card.mjs", "utf8");
  for (const banned of ["getSlotParam", "shadow_get_param", "get_param",
                        "setSlotParam", "shadow_set_param"]) {
    if (src.indexOf(banned) >= 0)
      fail("knob_card.mjs references " + banned + " -- the card renderer is pure " +
           "and reads nothing; the values come from knobCardOpen");
  }
  console.log("PASS: knob_card.mjs holds no param I/O");

  /* ---- 2. dynamic: draw it 60 times with a ctx that counts everything ---- */
  const params = [
    { key: "a", name: "Alpha", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "b", name: "Beta",  type: "float", min: 0, max: 1, step: 0.01 },
    { key: "c", name: "Gamma", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "d", name: "Delta", type: "enum",  options: ["Hall", "Room", "Plate"] },
  ];
  const mi = M.buildMetaIndex({ hierarchy: null, chainParams: params });
  const keys = ["a", "b", "c", "d", null, null, null, null];
  const values = { a: 0.2, b: 0.4, c: 0.6, d: 1 };

  let reads = 0;
  const fb = H.createFramebuffer();
  const base = H.drawContext(fb);
  const ctx = {};
  for (const k of Object.keys(base)) ctx[k] = base[k];
  /* Anything the card might reach for that would be a round trip on device. */
  for (const name of ["getSlotParam", "getParam", "read"]) {
    ctx[name] = () => { reads++; return ""; };
  }
  /* A values object that screams if the renderer asks for a key it was not
   * handed -- that is what a lazy read on the draw path would look like. */
  const guarded = new Proxy(values, {
    get(t, p) {
      if (typeof p === "string" && !(p in t) && p !== "then") reads++;
      return t[p];
    },
  });

  for (let i = 0; i < 60; i++) {
    KC.drawKnobCard(ctx, {
      name: "ALPHA", value: "0.62", row: 0, touched: i % 4,
      page: { kind: "knobs", keys }, metaIndex: mi, values: guarded,
    });
  }
  if (reads !== 0) fail("the card issued " + reads + " reads across 60 frames -- " +
                        "at ~2.8ms each that is more than redrawing the screen");
  if (fb.clipped() !== 0) fail("card drew outside the display");
  console.log("PASS: 60 frames, zero reads");
  console.log("OK");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
