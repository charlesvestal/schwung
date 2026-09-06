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
  /* THIS SCREEN IS CONTAINERS ONLY -- buses and New Bus. It carried a Send
     Mixer row too, so "New Bus" sat beside a mixer and the two ideas were one
     screen; the mixer is its own Sends row on Slot Settings now and holds Main.
     Neither a Main row nor a Sends row belongs here. */
  eq("no Main row", rows.some((r) => r.kind === "main"), false);
  eq("no Sends row", rows.some((r) => r.kind === "sends"), false);
  eq("containers only", rows.map((r) => r.kind).filter((k, i, a) => a.indexOf(k) === i)
     .every((k) => k === "bus" || k === "new"), true);
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
  eq("the list is as long as it can get", rows.length, M.SLOT_BUSES);
}

{
  /* A mixer with no faders is a row that answers a click by doing nothing, so
     an empty slot offers only New Bus. */
  const empty = M.parseBusesConfig(cfgJson([]));
  const rows = M.busListRows(empty, A2);
  eq("no buses, only New Bus", rows.map((r) => r.kind), ["new"]);
  /* MAIN SURVIVES A BUSLESS SLOT, and that is the point of moving it here: "the
     rest of the slot" is a send source every slot has, and most slots have no
     buses at all. It declared no params at all before, so the send mixer was
     unreachable for exactly the slots that needed it most. */
  eq("a busless slot still mixes Main", M.busSendGridParams(empty).map((p) => p.key),
     ["main_send1", "main_send2"]);
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
/* DERIVED FROM THE CAP, never a literal: raising SLOT_BUSES must not require
   editing the assertion that guards its edge, or the edge stops being tested. */
eq("...and the last one", M.busComponentKey(M.SLOT_BUSES - 1, M.BUS_FX_SLOTS - 1),
   `bus${M.SLOT_BUSES}:fx${M.BUS_FX_SLOTS}`);
eq("a bus past the cap has no key", M.busComponentKey(M.SLOT_BUSES, 0), null);
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
  /* MAIN plus the present buses, times the two sends. A hole is not a row. */
  eq("a send mixer has Main and one cell per present bus, per send",
     params.length, (2 + 1) * M.BUS_SENDS);
  eq("Main heads each send page, then the present buses",
     params.slice(0, 3).map((p) => p.name), ["Main", "Kick", "Hats"]);
  eq("Main appears once per send and no more",
     params.filter((p) => /^main_send/.test(p.key)).length, M.BUS_SENDS);
  eq("every cell is an int over the real range",
     params.every((p) => p.type === "int" && p.min === 0 && p.max === M.SEND_LEVEL_MAX),
     true);
  /* THE HOLE DOES NOT RENUMBER: the second present bus is bus 3, and its key
     must say 3. This is the one that edits the wrong bus when it is wrong.
     Indexed off the bus keys rather than off a fixed offset, so MAIN sitting at
     the head of each send page cannot silently shift what is being asserted. */
  const busKeys = params.filter((p) => /^bus/.test(p.key)).map((p) => p.key);
  eq("a bus keeps its own number", M.busSendGridRealKey(busKeys[1]), "bus3:send1");
  const busKeysB = params.filter((p) => /^bus\d+_send2$/.test(p.key)).map((p) => p.key);
  eq("send B is a different key", M.busSendGridRealKey(busKeysB[1]), "bus3:send2");
  /* MAIN IS A MIXER KEY NOW, and this pin is the reverse of what it said.
     It used to assert the spelling mapped to nothing, on the rule that the
     two levels of the slot itself were a SLOT fact edited in Slot Settings. That split
     the sends across two screens filed under two different ideas, and left the
     screen calling itself the send mixer without the source most slots use --
     turn every fader up on a slot with no buses and it still sent nothing.
     The key is unchanged; only where it is edited moved. */
  eq("Main is a mixer key", M.busSendGridRealKey("main_send1"), "buses:main_send1");
  eq("Main send B too", M.busSendGridRealKey("main_send2"), "buses:main_send2");
  eq("but only within range", M.busSendGridRealKey("main_send9"), null);
  eq("a key naming no send maps to nothing", M.busSendGridRealKey("volume"), null);
  eq("a bus past the cap maps to nothing", M.busSendGridRealKey("bus9_send1"), null);
  eq("a send past the cap maps to nothing", M.busSendGridRealKey("bus1_send3"), null);

  const h = M.busSendGridHierarchy(cfg);
  /* Root carries NO knobs: the planner names a walk root page "Main" whatever
     it declares, and "Main / Send B" is not a mixer. */
  eq("the root page is empty", h.levels.root.knobs.length, 0);
  eq("one level per send", [h.levels.send_a.label, h.levels.send_b.label],
     ["Send A", "Send B"]);
  eq("each send page is Main plus one knob per bus",
     [h.levels.send_a.knobs.length, h.levels.send_b.knobs.length], [3, 3]);
  eq("and Main is first on each", [h.levels.send_a.knobs[0], h.levels.send_b.knobs[0]],
     ["main_send1", "main_send2"]);
  /* AT THE CAP A SEND PAGE NO LONGER FITS: Main plus SLOT_BUSES faders is 9
     cells against 8 knobs. That is why enterBusSendsGrid asks
     (busCount + 1) > NUM_KNOBS rather than pinning paginate:false -- the ninth
     fader is the one that would otherwise have nowhere to go. */
  const full = M.parseBusesConfig(JSON.stringify({
    buses: Array.from({ length: M.SLOT_BUSES }, (_, i) => (
      { present: 1, name: "B" + i, orphans: 0, voices: [], sends: [0, 0], fx: [] })),
    main_sends: [0, 0] }));
  eq("a full slot is Main plus one cell per bus",
     M.busSendGridHierarchy(full).levels.send_a.knobs.length, M.SLOT_BUSES + 1);
  /* Stated as the INEQUALITY the grid has to satisfy, so raising SLOT_BUSES or
     the knob count re-answers it here rather than leaving a literal behind. */
  eq("...which is one more than the knobs can hold",
     M.SLOT_BUSES + 1 > 8, true);
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
  eq("the buses keep their positions", f.buses.map((b) => b.present),
     Array.from({ length: M.SLOT_BUSES }, (_, i) => (i === 0 || i === 2) ? 1 : 0));
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

/* ---- PER-VOICE SENDS ARE THE MODULE`S ----------------------------------- *
 *
 * They were the host`s, with a fader per voice on this mixer, a
 * "voice<N>_send<M>" grid key resolving to "buses:voice<V>:send<M>", and a
 * `voice_sends` array in the saved slot document. dr32 published the same
 * knobs on its own `pads` level, so the number had two homes. What is testable
 * here is that NOTHING of the host`s half is left behind: a second spelling
 * that still resolved would be a second way to set the same level, and the
 * winner would be whichever happened last.
 */
{
  const cfg = M.parseBusesConfig(JSON.stringify({
    buses: [
      { present: 1, name: "Hats", orphans: 0, voices: ["chh"], sends: [10, 0], fx: [] },
      { present: 0 }, { present: 0 }, { present: 0 },
    ],
    main_sends: [0, 0],
    /* An OLD document`s key. It must be ignored, not resurrected: the module`s
       own state blob is the authority now. */
    voice_sends: [{ id: "chh", sends: [20, 0] }],
  }));
  const voices = M.parseSplitVoices(JSON.stringify([
    { id: "bd", label: "Kick" }, { id: "chh", label: "CH" },
  ])).voices;

  eq("a stale voice_sends key is not parsed back in", cfg.voiceSends, undefined);
  eq("no voice level reader survives", typeof M.voiceSendValue, "undefined");
  eq("no voice grid key survives", typeof M.voiceSendGridKey, "undefined");
  eq("no voice row builder survives", typeof M.sendMixerVoiceRows, "undefined");

  /* THE SPELLING IS GONE. chain_bus.c no longer routes "voice<V>:send<M>", so a
     key that still resolved would be handed to a host that refuses it in
     silence -- a fader that moves and is never heard. */
  eq("voice grid key resolves to nothing", M.busSendGridRealKey("voice7_send2"), null);
  eq("bus real key unchanged", M.busSendGridRealKey("bus2_send1"), "bus2:send1");

  /* THE MIXER IS BUSES. Passing voices changes nothing -- the argument is gone,
     and a caller that still passes one must not resurrect a fader. */
  const params = M.busSendGridParams(cfg, voices);
  eq("mixer params are Main plus the buses", params.map((p) => p.key),
     ["main_send1", "bus1_send1", "main_send2", "bus1_send2"]);

  const h = M.busSendGridHierarchy(cfg, voices);
  eq("mixer levels", Object.keys(h.levels), ["root", "send_a", "send_b"]);
  eq("root carries no knobs", h.levels.root.knobs, []);
  eq("send_a is Main then the buses", h.levels.send_a.knobs, ["main_send1", "bus1_send1"]);
  for (const id of Object.keys(h.levels))
    if ("paginate" in h.levels[id]) fail("level " + id + " declares a paginate the planner never reads");

  /* THE DOOR CLOSES AGAIN FOR A BUSLESS SLOT. It opened for one while the
     per-voice faders lived here -- a splittable rack needs no bus -- and with
     those faders gone a mixer with nothing on it is a row that answers a click
     by doing nothing. */
  const busless = M.parseBusesConfig(JSON.stringify({
    buses: [{ present: 0 }, { present: 0 }, { present: 0 }, { present: 0 }],
    main_sends: [0, 0],
  }));
  eq("busless rows, voices or not",
     M.busListRows(busless, undefined, voices).map((r) => r.kind), ["new"]);
  /* A BUSLESS SLOT STILL HAS A MIXER, holding Main alone. It declared none while
     Main lived elsewhere, which was consistent then and is the bug now: "the
     rest of the slot" is a send source every slot has, and most slots have no
     buses at all. An UNRESOLVED config is still no mixer -- that distinction is
     the tri-state, and it is asserted just below. */
  const buslessH = M.busSendGridHierarchy(busless, voices);
  eq("a busless slot mixes Main alone", buslessH && buslessH.levels.send_a.knobs,
     ["main_send1"]);
  eq("an UNRESOLVED config still declares no mixer",
     M.busSendGridHierarchy({ unresolved: true }), null);
  eq("...and no params", M.busSendGridParams({ unresolved: true }), []);
  eq("unresolved declares nothing",
     M.busSendGridHierarchy({ unresolved: true }, voices), null);
  eq("unresolved declares no params",
     M.busSendGridParams({ unresolved: true, buses: [] }, voices), []);

  /* THE PRODUCER WRITES NO voice_sends. A second copy in the slot document
     would race the synth`s own state blob on the way back in. */
  const fields = M.busPatchFields(cfg);
  eq("producer key order", Object.keys(fields), ["main_sends", "buses"]);
}

if (failures) process.exit(1);
console.log("PASS: bus model — the tri-state read, positional buses, retained " +
            "orphans, one-bus-per-voice, and the knob grid keys (per-voice " +
            "sends belong to the module)");
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
