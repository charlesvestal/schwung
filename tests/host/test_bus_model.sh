#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE BUS MODEL: the tri-state read, the orphan carry, and the exclusivity of a
# voice.
#
# These three are the rules that cost something when they are wrong, and none of
# them is visible in a render:
#
#   - `synth:split_voices` has THREE answers. A read that did not COMPLETE must
#     not become "this module cannot split", because that answer is what removes
#     every bus affordance from the slot -- silently, and permanently once the
#     UI has acted on it. chain_host.c clamps a plugin's -1 to "" precisely so
#     the two cannot collide, so a null arriving here is a real channel failure.
#
#   - A bus stores voice IDS and RETAINS the ones that no longer resolve. Every
#     write to `bus<N>:voices` is a whole-list replace, so a list rebuilt from
#     only the resolvable voices would erase the very ids the orphan count
#     exists to report -- and the count would then be right about nothing.
#
#   - A voice renders into exactly ONE buffer. Adding a voice to a bus must
#     remove it from whichever other bus holds it, or the config claims a
#     routing the audio path cannot perform.
#
# Run under node against the shared module, which is pure for this reason: the
# screens that draw it (shadow_ui_buses.mjs) resolve their imports from
# /data/UserData/schwung and cannot be loaded here at all.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import * as M from "./src/shared/bus_model.mjs";
import fs from "node:fs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const eq = (what, got, want) => {
  const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) fail(what + ": got " + a + ", want " + b);
};

/* ---- the tri-state ---------------------------------------------------- */

/* null is NOT "cannot split". Nothing may act on it. */
eq("null split_voices is unresolved", M.parseSplitVoices(null).unresolved, true);
eq("undefined split_voices is unresolved", M.parseSplitVoices(undefined).unresolved, true);
/* "" IS "cannot split", and it is resolved -- the affordance is removed on
   this answer and only on this one. */
eq("empty split_voices is resolved", M.parseSplitVoices("").unresolved, false);
eq("empty split_voices has no voices", M.parseSplitVoices("").voices.length, 0);
{
  const p = M.parseSplitVoices(JSON.stringify(
    [{ id: "kick", label: "Kick" }, { id: "chh", label: "Closed Hat" }]));
  eq("a real list resolves", p.unresolved, false);
  eq("a real list keeps its ids", p.voices.map((v) => v.id), ["kick", "chh"]);
  eq("a real list keeps its labels", p.voices.map((v) => v.label), ["Kick", "Closed Hat"]);
}
/* A HOLE keeps its index. The table index IS the render-buffer index
   (split_voices_parse.h), so compacting an unusable entry away re-points every
   voice behind it at the wrong buffer -- silently. */
{
  const p = M.parseSplitVoices(JSON.stringify([{ id: "kick" }, { }, { id: "chh" }]));
  eq("a hole is counted", p.voices.length, 3);
  eq("a hole keeps its index", p.voices[2].id, "chh");
  eq("a hole has no id", p.voices[1].id, "");
}
/* Unparseable is not a failed READ -- the channel answered, with rubbish. It
   must not become a permanent retry. */
eq("garbage split_voices is resolved", M.parseSplitVoices("{oops").unresolved, false);

/* buses:config: a null or an unusable document is unresolved, and unresolved
   produces NO ROWS rather than an empty bus list -- an empty list is a claim
   about the slot and a failed read makes none. */
eq("null buses config is unresolved", M.parseBusesConfig(null).unresolved, true);
eq("garbage buses config is unresolved", M.parseBusesConfig("{oops").unresolved, true);
eq("unresolved config lists no rows", M.busListRows({ unresolved: true }).length, 0);

/* ---- rows ------------------------------------------------------------- */

const cfgJson = (buses, mainSends) => JSON.stringify({
  buses: Array.from({ length: M.SLOT_BUSES }, (_, b) => {
    const d = buses[b];
    return d
      ? { present: 1, name: d.name, orphans: d.orphans || 0, voices: d.voices || [],
          sends: d.sends || [0, 0],
          fx: Array.from({ length: M.BUS_FX_SLOTS }, (_, k) =>
            ({ module: (d.fx || [])[k] || "", bypassed: 0 })) }
      : { present: 0, name: "Bus " + (b + 1), orphans: 0, voices: [], sends: [0, 0],
          fx: Array.from({ length: M.BUS_FX_SLOTS }, () => ({ module: "", bypassed: 0 })) };
  }),
  main_sends: mainSends || [0, 0],
});
const A2 = (m) => String(m).slice(0, 2).toUpperCase();

{
  const cfg = M.parseBusesConfig(cfgJson(
    [{ name: "Kick", sends: [20, 0], fx: ["tapescam"] }, null,
     { name: "Snare", sends: [5, 5], fx: [] }], [1, 2]));
  const rows = M.busListRows(cfg, A2);
  /* POSITIONAL: a deleted bus 2 is a hole, and the buses behind it keep their
     indices. Compacting renumbers, which is the defect that lost the Master FX
     chain. */
  eq("a hole does not renumber", rows.filter((r) => r.kind === "bus").map((r) => r.index),
     [0, 2]);
  /* THERE IS NO MAIN ROW. Its two send levels were a wired, persisted,
     documented control that NO AUDIO PATH READ (chain_drain_sends says so), so
     the row is the door into the send mixer and carries no level of its own. */
  eq("no Main row", rows.some((r) => r.kind === "main"), false);
  eq("the Sends row is the door", rows[rows.length - 2].kind, "sends");
  eq("the Sends row carries no level", M.busRowValue(rows[rows.length - 2]), "");
  eq("a free bus offers New Bus", rows[rows.length - 1].kind, "new");
  eq("the hole is the next bus made", M.firstFreeBus(cfg), 1);
}
{
  /* At the cap there is no New Bus row -- a row that answers a click by doing
     nothing is what this repo does not ship. */
  const cfg = M.parseBusesConfig(cfgJson(
    Array.from({ length: M.SLOT_BUSES }, (_, i) => ({ name: "B" + i }))));
  const rows = M.busListRows(cfg, A2);
  eq("no New Bus at the cap", rows.some((r) => r.kind === "new"), false);
  eq("no free bus at the cap", M.firstFreeBus(cfg), -1);
  eq("the list is as long as it can get", rows.length, M.SLOT_BUSES + 1);
}

{
  /* A mixer with no faders is a row that answers a click by doing nothing, so
     an empty slot offers only New Bus. */
  const empty = M.parseBusesConfig(cfgJson([]));
  const rows = M.busListRows(empty, A2);
  eq("no buses, no Sends row", rows.map((r) => r.kind), ["new"]);
  eq("no buses, no mixer params", M.busSendGridParams(empty).length, 0);
}

/* The summary is COUNTED past two, because the value column carries both send
   levels as well and a third abbreviation pushes them off the row. */
eq("no inserts", M.insertSummary([], A2), "--");
eq("one insert", M.insertSummary([{ module: "tapescam" }], A2), "TA");
eq("two inserts", M.insertSummary([{ module: "cloudseed" }, { module: "psxverb" }], A2),
   "CL>PS");
eq("three inserts count", M.insertSummary(
   [{ module: "a" }, { module: "b" }, { module: "c" }], A2), "3 FX");

/* The orphan mark is on the LABEL, so it survives the value column being
   truncated -- the value is where the summary and the levels compete. */
{
  const cfg = M.parseBusesConfig(cfgJson([{ name: "Kick", orphans: 2 }]));
  eq("an orphaned bus is marked", M.busRowLabel(M.busListRows(cfg, A2)[0]), "Kick !");
  const clean = M.parseBusesConfig(cfgJson([{ name: "Kick" }]));
  eq("a clean bus is not", M.busRowLabel(M.busListRows(clean, A2)[0]), "Kick");
}

/* ---- voices ----------------------------------------------------------- */

const VOICES = M.parseSplitVoices(JSON.stringify(
  [{ id: "kick", label: "Kick" }, { id: "snare", label: "Snare" },
   { id: "chh", label: "Closed Hat" }])).voices;
const VCFG = M.parseBusesConfig(cfgJson([
  { name: "Kick", voices: ["kick", "gone1"], orphans: 1 },
  { name: "Hats", voices: ["chh"] }]));

{
  const rows = M.voiceRows(VCFG, VOICES, 0);
  eq("every declared voice is a row", rows.filter((r) => r.kind === "voice").length, 3);
  eq("this bus`s voice is marked mine", rows[0].mine, true);
  /* Another bus`s voice says WHOSE, so moving it is an informed choice. */
  eq("another bus`s voice is not mine", rows[2].mine, false);
  eq("another bus`s voice names the bus", M.voiceRowValue(rows[2], VCFG), "Hats");
  eq("a Main voice says nothing", M.voiceRowValue(rows[1], VCFG), "");
  /* THE ORPHAN IS A ROW. Its id is all that is left of it, and clearing it is
     the only thing that clears the count -- so it has to be reachable. */
  const orphans = rows.filter((r) => r.kind === "orphan");
  eq("the orphan is listed", orphans.map((r) => r.id), ["gone1"]);
  eq("the orphan is this bus`s", orphans[0].mine, true);
}

/* THE WRITE CARRIES THE ORPHANS. A whole-list replace rebuilt from only the
   resolvable voices erases exactly the ids the count reports. */
eq("adding keeps the orphan", M.toggledVoiceIds(VCFG, 0, "snare"),
   ["kick", "gone1", "snare"]);
eq("removing keeps the orphan", M.toggledVoiceIds(VCFG, 0, "kick"), ["gone1"]);
/* And toggling the orphan itself is how it goes. */
eq("the orphan can be cleared", M.toggledVoiceIds(VCFG, 0, "gone1"), ["kick"]);

/* EXCLUSIVITY: a voice renders into one buffer, so taking it means the other
   bus gives it up. */
eq("taking a voice frees the other bus", M.voiceMoveWrites(VCFG, 0, "chh"),
   [{ bus: 1, ids: [] }]);
eq("an unowned voice moves nothing", M.voiceMoveWrites(VCFG, 0, "snare"), []);
eq("the bus does not free itself", M.voiceMoveWrites(VCFG, 0, "kick"), []);

/* ---- the bus menu and the insert chain -------------------------------- */

{
  const rows = M.busListRows(VCFG, A2);
  const busItems = M.busActionItems(rows[0]).map((i) => i.id);
  eq("a bus offers everything", busItems,
     ["voices", "chain", "send1", "send2", "rename", "delete"]);
  /* ONLY a bus has a menu. The Sends row opens the mixer and New Bus creates;
     an action list for either would be rows that do nothing. */
  const sendsRow = rows.find((r) => r.kind === "sends");
  eq("the Sends row has no menu", M.busActionItems(sendsRow).length, 0);
  eq("New Bus has no menu", M.busActionItems({ kind: "new" }).length, 0);
  eq("send A reads the first level", M.busSendValue({ sends: [7, 9] }, "send1"), 7);
  eq("send B reads the second", M.busSendValue({ sends: [7, 9] }, "send2"), 9);
}

{
  /* An empty chain is ONE box -- the `+` -- not eight of nothing, which is the
     shape Master FX settled on for the same reason. */
  const none = M.busChainComponents([]);
  eq("an empty chain is one add box", none.map((c) => c.kind), ["add"]);
  /* A hole stays a hole: the config is positional and never compacted, so
     neither may the picture of it. */
  const holed = M.busChainComponents([{ module: "a" }, { module: "" }, { module: "c" }]);
  eq("a hole keeps its box", holed.map((c) => c.id),
     ["fx1", "fx2", "fx3", "add_fx"]);
  eq("the hole holds nothing", holed[1].module, "");
  /* At the cap there is no `+` -- there is nowhere for it to add. */
  const full = M.busChainComponents(
    Array.from({ length: M.BUS_FX_SLOTS }, () => ({ module: "x" })));
  eq("no add box at the cap", full.some((c) => c.kind === "add"), false);
  eq("the full chain is the cap", full.length, M.BUS_FX_SLOTS);
}

/* ---- the knob grid ----------------------------------------------------- */

/* A bus insert component key IS its DSP prefix, which is what lets the knob
   grid address one with no mapping of its own. Both directions, and both
   bounds: an out-of-range key routed as a real position lands on whatever the
   chain host does with an unmatched key. */
eq("a bus insert key is its prefix", M.busComponentKey(0, 1), "bus1:fx2");
eq("...and the last one", M.busComponentKey(3, 7), "bus4:fx8");
eq("a bus past the cap has no key", M.busComponentKey(4, 0), null);
eq("a position past the cap has no key", M.busComponentKey(0, 8), null);
eq("the key parses back", M.parseBusComponentKey("bus1:fx2"), { bus: 0, fx: 1 });
eq("a key past the cap does not parse", M.parseBusComponentKey("bus9:fx1"), null);
eq("a slot chain key is not a bus key", M.parseBusComponentKey("fx2"), null);
eq("a master key is not a bus key", M.parseBusComponentKey("master_fx:fx2"), null);
{
  const roundTrip = M.parseBusComponentKey(M.busComponentKey(2, 4));
  eq("the two spellings agree", roundTrip, { bus: 2, fx: 4 });
}

/* THE SEND MIXER contract. The mapping to the two REAL spellings is the
   whole of what its io does, and getting it wrong edits the wrong bus
   silently -- the same hazard busSendKey exists to prevent on the list path. */
{
  const cfg = M.parseBusesConfig(JSON.stringify({
    buses: [
      { present: 1, name: "Kick", orphans: 0, voices: [], sends: [20, 0], fx: [] },
      { present: 0, name: "Bus 2", orphans: 0, voices: [], sends: [0, 0], fx: [] },
      { present: 1, name: "Hats", orphans: 0, voices: [], sends: [5, 9], fx: [] },
      { present: 0, name: "Bus 4", orphans: 0, voices: [], sends: [0, 0], fx: [] }],
    main_sends: [3, 4] }));
  const params = M.busSendGridParams(cfg);
  /* The present buses, times the two sends. A hole is not a row, and neither
     is Main: the slot`s own two levels have no reader in the audio path. */
  eq("a send mixer has one cell per bus per send", params.length, 4);
  eq("only the present buses are faders",
     params.slice(0, 2).map((p) => p.name), ["Kick", "Hats"]);
  eq("no main_send key is offered",
     params.some((p) => /main/.test(p.key)), false);
  eq("every cell is an int over the real range",
     params.every((p) => p.type === "int" && p.min === 0 && p.max === M.SEND_LEVEL_MAX),
     true);
  /* THE HOLE DOES NOT RENUMBER: the second present bus is bus 3, and its key
     must say 3. This is the one that edits the wrong bus when it is wrong. */
  eq("a bus keeps its own number", M.busSendGridRealKey(params[1].key), "bus3:send1");
  eq("send B is a different key", M.busSendGridRealKey(params[3].key), "bus3:send2");
  /* And the retired spelling maps to nothing at all: a grid key that still
     said "main_send1" would write a level nothing reads. */
  eq("the retired Main spelling is gone", M.busSendGridRealKey("main_send1"), null);
  eq("a key naming no send maps to nothing", M.busSendGridRealKey("volume"), null);
  eq("a bus past the cap maps to nothing", M.busSendGridRealKey("bus9_send1"), null);
  eq("a send past the cap maps to nothing", M.busSendGridRealKey("bus1_send3"), null);

  const h = M.busSendGridHierarchy(cfg);
  /* Root carries NO knobs: the planner names a walk root page "Main" whatever
     it declares, and "Main / Send B" is not a mixer. */
  eq("the root page is empty", h.levels.root.knobs.length, 0);
  eq("one level per send", [h.levels.send_a.label, h.levels.send_b.label],
     ["Send A", "Send B"]);
  eq("each send page is one knob per bus",
     [h.levels.send_a.knobs.length, h.levels.send_b.knobs.length], [2, 2]);
  /* Bounded BY CONSTRUCTION at SLOT_BUSES cells, which is why the page is
     handed paginate:false rather than being allowed to split. */
  const full = M.parseBusesConfig(JSON.stringify({
    buses: [0, 1, 2, 3].map((i) => (
      { present: 1, name: "B" + i, orphans: 0, voices: [], sends: [0, 0], fx: [] })),
    main_sends: [0, 0] }));
  eq("a full slot is one cell per bus",
     M.busSendGridHierarchy(full).levels.send_a.knobs.length, M.SLOT_BUSES);
}
/* AND THE READ THAT DID NOT COMPLETE MAKES NO CONTRACT. An empty one would be
   a claim -- "this slot has no buses" -- drawn as a mixer with no faders.

   The PARAMS half is upheld by busListRows` refusal, not by a second copy of it
   inside busSendGridParams: a duplicate guard there was unkillable, because the
   rows were already empty when it ran, so this assertion passed for a reason
   other than the one it names. Mutating busListRows` `|| config.unresolved`
   away now kills BOTH lines below. */
eq("an unresolved config declares no hierarchy",
   M.busSendGridHierarchy({ unresolved: true }), null);
eq("an unresolved config declares no params",
   M.busSendGridParams({ unresolved: true }).length, 0);
eq("an unresolved config lists no rows either -- the one refusal",
   M.busListRows({ unresolved: true }).length, 0);

/* ---- THE TWO SPELLINGS OF A SEND KEY --------------------------------- */

/* busSendKey (shadow_ui.js, the LIST path) and busSendGridRealKey (here, the
   GRID path) produce the same two real keys from different arguments -- a row
   object and a flat grid key. A comment saying they must agree, with nothing
   joining them, is the duplication it claims to have closed, so busSendKey is
   LIFTED out of shadow_ui.js and the two are run against every row of a slot.
   Getting this wrong edits the wrong bus, silently. */
{
  const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
  const at = src.indexOf("function busSendKey(");
  if (at < 0) fail("busSendKey is gone from shadow_ui.js");
  else {
    const end = src.indexOf("\n}\n", at);
    const busSendKey = new Function(
      "return " + src.slice(at, end + 2))();
    const cfg = M.parseBusesConfig(JSON.stringify({
      buses: [
        { present: 1, name: "Kick", orphans: 0, voices: [], sends: [0, 0], fx: [] },
        { present: 0, name: "Bus 2", orphans: 0, voices: [], sends: [0, 0], fx: [] },
        { present: 1, name: "Hats", orphans: 0, voices: [], sends: [0, 0], fx: [] },
        { present: 1, name: "Perc", orphans: 0, voices: [], sends: [0, 0], fx: [] }],
      main_sends: [0, 0] }));
    const rows = M.busListRows(cfg).filter((r) => r.kind === "bus");
    let checked = 0;
    for (const row of rows) {
      for (let n = 1; n <= M.BUS_SENDS; n++) {
        const viaGrid = M.busSendGridRealKey(M.sendGridKey(row, n));
        const viaList = busSendKey(row, "send" + n);
        eq("the two paths agree on " + row.name + " send " + n, viaGrid, viaList);
        checked++;
      }
    }
    /* Every present bus plus Main, both sends -- and the HOLE at bus 2 is what
       makes this worth running: a path that renumbered would send Hats` level
       to bus 2. */
    eq("every row of a holed slot was compared", checked,
       (rows.length) * M.BUS_SENDS);
    eq("...and the hole did not renumber",
       M.busSendGridRealKey(M.sendGridKey(rows[1], 1)), "bus3:send1");
  }
}


/* ---- A DUPLICATE VOICE ID: THE HIGHEST BUS WINS ----------------------- */

/* Reachable whenever a voiceMoveWrites removal fails (it is a blocking write
   that can be refused) or from an externally authored patch. The screen said
   one bus and the audio used another, silently, because voiceRows took the
   FIRST claimant while the C applies every bus in ascending order into one map
   with an unconditional store -- so the LAST one wins. Pinned here against the
   C, not against itself. */
{
  const dup = M.parseBusesConfig(cfgJson([
    { name: "Kick", voices: ["kick"] }, null,
    { name: "Hats", voices: ["kick"] }]));
  const rows = M.voiceRows(dup, VOICES, 0);
  const kick = rows.find((r) => r.id === "kick");
  eq("a duplicated voice belongs to the HIGHEST bus", kick.on, 2);
  eq("...so it is not this bus`s", kick.mine, false);
  eq("...and the row names the bus that has it", M.voiceRowValue(kick, dup), "Hats");
  eq("the bus that does have it says so", M.voiceRows(dup, VOICES, 2)
     .find((r) => r.id === "kick").mine, true);
}
{
  /* THE C RULE THIS MIRRORS. bus_voice_apply stores unconditionally and
     chain_bus_rebuild_voice_map applies the buses ascending, so the last write
     stands. Either half changing turns the JS rule above into a lie. */
  const apply = fs.readFileSync("src/host/bus_voice_apply.h", "utf8");
  if (!/voice_bus\[idx\] = \(int8_t\)bus;/.test(apply))
    fail("bus_voice_apply no longer stores the bus index the way this mirrors");
  if (/if\s*\(\s*voice_bus\[idx\]/.test(apply))
    fail("bus_voice_apply now branches on the existing owner -- the JS rule (highest wins) must move with it");
  const cbus = fs.readFileSync("src/modules/chain/dsp/chain_bus.c", "utf8");
  const at = cbus.indexOf("void chain_bus_rebuild_voice_map(");
  const body = cbus.slice(at, cbus.indexOf("\n}\n", at));
  if (!/for \(int b = 0; b < SLOT_BUSES; b\+\+\)/.test(body))
    fail("chain_bus_rebuild_voice_map no longer walks the buses ASCENDING -- which bus wins a duplicate id changes with it");
}

/* ---- THE PRODUCER: the half of the file format that did not exist ------ */

/* chain_patch.c has read "buses"/"main_sends" out of a saved slot since the
   feature landed and NOTHING EMITTED THEM, so every load reset all four buses
   and destroyed a live kit in silence. The end-to-end assertion (this producer
   feeding the real C parser) is tests/host/test_chain_patch_roundtrip.sh; what
   is pinned here is the SHAPE and the WIRING. */
{
  eq("an unresolved config produces no document", M.busPatchFields({ unresolved: true }), null);

  const cfg = M.parseBusesConfig(cfgJson(
    [{ name: "Kick", voices: ["kick"], sends: [20, 0], fx: ["tapescam"] }, null,
     { name: "Hats", voices: ["chh"], sends: [0, 15], fx: ["chorus"] }], [5, 30]));
  const f = M.busPatchFields(cfg, (b, k) => (b === 0 && k === 0 ? { drive: 0.5 } : undefined));

  /* POSITIONAL. A hole is {"present":0} and never a compaction: bus 2 must
     still parse back as bus 2. */
  eq("every bus has an entry", f.buses.length, M.SLOT_BUSES);
  eq("a hole is present:0 and nothing else", f.buses[1], { present: 0 });
  eq("the buses keep their positions", f.buses.map((b) => b.present), [1, 0, 1, 0]);
  eq("the slot sends ride along", f.main_sends, [5, 30]);

  /* KEY ORDER IS LOAD-BEARING: bus_field takes the FIRST hit inside the
     object`s span, and an insert`s opaque state is inside that span. "name"
     before "fx", and "module"/"bypassed" before "state". */
  eq("a bus names itself before its inserts", Object.keys(f.buses[0]),
     ["present", "name", "voices", "sends", "fx"]);
  eq("an insert names itself before its state", Object.keys(f.buses[0].fx[0]),
     ["module", "bypassed", "state"]);
  eq("the state is carried", f.buses[0].fx[0].state, { drive: 0.5 });
  /* ABSENT, not null or "": the parser reads a state that is neither an object
     nor a string as no state, and an empty one staged over a running insert
     would wipe its parameters on the next load. */
  eq("no state, no key", "state" in f.buses[2].fx[0], false);
  eq("the trailing holes are not emitted", f.buses[0].fx.length, 1);
}

/* AND IT IS WIRED. bus_model can be perfect and unreferenced -- which is the
   exact shape of the defect this fixes, a reader with no writer. */
{
  const src = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
  const at = src.indexOf("function buildSlotPatchJson(");
  const body = at < 0 ? "" : src.slice(at, src.indexOf("\n}\n", at));
  if (at < 0) fail("buildSlotPatchJson is gone");
  if (!/BusModel\.busPatchFields\(/.test(body))
    fail("buildSlotPatchJson does not call busPatchFields -- a saved slot with no \"buses\" key WIPES the buses on load");
  if (!/patch\.buses\s*=/.test(body) || !/patch\.main_sends\s*=/.test(body))
    fail("buildSlotPatchJson does not assign patch.buses / patch.main_sends");
  /* Both keys are DECLARED in the initial literal so they stringify AHEAD of
     every opaque state blob: bus_parse_section scans the whole document and
     takes the first hit. */
  const lit = body.slice(body.indexOf("const patch = {"), body.indexOf("audio_fx: []"));
  if (!/main_sends: undefined/.test(lit) || !/buses: undefined/.test(lit))
    fail("patch.buses/main_sends are no longer declared ahead of the components -- a module state carrying either key would answer for the slot");
  /* A FAILED READ MUST NOT PRODUCE A DOCUMENT. It is not a missing field, it
     is a document that deletes the user`s buses on the next load. */
  const guard = body.indexOf("busCfg.unresolved");
  if (guard < 0 || body.indexOf("return null", guard) < 0)
    fail("an unresolved buses:config no longer bails the save");
}

/* ---- PER-VOICE SENDS --------------------------------------------------- *
 *
 * The superset: a voice send is taken from the voice`s own PRE-INSERT audio,
 * a bus send from the bus`s POST-INSERT sum, and they land in the same two
 * accumulators. What is testable here is the CONTRACT that reaches the DSP --
 * the spellings, which faders are offered, and what survives a save.
 */
{
  const cfg = M.parseBusesConfig(JSON.stringify({
    buses: [
      { present: 1, name: "Hats", orphans: 0, voices: ["chh"], sends: [10, 0], fx: [] },
      { present: 0 }, { present: 0 }, { present: 0 },
    ],
    main_sends: [0, 0],
    voice_sends: [{ id: "chh", sends: [20, 0] },
                  { id: "bd", sends: [0, 0] },
                  { id: "gone", sends: [77, 77] }],
  }));
  const voices = M.parseSplitVoices(JSON.stringify([
    { id: "bd", label: "Kick" }, { id: "chh", label: "CH" },
  ])).voices;

  /* A ZERO entry is a REAL answer and is kept. Dropping it would spring a
     fader the user just pulled down back to its old value. */
  eq("zero voice send is carried", M.voiceSendValue(cfg, "bd", 1), 0);
  eq("voice send A", M.voiceSendValue(cfg, "chh", 1), 20);
  eq("voice send B", M.voiceSendValue(cfg, "chh", 2), 0);
  /* A voice the config says nothing about is zero, not unknown: an absent
     entry IS a level of zero, and that is what the DSP answers too. */
  eq("unknown voice is zero", M.voiceSendValue(cfg, "nobody", 1), 0);

  /* THE SPELLINGS. A voice key carries the "buses:" prefix because
     chain_host.c routes "bus<N>:" to a bus and "buses:" to the slot -- a bare
     "voice7:send1" matches neither and is handed to the synth plugin, i.e. a
     write to somebody else`s parameter. */
  eq("voice grid key", M.voiceSendGridKey(6, 2), "voice7_send2");
  eq("voice real key", M.busSendGridRealKey("voice7_send2"), "buses:voice7:send2");
  eq("bus real key unchanged", M.busSendGridRealKey("bus2_send1"), "bus2:send1");
  /* Bounded at the caps, both ends, both kinds. An out-of-range key must
     answer null rather than reach a chain host that would refuse it in
     silence -- the caller can then draw nothing instead of a dead fader. */
  eq("voice past the cap", M.busSendGridRealKey("voice33_send1"), null);
  eq("voice zero", M.busSendGridRealKey("voice0_send1"), null);
  eq("voice send past the cap", M.busSendGridRealKey("voice1_send3"), null);
  eq("not a send", M.busSendGridRealKey("voice1_gain"), null);

  /* THE MIXER OFFERS EVERY DECLARED VOICE, not only the ones with a level:
     built from voice_sends instead, a pad would get a fader only after you had
     already found some other way to set one. */
  const params = M.busSendGridParams(cfg, voices);
  eq("mixer params", params.map((p) => p.key),
     ["bus1_send1", "bus1_send2",
      "voice1_send1", "voice2_send1", "voice1_send2", "voice2_send2"]);
  eq("voice fader is named for the voice",
     params.filter((p) => p.key === "voice1_send1").map((p) => p.name), ["Kick"]);
  /* An ORPHAN ("gone") gets no fader: it names no voice the module declares,
     so there is no cell it could belong to. It is still in the config and is
     still saved -- see the producer check below. */
  eq("orphan gets no fader",
     params.filter((p) => /^voice/.test(p.key)).length, 4);

  /* FOUR LEVELS, and buses and voices are never merged into one. Pagination is
     a whole-CONTRACT switch, so one merged level would force the four bus
     faders to page along with up to thirty-two voice ones -- and a row of
     cells that changed meaning halfway along (post-insert to pre-insert) would
     be a worse screen than two honest ones. */
  const h = M.busSendGridHierarchy(cfg, voices);
  eq("mixer levels", Object.keys(h.levels),
     ["root", "send_a", "send_b", "voice_a", "voice_b"]);
  eq("root carries no knobs", h.levels.root.knobs, []);
  eq("send_a is buses", h.levels.send_a.knobs, ["bus1_send1"]);
  eq("voice_a is voices", h.levels.voice_a.knobs, ["voice1_send1", "voice2_send1"]);
  /* NO PER-LEVEL `paginate`. The planner takes it once for the whole contract,
     so a flag written on a level would be read by nobody -- a promise the
     planner never made. */
  for (const id of Object.keys(h.levels))
    if ("paginate" in h.levels[id]) fail("level " + id + " declares a paginate the planner never reads");

  /* A LEVEL WITH NO KEYS IS OMITTED, not emitted empty. A module that cannot
     split gets exactly the two bus pages that shipped before. */
  const noVoices = M.busSendGridHierarchy(cfg, []);
  eq("no voices, no voice levels", Object.keys(noVoices.levels),
     ["root", "send_a", "send_b"]);

  /* THE REFERENCE CASE: a splittable rack with NO BUS AT ALL. Every level is
     per-voice, and the DOOR has to open -- before per-voice sends the Sends row
     appeared only when a bus existed, so this whole screen was unreachable for
     exactly the module the feature is for. */
  const busless = M.parseBusesConfig(JSON.stringify({
    buses: [{ present: 0 }, { present: 0 }, { present: 0 }, { present: 0 }],
    main_sends: [0, 0], voice_sends: [],
  }));
  eq("busless rows without voices",
     M.busListRows(busless, undefined, []).map((r) => r.kind), ["new"]);
  eq("busless rows with voices",
     M.busListRows(busless, undefined, voices).map((r) => r.kind), ["sends", "new"]);
  const bh = M.busSendGridHierarchy(busless, voices);
  eq("busless mixer is voices only", Object.keys(bh.levels),
     ["root", "voice_a", "voice_b"]);
  /* Nothing to ride at all is not a mixer. Reachable: the last bus can be
     deleted from the screen behind this one. */
  eq("nothing to ride is no contract", M.busSendGridHierarchy(busless, []), null);
  /* An unresolved config declares no mixer, voices or not: a read that did not
     complete is not "this slot has no buses". */
  eq("unresolved declares nothing",
     M.busSendGridHierarchy({ unresolved: true }, voices), null);
  eq("unresolved declares no params",
     M.busSendGridParams({ unresolved: true, buses: [] }, voices), []);

  /* THE PRODUCER carries every entry VERBATIM, orphan and zero alike. An id
     that does not resolve right now is an ORPHAN, not a deletion, and the chain
     host retains and counts it -- filtering here is how a level is lost by
     saving while the module is still loading. */
  const fields = M.busPatchFields(cfg);
  eq("producer carries every voice send",
     fields.voice_sends.map((e) => e.id + ":" + e.sends.join(",")),
     ["chh:20,0", "bd:0,0", "gone:77,77"]);
  /* KEY ORDER: main_sends and voice_sends before buses, because
     bus_parse_section scans the WHOLE document for the first two. */
  eq("producer key order", Object.keys(fields),
     ["main_sends", "voice_sends", "buses"]);
}

if (failures) process.exit(1);
console.log("PASS: bus model — the tri-state read, positional buses, retained " +
            "orphans, one-bus-per-voice, per-voice sends, and the knob grid keys");
'

# THE CAPS ARE MIRRORS, and a mirror that drifts is worse than a duplicate: the
# UI would offer a fifth bus the DSP has no slot for, or hide an eighth insert
# the DSP is running. Derived from the C headers rather than restated, the way
# test_master_fx_slots_js.sh derives MASTER_FX_SLOTS.
fail=0
c_val() { sed -n "s/^#define $2 \([0-9]*\).*/\1/p" "$1" | head -1; }
js_val() { sed -n "s/^export const $2 = \([0-9]*\);.*/\1/p" src/shared/bus_model.mjs | head -1; }

check() {   # <name-in-js> <name-in-c> <c-header>
  local js c
  js=$(js_val "" "$1"); c=$(c_val "$3" "$2")
  if [ -z "$js" ] || [ -z "$c" ]; then
    echo "FAIL: could not read $1 (js='$js') or $2 (c='$c')" >&2; fail=1; return
  fi
  if [ "$js" != "$c" ]; then
    echo "FAIL: $1 is $js in bus_model.mjs but $2 is $c in $3" >&2; fail=1
  fi
}
check SLOT_BUSES     SLOT_BUSES             src/modules/chain/dsp/chain_internal.h
check BUS_FX_SLOTS   MAX_AUDIO_FX           src/modules/chain/dsp/chain_internal.h
check BUS_SENDS      BUS_MIX_SENDS          src/host/bus_mix.h
check SEND_LEVEL_MAX BUS_MIX_SEND_LEVEL_MAX src/host/bus_mix.h
check SPLIT_VOICES_MAX SPLIT_VOICES_MAX     src/modules/chain/dsp/chain_internal.h
[ "$fail" = 0 ] || exit 1
echo "PASS: bus model caps match the C constants they mirror"

# The producer's OUTPUT must reach the document, not merely be computed.
#
# This branch shipped a file format with a reader and no writer: chain_patch.c
# parsed "buses", nothing emitted it, and every patch load therefore reset all
# four buses and zeroed the send levels — a two-bus kit silently destroyed by
# loading any preset. The unit tests above did not catch it because they
# exercise busPatchFields in ISOLATION, and a source pin on the CALL does not
# either: discarding the result (`const fields = {}` while still calling) is
# byte-for-byte the original bug and leaves both suites green.
#
# So pin the assignments, not the call.
src=src/shadow/shadow_ui.js
for f in main_sends buses; do
  if ! grep -qE "patch\.$f = fields\.$f;" "$src"; then
    echo "FAIL: buildSlotPatchJson does not write patch.$f from the producer's result" >&2
    echo "      (a computed-but-discarded producer is exactly the bug this pins)" >&2
    exit 1
  fi
done
echo "PASS: the bus producer output reaches the saved document"
