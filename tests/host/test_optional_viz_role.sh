#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# An OPTIONAL role that is not adjacent must be DROPPED, never fatal.
#
# detectFilter builds its slot run from cutoff, resonance AND whichever of
# mode/slope it found, then requires the whole run to be contiguous. So a mode
# knob parked at the far end of the page failed the adjacency check and took
# the corroborated cutoff/resonance pair down with it.
#
# schwung-filter is the case: cutoff and resonance on knobs 1 and 2, Mode on
# knob 8, run [0, 1, 7]. The module whose entire purpose is a filter drew two
# unrelated dials. 303 has no mode key on its root page at all and grouped
# correctly off the identical pair, which made this look like something
# specific to schwung-filter rather than what it was -- an optional role
# behaving like a required one.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/viz.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("node:fs"),
]).then(([V, M, fs]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));

  const groupsFor = (id, level) => {
    const mod = fx.modules.find((x) => x.id === id);
    if (!mod) fail("fixture has no module \"" + id + "\"");
    const lvl = (mod.ui_hierarchy && mod.ui_hierarchy.levels || {})[level];
    if (!lvl || !lvl.knobs) fail(id + " has no knobs on level " + level);
    const idx = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    const r = V.resolveViz({ keys: lvl.knobs, metaIndex: idx });
    return (r.groups || r || []);
  };
  const kinds = (gs) => gs.map((g) => g.kind);

  /* ---- the reported case ---------------------------------------------- */
  {
    const gs = groupsFor("filter", "root");
    const filt = gs.find((g) => g.kind === "filter");
    if (!filt)
      fail("schwung-filter root has cutoff on knob 1 and resonance on knob 2 and " +
           "still produced no filter group. Kinds: " + JSON.stringify(kinds(gs)));
    if (filt.roles.cutoff !== "cutoff" || filt.roles.resonance !== "resonance")
      fail("the filter group did not take the cutoff/resonance pair: " + JSON.stringify(filt.roles));
    /* Mode is on knob 8, four slots away -- dropped, not joined. */
    if (filt.roles.mode)
      fail("a mode four slots away joined the group: " + JSON.stringify(filt.roles));
    console.log("  ok  a far-away optional role is dropped, and the pair still groups");
  }

  /* ---- an ADJACENT optional still joins -------------------------------- */
  {
    const gs = groupsFor("surge", "filter1");
    const filt = gs.find((g) => g.kind === "filter");
    if (!filt) fail("surge filter 1 lost its group entirely");
    if (!filt.roles.mode)
      fail("surge declares filter1_type directly beside its cutoff/resonance and it " +
           "was dropped -- the fix went too far and optionals never join now");
    console.log("  ok  an adjacent optional role still joins the group");
  }

  /* ---- no stem check on the optionals ---------------------------------- *
   *
   * Tried, and wrong: noisemaker names its pair bare ("cutoff", "resonance")
   * and its mode "filter_type", so the stems disagree and the mode was
   * dropped from a group it plainly belongs to. Adjacency is what corroborates
   * an optional. */
  {
    const gs = groupsFor("noisemaker", "filter");
    const filt = gs.find((g) => g.kind === "filter");
    if (!filt) fail("noisemaker lost its filter group");
    if (!filt.roles.mode)
      fail("noisemaker names its pair bare and its mode \"filter_type\"; the mode was " +
           "dropped, so a stem check crept back onto the optional roles");
    console.log("  ok  a differently-stemmed but adjacent mode still joins");
  }

  /* ---- the required pair is still corroborated ------------------------- */
  {
    /* Built through buildMetaIndex rather than hand-rolled: resolveViz calls
     * getOrGuess, which a stub Map does not have, and a stub that happens to
     * satisfy today shape is one refactor from silently testing nothing. */
    const idx = M.buildMetaIndex({
      hierarchy: { levels: { root: { knobs: ["hp_cutoff", "eq_resonance"] } } },
      chainParams: [
        { key: "hp_cutoff", type: "float", min: 0, max: 1 },
        { key: "eq_resonance", type: "float", min: 0, max: 1 },
      ],
    });
    const r = V.resolveViz({ keys: ["hp_cutoff", "eq_resonance"], metaIndex: idx });
    const gs = (r.groups || r || []);
    if (gs.some((g) => g.kind === "filter"))
      fail("an unrelated hp_cutoff and eq_resonance grouped as a filter -- the stem " +
           "check on the REQUIRED pair was loosened too");
    console.log("  ok  the required pair still has to share a stem");
  }

  /* ---- envelopes: the longest ADJACENT RUN, not every role found -------
   *
   * Same rule, generalised. linein declares threshold/attack/release/range on
   * its Gate Settings knobs and leaves gate_hold undeclared, so the planner
   * appends it at the END: slots [1, 4, 2]. Requiring every role found to be
   * adjacent deleted an attack/release pair sitting side by side because of a
   * knob four positions away. */
  {
    const gs = groupsFor("linein", "gate_settings");
    const env = gs.find((g) => g.kind === "envelope");
    if (!env)
      fail("linein has gate_attack and gate_release adjacent and produced no envelope. " +
           "An undeclared gate_hold appended to the end of the page must not delete it.");
    if (env.roles.hold)
      fail("a hold two slots away joined the run: " + JSON.stringify(env.roles));
    console.log("  ok  a stray role far from the run does not delete the envelope");
  }

  /* ---- AHR is an envelope ---------------------------------------------- *
   *
   * gate and ducker declare attack/hold/release and nothing else. Hold was not
   * a role at all, so neither grouped -- and hold needs a word BOUNDARY, since
   * "threshold" ends in "hold" and gate declares one on the very same page. */
  for (const id of ["gate", "ducker"]) {
    const env = groupsFor(id, "root").find((g) => g.kind === "envelope");
    if (!env) fail(id + " declares attack/hold/release and produced no envelope group");
    if (env.roles.hold !== "hold")
      fail(id + ": hold is not bound to the hold knob: " + JSON.stringify(env.roles));
    if (/threshold/.test(String(env.roles.hold)))
      fail(id + ": the hold role bound to THRESHOLD -- /hold/ needs a boundary, " +
           "and the group would be drawn as a plateau whose height is a dB threshold");
  }
  console.log("  ok  attack/hold/release groups as an envelope, and hold is not threshold");

  console.log("PASS: an optional viz role is dropped, not fatal");
});
'
