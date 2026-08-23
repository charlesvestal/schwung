#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Metadata resolution for the param-page library
# (src/shared/param_pages/param_meta.mjs), checked against the 76-module fleet
# capture. The contract these tests pin:
#
#  - precedence matches the list editor's getParamMetadata:
#    { ...inlineHierarchyMeta, ...chainParamsMeta }. A param must not read
#    differently depending on whether you are in the list or on the grid.
#  - inline-only metadata survives (impressive-chords declares NO chain_params
#    at all; 330 inline defaults / 268 min-max / 116 options across the fleet).
#  - the library guesses a range only for keys declared nowhere, and never
#    guesses structure.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the param-page metadata tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("node:fs"),
]).then(([M, P, fs]) => {
  const { buildMetaIndex, inferFromValue, isTurnable, KIND_NUMBER, KIND_ENUM, KIND_OPAQUE } = M;
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const idxFor = (id) => {
    const mod = fx.modules.find((x) => x.id === id);
    if (!mod) fail("fixture has no module \"" + id + "\"");
    return { mod, idx: buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params }) };
  };

  /* ---- 1. chain_params wins over inline on shared fields ---------------- */
  {
    const meta = { type: "float", min: 0, max: 1 };
    const index = buildMetaIndex({
      hierarchy: { levels: { root: { knobs: ["x"], params: [{ key: "x", label: "Inline", min: -5, max: 5 }] } } },
      chainParams: [{ key: "x", name: "Chain", ...meta }],
    });
    const m = index.get("x");
    if (m.min !== 0 || m.max !== 1) fail("chain_params range should win over inline, got " + m.min + ".." + m.max);
    if (m.name !== "Chain") fail("chain_params name should win");
    /* label is inline-only (chain_params spells it `name`), so it survives. */
    if (m.label !== "Inline") fail("inline label should survive, got " + m.label);
  }

  /* ---- 2. inline-only metadata is honoured ------------------------------ */
  {
    /* This case used to be anchored on impressive-chords, which the fixture
     * recorded as publishing no chain_params at all. That was never true of
     * the module -- it is the literal "[]" that a failed read was believed to
     * be, the bug the param-read tri-state exists to prevent. A recapture
     * brings back 15 real params, so the fleet no longer has an instance of
     * "hierarchy metadata with no chain_params" to point at.
     *
     * Synthetic, therefore. The behaviour under test is a property of
     * buildMetaIndex, not of any particular module, and anchoring it to a
     * module made it hostage to that module\u0027s contract -- and, worse, kept a
     * known-bad capture alive as if it were a fact. */
    const idx = buildMetaIndex({
      hierarchy: { levels: { root: {
        knobs: ["mode", "amount"],
        params: [
          { key: "mode", label: "Mode", options: ["Off", "On"] },
          { key: "amount", label: "Amount", min: 0, max: 12 },
        ],
      } } },
      chainParams: [],
    });
    const withMeta = idx.keys.filter((k) => { const m = idx.get(k); return m && (m.options || m.max !== null); });
    if (withMeta.length === 0) fail("inline-only contract resolved no metadata");
    for (const k of idx.keys) {
      const m = idx.get(k);
      if (!m) fail("inline-only key \"" + k + "\" resolved to nothing");
      if (m.guessed) fail("inline-only key \"" + k + "\" was guessed despite inline metadata");
    }
  }

  /* ---- 3. only genuinely undeclared keys are guessed --------------------- */
  {
    const guessed = [];
    for (const mod of fx.modules) {
      const idx = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const r = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      for (const p of r.pages) for (const k of (p.keys || [])) {
        if (idx.getOrGuess(k).guessed) guessed.push(mod.id + ":" + k);
      }
    }
    /* sfz knob_0..7 and clap param_6..7 — generic slots the module labels at
     * runtime. If this grows, a module regressed its declaration. */
    if (guessed.length > 12) fail("guessed metadata spread to " + guessed.length + " keys: " + guessed.slice(0, 8).join(","));
    for (const g of guessed) {
      if (!/^(sfz|clap):/.test(g)) fail("unexpected guessed key " + g + " — module should declare it");
    }
  }

  /* ---- 4. classification drives whether a knob can turn it -------------- */
  {
    const { idx } = idxFor("mrsample");
    const wav = idx.get("loop_start");
    /* A RANGED wav_position is a number a knob can turn that ALSO opens a
   * waveform editor — divable and turnable are different questions. Both
   * wav_position params in the fleet declare min/max/step. */
  if (wav.kind !== KIND_NUMBER) fail("a ranged wav_position must classify number, got " + wav.kind);
  if (!wav.divable) fail("a ranged wav_position must still be divable");
  if (!isTurnable(wav)) fail("a ranged wav_position must be turnable");
  
    const { idx: pnp } = idxFor("pushnpull");
    if (pnp.get("view").kind !== KIND_OPAQUE) fail("canvas must classify opaque");
  if (!pnp.get("view").divable) fail("canvas must be divable");

    const { idx: bb } = idxFor("breakbeat");
    if (bb.get("A_sample_path").kind !== KIND_OPAQUE) fail("filepath must classify opaque");
  if (!bb.get("A_sample_path").divable) fail("filepath must be divable");
  }

  /* ---- 5. ui_type is an alias for type ---------------------------------- */
  {
    const index = buildMetaIndex({
      hierarchy: null,
      chainParams: [{ key: "p", type: "float", ui_type: "wav_position", min: 0, max: 1 }],
    });
    if (index.get("p").kind !== KIND_NUMBER) fail("ui_type:wav_position with a range must classify number even when type says float");
  if (!index.get("p").divable) fail("ui_type:wav_position must be divable");
  }

  /* ---- 6. toggle becomes a two-option enum ------------------------------ */
  {
    const index = buildMetaIndex({
      hierarchy: { levels: { root: { knobs: ["t"], params: [{ key: "t", label: "Rev", type: "toggle" }] } } },
      chainParams: [],
    });
    const m = index.get("t");
    if (m.kind !== KIND_ENUM) fail("toggle should classify as enum, got " + m.kind);
    if (!Array.isArray(m.options) || m.options.length !== 2) fail("toggle should get two options");
    if (m.max !== 1) fail("toggle max should be 1, got " + m.max);
  }

  /* ---- 7. enum ranges follow the option list ---------------------------- */
  {
    const { idx } = idxFor("arp");
    const mode = idx.get("mode");
    if (mode.kind !== KIND_ENUM) fail("arp mode should be enum");
    if (mode.max !== mode.options.length - 1) fail("enum max must be options.length-1");
    if (mode.step !== 1) fail("enum step must be 1");
  }

  /* ---- 8. a declared range is never overridden -------------------------- */
  {
    const { idx } = idxFor("obxd");
    const c = idx.get("cutoff");
    if (c.min !== 0 || c.max !== 100) fail("obxd cutoff range was rewritten to " + c.min + ".." + c.max);
    const v = idx.get("voice_count");
    if (v.min !== 1 || v.max !== 8) fail("obxd voice_count range was rewritten");
  }

  /* ---- 9. inference repairs a guess exactly once ------------------------ */
  {
    const g = { key: "x", type: "float", min: 0, max: 1, step: 0.01, kind: KIND_NUMBER, guessed: true };
    if (inferFromValue(g, "0.4") !== null) fail("a plausible float should keep the guess");
    if (inferFromValue(g, "") !== null) fail("an empty read should keep the guess");
    if (inferFromValue(g, "nope") !== null) fail("an unparseable read should keep the guess");

    const up = inferFromValue(g, "48");
    if (!up || up.type !== "int" || up.min !== 0 || up.max !== 64) fail("48 should infer int 0..64, got " + JSON.stringify(up));

    const bip = inferFromValue(g, "-12");
    if (!bip || bip.min !== -12 || bip.max !== 12) fail("-12 should infer a symmetric bipolar range, got " + JSON.stringify(bip));

    if (inferFromValue({ ...g, guessed: false }, "48") !== null) fail("inference must not touch declared metadata");
  }

  /* ---- 10. no module declares conflicting inline metadata --------------- */
  {
    const bad = [];
    for (const mod of fx.modules) {
      const idx = buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      if (idx.conflicts.length) bad.push(mod.id + " (" + idx.conflicts.join(",") + ")");
    }
    if (bad.length) fail("levels disagree about a key inline metadata: " + bad.join("; "));
  }

  console.log("PASS: param metadata resolution — precedence, inline survival, classification, bounded guessing");
});
'
