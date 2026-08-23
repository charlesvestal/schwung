#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Contract validator (src/shared/param_pages/validate_contract.mjs).
#
# Two things are pinned, and the second matters as much as the first:
#
#  1. the rules FIRE on the fleet modules that genuinely trip them
#  2. they do NOT fire on legitimate patterns. A validator that cries wolf
#     stops being read, so the known false-positive traps are regression-
#     tested: a preset index declares an empty range on purpose (its size
#     comes from count_param at runtime), and a preset/items key is reached
#     through its own page kind rather than through knobs[]/params[].

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the contract validator tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/validate_contract.mjs"),
  import("node:fs"),
]).then(([V, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const check = (id) => {
    const m = fx.modules.find((x) => x.id === id);
    if (!m) fail("fixture has no module \"" + id + "\"");
    return V.validateContract({ id, hierarchy: m.ui_hierarchy, chainParams: m.chain_params }).findings;
  };
  const rules = (id) => new Set(check(id).map((f) => f.rule));

  /* ---- 1. rules fire where they should --------------------------------- */
  {
    if (!rules("forge").has("undrawable-text")) fail("forge ships \"Copy A→B\"; undrawable-text should fire");
    if (!rules("sfz").has("undrawable-text")) fail("sfz ships the cents sign; undrawable-text should fire");
    if (!rules("branchage").has("no-hierarchy")) fail("branchage publishes no ui_hierarchy");
    if (!rules("sfz").has("undeclared-knob-params")) fail("sfz puts knob_0..7 on knobs with no metadata");
    if (!rules("mrdrums").has("unreachable-params")) fail("mrdrums declares 209 per-pad keys no level lists");
    if (!rules("breakbeat").has("level-over-eight")) fail("breakbeat declares a 17-knob level");
  }

  /* ---- 2. the known false positives stay dead --------------------------- */
  {
    /* A preset index whose size comes from count_param at runtime declares an
     * empty range (0..-1) ON PURPOSE. That must be info, never a warning.
     *
     * Synthetic rather than fleet-anchored. This used to name hush1 and sf2,
     * and a recapture found both now declaring 0..9999 -- the modules moved,
     * and the test failed for a reason that had nothing to do with the
     * validator. No module in the fleet declares max < min any more, so
     * anchoring the case to one would just wait to break again. */
    {
      const runtimeRanged = V.validateContract({
        id: "synthetic-runtime-ranged",
        hierarchy: { levels: { root: {
          list_param: "preset", count_param: "preset_count", name_param: "preset_name",
          knobs: ["preset"],
        } } },
        chainParams: [{ key: "preset", name: "Preset", type: "int", min: 0, max: -1 }],
      }).findings;
      const r = new Set(runtimeRanged.map((f) => f.rule));
      if (r.has("empty-range"))
        fail("a runtime-sized preset index must not be reported as an empty range");
      if (!r.has("runtime-ranged"))
        fail("the runtime-sized index should still be noted as info");
    }
    /* the dexed "preset" is reached through the preset page, not through a list. */
    const dexed = check("dexed").filter((f) => f.rule === "unreachable-params");
    if (dexed.some((f) => /\bpreset\b/.test(f.message))) {
      fail("dexed: a preset index reached through its own page kind is not unreachable");
    }
    /* A clean module should produce nothing at all. */
    const clean = check("chord");
    if (clean.length) fail("chord is a clean module but reported: " + clean.map((f) => f.rule).join(", "));
  }

  /* ---- 3. synthetic cases for rules the fleet does not cover --------- */
  {
    const one = (hierarchy, chainParams) =>
      new Set(V.validateContract({ id: "t", hierarchy, chainParams }).findings.map((f) => f.rule));

    if (!one(null, []).has("nothing-declared")) fail("a module declaring nothing should be an error");

    const dup = one({ levels: { root: { knobs: ["a"] } } }, [{ key: "a" }, { key: "a" }]);
    if (!dup.has("duplicate-param")) fail("a duplicated chain_params key should be reported");

    const en = one({ levels: { root: { knobs: ["e"] } } }, [{ key: "e", type: "enum" }]);
    if (!en.has("enum-without-options")) fail("an enum with no options should be reported");

    const bad = one({ levels: { root: { knobs: ["x"] } } }, [{ key: "x", type: "wobble" }]);
    if (!bad.has("unknown-type")) fail("an unknown type should be reported");

    const rng = one({ levels: { root: { knobs: ["x"] } } }, [{ key: "x", type: "float", min: 1, max: 0 }]);
    if (!rng.has("empty-range")) fail("a genuinely inverted range should be reported");

    const pl = one({ levels: { root: { knobs: [], list_param: "p" } } }, [{ key: "p" }]);
    if (!pl.has("preset-without-count")) fail("list_param without count_param should be reported");

    const it = one({ levels: { root: { knobs: [], items_param: "i" } } }, [{ key: "i" }]);
    if (!it.has("items-without-select")) fail("items_param without select_param should be reported");

    const ch = one({ levels: { root: { knobs: ["a"], child_prefix: "p" } } }, [{ key: "a" }]);
    if (!ch.has("child-without-count")) fail("child_prefix without child_count should be reported");
  }

  /* ---- 4. no module is in an unrenderable state ------------------------- */
  {
    const errors = [];
    for (const m of fx.modules) {
      const f = V.validateContract({ id: m.id, hierarchy: m.ui_hierarchy, chainParams: m.chain_params })
        .findings.filter((x) => x.level === "error");
      if (f.length) errors.push(m.id + ": " + f[0].message);
    }
    if (errors.length) fail("modules that cannot render at all: " + errors.join("; "));
  }

  const { reports } = V.validateFleet(fx.modules);
  console.log("PASS: contract validator — rules fire on " + reports.length + "/" + fx.modules.length +
              " fleet modules, known false positives stay dead, no module is unrenderable");
});
'
