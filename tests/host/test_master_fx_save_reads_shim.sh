#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The master FX saver must persist what the SHIM has loaded, not what its own
# in-file mirror happens to remember — and it must not act on a read that did
# not complete.
#
# masterFxConfig only learns about a position when something in shadow_ui.js
# puts it there, so a module loaded by writing `master_fx:fxN:module` to the
# shim directly was invisible to the saver: an overtake tool (Movy), or any
# Remote UI client, since schwung-manager's handleSetMasterFxParam forwards
# whatever key it is handed. The saver wrote "{}" over a loaded position and the
# master chain was gone on the next boot. Two Discord reports, PR #221.
#
# This DRIVES the real function rather than grepping for it. The previous
# version of this file was five `rg -q` source pins in tests/shadow — a
# directory CI does not run — so deleting the fix would not have failed
# anything, anywhere. The lift below hands saveMasterFxChainConfig a fixed
# dependency list, so a fix that quietly stops being called throws instead of
# passing.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the master FX save tests" >&2
  exit 1
fi

node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };
const check = (name, got, want) => {
  if (JSON.stringify(got) !== JSON.stringify(want))
    fail(`${name}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
  else console.log("ok: " + name);
};

function body(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return src.slice(at, end + 2);
}
function lift(name, deps, extra) {
  const b = body(name);
  if (!b) return null;
  return new Function(...deps, (extra || "") + b + "\nreturn " + name + ";");
}

const MASTER_FX_SLOTS = 8;
const CONTRACT_SETTLE_MS = 500;

/*
 * A world with a shim that answers however the case wants, and a filesystem
 * that remembers what was written. `answers(key)` returns a string (served),
 * "" (served, nothing there) or null (the read did not complete) — the three
 * answers a param read has.
 */
function world(answers, mirror, opts) {
  opts = opts || {};
  const files = Object.assign({}, opts.files || {});
  const writes = [];
  const reads = [];
  const caches = [{}, {}, {}];
  const masterFxConfig = {};
  for (let i = 1; i <= MASTER_FX_SLOTS; i++) {
    masterFxConfig[`fx${i}`] = { module: mirror[`fx${i}`] || "" };
    /* Seeded for EVERY position, not just the ones the mirror knows about.
       Seeding only the known ones left the reported case — mirror empty, shim
       loaded — with nothing to invalidate, so the check passed with the
       invalidation deleted. */
    for (const c of caches) c[`master:fx${i}`] = "stale";
  }
  const shadow_get_param = (slot, key) => { reads.push(key); return answers(key); };

  const shimValue = lift("masterFxShimValue", ["shadow_get_param"])(shadow_get_param);
  const shimSnapshot = lift("masterFxShimSnapshot", ["shadow_get_param"])(shadow_get_param);
  const shimSlot = lift("masterFxShimSlot", ["masterFxShimValue"])(shimValue);

  /* needsRedraw is ASSIGNED by the adopt, so it cannot be a parameter — a
   * parameter assignment is invisible to the caller. Declared inside the lift
   * and read back through an accessor instead. */
  const adoptPair = new Function(
    "masterFxConfig", "debugLog", "fxDisplayNameCache", "fxDisplayNameSkip",
    "fxDisplayNameBackoff",
    "let needsRedraw = false;" + body("adoptMasterFxShimModule") +
    "\nreturn { adopt: adoptMasterFxShimModule, redrew: () => needsRedraw };")(
      masterFxConfig, () => {}, caches[0], caches[1], caches[2]);

  const masterFxModuleWriteAt = Object.assign({}, opts.writeAt || {});

  const save = lift("saveMasterFxChainConfig", [
    "activeSlotStateDir", "adoptMasterFxShimModule", "cachedLatencyCompEnabled",
    "cachedLinkAudioPublish", "cachedLinkAudioRouting", "cachedMasterFxMidiChannel",
    "cachedResampleBridgeMode", "cachedUsbcOutPersist", "CONTRACT_SETTLE_MS",
    "currentMasterPresetName", "debugLog", "getMasterFxChainParams",
    "host_read_file", "host_write_file", "MASTER_FX_OPTIONS", "MASTER_FX_SLOTS",
    "masterFxConfig", "masterFxModuleWriteAt", "masterFxShimSlot",
    "masterFxShimSnapshot", "overlay_knobs_get_mode", "shadow_get_param",
    "tts_get_debounce",
  ])(
    "/state", adoptPair.adopt, 0, 0, 0, 0, 0, 0, CONTRACT_SETTLE_MS, "",
    () => {}, () => [], (p) => files[p], (p, v) => { files[p] = v; writes.push(p); },
    opts.options || [], MASTER_FX_SLOTS, masterFxConfig, masterFxModuleWriteAt,
    shimSlot, shimSnapshot, undefined, shadow_get_param, undefined);

  return { save, files, writes, reads, masterFxConfig, caches, adoptPair };
}

/* The shim as it answers when clap is loaded in position 1 and nothing else. */
const CLAP = "/data/UserData/schwung/modules/audio_fx/clap/dsp.so";
const snapshotWith = (entries) => JSON.stringify(
  Array.from({ length: MASTER_FX_SLOTS },
             (_, i) => entries[i] || { id: "", path: "" }));

const loadedAnswers = (key) => {
  if (key === "master_fx:modules") return snapshotWith([{ id: "clap", path: CLAP }]);
  if (key === "master_fx:fx1:state") return "{\"a\":1}";
  if (key.startsWith("master_fx:lfo")) return "";
  return "";
};

console.log("== a module loaded through the shim is adopted and persisted ==");
{
  /* The reported bug: the mirror never heard about it, so the save wrote "{}"
     over a position the shim genuinely had loaded. */
  const w = world(loadedAnswers, {}, { files: { "/state/master_fx_0.json": "OLD" } });
  w.save();
  const wrote = JSON.parse(w.files["/state/master_fx_0.json"]);
  check("the position is adopted into the mirror", w.masterFxConfig.fx1.module, "clap");
  check("module_id is persisted", wrote.module_id, "clap");
  check("module_path is persisted", wrote.module_path, CLAP);
  check("the adopted position display-name caches were invalidated",
        w.caches.map((c) => c["master:fx1"]), [undefined, undefined, undefined]);
  check("and a position that did NOT change keeps its cache",
        w.caches.map((c) => c["master:fx2"]), ["stale", "stale", "stale"]);
}

console.log("== ONE read for the whole chain ==");
{
  /* The per-position form was 8 `:name` reads plus a `:module` each, on the
     autosave frame. If a change reintroduces them this count moves. */
  const w = world(loadedAnswers, {});
  w.save();
  const perPosition = w.reads.filter((k) => /^master_fx:fx\d+:(name|module)$/.test(k));
  check("no per-position name/module reads", perPosition, []);
  check("exactly one snapshot read",
        w.reads.filter((k) => k === "master_fx:modules").length, 1);
}

console.log("== a failed snapshot does not empty anything ==");
{
  /* null is "the read did not complete", NOT "the chain is empty". Adopting it
     would erase every loaded position at once. */
  const w = world((key) => (key.startsWith("master_fx:lfo") ? "" : null),
                  { fx1: "clap" },
                  { files: { "/state/master_fx_0.json": "GOOD" } });
  w.save();
  check("the mirror stands", w.masterFxConfig.fx1.module, "clap");
  check("the good state file is preserved", w.files["/state/master_fx_0.json"], "GOOD");
}

console.log("== a shim that predates master_fx:modules still works ==");
{
  /* Version skew is a real field state: the web updater mirrors the shim
     separately from the JS. Without the per-position fallback this silently
     reverts to the data-loss bug. */
  const answers = (key) => {
    if (key === "master_fx:modules") return null;      /* unknown key */
    if (key === "master_fx:fx1:name") return "clap";
    if (key === "master_fx:fx1:module") return CLAP;
    if (key === "master_fx:fx1:state") return "{\"a\":1}";
    if (/^master_fx:fx\d+:name$/.test(key)) return "";
    return "";
  };
  const w = world(answers, {});
  w.save();
  check("adopted through the fallback", w.masterFxConfig.fx1.module, "clap");
  check("path came from the fallback too",
        JSON.parse(w.files["/state/master_fx_0.json"]).module_path, CLAP);
}

console.log("== a write still in flight is not read back over ==");
{
  /* Under overtake shadow_set_param is fire-and-forget, so a save reached from
     a tool through ctx can read the position BEFORE its own write lands. Adopt
     that and the module the user just picked is silently reverted. */
  const answers = (key) => {
    if (key === "master_fx:modules")
      return snapshotWith([{ id: "old", path: "/x/old/dsp.so" }]);
    if (key === "master_fx:fx1:state") return "{\"a\":1}";
    return "";
  };
  const fresh = world(answers, { fx1: "new" },
                      { writeAt: { fx1: Date.now() },
                        options: [{ id: "new", dspPath: "/x/new/dsp.so" }] });
  fresh.save();
  check("inside the settle window the mirror wins",
        fresh.masterFxConfig.fx1.module, "new");

  const settled = world(answers, { fx1: "new" },
                        { writeAt: { fx1: Date.now() - (CONTRACT_SETTLE_MS + 50) },
                          options: [{ id: "new", dspPath: "/x/new/dsp.so" }] });
  settled.save();
  check("once it has settled the shim wins",
        settled.masterFxConfig.fx1.module, "old");
}

console.log("== the id and the path come from the SAME answer ==");
{
  /* Read independently they can disagree — this position id beside the last
     module path — and the boot loader restores by PATH and never reads the id,
     so the wrong module comes back with no sign of it. */
  const w = world(loadedAnswers, {},
                  { options: [{ id: "clap", dspPath: "/stale/scan/path.so" }] });
  w.save();
  check("the shim path beats a stale scan entry",
        JSON.parse(w.files["/state/master_fx_0.json"]).module_path, CLAP);

  /* And when the pair cannot be trusted as a pair — the fallback served an id
     but not a path — the path must come from the id own scan entry, never from
     a different read. */
  const answers = (key) => {
    if (key === "master_fx:modules") return null;
    if (key === "master_fx:fx1:name") return "clap";
    if (key === "master_fx:fx1:module") return null;   /* did not arrive */
    if (key === "master_fx:fx1:state") return "{\"a\":1}";
    return "";
  };
  const p = world(answers, {}, { options: [{ id: "clap", dspPath: "/scan/clap.so" }] });
  p.save();
  check("a half-served pair falls back to the scan",
        JSON.parse(p.files["/state/master_fx_0.json"]).module_path, "/scan/clap.so");
}

console.log("== an id with no path preserves the file rather than blanking it ==");
{
  /* The shim named a module the startup scan has never seen (installed over the
     web manager, which does not restart shadow_ui) and the path did not arrive.
     A state file with no module_path restores NOTHING — the boot loader skips
     it — so writing one over a good file is the same erase as writing "{}". */
  const answers = (key) => {
    if (key === "master_fx:modules") return null;
    if (key === "master_fx:fx1:name") return "brandnew";
    if (key === "master_fx:fx1:module") return null;
    if (key === "master_fx:fx1:state") return "{\"a\":1}";
    return "";
  };
  const w = world(answers, {}, { files: { "/state/master_fx_0.json": "GOOD" } });
  w.save();
  check("the good file is preserved", w.files["/state/master_fx_0.json"], "GOOD");
}

console.log("== an empty position is still written empty ==");
{
  /* The other drift direction: a position CLEARED through the shim used to be
     written back from the stale mirror, silently reverting the change. */
  const answers = (key) => {
    if (key === "master_fx:modules") return snapshotWith([]);
    return "";
  };
  const w = world(answers, { fx1: "clap" },
                  { files: { "/state/master_fx_0.json": "OLD" } });
  w.save();
  check("the mirror follows the shim", w.masterFxConfig.fx1.module, "");
  check("and the file is emptied", w.files["/state/master_fx_0.json"].trim(), "{}");
}

if (failures) {
  console.log(`\n${failures} master FX save check(s) FAILED`);
  process.exit(1);
}
console.log("\nPASS: the master FX saver reads the shim, as one answer, and never acts on a failed read");
'
