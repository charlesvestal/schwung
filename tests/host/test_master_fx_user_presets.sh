#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# User Presets ("My Presets") on a MASTER FX position (issue #305).
#
# A Master FX position gets the slot chain's "My Presets" page through the SAME
# code (componentTrailingMenus / runComponentActionFromGrid / the preset flow in
# shadow_ui_presets.mjs / currentUserPresets); test_trailing_pages_wiring.sh
# pins the page and action halves. This file pins what is master-specific:
#
#   1. the preset flow, entered with the master target, addresses IPC slot 0
#      WITHOUT touching ctx.selectedSlot or the focused slot, never auditions,
#      and every exit lands on VIEWS.MASTER_FX (the slot default is unchanged);
#   2. maybeReturnToMasterComponentGrid fires/holds/drops correctly, and is
#      polled;
#   3. the per-position record follows a reshaped chain (remove / insert /
#      move), is cleared on swap / clear / whole-chain preset / adopt, and is
#      persisted AFTER `state` and reloaded at boot and on a set change.
#
# Lifted and RUN where there is a decision in it (a grep can pass over code
# that does nothing); structural only where the site sits inside a function too
# large to lift (the master saver, boot restore, the set-change loop).

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
REPO="$PWD"

# ---------------------------------------------------------------------------
# 1. The preset flow, run for real with its device imports pointed at the repo.
# ---------------------------------------------------------------------------
cat > "$TMP/os.mjs" <<'EOF'
export const files = {};
export function readdir(dir) {
    const names = Object.keys(files).filter((p) => p.startsWith(dir + "/")).map((p) => p.slice(dir.length + 1));
    return [names, 0];
}
export function remove(path) { delete files[path]; return 0; }
EOF
sed -e "s#/data/UserData/schwung/shared/#${REPO}/src/shared/#g" \
    -e "s#'./shadow_ui_ctx.mjs'#'${REPO}/src/shadow/shadow_ui_ctx.mjs'#" \
    -e "s#import \* as os from 'os';#import * as os from '${TMP}/os.mjs';#" \
    src/shadow/shadow_ui_presets.mjs > "$TMP/presets.mjs"
grep -q "${TMP}/os.mjs" "$TMP/presets.mjs" || { echo "FAIL: could not redirect the os import" >&2; exit 1; }

TMP="$TMP" REPO="$REPO" node --input-type=module -e '
const { TMP, REPO } = process.env;
const fail = (m) => { console.error("FAIL: " + m); process.exit(1); };
for (const g of ["clear_screen", "fill_rect", "print", "draw_line", "fill_circle", "host_send_screenreader"])
    globalThis[g] = () => {};
globalThis.text_width = (t) => String(t || "").length * 6;
const OS = await import(TMP + "/os.mjs");
globalThis.host_read_file = (p) => OS.files[p] || null;
globalThis.host_write_file = (p, b) => { OS.files[p] = b; return true; };
globalThis.host_file_exists = (p) => p in OS.files;
globalThis.host_ensure_dir = () => true;

const { ctx } = await import(REPO + "/src/shadow/shadow_ui_ctx.mjs");
const P = await import(TMP + "/presets.mjs");

const DIR = "/data/UserData/schwung/presets/tunnel";
OS.files[DIR + "/Wide.json"] = JSON.stringify({ name: "Wide", module: "tunnel", version: 1, state: { mix: 0.9 } });
OS.files[DIR + "/Dry.json"] = JSON.stringify({ name: "Dry", module: "tunnel", version: 1, state: "opaque,csv" });

let views = [], writes = [], reads = [], focused = [], loaded = [], deleted = [];
Object.assign(ctx, {
    VIEWS: { CHAIN_EDIT: "chain", MASTER_FX: "mfx", PRESETS: "presets", PRESET_DETAIL: "detail" },
    setView: (v) => views.push(v),
    updateFocusedSlot: (s) => focused.push(s),
    getSlotParam: (s, k) => (k.endsWith(":name") ? "tunnel" : ""),
    setSlotParam: (s, k, v) => { writes.push([s, k, v]); return true; },
    getSlotStateWithRetry: (s, k) => { reads.push([s, k]); return "{\"mix\":0.1}"; },
    auditionEnabled: () => true,
    onPresetLoaded: (s, p, n) => loaded.push([s, p, n]),
    onPresetDeleted: (s, p, n) => deleted.push([s, p, n]),
    onPresetSaved: () => {},
    showWarning: () => {},
});
ctx.selectedSlot = 2;     /* the user is on instrument slot 3 */
const MASTER = { returnView: "mfx", returnLabel: "Master FX", focusSlot: false, audition: false, label: "Tunnel" };

/* Load: browse, jog to "Wide", pick. */
P.enterPresetBrowser(0, "master_fx:fx2", "tunnel", "master_fx:fx2", MASTER);
if (ctx.selectedSlot !== 2) fail("a Master FX flow must not move ctx.selectedSlot (got " + ctx.selectedSlot + ")");
if (focused.length) fail("a Master FX flow must not refocus an instrument slot, got " + JSON.stringify(focused));
if (reads.length) fail("audition is OFF for Master FX even with the switch on: no state capture on entry, got " +
                       JSON.stringify(reads));
if (views.pop() !== "presets") fail("enterPresetBrowser must show VIEWS.PRESETS");
P.handlePresetsJog(1);            /* Dry -> Wide (sorted) */
P.tickPresetPreview(); for (let i = 0; i < 10; i++) P.tickPresetPreview();
if (writes.length) fail("no audition write may happen on a Master FX list, got " + JSON.stringify(writes));
P.handlePresetsSelect();
if (JSON.stringify(writes) !== JSON.stringify([[0, "master_fx:fx2:state", "{\"mix\":0.9}"]]))
    fail("picking must apply the blob at slot 0 under master_fx:fx2:state, got " + JSON.stringify(writes));
if (JSON.stringify(loaded) !== JSON.stringify([[0, "master_fx:fx2", "Wide"]]))
    fail("the load must be reported for (0, master_fx:fx2, Wide), got " + JSON.stringify(loaded));
if (views.pop() !== "mfx") fail("a completed Master FX load must land on VIEWS.MASTER_FX");

/* Back out of the browser. */
P.enterPresetBrowser(0, "master_fx:fx2", "tunnel", "master_fx:fx2", MASTER);
P.handlePresetsBack();
if (views.pop() !== "mfx") fail("Back from a Master FX browser must land on VIEWS.MASTER_FX");

/* Delete: confirm No, then Yes. */
P.enterPresetDeleteConfirm(0, "master_fx:fx2", "tunnel", "master_fx:fx2", "Dry", MASTER);
if (views.pop() !== "detail") fail("the delete confirm must show VIEWS.PRESET_DETAIL");
P.handlePresetDetailBack();
if (views.pop() !== "mfx") fail("Back from a Master FX delete confirm must land on VIEWS.MASTER_FX");
P.enterPresetDeleteConfirm(0, "master_fx:fx2", "tunnel", "master_fx:fx2", "Dry", MASTER);
P.handlePresetDetailJog(1);
P.handlePresetDetailSelect();
if ((DIR + "/Dry.json") in OS.files) fail("confirming Yes must delete the preset file");
if (JSON.stringify(deleted) !== JSON.stringify([[0, "master_fx:fx2", "Dry"]]))
    fail("the delete must be reported for (0, master_fx:fx2, Dry), got " + JSON.stringify(deleted));
if (views.pop() !== "mfx") fail("a completed Master FX delete must land on VIEWS.MASTER_FX");
if (ctx.selectedSlot !== 2 || focused.length) fail("no Master FX step may touch the instrument slot focus");

/* The SLOT default is unchanged: focus moves, exits go to CHAIN_EDIT. */
writes = [];
P.enterPresetBrowser(1, "fx1", "tunnel", "fx1");
if (ctx.selectedSlot !== 1 || focused.pop() !== 1) fail("a slot flow still focuses its slot");
P.handlePresetsSelect();
if (!writes.length || writes[0][0] !== 1 || writes[0][1] !== "fx1:state") fail("a slot load still applies at its slot");
if (views.pop() !== "chain") fail("a slot flow still exits to VIEWS.CHAIN_EDIT");
P.enterPresetBrowser(1, "fx1", "tunnel", "fx1");
P.handlePresetsBack();
if (views.pop() !== "chain") fail("a slot Back still exits to VIEWS.CHAIN_EDIT (a master flow must not leak its target)");
console.log("  ok  preset flow with the master target: slot 0, no focus change, no audition, every exit to MASTER_FX; slot default unchanged");
'

# ---------------------------------------------------------------------------
# 2-3. shadow_ui.js: the reconciler and the record plumbing.
# ---------------------------------------------------------------------------
node -e '
const fs = require("fs");
const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const fail = (m) => { console.error("FAIL: " + m); process.exit(1); };
const grab = (name) => {
    const m = src.match(new RegExp("^function " + name + "\\([^]*?^}", "m"));
    if (!m) fail("could not lift " + name + "()");
    return m[0];
};

/* ---- remapPositionRecords: the DSP permutation, per record ----------- */
const remap = new Function(grab("remapPositionRecords") + "\nreturn remapPositionRecords;")();
const R = ["a", "b", "c", null];
const eq = (got, want, what) => { if (JSON.stringify(got) !== JSON.stringify(want))
    fail(what + ": expected " + JSON.stringify(want) + ", got " + JSON.stringify(got)); };
eq(remap(R, { kind: "remove", index: 0 }), ["b", "c", null, null], "remove fx1 closes the gap");
eq(remap(R, { kind: "remove", index: 1 }), ["a", "c", null, null], "remove fx2");
eq(remap(R, { kind: "insert", index: 1 }), ["a", null, "b", "c"], "insert at fx2 opens a hole");
eq(remap(R, { kind: "move", index: 0, to: 2 }), ["b", "c", "a", null], "move fx1 -> fx3");
eq(remap(R, { kind: "move", index: 2, to: 0 }), ["c", "a", "b", null], "move fx3 -> fx1");
eq(remap(R, { kind: "move", index: 0, to: 9 }), R, "an out-of-range move changes nothing");
eq(remap(R, { kind: "bogus", index: 0 }), R, "an unknown verb changes nothing");
if (remap(R, { kind: "remove", index: 0 }) === R) fail("remap must not mutate/return its input");
console.log("  ok  remapPositionRecords: remove / insert / move follow the DSP permutation");

/* ---- the record helpers, against a real record map ------------------- */
const harness = new Function([
    "const MASTER_FX_SLOTS = 8;",
    src.match(/^const FX_BUSES = \[[^]*?^\];/m)[0],
    "let currentFxBusIndex = 0;",
    "function fxBus() { return FX_BUSES[currentFxBusIndex] || FX_BUSES[0]; }",
    "function fxBusIsMaster() { return fxBus().send < 0; }",
    "const MASTER_CHAIN_TARGET = { slot: 0 };",
    "const currentUserPresets = Object.create(null);",
    "const userPresetKey = (slot, prefix) => `${slot}:${prefix}`;",
    "let userPresetLiveBlobCache = Object.create(null);",
    grab("getUserPresetRecord"), grab("setUserPresetRecord"), grab("entryUserPreset"),
    grab("masterFxComponentKey"), grab("masterUserPresetPrefix"), grab("remapPositionRecords"),
    grab("remapMasterUserPresetRecords"), grab("clearMasterUserPresetRecord"),
    grab("syncMasterUserPresetRecord"), grab("masterUserPresetField"),
    "return { currentUserPresets, cache: () => userPresetLiveBlobCache, setBus: (i) => { currentFxBusIndex = i; },",
    "  remapMasterUserPresetRecords, clearMasterUserPresetRecord, syncMasterUserPresetRecord, masterUserPresetField,",
    "  getUserPresetRecord, setUserPresetRecord };",
].join("\n"))();
const H = harness;
const rec = (n) => ({ name: n, hash: "h:" + n });
H.setUserPresetRecord(0, "master_fx:fx1", rec("A"));
H.setUserPresetRecord(0, "master_fx:fx2", rec("B"));
H.setUserPresetRecord(0, "fx1", rec("SLOT1"));     /* instrument slot 1, fx1 -- must never move */
H.cache()["0:master_fx:fx2"] = "{}";
H.remapMasterUserPresetRecords({ kind: "remove", section: "fx", index: 0 });
if (!H.getUserPresetRecord(0, "master_fx:fx1") || H.getUserPresetRecord(0, "master_fx:fx1").name !== "B" ||
    H.getUserPresetRecord(0, "master_fx:fx2"))
    fail("removing fx1 must move fx2s record to fx1 and leave fx2 empty, got " + JSON.stringify(H.currentUserPresets));
if (H.getUserPresetRecord(0, "fx1").name !== "SLOT1") fail("a master remap must not touch instrument slot 1s 0:fx1 record");
if ("0:master_fx:fx2" in H.cache()) fail("a remap must drop the cached live blobs it re-keys");
H.setBus(1);   /* Send A on screen */
H.remapMasterUserPresetRecords({ kind: "insert", section: "fx", index: 0 });
if (H.getUserPresetRecord(0, "master_fx:fx1").name !== "B") fail("a SEND reshape must not touch the master records");
H.setBus(0);
H.clearMasterUserPresetRecord(0);
if (H.getUserPresetRecord(0, "master_fx:fx1")) fail("clearMasterUserPresetRecord must clear the position");

H.syncMasterUserPresetRecord(3, { module_id: "tunnel", state: {}, user_preset: { name: "Wide", hash: "h:9" } });
const r4 = H.getUserPresetRecord(0, "master_fx:fx4");
if (!r4 || r4.name !== "Wide" || r4.hash !== "h:9") fail("sync must load the saved record, got " + JSON.stringify(r4));
H.syncMasterUserPresetRecord(3, { module_id: "tunnel", state: {} });
if (H.getUserPresetRecord(0, "master_fx:fx4")) fail("sync from an entry with NO user_preset must CLEAR the record");
H.setUserPresetRecord(0, "master_fx:fx4", rec("X"));
H.syncMasterUserPresetRecord(3, null);
if (H.getUserPresetRecord(0, "master_fx:fx4")) fail("sync from a missing file must clear the record");

H.setUserPresetRecord(0, "master_fx:fx5", rec("Pad"));
const f = H.masterUserPresetField(4, { mix: 1 });
if (!f || f.name !== "Pad" || f.hash !== "h:Pad") fail("the persisted field must carry name+hash, got " + JSON.stringify(f));
if (H.masterUserPresetField(4, null) !== null) fail("no user_preset may be persisted without a state");
if (H.masterUserPresetField(5, { mix: 1 }) !== null) fail("no field for a position with no record");
console.log("  ok  master record helpers: remap re-keys only master positions on the master bus; sync loads/clears; persisted field needs a state");

/* ---- structure: every site that moves a master position calls in ----- */
const body = (name) => grab(name);
const has = (name, needle, why) => { if (!body(name).includes(needle)) fail(name + " must " + why + " (" + needle + ")"); };
has("writeChainShape", "target.onReshape(op)", "hand every shape verb to the target");
if (!/onReshape: \(op\) => \{ remapMasterUserPresetRecords\(op\); \}/.test(src.match(/^const MASTER_CHAIN_TARGET = \{[^]*?^\};/m)[0]))
    fail("MASTER_CHAIN_TARGET.onReshape must re-key the master records");
has("applyMasterFxModuleSelection", "clearMasterUserPresetRecord(comp.index)", "clear the record when a module is replaced");
has("clearMasterFxOnMaster", "clearMasterUserPresetRecord(i)", "clear every master record");
has("loadMasterPreset", "clearMasterUserPresetRecord(i)", "clear records when a whole-chain preset loads");
has("adoptMasterFxShimModule", "clearMasterUserPresetRecord(", "clear the record when another client changed the module");
has("loadMasterFxChainFromConfigOnMaster", "syncMasterUserPresetRecord(i, stateFile)", "reload records at boot");
if (!/syncMasterUserPresetRecord\(mfxi, mfxDspPath \? mfxData : null\)/.test(src))
    fail("the set-change master reload must sync (or clear) each position record");

/* Persisted AFTER state, in both writes: the shim parser takes the first "state". */
const saver = body("saveMasterFxChainConfigOnMaster");
const sState = saver.indexOf("stateFile.state = stateObj;");
const sUp = saver.indexOf("stateFile.user_preset = userPreset;");
if (sState < 0 || sUp < 0 || sUp < sState) fail("master_fx_N.json must write user_preset AFTER state");
const cState = saver.indexOf("slotConfig.state = stateObj;");
if (saver.indexOf("slotConfig.user_preset") >= 0)
    fail("config.master_fx_chain must NOT carry user_preset -- it is read back from master_fx_N.json only, and the shim copies master_fx_chain");
console.log("  ok  every master reshape/swap/clear/adopt/boot/set-change site updates the record; persisted after state");

/* ---- maybeReturnToMasterComponentGrid, lifted and run ----------------- */
const recon = grab("maybeReturnToMasterComponentGrid");
const run = (state) => {
    const s = Object.assign({ flag: true, key: "master_fx:fx2", mod: "tunnel", view: "mfx", master: true,
                              moduleNow: "tunnel", textEntry: false, enter: false }, state);
    const log = { entered: [] };
    const patched = recon
        .replace(/\bmasterComponentModalFromGrid\b/g, "s.flag")
        .replace(/\bmasterComponentGridReturnKey\b/g, "s.key")
        .replace(/\bmasterComponentGridReturnModule\b/g, "s.mod")
        .replace(/\bcomponentGridReturnEnter\b/g, "s.enter")
        .replace(/\bview\b/g, "s.view")
        .replace(/\bneedsRedraw\b/g, "s.redraw");
    const fn = new Function("s", "log", [
        "const VIEWS = { MASTER_FX: \"mfx\", PRESETS: \"presets\", PRESET_DETAIL: \"detail\" };",
        "const MASTER_CHAIN_TARGET = { slot: 0 };",
        "function isTextEntryActive() { return s.textEntry; }",
        "function fxBusIsMaster() { return s.master; }",
        "function masterComponentModuleId() { return s.moduleNow; }",
        "function getComponentParamPrefix(k) { return k; }",
        "function componentParamPagesIo(slot, k) { return { io: slot + \":\" + k }; }",
        "function paramPagesChromeFor() { return { chrome: 1 }; }",
        "function enterParamPages(...a) { log.entered.push(a); }",
        patched, "return maybeReturnToMasterComponentGrid;"].join("\n"))(s, log);
    return { fired: fn(), s, log };
};
let t = run({});
if (!t.fired || t.log.entered.length !== 1 || t.s.flag !== false) fail("must fire on a MASTER_FX arrival with the same module");
const a = t.log.entered[0];
if (a[0] !== 0 || a[1] !== "master_fx:fx2" || a[3] !== "My Presets" || !a[4] || a[4].io !== "0:master_fx:fx2" ||
    !a[6] || a[6].enter !== false || t.s.enter !== true)
    fail("must re-enter the master grid on My Presets with the master io and the done-disposition, got " + JSON.stringify(a));
t = run({ view: "presets" });
if (t.fired || t.s.flag !== true) fail("must HOLD while the preset browser is still up");
t = run({ view: "detail" });
if (t.fired || t.s.flag !== true) fail("must HOLD while the delete confirm is still up");
t = run({ textEntry: true });
if (t.fired || t.s.flag !== true) fail("must HOLD while the keyboard is up");
t = run({ view: "chain" });
if (t.fired || t.s.flag !== false || t.log.entered.length) fail("an arrival ELSEWHERE must drop the flag without firing");
t = run({ moduleNow: "" });
if (t.fired || t.s.flag !== false) fail("a now-empty position must not be re-entered (the Loading... trap)");
t = run({ moduleNow: "wettt" });
if (t.fired || t.s.flag !== false) fail("a swapped position must not be re-entered as the old grid");
t = run({ master: false });
if (t.fired || t.s.flag !== false) fail("must not fire while a send bus is on screen");
t = run({ flag: false });
if (t.fired || t.log.entered.length) fail("no flag, no return");
console.log("  ok  maybeReturnToMasterComponentGrid: fires on MASTER_FX, holds on the preset screens/keyboard, drops elsewhere/emptied/swapped/send");

/* ---- polled before the draw, beside the other reconcilers ------------- */
const pollAt = src.indexOf("if (view === VIEWS.CHAIN_EDIT) maybeReturnToComponentGrid();");
const masterAt = src.indexOf("\n    maybeReturnToMasterComponentGrid();", pollAt);
if (pollAt < 0 || masterAt < 0 || masterAt - pollAt > 1000)
    fail("maybeReturnToMasterComponentGrid() must be polled unconditionally right after maybeReturnToComponentGrid()");
console.log("  ok  maybeReturnToMasterComponentGrid is polled at the reconcile site");
'

echo "PASS: Master FX user presets"
