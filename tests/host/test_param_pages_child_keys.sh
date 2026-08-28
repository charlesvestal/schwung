#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Addressing repeated elements from declared metadata
# (src/shared/param_pages/child_key.mjs).
#
# Why this exists: fleet-wide, the largest source of unreachable params is
# modules that declare 200+ concrete per-instance keys (p01_vol, p02_vol, …)
# and list only an alias in their hierarchy — mrdrums 209, weird-dreams 187,
# forge 106. Schwung already documents child_prefix/child_count for exactly
# this, but assumes one fixed key shape that none of them use, so six modules
# solved it again in a third-party config file.
#
# These tests pin the smallest extension that lets them stop — an explicit key
# template, index base, zero-padding, per-key overrides — and, critically, that
# the legacy child_prefix form keeps producing byte-identical keys.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the child-key tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/child_key.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/validate_contract.mjs"),
  import("node:fs"),
]).then(([CK, P, V, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));

  /* ---- 1. the legacy form is unchanged, byte for byte ------------------- */
  {
    /* docs/MODULES.md: keys become <child_prefix><index>_<key>, zero-based.
     * minijv is the one fleet module using it, so a regression here breaks a
     * shipping module. */
    const lvl = { child_prefix: "nvram_tone_", child_count: 4, knobs: ["cutofffrequency"] };
    if (CK.resolveChildKey(lvl, 0, "cutofffrequency") !== "nvram_tone_0_cutofffrequency") {
      fail("legacy child_prefix changed shape: " + CK.resolveChildKey(lvl, 0, "cutofffrequency"));
    }
    if (CK.resolveChildKey(lvl, 3, "level") !== "nvram_tone_3_level") fail("legacy index is zero-based");
    if (CK.childCount(lvl) !== 4) fail("child_count not read");

    const jv = fx.modules.find((m) => m.id === "minijv");
    const part = jv.ui_hierarchy.levels.part_selector;
    if (!CK.hasChildren(part)) fail("minijv part_selector should still be recognised");
    if (CK.resolveChildKey(part, 0, "x") !== "sram_part_0_x") fail("minijv key shape changed");
  }

  /* ---- 2. template, index base and zero padding ------------------------- */
  {
    const pads = {
      child_count: 16, child_label: "Pad",
      child_key_template: "p{index}_{key}", child_index_base: 1, child_index_digits: 2,
      knobs: ["vol", "pan"],
    };
    if (CK.resolveChildKey(pads, 0, "vol") !== "p01_vol") fail("pad 1 should be p01_vol, got " + CK.resolveChildKey(pads, 0, "vol"));
    if (CK.resolveChildKey(pads, 15, "vol") !== "p16_vol") fail("pad 16 should be p16_vol");
    if (CK.childLabel(pads, 0) !== "Pad 1") fail("labels should honour the index base, got " + CK.childLabel(pads, 0));
    if (CK.childLabel(pads, 15) !== "Pad 16") fail("last label wrong");

    /* Without padding or a base, the same template is plain. */
    const plain = { child_count: 4, child_key_template: "v{index}_{key}", knobs: ["a"] };
    if (CK.resolveChildKey(plain, 0, "a") !== "v0_a") fail("default base should be 0 and unpadded");
  }

  /* ---- 3. per-key overrides ------------------------------------------- */
  {
    /* forge: sends and pan use v{pad}_ while everything else uses pv{pad}_.
     * Without overrides the whole module needs a config file for two keys. */
    const lvl = {
      child_count: 8, child_key_template: "pv{index}_{key}", child_index_base: 1,
      child_key_overrides: { fx1: "v{index}_{key}" },
      knobs: ["vol", "fx1"],
    };
    if (CK.resolveChildKey(lvl, 0, "vol") !== "pv1_vol") fail("main template not applied");
    if (CK.resolveChildKey(lvl, 0, "fx1") !== "v1_fx1") fail("override not applied, got " + CK.resolveChildKey(lvl, 0, "fx1"));
  }

  /* ---- 4. a level with no children answers cleanly --------------------- */
  {
    const plain = { knobs: ["a", "b"] };
    if (CK.hasChildren(plain)) fail("a plain level must not look like a child level");
    if (CK.resolveChildKey(plain, 0, "a") !== null) fail("a plain level should resolve to null, not a mangled key");
    if (CK.childCount(plain) !== 0) fail("a plain level has no instances");
    if (CK.childKeysFor(plain, 0).length) fail("a plain level addresses no child keys");
    /* Declared template but no count is malformed, not a child level. */
    if (CK.hasChildren({ child_key_template: "p{index}_{key}" })) fail("a template with no count is not usable");
  }

  /* ---- 5. every instance is enumerable --------------------------------- */
  {
    const pads = {
      child_count: 16, child_key_template: "p{index}_{key}",
      child_index_base: 1, child_index_digits: 2,
      knobs: ["vol", "pan", "tune"], params: [{ key: "decay" }, { level: "elsewhere" }],
    };
    const one = CK.childKeysFor(pads, 0);
    if (one.length !== 4) fail("instance 0 should address 4 keys, got " + one.length + ": " + one.join(","));
    if (one.includes("p01_elsewhere")) fail("a nav entry is not a param");
    const all = CK.allChildKeys(pads);
    if (all.size !== 64) fail("16 pads x 4 keys should be 64 addressable keys, got " + all.size);
    if (!all.has("p16_decay")) fail("the last instance is missing");
  }

  /* ---- 6. the planner emits a selector carrying the level -------------- */
  {
    const hierarchy = {
      levels: {
        root: { knobs: ["gain"], params: [{ level: "pads", label: "Pads" }] },
        pads: {
          child_count: 16, child_label: "Pad", child_key_template: "p{index}_{key}",
          child_index_base: 1, child_index_digits: 2, knobs: ["vol", "pan"],
        },
      },
    };
    const { pages } = P.planPages({ hierarchy, chainParams: [{ key: "gain" }] });
    /* The selector is an ITEMS page carrying childOf -- it is the same
       gesture (a list, a cursor, a current marker, click to choose) and reuses
       that machinery rather than being a second page kind with its own state,
       jog handler and renderer. */
    const sel = pages.find((p) => p.kind === P.PAGE_ITEMS && p.childOf);
    if (!sel) fail("no child selector page emitted");
    if (sel.childCount !== 16) fail("selector lost its count");
    if (!sel.childLevel) fail("the selector must carry its level so keys can be resolved without re-reading");
    if (CK.resolveChildKey(sel.childLevel, 2, "vol") !== "p03_vol") fail("keys not resolvable from the page");
  }

  /* ---- 7. the validator stops calling covered keys unreachable ---------- */
  {
    /*
     * Measured BOTH WAYS from the shipping contract, not against a pinned 209.
     *
     * This used to read mrdrums straight out of the fixture as the "before"
     * case and assert it began at exactly 209 unreachable params, because at
     * the time mrdrums declared no child keys. It ships them now (the
     * 2026-08-28 recapture has it at 14), so the pinned number failed -- on
     * the module getting FIXED, which is the one outcome a test guarding a fix
     * should never punish.
     *
     * The invariant was never the number. It is that expressing per-pad
     * scoping through child keys turns a wall of unreachable concrete keys
     * into addressable ones. So derive the unextended case by STRIPPING the
     * child declarations, and compare the two. That holds whatever mrdrums
     * ships next, and it keeps working when some other module becomes the
     * interesting one.
     */
    const shipped = JSON.parse(JSON.stringify(fx.modules.find((m) => m.id === "mrdrums")));
    const unreachable = (mod) => {
      const f = V.validateContract({
        id: "mrdrums", hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params,
      }).findings.find((x) => x.rule === "unreachable-params");
      return f ? parseInt(f.message, 10) : 0;
    };

    const stripped = JSON.parse(JSON.stringify(shipped));
    for (const lv of Object.values(stripped.ui_hierarchy.levels || {}))
      for (const k of Object.keys(lv)) if (k.startsWith("child_")) delete lv[k];

    const before = unreachable(stripped);
    const after = unreachable(shipped);

    /* Without child keys the per-pad params are simply not addressable: 16
     * pads worth of concrete keys listed in no level. 200 is a floor, not a
     * pin -- it must not become the next 209. */
    if (before < 200)
      fail("stripping child declarations should leave the per-pad keys unreachable, got " + before);
    if (after > 20)
      fail("with child keys declared only module-level globals should remain unreachable, got " + after);
    if (!(after * 10 < before))
      fail("child keys should make an order of magnitude more params addressable: " +
           before + " -> " + after);
  }

  /* ---- 8. malformed declarations are reported, not guessed -------------- */
  {
    const rules = (levels) =>
      new Set(V.validateContract({ id: "t", hierarchy: { levels }, chainParams: [{ key: "a" }] })
        .findings.map((f) => f.rule));

    if (!rules({ root: { knobs: ["a"], child_key_template: "p{index}_{key}" } }).has("child-without-count")) {
      fail("a template with no count should be reported");
    }
    if (!rules({ root: { knobs: ["a"], child_count: 4, child_key_overrides: { a: "x{index}" } } })
        .has("child-overrides-without-template")) {
      fail("overrides with nothing to override should be reported");
    }
  }

  console.log("PASS: child keys — legacy child_prefix byte-identical, templates with base/padding/overrides, " +
              "mrdrums per-pad params become addressable when child keys are declared");
});
'
