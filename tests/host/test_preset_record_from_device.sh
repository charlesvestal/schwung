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

/* what we hand the module, and what a normalising module reports back */
const WROTE = "{\"b\":2,\"a\":1}";
const DEVICE_ANSWER = "{\"a\":1.000,\"b\":2.000}";


/* ---- 1. the record is built from a DEVICE read ------------------------- */

/*
 * BEHAVIOURAL, not a grep. The first version of this test asserted that
 * recordUserPresetFromDevice mentions refreshUserPresetLiveBlob before
 * makeRecord -- and PASSED while refreshUserPresetLiveBlob did not exist at
 * all, because a rewrite had deleted it. A grep proves a call is written, not
 * that it resolves; node --check does not catch it either, since an undefined
 * function is a runtime ReferenceError, not a syntax error. On the device it
 * threw inside onPresetLoaded AFTER the state had been applied, so a preset
 * loaded its values and then never left the browser.
 *
 * So: LIFT the real function and RUN it, with every free identifier supplied.
 * A missing one now throws here instead of on the device.
 */
{
  const at = src.indexOf("function recordUserPresetFromDevice(");
  if (at < 0) fail("recordUserPresetFromDevice is gone");
  else {
    const body = src.slice(at, src.indexOf("\n}\n", at) + 2);
    const calls = [];
    const deps = {
      /* the device answers with a NORMALISED string, not what we wrote */
      getSlotStateWithRetry: (slot, key) => { calls.push(["read", slot, key]); return DEVICE_ANSWER; },
      userPresetLiveBlobCache: Object.create(null),
      userPresetKey: (s2, p2) => s2 + ":" + p2,
      setUserPresetRecord: (s2, p2, rec) => { calls.push(["record", s2, p2, rec]); deps._rec = rec; },
      makeRecord: CP.makeRecord,
      paramPagesRefreshTrailing: () => calls.push(["refresh"]),
      notifyModuleUiPresetsChanged: () => calls.push(["notify"]),
      needsRedraw: false,
    };
    let fn;
    try {
      fn = new Function(...Object.keys(deps), body + "\nreturn recordUserPresetFromDevice;")
             (...Object.values(deps));
    } catch (e) { fail("could not lift recordUserPresetFromDevice: " + e.message); }
    if (fn) {
      try {
        fn(1, "synth", "tst");
        ok("recordUserPresetFromDevice RUNS without throwing (a missing helper would ReferenceError here)");
      } catch (e) {
        fail("recordUserPresetFromDevice THREW: " + e.message +
             " -- on device this aborts onPresetLoaded after the state was applied, " +
             "so the preset loads its values and never leaves the browser");
      }
      const read = calls.find((c) => c[0] === "read");
      if (read && read[2] === "synth:state") ok("it reads <prefix>:state back from the device");
      else fail("it did not read the state back from the device: " + JSON.stringify(calls));
      const rec = deps._rec;
      if (rec && !CP.isModified(rec, DEVICE_ANSWER)) {
        ok("the record it builds matches what the DEVICE reports -- the star clears");
      } else {
        fail("the record does not match the device answer, so the star would stay stuck");
      }
      if (rec && CP.isModified(rec, WROTE)) {
        ok("and it did NOT hash the blob we wrote (which the module normalised away)");
      } else {
        fail("the record matches the string we wrote -- that is the stuck-star bug");
      }
      if (deps.userPresetLiveBlobCache["1:synth"] !== DEVICE_ANSWER) {
        fail("the header cache was not refreshed, so the header mark would lag");
      }
      if (!calls.some((c) => c[0] === "refresh")) fail("the trailing rows were not refreshed");
    }
  }
}

/* Both hooks must go through it rather than hashing stateJson themselves. */
for (const hook of ["onUserPresetLoaded", "onUserPresetSaved"]) {
  const at = src.indexOf("function " + hook + "(");
  if (at < 0) { fail(hook + " is gone"); continue; }
  const h = src.slice(at, src.indexOf("\n}\n", at) + 2);
  if (h.includes("recordUserPresetFromDevice")) ok(hook + " routes through the shared helper");
  else fail(hook + " builds its own record -- it will hash the blob we wrote");
  if (/makeRecord\(\s*name\s*,\s*stateJson\s*\)/.test(h)) {
    fail(hook + " still hashes stateJson (the string we WROTE) -- the stuck-star bug");
  }
}

/* Behavioural: a NORMALISING module is the whole point. Write one string, have
   the device answer with a different-but-equivalent one, and prove the star is
   clear afterwards. Hashing the written string fails this; hashing the read
   string passes. */
{
  const written = WROTE, reported = DEVICE_ANSWER;

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
