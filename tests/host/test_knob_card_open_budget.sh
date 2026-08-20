#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# What raising the knob card COSTS, once, on touch-down.
#
# The whole design argument for this feature is a read count: an IPC round trip
# is ~2.8ms against a 1.68ms whole-page render, so every value the card shows is
# read once here, on an input event, and never on the draw path. Two tests
# already pin the per-frame half of that claim (zero). Nobody pinned this half,
# and it was wrong -- the comment above knobCardOpen said "four is the whole
# bill" while the code spent SIX, because ui_hierarchy and chain_params are each
# a round trip of their own on top of one per key in the touched row. Six is
# ~17ms, a whole frame, and the number that was documented was not the number
# being paid.
#
# So: run the real knobCardOpen and count. Not a source-text pin -- the reads
# are spread across getComponentHierarchy, getComponentChainParams and the value
# loop, and a new one could arrive through any of them.
#
# The knob-context cache is WARM here, as it is on the device: showKnobOverlay
# resolves the touched knob before the card opens, and that rebuild fetches all
# eight contexts. A cold cache costs its own rebuild on top -- pre-existing, not
# the cards bill, and deliberately not measured here.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob card open-budget test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/viz.mjs"),
  import("node:fs"),
]).then(([M, V, fs]) => {
  const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
  let failures = 0;
  const fail = (m) => { console.log("FAIL: " + m); failures++; };

  /* Lift the card block out of shadow_ui.js and hand it its dependencies as
     parameters. The file cannot be imported -- it is a device UI module full of
     host globals -- but this block closes over a handful of named things, so it
     can be RUN, which is the difference between pinning a comment and pinning a
     cost. */
  const marker = src.indexOf("const KNOB_CARD_DECAY_MS");
  const fnAt = src.indexOf("function showKnobFeedback(", marker);
  const end = src.indexOf("\n}\n", fnAt);
  if (marker < 0 || fnAt < 0 || end < 0) {
    fail("the knob card block is gone from shadow_ui.js");
    process.exit(1);
  }
  const body = src.slice(marker, end + 2);

  const deps = ["NUM_KNOBS", "view", "VIEWS", "showOverlay", "hideOverlay",
    "announceParameter", "debugLog", "slotChainComponents", "selectedSlot",
    "selectedChainComponent", "isChainModuleKey", "getComponentHierarchy",
    "getComponentChainParams", "getKnobContext", "buildMetaIndex", "resolveViz",
    "getComponentParamPrefix", "getSlotParam"];

  /* Two components in one slot, with DIFFERENT parameters -- which is the only
     way a card resolved against the wrong one is visible at all. */
  const COMPS = [{ key: "synth", label: "Synth" }, { key: "fx1", label: "FX 1" }];
  const PARAMS = {
    synth: ["cutoff", "reso", "attack", "decay", "release", "gain", "pan", "drive"],
    fx1: ["size", "damp", "width", "mix", "predly", "lowcut", "hicut", "freeze"],
  };
  const hierOf = (c) => ({ levels: { root: { label: c, knobs: PARAMS[c] } } });
  const cpOf = (c) => PARAMS[c].map((k) =>
    ({ key: k, name: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const STATE = {};
  for (const c of Object.keys(PARAMS)) for (const k of PARAMS[c]) STATE[c + ":" + k] = "0.5";

  const reads = [];
  /* The lifted block reads `selectedSlot` and `selectedChainComponent` as
     parameters, so the test moves them through setters that assign to those
     bindings from the inside; `here` mirrors them for the stubs, which live out
     here and cannot see a lifted parameter. */
  const here = { slot: 0, comp: 0 };
  function build(component) {
    reads.length = 0;
    here.slot = 0;
    here.comp = component;
    const fn = new Function(...deps, body +
      "\nreturn { knobCardOpen, knobCardClose, showKnobFeedback," +
      " state: () => ({ knobCardKnob: knobCardKnob, keys: knobCardKeys," +
      " values: knobCardRowValues })," +
      " setComp: (c) => { selectedChainComponent = c; }," +
      " setSlot: (s) => { selectedSlot = s; }," +
      " touch: (i, v) => { knobTouched[i] = v; } };");
    return fn(8, "chain_edit", { CHAIN_EDIT: "chain_edit" },
      () => {}, () => {}, () => {}, () => {},
      () => COMPS, 0, component,
      (k) => k === "synth" || k === "fx1",
      /* the two whole-component reads, each a real round trip on device */
      (slot, key) => { reads.push("hierarchy"); return hierOf(key); },
      (slot, key) => { reads.push("chain_params"); return cpOf(key); },
      (i) => { const c = COMPS[here.comp].key;
               return { key: PARAMS[c][i], fullKey: c + ":" + PARAMS[c][i] }; },
      M.buildMetaIndex, V.resolveViz, (k) => k,
      (slot, key) => { reads.push(key); return STATE[key]; });
  }
  const KNOBS = PARAMS.synth;

  /* ---- 1. a full row: the whole bill, itemised ---- */
  {
    const w = build(0);
    w.knobCardOpen(1);
    const values = reads.filter((r) => r.indexOf(":") >= 0);
    const EXPECT = 6;
    if (reads.length !== EXPECT)
      fail("raising the card costs " + reads.length + " reads (~" +
           (reads.length * 2.8).toFixed(0) + "ms), not " + EXPECT +
           " -- reads were [" + reads.join(", ") + "]. If this is a deliberate " +
           "change, the itemised comment above knobCardOpen has to change with it");
    if (values.length !== 4)
      fail("expected one value read per key in the touched row, got " + values.length +
           ": " + values.join(", "));
    /* The touched knob is 1, so the row is knobs 0-3 and nothing else. Reading
       all eight would be invisible in a total that only counts. */
    if (values.join(",") !== "synth:cutoff,synth:reso,synth:attack,synth:decay")
      fail("the wrong row was read: " + values.join(", "));
    const st = w.state();
    if (!st.keys || st.keys.length !== 8) fail("the row keys were not resolved");
  }

  /* ---- 2. the short card is FREE ---- */
  /* Nothing selected means no component to resolve a row from, so there is
     nothing to read -- and this is the path the global slot knobs take, which
     fires once per tick while a knob is turning. A read here would be a read
     per tick wearing the costume of a one-off. */
  {
    const w = build(-1);
    w.knobCardOpen(3);
    if (reads.length !== 0)
      fail("the header-only card cost " + reads.length + " reads: " + reads.join(", "));
  }

  /* ---- 3. turning does not re-read ---- */
  /* The claim that survives the gesture: after the card is up, the turned knob
     moves by local arithmetic and the neighbours hold still. */
  {
    const w = build(0);
    w.touch(1, true);
    w.showKnobFeedback(1, "Reso", "0.50", 0.5);
    reads.length = 0;
    for (let i = 0; i < 60; i++) w.showKnobFeedback(1, "Reso", "0.5" + (i % 10), 0.5 + i / 1000);
    if (reads.length !== 0)
      fail("60 turns of an open card cost " + reads.length + " reads: " +
           reads.slice(0, 8).join(", "));
    if (w.state().values["reso"] === "0.5")
      fail("the turned value was not updated locally");
  }

  /* ---- 4. what the card is resolved AGAINST is part of its identity ---- */
  /* A budget test alone would bless the bug this is here for: the card is
     resolved against a slot and a component, and both move without a view
     change (the jog steps the component, Track 1-4 sets the slot). Reopening on
     the knob index alone therefore keeps the previous module`s keys, labels and
     meta while writing the NEW value in under an OLD key -- a wrong reading,
     not a stale-looking one. Cheap to detect and invisible to a read count, so
     it gets its own assertion rather than a hope. */
  {
    const w = build(0);
    w.touch(1, true);
    w.showKnobFeedback(1, "Reso", "0.50", 0.5);
    if (w.state().keys[0] !== "cutoff") fail("setup: the synth row did not resolve");

    /* jog to the next component, same knob still under the finger */
    here.comp = 1;
    w.setComp(1);
    reads.length = 0;
    w.showKnobFeedback(1, "Damp", "0.20", 0.2);
    const keys = w.state().keys;
    if (!keys || keys[0] !== "size")
      fail("after the selection moved to another component the card still shows [" +
           (keys || []).slice(0, 4).join(", ") + "] -- the row belongs to the module " +
           "that is no longer selected, so the value being turned is labelled with " +
           "another parameter`s name");
    if (reads.length === 0)
      fail("the card did not re-resolve when the selected component changed");

    /* and the same for the slot, which Track 1-4 moves */
    const w2 = build(0);
    w2.touch(0, true);
    w2.showKnobFeedback(0, "Cutoff", "0.50", 0.5);
    here.slot = 2;
    w2.setSlot(2);
    reads.length = 0;
    w2.showKnobFeedback(0, "Cutoff", "0.50", 0.5);
    if (reads.length === 0)
      fail("the card did not re-resolve when the selected slot changed");
  }

  if (failures) process.exit(1);
  console.log("PASS: raising the knob card costs 6 reads, turning it costs none, " +
              "and it re-resolves when the slot or component moves");
}).catch((e) => { console.log("FAIL: " + e); process.exit(1); });
'
