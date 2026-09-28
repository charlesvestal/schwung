#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Which of a drum module's parameters belong to which voice, for a
# voice-scoped paste. Move's drum paste copies ONLY the selected voice's notes,
# so the automation that follows it must be only that voice's.
#
# The fixture is dr32's SHAPE: one pad spread over three 32-pad child levels
# sharing a child index, plus keys every pad level lists (the focus param, a
# link switch) that belong to the track rather than any voice.

node -e '
import("./src/shared/lane_voice_map.mjs").then((M) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };
  const pad = (name, keys, extra) => Object.assign({
    name, child_prefix: "pad", child_count: 3, child_index_base: 1,
    child_index_param: "ui_current_pad",
    child_key_overrides: { ui_current_pad: "ui_current_pad", link: "link" },
    knobs: keys, params: keys.map((k) => ({ key: k })),
  }, extra || {});
  const h = {
    pad_layout: "drums",
    levels: {
      root: { params: [{ level: "pads" }, { level: "pad_shape" }, { level: "pad_mix" }], knobs: [] },
      pads: pad("Sample", ["ui_current_pad", "sample", "start"], { child_note_base: 36 }),
      pad_shape: pad("Shape", ["attack", "decay"]),
      pad_mix: pad("Mix", ["volume", "pan", "link"]),
    },
  };
  const s = M.laneVoiceMap(h);
  const want = "36:pad1_sample,pad1_start,pad1_attack,pad1_decay,pad1_volume,pad1_pan;" +
               "37:pad2_sample,pad2_start,pad2_attack,pad2_decay,pad2_volume,pad2_pan;" +
               "38:pad3_sample,pad3_start,pad3_attack,pad3_decay,pad3_volume,pad3_pan";
  if (s !== want) fail("dr32 shape: got\n  " + s + "\nwant\n  " + want);
  if (/ui_current_pad|link/.test(s)) fail("a key every voice lists is the TRACK, not a voice");

  /* Not a rack: melodic, unspecified, nothing. "" = whole-step paste. */
  const chrom = Object.assign({}, h, { pad_layout: "chromatic" });
  if (M.laneVoiceMap(chrom) !== "") fail("a chromatic module must not be voice-scoped");
  const unspec = Object.assign({}, h); delete unspec.pad_layout;
  if (M.laneVoiceMap(unspec) !== "") fail("absent pad_layout is not drums");
  if (M.laneVoiceMap(null) !== "") fail("null hierarchy");

  /* Too big to carry is whole-step, never a truncated (wrong) map. */
  const big = JSON.parse(JSON.stringify(h));
  for (const n of ["pads", "pad_shape", "pad_mix"]) big.levels[n].child_count = 400;
  if (M.laneVoiceMap(big) !== "") fail("an oversize map must be dropped whole");

  /* A key with a separator in it would corrupt the wire; it is left out. */
  const odd = JSON.parse(JSON.stringify(h));
  odd.levels.pad_mix.knobs.push("x;y"); odd.levels.pad_mix.params.push({ key: "x;y" });
  if (/x;y/.test(M.laneVoiceMap(odd))) fail("separator in a key leaked into the wire");

  if (bad) process.exit(1);
  console.log("PASS: lane voice map");
});
'
