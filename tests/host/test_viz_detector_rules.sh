#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Three detector rules, each pinned by the case that motivated it AND by the
# case it must keep refusing.
#
# All three were found the same way: rendering every page of every fleet module
# and looking at them. Each is a graphic that should have been drawn and was
# not, and in every case the module was blameless -- it had spelled a unit, an
# abbreviation, or a second time base, and the detector took that for a
# different subsystem.
#
# The negative half of each pair is the point. A detector that says yes more
# often is not better; it is only better if it still says no to the thing next
# door, which is why every block below asserts both.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/viz.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
]).then(([V, M]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* A page of params -> the group kinds detected over it. */
  const kinds = (params) => {
    const ix = M.buildMetaIndex({ chainParams: params });
    const { groups } = V.resolveViz({ keys: params.map((p) => p.key), metaIndex: ix });
    return groups.map((g) => g.kind);
  };
  const has = (params, kind) => kinds(params).includes(kind);

  const F = (key, extra) => Object.assign({ key, name: key, type: "float", min: 0, max: 1, step: 0.01 }, extra || {});

  /* ---- 1. a dB level is a fader; a trim and a balance are not ---------- */
  {
    /* usefulity: Gain -100..35. The floor is silence, not a negative half. */
    if (!has([F("gain_db", { min: -100, max: 35 })], "fader"))
      fail("a dB gain with a silence floor should draw as a fader");
    /* ottx declares the unit instead of spelling it in the key. */
    if (!has([F("in_gain", { min: -60, max: 30, unit: "dB" })], "fader"))
      fail("unit dB should be honoured, not just a _db suffix");

    /* The three shapes that must STAY knobs. */
    if (has([F("level_pfg", { min: -48, max: 48 })], "fader"))
      fail("a symmetric +-48 is a trim around unity, not a fader");
    if (has([F("lf_gain", { min: -15, max: 15, unit: "dB" })], "fader"))
      fail("an EQ band gain is a trim; it must stay available to the EQ detector");
    if (has([F("volume", { min: -1, max: 1, unit: "%" })], "fader"))
      fail("a bipolar % volume is a balance, not a level");
    /* Unipolar is unaffected -- the original rule, still doing its job. */
    if (!has([F("level", { min: 0, max: 1 })], "fader"))
      fail("a plain unipolar level stopped being a fader");
  }

  /* ---- 2. `spd` is a rate ---------------------------------------------- */
  {
    const wave = { key: "vlfo1_wave", name: "Wave", type: "enum",
                   options: ["Tri", "Sin", "Sqr", "Saw"] };
    /* schwung-work: four LFOs, all spelled spd, all drawn as loose dials. */
    const abbreviated = [F("vlfo1_spd", { min: 0, max: 127, type: "int" }),
                         wave,
                         F("vlfo1_depth", { min: 0, max: 127, type: "int" })];
    if (!has(abbreviated, "lfo")) fail("spd should be recognised as a rate");
    /* The unabbreviated spelling still works. Its shape param has to share the
     * stem, or it sits BETWEEN rate and depth as an outsider and the run is no
     * longer adjacent -- which is a real constraint (a graphic occupies
     * consecutive cells), and it is what this test got wrong the first time. */
    const spelled = [F("lfo_speed"),
                     { key: "lfo_wave", name: "Wave", type: "enum", options: ["Tri", "Sin", "Saw"] },
                     F("lfo_depth")];
    if (!has(spelled, "lfo")) fail("speed stopped being a rate");
  }

  /* ---- 3. a unit qualifier is not a different subsystem ---------------- */
  {
    /* granny page 5 is NAMED "ADSR Envelope" and drew four faders: the _ms
     * suffix on three of the four roles broke stem agreement, and amp_ matched
     * the fader vocabulary. */
    const ms = [F("amp_attack_ms"), F("amp_decay_ms"), F("amp_sustain"), F("amp_release_ms")];
    if (!has(ms, "envelope"))
      fail("an ADSR whose times carry a _ms unit should still be one envelope");
    if (has(ms, "fader"))
      fail("the envelope roles are still being claimed as faders");

    /* But an unknown extra token is a DIFFERENT thing, not a unit. Two LFOs
     * must not merge just because one name is a prefix of the other. */
    const two = [F("lfo_rate"), F("lfo_2_depth")];
    if (has(two, "lfo"))
      fail("lfo_rate and lfo_2_depth are two different LFOs and must not group");
  }

  /* ---- 3b. a trigger is not a switch ----------------------------------- */
  {
    /*
     * The one that cost a device round trip. A covered cell never reaches
     * drawKnobWidget, so a switch GRAPHIC over a write-only param silently
     * overruled the button widget -- and declaring access write looked like it
     * did nothing, on modules that had declared it correctly.
     */
    const trigEnum = { key: "rnd_patch", name: "Rnd Patch", type: "enum",
                       options: ["0", "1"], access: "write" };
    if (has([trigEnum], "switch")) fail("a write-only two-option enum must not draw as a switch");
    const trigInt = { key: "rnd_preset", name: "Rnd Preset", type: "int",
                      min: 0, max: 1, step: 1, access: "write" };
    if (has([trigInt], "switch")) fail("a write-only int 0..1 must not draw as a switch");
    /* A REAL boolean still draws as a switch -- that is the whole point of it. */
    const realSwitch = { key: "tempo_sync", name: "Sync", type: "enum", options: ["Off", "On"] };
    if (!has([realSwitch], "switch")) fail("an ordinary two-option enum stopped drawing as a switch");
  }

  /* ---- 4. one LFO, two rates: take the one that can be drawn ----------- */
  {
    /* schwung-filter: a tempo-syncable LFO publishes rate twice. Taking the
     * first match took the ENUM of divisions, which is not numeric, and the
     * group was refused while a float rate sat in the next slot. */
    const synced = [
      F("lfo_amount"),
      F("lfo_rate_hz", { min: 0.01, max: 20, unit: "Hz" }),
      { key: "lfo_shape", name: "Shape", type: "enum", options: ["Sine", "Tri", "Saw", "Sqr"] },
      { key: "lfo_rate_div", name: "Div", type: "enum", options: ["1/1", "1/2", "1/4", "1/8"] },
    ];
    const ix = M.buildMetaIndex({ chainParams: synced });
    const { groups } = V.resolveViz({ keys: synced.map((p) => p.key), metaIndex: ix });
    const lfo = groups.find((g) => g.kind === "lfo");
    if (!lfo) fail("a synced LFO with both a division enum and a Hz float should still group");
    if (lfo.roles.rate !== "lfo_rate_hz")
      fail("the rate ROLE must be the numeric one, got " + lfo.roles.rate);

    /*
     * KNOWN LIMITATION, pinned so it is a decision and not a surprise.
     *
     * The division enum above sits AFTER the group. Put it between the roles --
     * which is exactly how schwung-filter orders its LFO page (amount, div, hz,
     * shape) -- and the run is no longer adjacent, so no graphic is drawn at
     * all. A graphic must occupy consecutive cells, and nothing here promotes
     * an auxiliary param into the group to close the gap.
     *
     * schwung-work has the same shape: vlfo1_mult sits between vlfo1_spd and
     * vlfo1_depth. Both are fixable today by ordering the level`s knobs so the
     * roles are contiguous; closing it in the host means giving the LFO group
     * auxiliary members (div, mult, sync) with no role, which is a feature and
     * not a detector tweak.
     *
     * Asserted as CURRENT BEHAVIOUR so that implementing that feature trips
     * here and this note gets rewritten rather than silently outliving it.
     */
    const split = [F("lfo_amount"),
      { key: "lfo_rate_div", name: "Div", type: "enum", options: ["1/1", "1/2"] },
      F("lfo_rate_hz", { min: 0.01, max: 20 }),
      { key: "lfo_shape", name: "Shape", type: "enum", options: ["Sine", "Tri"] }];
    if (has(split, "lfo"))
      fail("an auxiliary param between the roles now groups -- good, but the "
         + "note above and the filter/work reports need updating");

    /* With only the enum rate there is nothing numeric to draw a rate from,
     * and refusing is correct -- the graphic would have no value to plot. */
    const enumOnly = [F("lfo_amount"),
      { key: "lfo_rate_div", name: "Div", type: "enum", options: ["1/1", "1/2"] }];
    if (has(enumOnly, "lfo"))
      fail("an LFO whose only rate is an enum has no numeric rate to draw");
  }

  console.log("PASS: dB levels fade, spd is a rate, unit qualifiers keep a stem, "
            + "and a numeric rate wins over a division enum");
});
'
