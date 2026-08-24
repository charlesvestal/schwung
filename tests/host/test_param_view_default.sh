#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Param View defaults to the KNOB GRID (1), not the hierarchy list (0).
#
# The grid shipped opt-in because it could not draw everything the list could.
# That gap is closed -- mode selectors, child levels and enum pickers all
# landed, and the fleet contract fixture was recaptured (76 modules -> 95) so
# that "the grid covers the fleet" is a measurement rather than a hope.
#
# The half that is easy to break silently is the MIGRATION. param_view.json is
# written only when the user toggles the setting, so:
#
#   * a device that never touched it has NO file and must pick up the new
#     default -- if the setting were saved on every boot instead, every
#     existing install would be pinned to the old default forever and the flip
#     would appear to do nothing;
#   * a device where the user explicitly chose List has a file saying 0, and
#     that choice must still win.
#
# Both halves are asserted here because both are one line away from wrong, and
# neither is visible on a developer device that has already toggled the setting
# at some point.

fail() { echo "FAIL: $*" >&2; exit 1; }

file="src/shadow/shadow_ui.js"

# ---- the default itself -----------------------------------------------------
decl=$(command grep '^let paramViewGlobal = ' "$file" || true)
[ -n "$decl" ] || fail "could not find the paramViewGlobal declaration in $file"
if [ "$decl" != "let paramViewGlobal = 1;" ]; then
  fail "Param View no longer defaults to the knob grid: $decl
      0 is the hierarchy list, which is the pre-recapture opt-in default."
fi
echo "  ok  Param View defaults to the knob grid (1)"

# ---- the config is written on TOGGLE, never unconditionally ------------------
#
# Asserted as a count rather than by inspecting the call site: one call means
# the toggle. A second call anywhere -- init, a load path, an autosave -- is how
# every existing install would get pinned to whatever it booted with.
calls=$(command grep -c 'saveParamViewConfig();' "$file" || true)
if [ "$calls" != "1" ]; then
  fail "saveParamViewConfig() is called $calls times, expected exactly 1 (the
      toggle). Writing the file anywhere else pins every existing device to the
      value it happened to boot with, and the default can never change again."
fi
echo "  ok  the config is written only by the toggle, so an untouched device follows the default"

# ---- an explicit choice still wins ------------------------------------------
if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

const grab = (name) => {
  const re = new RegExp("^function " + name + "[(][^]*?^}", "m");
  const m = src.match(re);
  if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
  return m[0];
};

/* Lift the loader and drive it with a fake file. shadow_ui.js imports by
 * absolute on-device paths and cannot be loaded off the Move. */
const run = (fileContents) => {
  let paramViewGlobal = 1;               /* the new default */
  const host_read_file = () => fileContents;
  const PARAM_VIEW_CONFIG_PATH = "/param_view.json";
  const fn = new Function("host_read_file", "PARAM_VIEW_CONFIG_PATH", "startValue",
    "let paramViewGlobal = startValue;\n" +
    grab("loadParamViewConfig") +
    "\nloadParamViewConfig();\nreturn paramViewGlobal;");
  return fn(host_read_file, PARAM_VIEW_CONFIG_PATH, paramViewGlobal);
};

/* No file: a device that never touched the setting follows the default. */
if (run(null) !== 1)
  fail("a device with no param_view.json did not follow the new default");
if (run("") !== 1)
  fail("an empty param_view.json did not follow the new default");

/* An explicit List choice survives the flip -- this is somebody who tried the
 * grid and went back, and the release must not silently overrule them. */
if (run(JSON.stringify({ param_view: 0 })) !== 0)
  fail("an explicitly saved List choice was overruled by the new default");

/* An explicit Knobs choice is a no-op, and unparseable JSON must not throw. */
if (run(JSON.stringify({ param_view: 1 })) !== 1)
  fail("an explicitly saved Knobs choice did not survive");
if (run("{not json") !== 1)
  fail("a corrupt config threw or changed the value instead of being ignored");

console.log("  ok  no file follows the default; an explicit List choice still wins");
console.log("PASS: Param View defaults to the knob grid without overruling anybody");
'
