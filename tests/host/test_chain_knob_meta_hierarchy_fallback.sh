#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Where a knob gets a parameter's TYPE from, when chain_params does not have it.
#
# Reported from the device 2026-08-21, against `impressive-chords`: "i could see
# the values change, but when i release, it reset to the default". The turn was
# real -- the overlay moves by local JS arithmetic -- and the write was real
# too. What was wrong was the NUMBER being written.
#
# The module declares all fifteen of its parameters inline in its
# ui_hierarchy's root level (`{key, name, type: "int", min: -24, max: 24}`) and
# ships no chain_params.json, so its DSP answers `<prefix>:chain_params` with
# the two characters "[]" -- and the chain host's fallback, which renders the
# CORRECT metadata parsed out of module.json, is skipped because the plugin
# "answered" (`result > 0`).
#
# So chain_params is empty. buildChainKnobContext resolved meta from
# chain_params ALONE, `find` returned undefined, and knobConfigFromMeta(meta)
# invents a float 0..1 step 0.01 for anything it is not told about. Turning
# `transpose` (int, -24..24) then wrote "0.058750", which the module's atoi
# reads as 0; turning `retrig` (enum) wrote the same string, whose first
# character is '0', selecting option 0 -- the default. The screen moved, the
# DSP did not, and the next read snapped it back.
#
# The hierarchy is the same object that NAMED the knob, and it is carrying the
# metadata one level up from where the knob row was read. The list editor has
# always merged it (getParamMetadata: `{...hierarchyMeta, ...chainMeta}`); the
# knob path did not, which is exactly why the same parameter is editable from
# the menu and not from the knob. This pins the merge, on both chains.
#
# PRE-EXISTING, not a regression: the pre-merge builder at 5e0a7537 did the
# same chain_params-only lookup.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob metadata fallback test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/chain_model.mjs"),
  import("./src/shared/knob_engine.mjs"),
  import("node:fs"),
]).then(([CM, KE, fs]) => {
  const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
  let failures = 0;
  const fail = (m) => { console.log("FAIL: " + m); failures++; };

  function lift(name, deps) {
    const at = src.indexOf("function " + name + "(");
    const end = at >= 0 ? src.indexOf("\n}\n", at) : -1;
    if (end < 0) { fail(name + " is gone from shadow_ui.js"); process.exit(1); }
    return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
  }

  /* ---------------------------------------------------------------- world */

  /* A module shaped like the ones that hit this: every parameter declared
     INLINE in the hierarchy level that also names the knob row, and a DSP that
     answers chain_params with an empty array rather than declining. */
  const LEVEL_PARAMS = [
    { key: "transpose", name: "Transpose", type: "int", min: -24, max: 24, step: 1 },
    { key: "base_note", name: "Base Note", type: "int", min: 0, max: 127, step: 1 },
    { key: "timing", name: "Timing", type: "enum", options: ["Straight", "Dotted", "Triplet"] },
  ];
  const HIER = JSON.stringify({
    levels: { root: { name: "Inline", params: LEVEL_PARAMS,
                      knobs: ["transpose", "base_note", "timing"] } }
  });

  /* `answer` decides what the position serves for chain_params. */
  function world(prefix, answer) {
    const STATE = {};
    STATE[prefix + ":ui_hierarchy"] = HIER;
    STATE[prefix + ":chain_params"] = answer;
    return (slot, key) => (key in STATE ? STATE[key] : "");
  }

  function builder(getSlotParam) {
    const chainComponentId = lift("chainComponentId", [])();
    const isChainModuleKey = lift("isChainModuleKey",
      ["chainComponentId", "parseChainId"])(chainComponentId, CM.parseId);
    const chainComponentParamKey = lift("chainComponentParamKey",
      ["isChainModuleKey", "chainComponentId"])(isChainModuleKey, chainComponentId);
    const chainTargetGetParam = lift("chainTargetGetParam", ["getSlotParam"])(getSlotParam);
    const chainTargetChainParams = lift("chainTargetChainParams",
      ["chainTargetGetParam"])(chainTargetGetParam);
    const chainTargetHierarchy = lift("chainTargetHierarchy",
      ["chainTargetGetParam"])(chainTargetGetParam);
    const knobLevelForHierarchy = lift("knobLevelForHierarchy", [])();
    const hierarchyLevelParamMeta = lift("hierarchyLevelParamMeta", [])();
    /* normalizeExpandedParamMeta is identity here: it resolves dynamic pickers
       and note/rate/canvas shapes, none of which this is about, and stubbing it
       keeps the assertion on WHICH object was chosen. */
    return lift("buildChainKnobContext",
      ["chainTargetHierarchy", "knobLevelForHierarchy", "chainTargetChainParams",
       "hierarchyLevelParamMeta", "normalizeExpandedParamMeta", "debugLog"])(
      chainTargetHierarchy, knobLevelForHierarchy, chainTargetChainParams,
      hierarchyLevelParamMeta, (key, meta) => meta, () => {});
    }

  const TARGETS = {
    slot: { prefix: "midi_fx1", comp: { key: "midiFx", label: "MIDI FX" },
            target: (get) => ({ slot: 0, label: "S1",
              key: (c, s) => keyer(c, s) }) },
    master: { prefix: "master_fx:fx1", comp: { key: "fx1", label: "FX 1" },
              target: () => ({ slot: 0, label: "MFX",
                key: (c, s) => (CM.parseId(c) ? "master_fx:" + c + ":" + s : null) }) },
  };
  /* The slot chain spells its keys through the real chainComponentParamKey. */
  let keyer = null;
  {
    const chainComponentId = lift("chainComponentId", [])();
    const isChainModuleKey = lift("isChainModuleKey",
      ["chainComponentId", "parseChainId"])(chainComponentId, CM.parseId);
    keyer = lift("chainComponentParamKey",
      ["isChainModuleKey", "chainComponentId"])(isChainModuleKey, chainComponentId);
  }

  /* ================================================================ cases */

  for (const kind of ["slot", "master"]) {
    const T = TARGETS[kind];
    const at = kind + ": ";

    /* ---- 1. chain_params empty: the hierarchy is the metadata ---- */
    for (const answer of ["[]", "", "   "]) {
      const get = world(T.prefix, answer);
      const build = builder(get);
      const target = T.target(get);

      const c0 = build(target, T.comp, 0, "mod", true);
      if (!c0.meta || c0.meta.type !== "int")
        fail(at + "with chain_params " + JSON.stringify(answer) + ", knob 1 got meta " +
             JSON.stringify(c0.meta) + " for an int declared in the hierarchy level " +
             "that named the knob. knobConfigFromMeta turns that into a float 0..1 " +
             "step 0.01, so the turn writes a fraction and the module reads it as 0");
      if (c0.meta && (c0.meta.min !== -24 || c0.meta.max !== 24))
        fail(at + "knob 1 range is " + c0.meta.min + ".." + c0.meta.max + ", not -24..24");
      if (c0.displayName !== "Transpose")
        fail(at + "knob 1 is labelled " + JSON.stringify(c0.displayName) +
             ", not the declared name");

      const c2 = build(target, T.comp, 2, "mod", true);
      if (!c2.meta || c2.meta.type !== "enum" || !Array.isArray(c2.meta.options) ||
          c2.meta.options.length !== 3)
        fail(at + "with chain_params " + JSON.stringify(answer) + ", knob 3 got meta " +
             JSON.stringify(c2.meta) + " for an enum declared in the hierarchy. " +
             "Without options the knob drives it as a float and the written string " +
             "selects option 0 -- the default the user sees it snap back to");
    }

    /* ---- 2. chain_params still WINS where it has an answer ---- */
    /* The host renders chain_params from the same module.json in the ordinary
       case, but a plugin that serves its own is serving something it computed
       at runtime (a dynamic max, a live options list). The hierarchy is a
       fallback underneath it, never an override. */
    {
      const dynamic = JSON.stringify([
        { key: "transpose", name: "Transpose", type: "int", min: -7, max: 7, step: 1 },
      ]);
      const build = builder(world(T.prefix, dynamic));
      const c0 = build(T.target(), T.comp, 0, "mod", true);
      if (!c0.meta || c0.meta.min !== -7 || c0.meta.max !== 7)
        fail(at + "chain_params lost to the hierarchy: knob 1 range is " +
             (c0.meta && c0.meta.min) + ".." + (c0.meta && c0.meta.max) + ", not -7..7");
      /* and a key chain_params does not carry still falls through */
      const c2 = build(T.target(), T.comp, 2, "mod", true);
      if (!c2.meta || c2.meta.type !== "enum")
        fail(at + "a partial chain_params suppressed the hierarchy fallback for the " +
             "keys it does not mention");
    }
  }

  /* ---- 3. the consequence, stated in the units the user saw ---- */
  /* Not a restatement of case 1: this runs the real knob engine and shows what
     the missing metadata actually WRITES, which is the thing that was reported
     and the thing a future "simplification" of the merge would break. */
  {
    const build = builder(world("midi_fx1", "[]"));
    const target = { slot: 0, label: "S1", key: (c, s) => keyer(c, s) };
    const ctx = build(target, TARGETS.slot.comp, 0, "mod", true);
    const cfg = KE.knobConfigFromMeta(ctx.meta);
    const st = KE.knobInit(0);
    let v = 0;
    for (let i = 0; i < 40; i++) v = KE.knobTick(st, cfg, 1, Date.now() + i * 100);
    if (parseInt(String(v), 10) === 0 && v !== 0)
      fail("40 detents on transpose produced " + v + ", which atoi() reads as 0 -- " +
           "the knob moves on screen and the DSP never changes. cfg was " +
           JSON.stringify(cfg));
    if (cfg.max === 1 && cfg.min === 0)
      fail("transpose is being driven as a 0..1 float; the metadata merge is not " +
           "reaching knobConfigFromMeta");
  }

  if (failures) process.exit(1);
  console.log("PASS: a knob takes its type from the hierarchy level that named it when " +
              "chain_params has nothing to say, on BOTH chains, and chain_params still wins");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
