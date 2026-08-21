#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# What an LFO's target is CALLED (src/shared/lfo_target_label.mjs).
#
# The routing is stored as two internal keys and every surface used to print
# them raw — "fx1:room_size" in the list, "FX" in a 30px grid cell — while the
# picker that SET the routing had already resolved the same pair to "FX 1:
# Freeverb" and "Room Size". This module is that mapping, and it is pure: it
# takes the two arrays the picker builds and does no lookups of its own,
# because resolving a component costs several ~2.8ms IPC round trips and when
# to spend those is the caller's decision.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the LFO target label tests" >&2
  exit 1
fi

node -e '
import("./src/shared/lfo_target_label.mjs").then((L) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const eq = (got, want, what) => {
    if (got !== want) fail(what + ": expected " + JSON.stringify(want) + ", got " + JSON.stringify(got));
  };

  const components = [
    { key: "synth", label: "Synth: Braids" },
    { key: "fx1",   label: "FX 1: Freeverb" },
    { key: "midi_fx1", label: "MIDI FX 1: Arp" },
    { key: "lfo2",  label: "LFO 2" },
  ];
  const params = [
    { key: "room_size", label: "Room Size" },
    { key: "damping",   label: "Damping" },
  ];

  /* ---- 1. the three forms, each sized for its surface ------------------ */
  {
    const d = L.describeLfoTarget({ target: "fx1", targetParam: "room_size", components, params });
    eq(d.short, "Room Size",           "the grid cell gets the param alone");
    /* The MODULE, never the slot. Which slot you are looking at is the one
     * thing already on screen; which module is loaded there is not. */
    eq(d.long,  "Freeverb: Room Size", "anywhere with room names the module");
    if ("header" in d) fail("there is no separate header form any more");
    if (d.empty) fail("a routed LFO is not empty");
  }

  /* ---- 2. no routing --------------------------------------------------- */
  for (const missing of [
      { target: "", targetParam: "room_size" },
      { target: "fx1", targetParam: "" },
      {},
  ]) {
    const d = L.describeLfoTarget({ ...missing, components, params });
    if (!d.empty) fail("an unrouted LFO should report empty: " + JSON.stringify(missing));
    eq(d.long, L.NO_TARGET, "an unrouted LFO reads None");
  }

  /* ---- 3. a component label with no separator is both parts ------------ */
  {
    const d = L.describeLfoTarget({ target: "lfo2", targetParam: "depth", components, params: [] });
    eq(d.long, "LFO 2: Depth", "a label with no separator is the module name itself");
  }

  /* ---- 4. an undeclared param falls back to the key, made readable -----
   *
   * Reached when the routing outlives the contract that named it. It must not
   * print the raw token and must not claim a name the module never gave. */
  {
    const d = L.describeLfoTarget({ target: "fx1", targetParam: "pre_delay_ms", components, params });
    eq(d.short, "Pre Delay Ms", "an undeclared param is prettified from its key");
  }

  /* ---- 5. a component that is no longer on offer ----------------------
   *
   * A module swapped out from under a routing. The routing is still stored and
   * still drives that param if the module returns, so it must say WHICH — not
   * "None", which would read as unrouted. */
  {
    const d = L.describeLfoTarget({ target: "fx2", targetParam: "mix", components, params: [] });
    if (d.empty) fail("a stale routing is not the same as no routing");
    eq(d.long, "Fx2: Mix", "a vanished component still names itself");
  }

  /* ---- 6. a declared label always wins over the derived one ------------ */
  {
    const derived  = L.describeLfoTarget({ target: "fx1", targetParam: "room_size",
                                           components, params: [] });
    const declared = L.describeLfoTarget({ target: "fx1", targetParam: "room_size", components,
                                           params: [{ key: "room_size", label: "Size" }] });
    eq(derived.short,  "Room Size", "with nothing declared, the key is prettified");
    eq(declared.short, "Size",      "a declared label must override the derived one");
  }

  console.log("PASS: LFO target label — two forms, unrouted, stale routing, "
            + "undeclared params fall back to a readable key");
});
'
