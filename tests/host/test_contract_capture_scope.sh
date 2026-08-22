#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The fleet contract capture reported success and captured NOTHING.
#
# Two defects, one cause. shadow_ui.js is an ES module and gets `os` from
# `import * as os from 'os'` -- a MODULE-scoped binding, not a property of
# globalThis. The trigger loaded the capture tool with `(0, eval)(src)`, and
# indirect eval evaluates in GLOBAL scope, so the tool's `os.readdir` calls
# threw ReferenceError.
#
# That alone would have been a visible failure. What made it invisible is the
# second defect: the tool caught each category scan's exception with a bare
# `continue`, which cannot tell "no modules of this kind" from "os is not
# defined". It wrote module_count 0 in 18ms and logged a success line, and the
# recapture was believed to have run twice.
#
# Both halves are pinned, because either one alone lets this recur silently.

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- A: the hook must not evaluate the tool in global scope -----------------
hook_file="src/shadow/shadow_ui.js"

# The CALL form, not the mention -- the fix's own comment names `(0, eval)` to
# explain why it is wrong, and a bare substring match fails on that comment.
if command grep -q '(0, *eval)(' "$hook_file"; then
  fail "$hook_file uses indirect eval. It runs in GLOBAL scope, where the
      module's \`os\` / \`std\` imports do not exist -- the capture tool's
      fleet scan throws ReferenceError and captures zero modules."
fi

if ! command grep -q 'new Function("os", "std", src)(os, std)' "$hook_file"; then
  fail "the contract-capture hook no longer passes os/std explicitly into the
      evaluated tool. The tool enumerates the fleet with os.readdir and cannot
      reach a module-scoped import any other way."
fi
echo "  ok  the capture tool is evaluated with os/std passed in, not in global scope"

# ---- A2: the trigger must be REMOVED, not emptied ---------------------------
#
# contractDumpDone only latches for the life of the process, so a trigger that
# is emptied but left in place re-fires the entire loud capture on the next
# service restart -- which is what every deploy does. Observed on hardware:
# install.sh restarted the shadow UI and the capture ran again unbidden.
if ! command grep -q 'os.remove("/data/UserData/schwung/dump_contracts_trigger")' "$hook_file"; then
  fail "the contract-capture trigger is not removed. Emptying it is not enough:
      host_file_exists is still true, and the next service restart re-runs the
      capture."
fi
if command grep -q 'host_write_file("/data/UserData/schwung/dump_contracts_trigger"' "$hook_file"; then
  fail "the trigger is written back after being removed, which re-arms it"
fi
echo "  ok  the trigger is deleted, so a restart cannot re-fire the capture"

# ---- B: the tool must refuse to write an empty capture ----------------------
if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
const fs = require("fs");
let src = fs.readFileSync("tools/param-pages/dump_contracts_device.js", "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

/* The confirm window is 8s of BUSY SPIN per attempt on the device. Shortened
 * here so the negative case is testable at all -- the substitution is asserted
 * to have happened, so a rename of the constant fails loudly instead of
 * quietly leaving a 16-second test. */
const before = src;
src = src.replace(/var LOAD_CONFIRM_MS = \d+;/, "var LOAD_CONFIRM_MS = 200;");
if (src === before) fail("LOAD_CONFIRM_MS is gone -- the confirm-by-readback loop was renamed or removed");

/* Evaluate the tool the way the fixed hook does: os/std as parameters. The
 * point of the test is what happens when that scan cannot see a fleet. */
const load = (osStub, getParam, setParam) => {
  const g = {};
  const writes = [];
  const fn = new Function("os", "std", "globalThis", "host_read_file",
                          "host_write_file", "shadow_get_param",
                          "shadow_set_param", "print", src);
  fn(osStub, {}, g,
     () => null,
     (p, v) => writes.push([p, v]),
     getParam || (() => ""), setParam || (() => true), () => {});
  return { run: g.dumpModuleContracts, writes };
};
const oneModule = { readdir: (p) => p.endsWith("sound_generators") ? [["braids"], 0] : [[], 0] };

/* 1. The exact production failure: `os` present but readdir throwing, which is
 *    what a missing binding looked like from inside the catch. */
{
  const { run, writes } = load({ readdir: () => { throw new ReferenceError("os is not defined"); } });
  let threw = null;
  try { run(); } catch (e) { threw = e; }
  if (!threw)
    fail("a fleet scan that threw on every category still completed. It wrote " +
         writes.length + " file(s); an empty capture would have overwritten the " +
         "fixture source with module_count 0.");
  if (writes.length)
    fail("the tool wrote " + JSON.stringify(writes[0][0]) + " despite scanning nothing");
  if (!/os is not defined/.test(String(threw.message)))
    fail("the failure did not carry the underlying scan error, so the next " +
         "reader sees \"found no modules\" and not the real cause: " + threw.message);
  console.log("  ok  a failed scan throws, names the underlying error, and writes nothing");
}

/* 2. A genuinely empty tree is still a refusal -- there is always a fleet, so
 *    zero is never a fact about the device. */
{
  const { run, writes } = load({ readdir: () => [[], 0] });
  let threw = null;
  try { run(); } catch (e) { threw = e; }
  if (!threw) fail("an empty module tree produced a capture instead of a refusal");
  if (writes.length) fail("an empty module tree was written to disk");
  console.log("  ok  an empty fleet is refused rather than captured");
}

/* 3. A real fleet still captures -- the guard must not be a blanket refusal. */
{
  const { run, writes } = load(oneModule);
  const out = run();
  if (!writes.length) fail("a non-empty fleet wrote nothing");
  const doc = JSON.parse(writes[0][1]);
  if (doc.module_count !== 1)
    fail("expected module_count 1 from a one-module fleet, got " + doc.module_count);
  if (!Array.isArray(doc.scan_errors))
    fail("the capture does not record scan_errors, so a partial scan reads as complete");
  if (typeof out !== "string") fail("the tool no longer returns its output path");
  console.log("  ok  a real fleet captures, and records scan_errors alongside it");
}

/* ---- C: a refused WRITE is not a verdict --------------------------------
 *
 * shadow_set_param returning false means the parameter channel refused or
 * timed out; it does NOT mean the module failed to load. The first hardware
 * run recorded 50 of 96 modules as "load failed", and they were exactly the
 * heavy ones -- surge, osirus, minijv, cloudseed -- because those load slowly
 * enough to miss the channel timeout. The capture then looked complete while
 * silently dropping the complicated half of the fleet.
 *
 * Same shape as the param-read tri-state: never let a failed transaction
 * produce a verdict. */
{
  const { run, writes } = load(oneModule, (s, k) => k.endsWith(":chain_params") ? "[{}]" : "",
                               () => false);
  run();
  const doc = JSON.parse(writes[0][1]);
  if (doc.modules[0].status !== "ok")
    fail("a module whose set_param returned FALSE but whose contract came back " +
         "was recorded as " + JSON.stringify(doc.modules[0].status) + ". The return " +
         "value of the write is not the verdict -- what the module serves is.");
  console.log("  ok  a refused write is confirmed against the contract, not believed");
}

/* ---- D: the confirm must watch a key that is actually SERVED -------------
 *
 * The first version of the confirm read `<comp>:module` back. Nothing serves a
 * GET for it, so it errored every time: on hardware every module loaded twice,
 * LOAD_CONFIRM_MS apart, and the run was heading for a fleet marked entirely
 * load-failed. A confirm against an unreadable key is worse than no confirm --
 * it turns every success into a slow failure.
 *
 * Pinned as a behaviour, not as a string match on the key: with ONLY
 * chain_params served, the capture must still succeed. */
{
  const { run, writes } = load(oneModule,
    (s, k) => k.endsWith(":chain_params") ? "[{}]" : null, () => true);
  run();
  const doc = JSON.parse(writes[0][1]);
  if (doc.modules[0].status !== "ok")
    fail("with chain_params served and everything else erroring, the module was " +
         "recorded as " + JSON.stringify(doc.modules[0].status) + " -- the confirm " +
         "is keyed on something the device does not answer");
  console.log("  ok  the confirm keys on a served value, not on <comp>:module");
}

/* ---- E: an unconfirmed load is captured, not dropped ---------------------
 *
 * Two modules in a row whose chain_params are byte-identical look unconfirmed,
 * and so does a module whose contract is genuinely empty. Skipping those would
 * repeat the exact bias the confirm was added to fix: silently missing the
 * awkward cases. They are captured and LABELLED instead. */
{
  const two = { readdir: (p) => p.endsWith("sound_generators") ? [["a", "b"], 0] : [[], 0] };
  const { run, writes } = load(two, (s, k) => k.endsWith(":chain_params") ? "[{}]" : "", () => true);
  run();
  const doc = JSON.parse(writes[0][1]);
  if (doc.modules.length !== 2)
    fail("expected both modules in the capture, got " + doc.modules.length);
  if (doc.modules[0].status !== "ok")
    fail("the first module should confirm, got " + JSON.stringify(doc.modules[0].status));
  if (doc.modules[1].status !== "unconfirmed")
    fail("a second module with identical chain_params was recorded as " +
         JSON.stringify(doc.modules[1].status) + ", expected \"unconfirmed\"");
  if (!doc.modules[1].chain_params)
    fail("the unconfirmed module was captured without its contract -- dropping it " +
         "is the bias the confirm exists to prevent");
  console.log("  ok  an unconfirmed load is captured and labelled, not skipped");
}
{
  /* ...and a component that serves NOTHING is still a failure. The guard must
   * not have become "always ok". */
  const { run, writes } = load(oneModule, () => "", () => true);
  run();
  const doc = JSON.parse(writes[0][1]);
  if (doc.modules[0].status !== "load-failed")
    fail("a module that served nothing at all was recorded as " +
         JSON.stringify(doc.modules[0].status) + " -- the confirm is not confirming");
  console.log("  ok  a module that serves nothing is still a failure");
}

console.log("PASS: the contract capture cannot silently capture nothing");
'
