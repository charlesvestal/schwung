#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# User Presets ("My Presets") on a SEND FX position (Send A / Send B).
#
# A send position is an FX-bus position: same editor (VIEWS.MASTER_FX, with
# currentFxBusIndex on the send), same knob grid, same IPC addressing (slot 0,
# "send<N>:fx<M>:<param>", routed by send_fx_key.h through the same handler
# shape as master_fx). So it reuses the Master FX My Presets code, parametrised
# by BUS instead of duplicated. This file pins what the send half adds:
#
#   1. the preset flow entered with a send target addresses slot 0 under
#      send1:fx2:state, never moves the focused slot, never auditions, and every
#      exit lands on VIEWS.MASTER_FX;
#   2. componentParamPagesIo / componentTrailingMenus / runComponentActionFromGrid
#      give a send position "My Presets" only, with the module read from the
#      SEND's mirror and a flow target that announces the send;
#   3. the record "0:send1:fxN" never collides with the master's "0:master_fx:fxN"
#      nor slot 1's "0:fxN"; it follows a reshape of the bus ON SCREEN only; it is
#      cleared, synced and persisted per bus;
#   4. saveSendFxChainConfig writes user_preset AFTER state (the shim's boot
#      parser, shared with Master FX via fx_boot_restore_one, takes the first
#      "state"), drops a record whose module left the position, and
#      loadSendFxChainConfigForSet / the boot sync reload it per set;
#   5. maybeReturnToFxBusComponentGrid comes back to the SEND's grid only while
#      that send is the bus on screen.
#
# Lifted and RUN where there is a decision in it; structural only for call
# sites inside functions too large to lift (init, the module picker).

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
REPO="$PWD"

# ---------------------------------------------------------------------------
# 1. The preset flow, run for real with a SEND target.
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
ctx.selectedSlot = 3;
const SEND = { returnView: "mfx", returnLabel: "Send A", focusSlot: false, audition: false, label: "Tunnel" };

P.enterPresetBrowser(0, "send1:fx2", "tunnel", "send1:fx2", SEND);
if (ctx.selectedSlot !== 3 || focused.length) fail("a Send FX flow must not touch the instrument slot focus");
if (reads.length) fail("audition is OFF for a send even with the switch on, got reads " + JSON.stringify(reads));
if (views.pop() !== "presets") fail("enterPresetBrowser must show VIEWS.PRESETS");
P.handlePresetsJog(1);
for (let i = 0; i < 12; i++) P.tickPresetPreview();
if (writes.length) fail("no audition write may happen on a send list, got " + JSON.stringify(writes));
P.handlePresetsSelect();
if (JSON.stringify(writes) !== JSON.stringify([[0, "send1:fx2:state", "{\"mix\":0.9}"]]))
    fail("picking must apply the blob at slot 0 under send1:fx2:state, got " + JSON.stringify(writes));
if (JSON.stringify(loaded) !== JSON.stringify([[0, "send1:fx2", "Wide"]]))
    fail("the load must be reported for (0, send1:fx2, Wide), got " + JSON.stringify(loaded));
if (views.pop() !== "mfx") fail("a completed send load must land on VIEWS.MASTER_FX (the send editor)");
P.enterPresetBrowser(0, "send2:fx1", "tunnel", "send2:fx1", Object.assign({}, SEND, { returnLabel: "Send B" }));
P.handlePresetsBack();
if (views.pop() !== "mfx") fail("Back from a send browser must land on VIEWS.MASTER_FX");
P.enterPresetDeleteConfirm(0, "send1:fx2", "tunnel", "send1:fx2", "Dry", SEND);
P.handlePresetDetailJog(1);
P.handlePresetDetailSelect();
if ((DIR + "/Dry.json") in OS.files) fail("confirming Yes must delete the preset file");
if (JSON.stringify(deleted) !== JSON.stringify([[0, "send1:fx2", "Dry"]])) fail("delete reported wrong: " + JSON.stringify(deleted));
if (views.pop() !== "mfx") fail("a completed send delete must land on VIEWS.MASTER_FX");
console.log("  ok  preset flow with a send target: slot 0 / send1:fx2:state, no focus change, no audition, exits to MASTER_FX");
'

# ---------------------------------------------------------------------------
# 2-5. shadow_ui.js
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
const grabSet = (name) => {
    const m = src.match(new RegExp("^const " + name + " = [^\\n]*;$", "m"));
    if (!m) fail("could not lift " + name);
    return m[0];
};
const FX_BUSES_SRC = src.match(/^const FX_BUSES = \[[^]*?^\];/m)[0];

/* ---- the FX-bus record helpers, against a real record map ------------- */
const H = new Function([
    "const MASTER_FX_SLOTS = 8;",
    FX_BUSES_SRC,
    "let currentFxBusIndex = 0;",
    "function fxBus() { return FX_BUSES[currentFxBusIndex] || FX_BUSES[0]; }",
    "function fxBusIsMaster() { return fxBus().send < 0; }",
    "const MASTER_CHAIN_TARGET = { slot: 0 };",
    "const VIEWS = { MASTER_FX: \"mfx\" };",
    "const mirrors = [{}, {}, {}];",
    "function withFxBus(i, fn) { const p = currentFxBusIndex; currentFxBusIndex = i; try { return fn(); } finally { currentFxBusIndex = p; } }",
    "Object.defineProperty(globalThis, \"masterFxConfig\", { get: () => mirrors[currentFxBusIndex], configurable: true });",
    "function getModuleDisplayName(id) { return id === \"tunnel\" ? \"Tunnel\" : \"\"; }",
    "const currentUserPresets = Object.create(null);",
    "const userPresetKey = (slot, prefix) => `${slot}:${prefix}`;",
    "let userPresetLiveBlobCache = Object.create(null);",
    grab("getUserPresetRecord"), grab("setUserPresetRecord"), grab("entryUserPreset"),
    grab("masterFxComponentKey"), grab("masterFxIndexFromComponentKey"),
    grab("fxBusOfComponentKey"), grab("isFxBusComponentKey"), grab("fxBusUserPresetPrefix"),
    grab("fxBusComponentModuleId"), grab("fxBusPresetFlowOptions"), grab("remapPositionRecords"),
    grab("remapFxBusUserPresetRecords"), grab("clearFxBusUserPresetRecord"),
    grab("syncFxBusUserPresetRecord"), grab("fxBusUserPresetField"), grab("fxBusRecordOrphaned"),
    "return { currentUserPresets, mirrors, cache: () => userPresetLiveBlobCache, setBus: (i) => { currentFxBusIndex = i; },",
    "  fxBusOfComponentKey, isFxBusComponentKey, fxBusUserPresetPrefix, fxBusComponentModuleId, fxBusPresetFlowOptions,",
    "  remapFxBusUserPresetRecords, clearFxBusUserPresetRecord, syncFxBusUserPresetRecord, fxBusUserPresetField,",
    "  fxBusRecordOrphaned, getUserPresetRecord, setUserPresetRecord };",
].join("\n"))();
const rec = (n, m) => (m ? { name: n, hash: "h:" + n, module: m } : { name: n, hash: "h:" + n });

if (H.fxBusOfComponentKey("master_fx:fx1") !== 0 || H.fxBusOfComponentKey("send1:fx2") !== 1 ||
    H.fxBusOfComponentKey("send2:fx8") !== 2 || H.fxBusOfComponentKey("fx2") !== -1 ||
    H.fxBusOfComponentKey("bus1:fx2") !== -1 || H.fxBusOfComponentKey("send1:fx9") !== -1 ||
    H.fxBusOfComponentKey("send1:settings") !== -1)
    fail("fxBusOfComponentKey must name the bus of an FX-bus POSITION and -1 for anything else");
if (H.fxBusUserPresetPrefix(1, 1) !== "send1:fx2" || H.fxBusUserPresetPrefix(2, 0) !== "send2:fx1" ||
    H.fxBusUserPresetPrefix(0, 7) !== "master_fx:fx8" || H.fxBusUserPresetPrefix(1, 8) !== null ||
    H.fxBusUserPresetPrefix(5, 0) !== null)
    fail("fxBusUserPresetPrefix must spell the bus prefix + fxN, null out of range");

/* Module lookup: from THAT bus mirror, whichever bus is open. */
H.mirrors[0].fx2 = { module: "wettt" };
H.mirrors[1].fx2 = { module: "tunnel" };
if (H.fxBusComponentModuleId("send1:fx2") !== "tunnel" || H.fxBusComponentModuleId("master_fx:fx2") !== "wettt" ||
    H.fxBusComponentModuleId("send2:fx2") !== "" || H.fxBusComponentModuleId("fx2") !== "")
    fail("fxBusComponentModuleId must read the named bus mirror");

const o = H.fxBusPresetFlowOptions("send2:fx1", "tunnel");
if (o.returnView !== "mfx" || o.focusSlot !== false || o.audition !== false || o.returnLabel !== "Send B" || o.label !== "Tunnel")
    fail("a send flow target: back to MASTER_FX, no focus, no audition, announces the send, got " + JSON.stringify(o));
if (H.fxBusPresetFlowOptions("master_fx:fx1", "").returnLabel !== "Master FX") fail("the master target still announces Master FX");

/* No collisions between master, send and slot-1 records. */
H.setUserPresetRecord(0, "master_fx:fx1", rec("M1"));
H.setUserPresetRecord(0, "send1:fx1", rec("A1"));
H.setUserPresetRecord(0, "send1:fx2", rec("A2"));
H.setUserPresetRecord(0, "send2:fx1", rec("B1"));
H.setUserPresetRecord(0, "fx1", rec("SLOT1"));
H.cache()["0:send1:fx2"] = "{}";
H.setBus(1);                                  /* Send A on screen */
H.remapFxBusUserPresetRecords({ kind: "remove", section: "fx", index: 0 });
const nm = (k) => (H.getUserPresetRecord(0, k) || {}).name || null;
if (nm("send1:fx1") !== "A2" || nm("send1:fx2") !== null)
    fail("a Send A removal must move fx2s record to fx1, got " + JSON.stringify(H.currentUserPresets));
if (nm("master_fx:fx1") !== "M1" || nm("send2:fx1") !== "B1" || nm("fx1") !== "SLOT1")
    fail("a Send A reshape must touch no other bus or slot record, got " + JSON.stringify(H.currentUserPresets));
if ("0:send1:fx2" in H.cache()) fail("a remap must drop the cached live blobs it re-keys");
H.setBus(2);
H.remapFxBusUserPresetRecords({ kind: "insert", section: "fx", index: 0 });
if (nm("send2:fx2") !== "B1" || nm("send2:fx1") !== null || nm("send1:fx1") !== "A2")
    fail("a Send B insert re-keys Send B only, got " + JSON.stringify(H.currentUserPresets));
H.setBus(0);
H.remapFxBusUserPresetRecords({ kind: "move", section: "fx", index: 0, to: 1 });
if (nm("master_fx:fx2") !== "M1" || nm("send1:fx1") !== "A2") fail("a master move re-keys the master only");
H.remapFxBusUserPresetRecords({ kind: "remove", section: "synth", index: 0 });
H.clearFxBusUserPresetRecord(1, 0);
if (nm("send1:fx1") !== null || nm("send2:fx2") !== "B1") fail("clearFxBusUserPresetRecord clears that bus position only");

H.syncFxBusUserPresetRecord(2, 3, { module_id: "tunnel", state: {}, user_preset: { name: "Wide", hash: "h:9" } });
const r = H.getUserPresetRecord(0, "send2:fx4");
if (!r || r.name !== "Wide" || r.hash !== "h:9" || r.module !== "tunnel")
    fail("sync must load the saved record tagged with its module, got " + JSON.stringify(r));
H.syncFxBusUserPresetRecord(2, 3, { module_id: "tunnel", state: {} });
if (nm("send2:fx4") !== null) fail("sync from an entry with NO user_preset must CLEAR the record");
H.setUserPresetRecord(0, "send2:fx4", rec("X"));
H.syncFxBusUserPresetRecord(2, 3, null);
if (nm("send2:fx4") !== null) fail("sync from a missing file must clear the record");

H.setUserPresetRecord(0, "send1:fx5", rec("Pad", "tunnel"));
const f = H.fxBusUserPresetField(1, 4, { mix: 1 });
if (!f || JSON.stringify(f) !== JSON.stringify({ name: "Pad", hash: "h:Pad" }))
    fail("the persisted field is exactly {name, hash}, got " + JSON.stringify(f));
if (H.fxBusUserPresetField(1, 4, undefined) !== null || H.fxBusUserPresetField(1, 4, "") !== null)
    fail("no user_preset may be persisted without a state");
if (H.fxBusUserPresetField(2, 4, { mix: 1 }) !== null) fail("Send B fx5 has no record");

/* Orphaned: the module the record was made on is no longer there. */
if (H.fxBusRecordOrphaned(1, 4, "tunnel") || H.fxBusRecordOrphaned(1, 4, "TUNNEL"))
    fail("the same module (any case) keeps its record");
if (!H.fxBusRecordOrphaned(1, 4, "wettt")) fail("a different module orphans the record");
if (!H.fxBusRecordOrphaned(1, 4, "")) fail("an emptied position orphans the record");
H.setUserPresetRecord(0, "send1:fx6", rec("Old"));       /* untagged: cannot tell */
if (H.fxBusRecordOrphaned(1, 5, "wettt")) fail("an untagged record is not judged by module");
if (H.fxBusRecordOrphaned(1, 6, "")) fail("no record, nothing orphaned");
console.log("  ok  FX-bus record helpers: per-bus keys never collide; remap follows the bus on screen; sync/clear/field/orphan");

/* ---- componentParamPagesIo: a send position now gets an io -------------- */
{
    const calls = [];
    const io = new Function("calls", [
        "const MASTER_FX_SLOTS = 8;", FX_BUSES_SRC,
        grab("masterFxIndexFromComponentKey"),
        "const MASTER_CHAIN_TARGET = { slot: 0 };",
        "function getComponentParamPrefix(k) { return k; }",
        "function componentTrailingMenus(...a) { calls.push([\"menus\", ...a]); return []; }",
        "function runComponentActionFromGrid(...a) { calls.push([\"run\", ...a]); }",
        "function setSlotParam(s, k, v) { calls.push([\"set\", s, k, v]); return true; }",
        "function noteLaneWriteRefusal(s, k) { calls.push([\"refuse\", s, k]); }",
        "function markComponentParamWrite(s, c) { calls.push([\"mark\", s, c]); }",
        "const BusModel = { parseBusComponentKey: (k) => (/^bus[1-4]:fx[1-8]$/.test(k) ? {} : null) };",
        grab("componentParamPagesIo"),
        "return componentParamPagesIo;"].join("\n"))(calls);
    const calls2 = calls;
    const send = io(0, "send2:fx3");
    if (!send || typeof send.trailingMenus !== "function") fail("a send position must get a trailing-pages io");
    send.trailingMenus(); send.runAction("up_load"); send.setParam("send2:fx3:mix", "0.5");
    const want = [["menus", 0, "send2:fx3", "send2:fx3"], ["run", 0, "send2:fx3", "up_load"],
                  ["refuse", 0, "send2:fx3:mix"], ["set", 0, "send2:fx3:mix", "0.5"], ["mark", 0, "send2:fx3"]];
    if (JSON.stringify(calls2) !== JSON.stringify(want)) fail("send io wiring wrong: " + JSON.stringify(calls2));
    if (send.onValueWritten || send.heldStepIsHold || send.deleteHeld) fail("a send io carries no p-lock hooks");
    if (io(1, "bus1:fx2") !== null) fail("a bus insert stays excluded");
}
console.log("  ok  componentParamPagesIo(): a send position gets the FX-bus io at slot 0; bus inserts stay null");

/* ---- componentTrailingMenus / runComponentActionFromGrid branch on the FX bus */
const tm = grab("componentTrailingMenus");
const ra = grab("runComponentActionFromGrid");
if (!/isFxBusComponentKey\(componentKey\)/.test(tm) || !/fxBusComponentModuleId\(componentKey\)/.test(tm))
    fail("componentTrailingMenus must branch on isFxBusComponentKey and read fxBusComponentModuleId");
if (!/isFxBusComponentKey\(componentKey\)/.test(ra) || !/FX_BUS_COMPONENT_ACTIONS\.has\(action\)/.test(ra) ||
    !/fxBusPresetFlowOptions\(componentKey, moduleId\)/.test(ra) || !/fxBusComponentModalFromGrid = true/.test(ra))
    fail("runComponentActionFromGrid must gate, target and return an FX-bus position through the fxBus helpers");
if (!/^const FX_BUS_COMPONENT_ACTIONS = new Set\(\["up_load", "up_save", "up_save_as", "up_delete"\]\);$/m.test(src))
    fail("FX_BUS_COMPONENT_ACTIONS must be exactly the four preset actions");
const shims = grab("setupModuleParamShims");
if (!/const busPosition = isFxBusComponentKey\(componentKey\);/.test(shims))
    fail("a module-owned UI on ANY FX bus position gets no trailing pages (setupModuleParamShims)");
console.log("  ok  trailing pages / actions / module-UI shims branch on ANY FX-bus position");

/* ---- recordUserPresetFromDevice tags an FX-bus record with its module ---- */
{
    const out = {};
    const fn = new Function("out", [
        "let needsRedraw = false;",
        "const userPresetLiveBlobCache = {};",
        "const userPresetKey = (s, p) => s + \":\" + p;",
        "function getSlotStateWithRetry() { return \"{\\\"a\\\":1}\"; }",
        "function makeRecord(n, b) { return { name: n, hash: \"h\" }; }",
        "function setUserPresetRecord(s, p, r) { out[s + \":\" + p] = r; }",
        "function paramPagesRefreshTrailing() {}",
        "function notifyModuleUiPresetsChanged() {}",
        "function isFxBusComponentKey(k) { return /^(master_fx|send[12]):fx[1-8]$/.test(k); }",
        "function fxBusComponentModuleId(k) { return \"tunnel\"; }",
        grab("recordUserPresetFromDevice"),
        "return recordUserPresetFromDevice;"].join("\n"))(out);
    fn(0, "send1:fx2", "Wide");
    fn(1, "fx1", "Slot");
    if (!out["0:send1:fx2"] || out["0:send1:fx2"].module !== "tunnel" || out["0:send1:fx2"].name !== "Wide")
        fail("an FX-bus record must be tagged with the module it was made on, got " + JSON.stringify(out));
    if ("module" in out["1:fx1"]) fail("a slot record is unchanged (no module tag)");
}
console.log("  ok  recordUserPresetFromDevice tags FX-bus records with their module");

/* ---- saveSendFxChainConfig, lifted and RUN ------------------------------ */
{
    const files = {};
    const rec = Object.create(null);
    const answers = {
        "send1:modules": JSON.stringify([{ id: "tunnel", path: "/m/tunnel.so" }, { id: "wettt", path: "/m/wettt.so" }]
                                        .concat(Array.from({ length: 6 }, () => ({ id: "", path: "" })))),
        "send2:modules": JSON.stringify(Array.from({ length: 8 }, () => ({ id: "", path: "" }))),
        "send1:fx1:state": "{\"mix\":0.5}",
        "send1:fx2:state": "{\"mix\":0.7}",
    };
    const helpers = new Function("rec", [
        "const MASTER_FX_SLOTS = 8;", FX_BUSES_SRC,
        "const MASTER_CHAIN_TARGET = { slot: 0 };",
        "const userPresetKey = (s, p) => s + \":\" + p;",
        "const currentUserPresets = rec;",
        "let userPresetLiveBlobCache = {};",
        grab("getUserPresetRecord"), grab("setUserPresetRecord"), grab("masterFxComponentKey"),
        grab("fxBusUserPresetPrefix"), grab("clearFxBusUserPresetRecord"), grab("fxBusUserPresetField"),
        grab("fxBusRecordOrphaned"),
        "return { getUserPresetRecord, setUserPresetRecord, clearFxBusUserPresetRecord, fxBusUserPresetField, fxBusRecordOrphaned };",
    ].join("\n"))(rec);
    helpers.setUserPresetRecord(0, "send1:fx1", { name: "state", hash: "h1", module: "tunnel" });   /* name "state" on purpose */
    helpers.setUserPresetRecord(0, "send1:fx2", { name: "Gone", hash: "h2", module: "tunnel" });   /* wettt is there now */
    helpers.setUserPresetRecord(0, "send2:fx1", { name: "Empty", hash: "h3", module: "tunnel" });  /* position emptied */
    let levels = 0;
    const save = new Function("H", "files", "answers", "bump", [
        "const MASTER_FX_SLOTS = 8;", FX_BUSES_SRC,
        "const activeSlotStateDir = \"/set\";",
        "function shadow_get_param(s, k) { return (k in answers) ? answers[k] : \"\"; }",
        "function host_write_file(p, v) { files[p] = v; return true; }",
        "function saveSendLevels() { bump(); }",
        "const { clearFxBusUserPresetRecord, fxBusUserPresetField, fxBusRecordOrphaned } = H;",
        grab("saveSendFxChainConfig"),
        "return saveSendFxChainConfig;"].join("\n"))(helpers, files, answers, () => { levels++; });
    save();
    const f1 = files["/set/send_fx_0_0.json"];
    if (!f1) fail("send1 fx1 must be written");
    const j1 = JSON.parse(f1);
    if (!j1.user_preset || j1.user_preset.name !== "state" || j1.user_preset.hash !== "h1" || "module" in j1.user_preset)
        fail("send1 fx1 must persist user_preset {name, hash}, got " + f1);
    if (f1.indexOf("\"state\":") < 0 || f1.indexOf("\"user_preset\"") < f1.indexOf("\"state\":"))
        fail("user_preset must be written AFTER state (the boot parser takes the first \"state\"), got " + f1);
    /* What the C boot parser would take: the FIRST "state" key -- must be the blob. */
    const at = f1.indexOf("\"state\"");
    if (!/^"state": \{/.test(f1.slice(at))) fail("the first \"state\" in the file must be the state blob, got " + f1.slice(at, at + 30));
    const j2 = JSON.parse(files["/set/send_fx_0_1.json"]);
    if (j2.user_preset) fail("a record made on another module must not be persisted, got " + JSON.stringify(j2));
    if (helpers.getUserPresetRecord(0, "send1:fx2")) fail("...and is dropped from memory");
    if (helpers.getUserPresetRecord(0, "send2:fx1")) fail("a record on an emptied send position is dropped");
    if (files["/set/send_fx_1_0.json"] !== "{}\n") fail("an empty send position is still written as {}");
    if (!helpers.getUserPresetRecord(0, "send1:fx1")) fail("a valid record survives the save");
    if (levels !== 1) fail("the levels are still saved once");
}
console.log("  ok  saveSendFxChainConfig: user_preset after state (a preset named \"state\" cannot fool the parser), orphans dropped");

/* ---- loadSendFxChainConfigForSet + boot sync: reload per set ------------ */
{
    const synced = [];
    const files = {
        "/set/send_fx_0_0.json": JSON.stringify({ module_path: "/m/t.so", module_id: "tunnel", state: { a: 1 },
                                                  user_preset: { name: "Wide", hash: "h" } }),
        "/set/send_fx_1_2.json": "{}\n",
    };
    const deps = [
        "const MASTER_FX_SLOTS = 8;", FX_BUSES_SRC,
        "const activeSlotStateDir = \"/set\";",
        "function host_file_exists(p) { return p in files; }",
        "function host_read_file(p) { return (p in files) ? files[p] : null; }",
        "function shadow_set_param() { return true; }",
        "function waitForFxPositionSettled() {}",
        "function syncFxBusUserPresetRecord(b, i, e) { synced.push([b, i, e ? (e.user_preset || {}).name || \"-\" : null]); }",
    ];
    const load = new Function("files", "synced", deps.concat([grab("loadSendFxChainConfigForSet"),
        "return loadSendFxChainConfigForSet;"]).join("\n"))(files, synced);
    load();
    if (synced.length !== 16) fail("the set-change load must sync all 16 send positions, got " + synced.length);
    if (JSON.stringify(synced[0]) !== JSON.stringify([1, 0, "Wide"])) fail("Send A fx1 syncs its saved record, got " + JSON.stringify(synced[0]));
    if (synced.some((s, k) => k > 0 && s[2] !== null)) fail("every other position syncs null (clears), got " + JSON.stringify(synced));
    synced.length = 0;
    const boot = new Function("files", "synced", deps.concat([grab("syncSendUserPresetRecordsFromFiles"),
        "return syncSendUserPresetRecordsFromFiles;"]).join("\n"))(files, synced);
    boot();
    if (synced.length !== 16 || JSON.stringify(synced[0]) !== JSON.stringify([1, 0, "Wide"]) || synced[10][0] !== 2)
        fail("the boot sync must read every send file per bus, got " + JSON.stringify(synced));
    const initAt = src.indexOf("    loadMasterFxChainFromConfig();\n");
    const syncAt = src.indexOf("syncSendUserPresetRecordsFromFiles();", initAt);
    if (initAt < 0 || syncAt < 0 || syncAt - initAt > 400)
        fail("init must sync the send records right after the master ones (loadMasterFxChainFromConfig)");
}
console.log("  ok  send records reload per set (16 positions, absent = cleared) and at boot");

/* ---- the picker / adopt sites use the bus on screen --------------------- */
/* The picker commits through commitMasterFxPick, shared with Delete+JogClick. */
const pick = grab("commitMasterFxPick");
if (!/clearFxBusUserPresetRecord\(currentFxBusIndex, comp\.index\)/.test(pick) || /fxBusIsMaster\(\)\) clear/.test(pick))
    fail("a swap in ANY bus editor must clear that bus record (commitMasterFxPick)");
if (!/clearFxBusUserPresetRecord\(currentFxBusIndex, at\.index\)/.test(grab("adoptMasterFxShimModule")))
    fail("adoptMasterFxShimModule clears the record of the bus it adopts into");
if (!/onReshape: \(op\) => \{ remapFxBusUserPresetRecords\(op\); \}/.test(src.match(/^const MASTER_CHAIN_TARGET = \{[^]*?^\};/m)[0]))
    fail("MASTER_CHAIN_TARGET.onReshape must re-key the records of the bus on screen");
console.log("  ok  picker swap / adopt / reshape act on the bus on screen");

/* ---- maybeReturnToFxBusComponentGrid, lifted and run -------------------- */
const recon = grab("maybeReturnToFxBusComponentGrid");
const run = (state) => {
    const s = Object.assign({ flag: true, key: "send1:fx2", mod: "tunnel", view: "mfx", bus: 1,
                              moduleNow: "tunnel", textEntry: false, enter: false }, state);
    const log = { entered: [] };
    const patched = recon
        .replace(/\bfxBusComponentModalFromGrid\b/g, "s.flag")
        .replace(/\bfxBusComponentGridReturnKey\b/g, "s.key")
        .replace(/\bfxBusComponentGridReturnModule\b/g, "s.mod")
        .replace(/\bcomponentGridReturnEnter\b/g, "s.enter")
        .replace(/\bcurrentFxBusIndex\b/g, "s.bus")
        .replace(/\bview\b/g, "s.view")
        .replace(/\bneedsRedraw\b/g, "s.redraw");
    const fn = new Function("s", "log", [
        "const VIEWS = { MASTER_FX: \"mfx\", PRESETS: \"presets\", PRESET_DETAIL: \"detail\" };",
        "const MASTER_CHAIN_TARGET = { slot: 0 };",
        "function isTextEntryActive() { return s.textEntry; }",
        "function fxBusOfComponentKey(k) { return k.startsWith(\"send1:\") ? 1 : k.startsWith(\"send2:\") ? 2 : 0; }",
        "function fxBusComponentModuleId() { return s.moduleNow; }",
        "function getComponentParamPrefix(k) { return k; }",
        "function componentParamPagesIo(slot, k) { return { io: slot + \":\" + k }; }",
        "function paramPagesChromeFor() { return { chrome: 1 }; }",
        "function enterParamPages(...a) { log.entered.push(a); }",
        patched, "return maybeReturnToFxBusComponentGrid;"].join("\n"))(s, log);
    return { fired: fn(), s, log };
};
let t = run({});
if (!t.fired || t.log.entered.length !== 1) fail("must fire on a MASTER_FX arrival with Send A on screen");
const a = t.log.entered[0];
if (a[0] !== 0 || a[1] !== "send1:fx2" || a[3] !== "My Presets" || a[4].io !== "0:send1:fx2" || a[6].enter !== false)
    fail("must re-enter the SEND grid on My Presets, got " + JSON.stringify(a));
t = run({ bus: 0 });
if (t.fired || t.s.flag !== false) fail("must not re-enter a Send A grid while the MASTER bus is on screen");
t = run({ bus: 2 });
if (t.fired) fail("must not re-enter a Send A grid while Send B is on screen");
t = run({ key: "master_fx:fx2", bus: 0 });
if (!t.fired || t.log.entered[0][1] !== "master_fx:fx2") fail("the master position still returns to its grid");
t = run({ view: "presets" });
if (t.fired || t.s.flag !== true) fail("must HOLD while the preset browser is up");
t = run({ moduleNow: "wettt" });
if (t.fired || t.s.flag !== false) fail("a swapped send position must not be re-entered as the old grid");
const pollAt = src.indexOf("if (view === VIEWS.CHAIN_EDIT) maybeReturnToComponentGrid();");
const busAt = src.indexOf("\n    maybeReturnToFxBusComponentGrid();", pollAt);
if (pollAt < 0 || busAt < 0 || busAt - pollAt > 1000) fail("maybeReturnToFxBusComponentGrid() must be polled at the reconcile site");
console.log("  ok  maybeReturnToFxBusComponentGrid: back to the send grid only with that send on screen");
'

# ---------------------------------------------------------------------------
# The boot restore reads send files with the SAME function (and so the same
# first-"state" parser pinned by test_master_fx_saved_state.c) as Master FX.
# ---------------------------------------------------------------------------
grep -q 'snprintf(path, sizeof(path), "%s/send_fx_%d_%d.json", boot_state_dir, sb, i);' src/host/shadow_chain_mgmt.c ||
    { echo "FAIL: send boot restore path moved" >&2; exit 1; }
awk '/send_fx_%d_%d.json/{f=1} f&&/fx_boot_restore_one\(path, &t, what\);/{ok=1; exit} END{exit !ok}' src/host/shadow_chain_mgmt.c ||
    { echo "FAIL: send files must be restored by fx_boot_restore_one (shared first-\"state\" parser)" >&2; exit 1; }
grep -q 'master_fx_saved_state_copy' src/host/shadow_chain_mgmt.c ||
    { echo "FAIL: fx_boot_restore_one no longer uses master_fx_saved_state_copy" >&2; exit 1; }
echo "  ok  send state files go through fx_boot_restore_one / master_fx_saved_state_copy"

echo "PASS: Send FX user presets"
