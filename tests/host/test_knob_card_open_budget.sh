#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# What raising the knob card COSTS, once, on touch-down -- ON BOTH CHAINS.
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
# are spread across the hierarchy fetch, the chain_params fetch and the value
# loop, and a new one could arrive through any of them.
#
# EVERY CASE RUNS AGAINST BOTH TARGETS. That is the rule 1b of the Master FX
# variable-length design states: a feature that cannot say what it does on
# Master FX is not finished, and shared code is necessary but not sufficient --
# a shared function with one chain-shaped early return in it drifts just as
# well. The card shipped 2026-08-20 gated on `view === VIEWS.CHAIN_EDIT`, which
# is how it came to be a slot-chain-only feature one day after it landed.
#
# The bill must be the SAME six on both, and it is for a structural reason
# rather than a coincidence: the card asks the chain TARGET for the hierarchy,
# the chain_params and each value in the row, and a target answers in one round
# trip whatever the key is spelled like. Master FX addresses everything under
# "master_fx:" at slot 0; that is a longer string, not another read.
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
  import("./src/shared/chain_model.mjs"),
  import("node:fs"),
]).then(([M, V, CM, fs]) => {
  const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
  let failures = 0;
  const fail = (m) => { console.log("FAIL: " + m); failures++; };

  /* Lift a top-level function out of shadow_ui.js and hand it its dependencies
     as parameters. The file cannot be imported -- it is a device UI module full
     of host globals -- but a function that closes over a handful of named
     things can be RUN, which is the difference between pinning a comment and
     pinning a cost. */
  function lift(name, deps) {
    const at = src.indexOf("function " + name + "(");
    const end = at >= 0 ? src.indexOf("\n}\n", at) : -1;
    if (end < 0) { fail(name + " is gone from shadow_ui.js"); process.exit(1); }
    return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
  }

  /* ------------------------------------------------------------ the world */

  /* Two components in one slot chain, with DIFFERENT parameters -- which is the
     only way a card resolved against the wrong one is visible at all. On the
     master chain the same two modules sit at fx1 and fx2. */
  const PARAMS = {
    A: ["cutoff", "reso", "attack", "decay", "release", "gain", "pan", "drive"],
    B: ["size", "damp", "width", "mix", "predly", "lowcut", "hicut", "freeze"],
  };
  const hierOf = (w) => JSON.stringify({ levels: { root: { label: w, knobs: PARAMS[w] } } });
  const cpOf = (w) => JSON.stringify(PARAMS[w].map((k) =>
    ({ key: k, name: k, type: "float", min: 0, max: 1, step: 0.01 })));

  const reads = [];
  const STATE = {};
  /* prefix -> which param set that position holds. The two chains spell their
     keys differently and that is the entire point: the same six reads land on
     "fx1:cutoff" for a slot and "master_fx:fx1:cutoff" for the master bus. */
  const place = (prefix, which) => {
    STATE[prefix + ":ui_hierarchy"] = hierOf(which);
    STATE[prefix + ":chain_params"] = cpOf(which);
    for (const k of PARAMS[which]) STATE[prefix + ":" + k] = "0.5";
  };
  place("synth", "A");  place("fx1", "B");
  place("master_fx:fx1", "A");  place("master_fx:fx2", "B");

  const getSlotParam = (slot, key) => { reads.push(key); return STATE[key] || ""; };

  /* The REAL chain targets and the real key plumbing, lifted rather than
     restated: a target rebuilt here would spell the keys itself, which is
     exactly the drift it exists to end. */
  const chainComponentId = lift("chainComponentId", [])();
  const isChainModuleKey = lift("isChainModuleKey", ["chainComponentId", "parseChainId"])(
    chainComponentId, CM.parseId);
  const chainComponentParamKey = lift("chainComponentParamKey",
    ["isChainModuleKey", "chainComponentId"])(isChainModuleKey, chainComponentId);

  const SLOT_COMPS = [{ key: "synth", label: "Synth" }, { key: "fx1", label: "FX 1" }];
  const slotChainTarget = lift("slotChainTarget",
    ["chainComponentParamKey", "slotChainComponents"])(
    chainComponentParamKey, () => SLOT_COMPS);

  /* MASTER_CHAIN_TARGET is a const object, so it is sliced and evaluated. */
  const mfxCapM = src.match(/^const MASTER_FX_SLOTS = (\d+);/m);
  if (!mfxCapM) { fail("could not read MASTER_FX_SLOTS"); process.exit(1); }
  const mAt = src.indexOf("const MASTER_CHAIN_TARGET = {");
  const MASTER_TARGET = new Function("parseChainId", "MASTER_FX_SLOTS", "MASTER_FX_CHAIN_COMPONENTS",
    src.slice(mAt, src.indexOf("\n};\n", mAt) + 4) + "\nreturn MASTER_CHAIN_TARGET;")(
    CM.parseId, parseInt(mfxCapM[1], 10), null);
  const MASTER_COMPS = [{ key: "fx1", label: "FX 1" }, { key: "fx2", label: "FX 2" },
                        { key: "settings", label: "Settings" }];
  MASTER_TARGET.components = () => MASTER_COMPS;

  const chainTargetGetParam = lift("chainTargetGetParam", ["getSlotParam"])(getSlotParam);
  const chainTargetIsModulePosition = lift("chainTargetIsModulePosition", [])();
  const chainTargetChainParams = lift("chainTargetChainParams", ["chainTargetGetParam"])(chainTargetGetParam);
  const chainTargetHierarchy = lift("chainTargetHierarchy", ["chainTargetGetParam"])(chainTargetGetParam);

  /* ------------------------------------------------------- the card block */

  const marker = src.indexOf("const KNOB_CARD_DECAY_MS");
  const fnAt = src.indexOf("function showKnobFeedback(", marker);
  const end = src.indexOf("\n}\n", fnAt);
  if (marker < 0 || fnAt < 0 || end < 0) {
    fail("the knob card block is gone from shadow_ui.js");
    process.exit(1);
  }
  const body = src.slice(marker, end + 2);

  const deps = ["NUM_KNOBS", "showOverlay", "hideOverlay", "announceParameter",
    "debugLog", "chainEditorFocus", "chainTargetIsModulePosition",
    "chainTargetHierarchy", "chainTargetChainParams", "getKnobContext",
    "buildMetaIndex", "resolveViz", "getSlotParam"];

  /* `here` is what the stubs outside the lift read; chainEditorFocus is the ONE
     seam the whole feature now hangs off, so it is what the test drives. */
  const here = { target: null, comps: null, comp: 0, which: "A" };
  const TARGETS = {
    slot:   { target: () => slotChainTarget(0), comps: SLOT_COMPS,
              prefix: ["synth", "fx1"], which: ["A", "B"] },
    master: { target: () => MASTER_TARGET, comps: MASTER_COMPS,
              prefix: ["master_fx:fx1", "master_fx:fx2"], which: ["A", "B"] },
  };

  function build(kind, component) {
    const T = TARGETS[kind];
    reads.length = 0;
    here.target = T.target();
    here.comps = T.comps;
    here.comp = component;
    here.which = T.which[component] || "A";
    const chainEditorFocus = () => (here.target === null ? null : {
      target: here.target,
      comp: (here.comp >= 0 && here.comps[here.comp]) ? here.comps[here.comp] : null,
    });
    const fn = new Function(...deps, body +
      "\nreturn { knobCardOpen, knobCardClose, showKnobFeedback," +
      " state: () => ({ knobCardKnob: knobCardKnob, keys: knobCardKeys," +
      " values: knobCardRowValues, slot: knobCardSlot, comp: knobCardCompKey })," +
      " touch: (i, v) => { knobTouched[i] = v; } };");
    return fn(8, () => {}, () => {}, () => {}, () => {},
      chainEditorFocus, chainTargetIsModulePosition,
      chainTargetHierarchy, chainTargetChainParams,
      /* getKnobContext, as the warm cache answers it: the key of knob i at the
         component the focus currently names. */
      (i) => { const w = here.which; return { key: PARAMS[w][i] }; },
      M.buildMetaIndex, V.resolveViz, getSlotParam);
  }

  /* ================================================================= cases */
  for (const kind of ["slot", "master"]) {
    const T = TARGETS[kind];
    const at = kind + ": ";

    /* ---- 1. a full row: the whole bill, itemised ---- */
    {
      const w = build(kind, 0);
      w.knobCardOpen(1, { target: T.target(), comp: T.comps[0] });
      const EXPECT = 6;
      if (reads.length !== EXPECT)
        fail(at + "raising the card costs " + reads.length + " reads (~" +
             (reads.length * 2.8).toFixed(0) + "ms), not " + EXPECT +
             " -- reads were [" + reads.join(", ") + "]. If this is a deliberate " +
             "change, the itemised comment above knobCardOpen has to change with it");
      /* The touched knob is 1, so the row is knobs 0-3 and nothing else.
         Reading all eight would be invisible in a total that only counts. */
      const p = T.prefix[0];
      const want = ["cutoff", "reso", "attack", "decay"].map((k) => p + ":" + k);
      const values = reads.filter((r) => want.indexOf(r) >= 0);
      if (values.join(",") !== want.join(","))
        fail(at + "the wrong row was read: [" + reads.join(", ") + "]");
      /* And the two whole-component fetches, spelled for THIS chain. */
      for (const suffix of ["ui_hierarchy", "chain_params"])
        if (reads.indexOf(p + ":" + suffix) < 0)
          fail(at + "never fetched " + p + ":" + suffix);
      const st = w.state();
      if (!st.keys || st.keys.length !== 8) fail(at + "the row keys were not resolved");
      if (st.slot !== T.target().slot)
        fail(at + "the card recorded slot " + st.slot + ", not the targets " +
             T.target().slot);
    }

    /* ---- 2. the short card is FREE ---- */
    /* Nothing selected means no component to resolve a row from, so there is
       nothing to read -- and this is the path the global slot knobs take, which
       fires once per tick while a knob is turning. A read here would be a read
       per tick wearing the costume of a one-off. */
    {
      const w = build(kind, -1);
      w.knobCardOpen(3, { target: T.target(), comp: null });
      if (reads.length !== 0)
        fail(at + "the header-only card cost " + reads.length + " reads: " + reads.join(", "));
    }

    /* ---- 2b. a box that is not a module position is free too ---- */
    /* The slot settings box, a "+" box, Master FXs settings box: asking any of
       them for a param would be a real round trip answering "". */
    {
      const NOT_A_MODULE = { slot: [{ key: "settings", label: "Settings" }],
                             master: [{ key: "settings", label: "Settings" }] }[kind];
      const w = build(kind, 0);
      here.comps = NOT_A_MODULE;
      w.knobCardOpen(0, { target: T.target(), comp: NOT_A_MODULE[0] });
      if (reads.length !== 0)
        fail(at + "the settings box cost " + reads.length + " reads: " + reads.join(", "));
      here.comps = T.comps;
    }

    /* ---- 3. turning does not re-read ---- */
    /* The claim that survives the gesture: after the card is up, the turned knob
       moves by local arithmetic and the neighbours hold still. */
    {
      const w = build(kind, 0);
      w.touch(1, true);
      w.showKnobFeedback(1, "Reso", "0.50", 0.5);
      reads.length = 0;
      for (let i = 0; i < 60; i++) w.showKnobFeedback(1, "Reso", "0.5" + (i % 10), 0.5 + i / 1000);
      if (reads.length !== 0)
        fail(at + "60 turns of an open card cost " + reads.length + " reads: " +
             reads.slice(0, 8).join(", "));
      if (w.state().values["reso"] === "0.5")
        fail(at + "the turned value was not updated locally");
    }

    /* ---- 4. what the card is resolved AGAINST is part of its identity ---- */
    /* A budget test alone would bless the bug this is here for: the card is
       resolved against a chain and a component, and the component moves without
       a view change (the jog steps it). Reopening on the knob index alone
       therefore keeps the previous modules keys, labels and meta while writing
       the NEW value in under an OLD key -- a wrong reading, not a stale-looking
       one. Cheap to detect and invisible to a read count, so it gets its own
       assertion rather than a hope. */
    {
      const w = build(kind, 0);
      w.touch(1, true);
      w.showKnobFeedback(1, "Reso", "0.50", 0.5);
      if (w.state().keys[0] !== "cutoff") fail(at + "setup: the first row did not resolve");

      here.comp = 1;
      here.which = T.which[1];
      reads.length = 0;
      w.showKnobFeedback(1, "Damp", "0.20", 0.2);
      const keys = w.state().keys;
      if (!keys || keys[0] !== "size")
        fail(at + "after the selection moved to another component the card still shows [" +
             (keys || []).slice(0, 4).join(", ") + "] -- the row belongs to the module " +
             "that is no longer selected, so the value being turned is labelled with " +
             "another parameters name");
      if (reads.length === 0)
        fail(at + "the card did not re-resolve when the selected component changed");
      /* and it re-resolved against THIS chains spelling of the keys */
      if (!reads.some((r) => r.indexOf(T.prefix[1] + ":") === 0))
        fail(at + "the re-resolve read [" + reads.join(", ") + "], none of it addressed " +
             T.prefix[1]);
    }

    /* ---- 5. no chain editor at all: the centred box, and no reads ---- */
    {
      const w = build(kind, 0);
      here.target = null;
      reads.length = 0;
      w.showKnobFeedback(0, "Cutoff", "0.50", 0.5);
      if (reads.length !== 0)
        fail(at + "a knob turned outside a chain editor cost " + reads.length + " reads");
      if (w.state().knobCardKnob >= 0)
        fail(at + "a knob turned outside a chain editor raised the card anyway");
    }
  }

  /* ---- 6. the two chains cost the SAME ---- */
  /* Stated separately from the per-chain sixes above, because "both are six"
     is the claim and two independent assertions of "this one is six" would
     still pass if one of them were quietly re-baselined. */
  {
    const bill = {};
    for (const kind of ["slot", "master"]) {
      const T = TARGETS[kind];
      const w = build(kind, 0);
      w.knobCardOpen(1, { target: T.target(), comp: T.comps[0] });
      bill[kind] = reads.length;
    }
    if (bill.slot !== bill.master)
      fail("raising the card costs " + bill.slot + " reads on a slot chain and " +
           bill.master + " on Master FX -- one of them is doing something the " +
           "other is not, and the difference has to be stated");
  }

  if (failures) process.exit(1);
  console.log("PASS: raising the knob card costs 6 reads on BOTH chains, turning it " +
              "costs none, and it re-resolves when the selected component moves");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
