#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The Master FX param-pages header showed "MFX > /D" for every module.
#
# Two independent defects, and the fix is both:
#
#   A. the chrome asked for "master_fx:fxN:module", which serves the plugin
#      PATH. The module ID is served under ":name".
#   B. getModuleAbbrev's fallback is the first two characters of whatever it is
#      handed, so a path became "/D" -- a confident, plausible-looking label
#      rather than a visible failure.
#
# B is the one worth a test. A wrong KEY was already survivable: an unserved
# read comes back as "" and the header simply loses its name. This key was
# served with the wrong KIND of value, and the fallback dressed it up as an
# answer. Same shape as the param-read tri-state lesson -- a bad value believed
# because it parsed.
#
# Note the trap inside the fix: a module path names the plugin FILE
# (".../cloudseed/dsp.so"), so a plain basename yields "dsp.so" and an
# abbreviation of "DS" for every module in the fleet. That is the same bug in
# different letters, so it is asserted explicitly.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

file="src/shadow/shadow_ui.js"

# ---- A: the chrome must ask for the key that serves an ID -------------------
chrome=$(awk '/^function paramPagesChromeFor\(/,/^}/' "$file")
if [ -z "$chrome" ]; then
  echo "FAIL: could not find paramPagesChromeFor in $file" >&2
  exit 1
fi
if ! grep -q 'masterFxComponentKey(mfx), "name"' <<<"$chrome"; then
  echo "FAIL: the Master FX chrome does not ask for the \":name\" key." >&2
  echo "      \":module\" serves the plugin PATH, which is what produced \"MFX > /D\"." >&2
  exit 1
fi
if grep -q 'masterFxComponentKey(mfx), "module"' <<<"$chrome"; then
  echo "FAIL: the Master FX chrome is back on the \":module\" (path) key" >&2
  exit 1
fi
echo "  ok  Master FX chrome asks for :name (module id), not :module (path)"

# ---- B: an abbreviation must never be a slice of a path ---------------------
node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

/* Lift the function out rather than importing the file: shadow_ui.js imports
 * by absolute on-device paths and cannot be loaded off the Move.
 *
 * ONE function, with no free identifiers of its own -- which is why the path
 * handling is inline in the source rather than a helper. Six tests lift
 * getModuleAbbrev this way, each with its own dependency list, and a helper
 * made all of them throw ReferenceError. */
const grab = (name) => {
  const re = new RegExp("^function " + name + "[(][^]*?^}", "m");
  const m = src.match(re);
  if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
  return m[0];
};
const mk = (cache) => new Function("moduleAbbrevCache",
  grab("getModuleAbbrev") + "\nreturn getModuleAbbrev;")(cache);

const PATH = "/data/UserData/schwung/modules/audio_fx/cloudseed/dsp.so";

/* With the module cached, a path must resolve to its real abbreviation. */
{
  const abbrev = mk({ cloudseed: "CS" });
  const got = abbrev(PATH);
  if (got !== "CS")
    fail("a path with a cached module resolved to " + JSON.stringify(got) + ", expected \"CS\"");
}

/* With nothing cached, the fallback must still be derived from the module,
 * never from the path -- and never from the plugin FILENAME. */
{
  const abbrev = mk({});
  const got = abbrev(PATH);
  if (/[\/\\]/.test(got))
    fail("the abbreviation " + JSON.stringify(got) + " contains a path separator");
  if (got === "DS")
    fail("the abbreviation is \"DS\" -- taken from \"dsp.so\", the plugin FILE. " +
         "The module id is the directory holding it, so every module would " +
         "share this label: the original bug in different letters");
  if (got !== "CL")
    fail("expected \"CL\" from the module directory, got " + JSON.stringify(got));
}

/* A plain id is unaffected -- slot chains already worked and must keep working. */
{
  const abbrev = mk({ cloudseed: "CS" });
  if (abbrev("cloudseed") !== "CS") fail("a cached plain id stopped resolving");
  if (abbrev("granny") !== "GR") fail("an uncached plain id stopped falling back");
}

/* Degenerate values must not produce a path fragment or throw. */
for (const bad of ["", null, undefined, "/", "///", "/dsp.so"]) {
  const got = mk({})(bad);
  if (typeof got !== "string" || !got.length)
    fail("input " + JSON.stringify(bad) + " produced " + JSON.stringify(got));
  if (/[\/\\]/.test(got))
    fail("input " + JSON.stringify(bad) + " produced " + JSON.stringify(got) +
         ", which contains a path separator");
}

console.log("  ok  a path resolves through the cache, and never abbreviates to a path slice");
console.log("  ok  the plugin FILENAME is not mistaken for the module id");
console.log("  ok  plain ids (the slot-chain path that already worked) are unchanged");
console.log("PASS: the Master FX header names its module");
'
