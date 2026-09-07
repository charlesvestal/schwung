#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Which level the knob parameter picker enters, RUN rather than grepped.
#
# "root" is a convention, not a guarantee. A mode-based module names its top
# levels after its modes and has no `root` at all, and asking the hierarchy for
# a level that does not exist returns nothing -- so the picker showed an EMPTY
# parameter list for every such module. Found on hardware: a mode-based synth
# offered nothing while a root-based one beside it was fine.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const check = (c, m) => c ? console.log("  ok  " + m) : fail(m);

function lift(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return ""; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("no end for " + name); return ""; }
  return src.slice(at, end + 2);
}
const body = lift("findFirstParamLevel");
if (failures) process.exit(1);
const findFirstParamLevel = new Function(body + "\nreturn findFirstParamLevel;")();

/* Root-based, with a preset browser in front of the params: unchanged
   behaviour, and the reason the walk exists at all. */
const rootBased = { modes: null, levels: {
  root: { list_param: "preset", count_param: "preset_count", children: "main" },
  main: { params: [{ key: "cutoff" }] }
}};
check(findFirstParamLevel(rootBased) === "main",
      "a preset browser at root is skipped, as it always was");

const flatRoot = { levels: { root: { params: [{ key: "gain" }] } } };
check(findFirstParamLevel(flatRoot) === "root", "a root that holds params is entered directly");

/* Mode-based, no root at all -- the case that showed an empty picker. Its
   entry is the first declared mode, and the walk then follows two preset
   browsers down to the parameters. */
const modeBased = { modes: ["patch", "performance", "system"], mode_param: "mode", levels: {
  patch:      { list_param: "bank_list", children: "patch_list", params: [] },
  performance:{ list_param: "bank_list", children: "perf_list", params: [] },
  patch_list: { list_param: "patch", children: "patch_main", params: [] },
  perf_list:  { list_param: "performance", children: "perf_main", params: [] },
  patch_main: { params: [{ key: "cutoff" }, { key: "resonance" }] },
  system:     { params: [] }
}};
check(findFirstParamLevel(modeBased) === "patch_main",
      "a mode-based module lands on its parameters, not on a level that does not exist");

/* No root and no modes: take the first level there is rather than a name
   nothing declared. */
const noRootNoModes = { levels: { alpha: { params: [{ key: "a" }] }, beta: { params: [] } } };
check(findFirstParamLevel(noRootNoModes) === "alpha",
      "with neither root nor modes, the first declared level is entered");

/* Degenerate inputs answer something harmless rather than throwing. */
check(findFirstParamLevel(null) === "root", "a missing hierarchy answers root");
check(findFirstParamLevel({}) === "root", "a hierarchy with no levels answers root");

/* The safety net: even if the entered level yields nothing, the picker falls
   back to the all-levels scan instead of showing an empty list. */
check(/knobParamPickerParams\.length === 0/.test(src) &&
      /knobParamPickerParams = getKnobParamsForTarget\(knobEditorSlot, selected\.id\);/.test(src),
      "an empty level falls through to the all-levels scan rather than showing nothing");

if (failures) { console.error("\n" + failures + " check(s) failed"); process.exit(1); }
console.log("\nPASS: the knob picker enters a level that exists");
'
