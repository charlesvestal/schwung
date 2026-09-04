#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The voice model: what a module declares, and the ONE place both fleet shapes
# collapse into a single ordered list.
#
# Two shapes exist and neither can be dropped. mrdrums declares 16
# interchangeable pads through one key template; 9W9 declares 11
# differently-shaped sibling levels, three of which (Reverb, Delay, Main) are
# pages that sound nothing. A model that only fits one of them is what movy's
# 4-module override list already exists to work around.
#
# LAYOUT IS NEVER INFERRED. A melodic module may legitimately carry notes on
# per-zone or per-key pages, so "has notes" must not mean "is a drum rack" --
# the layout is declared or it is unspecified, and unspecified is a THIRD
# state, not a synonym for chromatic.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/voices.mjs").then((V) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  /* ---- layout is declared, never inferred ---------------------------- */

  if (V.layoutOf({ layout: "drums", levels: {} }) !== "drums")
    fail("declared drums not reported");
  if (V.layoutOf({ layout: "chromatic", levels: {} }) !== "chromatic")
    fail("declared chromatic not reported");
  /* Absent is a THIRD state. Answering "chromatic" here puts words in the
   * mouth of all 100 fleet modules, and makes "declared melodic"
   * indistinguishable from "never asked". */
  if (V.layoutOf({ levels: {} }) !== null)
    fail("absent layout did not report null");
  if (V.layoutOf(null) !== null)
    fail("null hierarchy did not report null");
  /* An unrecognised value is unspecified, not a default. */
  if (V.layoutOf({ layout: "isomorphic", levels: {} }) !== null)
    fail("unrecognised layout was coerced instead of reported unspecified");
  /* Notes present, layout absent -> still unspecified. This is the melodic
   * per-zone module, and inferring drums here is the bug this asserts. */
  if (V.layoutOf({ levels: { zone_a: { note: 60 }, zone_b: { note: 62 } } }) !== null)
    fail("layout was inferred from the presence of notes");

  /* ---- the sibling shape (9W9) --------------------------------------- */

  const SIBLING = {
    layout: "drums",
    levels: {
      root: { params: [
        { level: "bass_drum", label: "Bass Drum" },
        { level: "snare",     label: "Snare" },
        { level: "reverb",    label: "Reverb" },
      ] },
      bass_drum: { name: "Bass Drum", note: 36, role: "kick",  knobs: ["tune"] },
      snare:     { name: "Snare",     note: 38, role: "snare", knobs: ["tune"] },
      reverb:    { name: "Reverb", knobs: ["size"] },
      /* declared but NOT linked from root -- must still get a stable index */
      ride:      { name: "Ride", note: 51, knobs: ["tune"] },
    },
  };

  const sv = V.voicesOf(SIBLING);
  if (sv.length !== 3)
    fail("sibling shape: expected 3 voices, got " + sv.length);
  /* Reverb declares no note: a page, not a voice. */
  if (sv.some((v) => v.level === "reverb"))
    fail("a level with no note was counted as a voice");
  if (sv[0].level !== "bass_drum" || sv[1].level !== "snare")
    fail("sibling voices are not in root nav-link order");
  /* An unlinked voice level is APPENDED, not dropped: dropping it would make
   * two consumers disagree about the same list. */
  if (sv[2].level !== "ride")
    fail("a voice level root does not link was dropped instead of appended");
  if (sv[0].name !== "Bass Drum" || sv[0].note !== 36 || sv[0].role !== "kick")
    fail("sibling voice did not carry name/note/role");
  if (sv[0].index !== 0 || sv[2].index !== 2)
    fail("voice.index is not its position in the list");
  if (sv[0].childIndex !== null)
    fail("a sibling voice reported a childIndex");

  /* ---- the template shape (mrdrums) ---------------------------------- */

  const TEMPLATE = {
    layout: "drums",
    levels: {
      root: { params: [{ level: "pads", label: "Pads" }] },
      pads: {
        child_count: 4, child_label: "Pad",
        child_key_template: "p{index}_{key}",
        child_index_base: 1, child_index_digits: 2,
        child_index_param: "ui_current_pad",
        child_note_base: 36,
        child_names: ["Kick", "Snare", "Rim", "Clap"],
        knobs: ["vol"],
      },
    },
  };

  const tv = V.voicesOf(TEMPLATE);
  if (tv.length !== 4)
    fail("template shape: expected 4 voices, got " + tv.length);
  /* Contiguous map: instance i plays base + i. */
  if (tv[0].note !== 36 || tv[3].note !== 39)
    fail("child_note_base did not produce a contiguous note map");
  if (tv[0].name !== "Kick" || tv[3].name !== "Clap")
    fail("child_names were not carried onto the voices");
  if (tv[2].childIndex !== 2 || tv[2].level !== "pads")
    fail("a template voice did not carry its level and zero-based childIndex");
  /* Falls back to the child label when no names are declared. */
  const noNames = JSON.parse(JSON.stringify(TEMPLATE));
  delete noNames.levels.pads.child_names;
  if (V.voicesOf(noNames)[2].name !== "Pad 3")
    fail("undeclared voice name did not fall back to the child label");

  /* Sparse map wins over the base. */
  const sparse = JSON.parse(JSON.stringify(TEMPLATE));
  sparse.levels.pads.child_notes = [36, 38, 42, 46];
  const spv = V.voicesOf(sparse);
  if (spv[2].note !== 42 || spv[3].note !== 46)
    fail("child_notes did not override child_note_base");

  /* A child level with NO note map is not voices -- it is instances. This is
   * every multitimbral synth in the fleet, and counting them as drum voices
   * is the inference this whole model exists to refuse. */
  const noNotes = JSON.parse(JSON.stringify(TEMPLATE));
  delete noNotes.levels.pads.child_note_base;
  if (V.voicesOf(noNotes).length !== 0)
    fail("a child level with no note map produced voices");

  /* ---- lookups, all tri-state ---------------------------------------- */

  if (V.voiceIndexFromNote(sv, 38) !== 1)
    fail("note -> voice lookup failed on the sibling shape");
  /* The template map is contiguous from 36, so 38 is the THIRD voice. Note
   * lookup answers a POSITION IN THE LIST, never the note offset -- the two
   * coincide only when a rack happens to start at the note it starts at. */
  if (V.voiceIndexFromNote(tv, 38) !== 2)
    fail("note -> voice lookup failed on the template shape");
  if (V.voiceIndexFromNote(tv, 99) !== null)
    fail("an unmapped note did not report null");
  if (V.voiceIndexFromLevel(sv, "snare") !== 1)
    fail("level -> voice lookup failed");
  if (V.voiceIndexFromLevel(sv, "reverb") !== null)
    fail("a non-voice level resolved to a voice");
  /* The template shape needs instance -> voice, which is how a consumer turns
   * a child_index_param answer into a voice index. Untested, this export
   * would be the one path nothing exercises. */
  if (V.voiceIndexFromChild(tv, "pads", 2) !== 2)
    fail("child instance -> voice lookup failed");
  if (V.voiceIndexFromChild(tv, "pads", 9) !== null)
    fail("an out-of-range child instance resolved to a voice");
  if (V.voiceIndexFromChild(sv, "kick", 0) !== null)
    fail("a sibling voice resolved through the child lookup");

  /* The wire tri-state, which is the one that costs a user their edit:
   * a failed read must not move the focus to voice 0. */
  for (const raw of [null, undefined, "", "  ", "abc", "-1", "99"]) {
    if (V.voiceIndexFromWire(tv, raw) !== null)
      fail("wire value " + JSON.stringify(raw) + " resolved to a voice");
  }
  if (V.voiceIndexFromWire(tv, "2") !== 2)
    fail("a valid wire index did not resolve");

  /* ---- purity --------------------------------------------------------- */

  const src = "" + V.voicesOf + V.layoutOf + V.voiceIndexFromNote;
  if (/getParam|setParam|host_/.test(src))
    fail("voices.mjs reads or writes params -- it must stay pure");

  if (bad) process.exit(1);
  console.log("PASS: voices.mjs");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
