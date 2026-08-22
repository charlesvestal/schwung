#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# What the param-pages header says it is looking at.
#
# Two reports from the device, both about the same line of the screen:
#
#   1. "MFX shows CP or CS, not Capicola or Cloudseed as I'd expect."
#      An abbreviation is a placeholder for a name that has not arrived. On
#      Master FX, where a module often has no presets, it was the PERMANENT
#      answer.
#
#   2. "airwindows shows the preset name, but it only updates after going to
#      the knobs, not on preset change."
#      s.presetName was refreshed only by the knob page's read rotation, and
#      the preset-browser branch returns before it -- so scrolling a browser
#      changed the sound while the header kept naming the preset you started
#      on. It matters most exactly where it was worst: for airwindows the
#      browser IS the module identity, 519 effects behind one module id.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

# ---- 1: the header asks for a NAME, not an abbreviation ---------------------
pp="src/shadow/shadow_ui_param_pages.mjs"
blk=$(awk '/_abbrevCache = ctx.getModuleDisplayName/,/currentComponent.toUpperCase\(\)/' "$pp")
if [ -z "$blk" ]; then
  echo "FAIL: the header no longer resolves a module display name in $pp" >&2
  echo "      It would fall back to the two-letter abbreviation forever." >&2
  exit 1
fi
# The abbreviation must survive as the FALLBACK: module.json is read lazily, and
# a header that blanks while waiting is worse than one that is briefly terse.
if ! grep -q "getModuleAbbrev" <<<"$blk"; then
  echo "FAIL: the abbreviation is gone as a fallback -- the header would blank" >&2
  echo "      until module.json has been parsed" >&2
  exit 1
fi
grep -q "moduleNameCache\[id.toLowerCase()\] || getModuleAbbrev(id)" src/shadow/shadow_ui.js || {
  echo "FAIL: getModuleDisplayName does not fall back to the abbreviation" >&2; exit 1; }

# The lookup is useless if nothing FILLS the cache, and a lookup-only check
# passes happily while the header shows abbreviations forever -- that mutation
# survived the first version of this test.
cma=$(awk '/^function cacheModuleAbbrev\(/,/^}/' src/shadow/shadow_ui.js)
if ! grep -q "moduleNameCache\[json.id.toLowerCase()\] = json.name" <<<"$cma"; then
  echo "FAIL: cacheModuleAbbrev does not record json.name, so moduleNameCache" >&2
  echo "      is never populated and every header stays an abbreviation" >&2
  exit 1
fi
echo "  ok  header resolves a display name, with the abbreviation as fallback"
echo "  ok  the name cache is actually populated from module.json"

# ---- 2: the browser keeps the header current -------------------------------
node -e '
import("./src/shared/param_pages/page_controller.mjs").then((C) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* Modelled on airwindows: the browser names the module itself, and it
     spells its name param `plugin_name`, NOT `preset_name` -- which is why
     the fix keys off the name the PAGE declares rather than a fixed key. */
  const HIER = JSON.stringify({ modes: null, levels: { root: {
      label: "CLAP", list_param: "plugin_index", count_param: "plugin_count",
      name_param: "plugin_name", knobs: ["a"], params: [{ key: "a" }] } } });
  const CP = JSON.stringify([{ key: "a", name: "A", type: "float", min: 0, max: 1, step: 0.01 }]);
  const names = ["Air", "Bass", "Chorus"];
  let idx = 0;

  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      if (b === "plugin_count") return "3";
      if (b === "plugin_index") return String(idx);
      if (b === "plugin_name" || b === "preset_name") return names[idx];
      return "0";
    },
    setParam: (k, v) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "plugin_index") idx = parseInt(v, 10) || 0;
    },
    announce: () => {},
  });
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 12; i++) ctl.tick();

  const pi = ctl.pages.findIndex((p) => p.kind === "preset");
  if (pi < 0) fail("the fixture produced no preset browser page");

  /* enterIfDoor, because a browser is a door: paging PAST one must not
     audition every preset it goes by. Entering is the gesture that arms it. */
  ctl.goToPage(pi, { enterIfDoor: true });
  for (let i = 0; i < 9; i++) ctl.tick();
  if (ctl.presetName !== "Air")
    fail("on entering the browser the header said " + JSON.stringify(ctl.presetName));

  /* Scroll. The module moves; the header must move WITH it, without ever
     visiting a knob page. */
  for (const want of ["Bass", "Chorus"]) {
    ctl.onJog(1);
    for (let i = 0; i < 9; i++) ctl.tick();
    if (names[idx] !== want) fail("fixture desync: module at " + names[idx] + ", wanted " + want);
    if (ctl.presetName !== want)
      fail("the module is on \"" + want + "\" but the header still says " +
           JSON.stringify(ctl.presetName) + " -- it only catches up on the knob page");
  }

  console.log("  ok  scrolling a browser updates the header, with no extra reads");
  console.log("PASS: the header names what you are actually looking at");
});
'
