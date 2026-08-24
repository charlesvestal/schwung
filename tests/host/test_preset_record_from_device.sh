#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Two fixes reported from hardware, both about arriving somewhere and finding
# it in the wrong state.
#
# 1. THE RECORD IS HASHED FROM WHAT THE DEVICE REPORTS, NOT FROM WHAT WE WROTE.
#
#    "loading the loaded preset did reset values, did not clear the *,
#     including when i loaded another patch and then back"
#
#    A module may normalise state on the way in -- reorder keys, reformat
#    floats, fill in defaults it was not given -- so the string it reports back
#    is not the string we handed it. Hashing what we WROTE therefore produces a
#    record that can never match a later read: the star is stuck on, and the
#    only gesture that appears to fix it (press Save) bakes the normalised form
#    in. Read it back and hash that.
#
# 2. A RESTORED PAGE THAT IS A DOOR OPENS.
#
#    "load -> no presets saved -> back, should have me still in the menu, not
#     having to activate it"
#
#    Same rule as goToPage enterIfDoor: an arrival you asked for opens; one you
#    paged past stays shut. Entering writes nothing -- a browser auditions on
#    TURN -- so this hands over the jog without loading anything.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import fs from "node:fs";
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => { console.error("ok: " + m); };

const CP = await import(process.cwd() + "/src/shared/param_pages/current_preset.mjs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");

/* ---- 1. the record is built from a DEVICE read ------------------------- */

/* Structural: both hooks must route through the one helper, and that helper
   must refresh the cache BEFORE it builds the record. */
const body = (() => {
  const at = src.indexOf("function recordUserPresetFromDevice(");
  if (at < 0) return null;
  return src.slice(at, src.indexOf("\n}\n", at) + 2);
})();
if (!body) {
  fail("recordUserPresetFromDevice is gone -- the record would be hashed from the blob we wrote");
} else {
  const refreshAt = body.indexOf("refreshUserPresetLiveBlob");
  const recordAt = body.indexOf("makeRecord");
  if (refreshAt >= 0 && recordAt >= 0 && refreshAt < recordAt) {
    ok("the live blob is re-read from the device BEFORE the record is built");
  } else {
    fail("recordUserPresetFromDevice does not re-read the device before hashing");
  }
  if (!body.includes("cachedUserPresetBlob")) {
    fail("the record is not built from the refreshed cache");
  }
}
for (const hook of ["onUserPresetLoaded", "onUserPresetSaved"]) {
  const at = src.indexOf("function " + hook + "(");
  if (at < 0) { fail(hook + " is gone"); continue; }
  const h = src.slice(at, src.indexOf("\n}\n", at) + 2);
  if (h.includes("recordUserPresetFromDevice")) ok(hook + " routes through the shared helper");
  else fail(hook + " builds its own record -- it will hash the blob we wrote, not the device answer");
  if (/makeRecord\(\s*name\s*,\s*stateJson\s*\)/.test(h)) {
    fail(hook + " still hashes stateJson (the string we WROTE) -- this is the stuck-star bug");
  }
}

/* Behavioural: a NORMALISING module is the whole point. Write one string, have
   the device answer with a different-but-equivalent one, and prove the star is
   clear afterwards. Hashing the written string fails this; hashing the read
   string passes. */
{
  const written  = "{\"b\":2,\"a\":1}";          /* what we hand the module */
  const reported = "{\"a\":1.000,\"b\":2.000}";  /* what it reports back */

  const fromWritten = CP.makeRecord("tst", written);
  if (!CP.isModified(fromWritten, reported)) {
    fail("test is not exercising normalisation -- the two strings must hash differently");
  } else {
    ok("a normalising module really does report a different string than it was given");
  }

  const fromDevice = CP.makeRecord("tst", reported);
  if (CP.isModified(fromDevice, reported)) {
    fail("hashing the DEVICE answer still reports modified -- the star would stay stuck");
  } else {
    ok("hashing the device answer clears the star immediately after a Load");
  }
  if (CP.presetRowValue(fromDevice, reported) !== "tst") {
    fail("row should read the bare name after a Load, got " +
         JSON.stringify(CP.presetRowValue(fromDevice, reported)));
  }
  /* And a real edit after the load must still be caught. */
  if (!CP.isModified(fromDevice, "{\"a\":9.000,\"b\":2.000}")) {
    fail("a genuine edit after a Load is no longer detected");
  } else {
    ok("a genuine edit after the Load is still detected");
  }
}

/* ---- 2. a restored page that is a door opens --------------------------- */
{
  const pc = fs.readFileSync("src/shared/param_pages/page_controller.mjs", "utf8");
  const at = pc.indexOf("function applyPendingRestore(");
  const fn = at >= 0 ? pc.slice(at, pc.indexOf("\n    }", at)) : "";
  if (!fn) fail("applyPendingRestore is gone");
  else if (/isDoor\(page\(\)\)/.test(fn) && /enterMenu\(\)/.test(fn)) {
    ok("applyPendingRestore opens the door it restores onto");
  } else {
    fail("applyPendingRestore restores the page but leaves the door shut -- " +
         "coming Back from an empty browser would need a second click");
  }
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
