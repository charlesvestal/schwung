#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two rules about what a NUMBER means on the grid, each pinned with the case it
# must keep refusing.
#
# Both came from the fleet audit, and both are about the same confusion: an int
# is not automatically a position on a range. Sometimes it is a COUNT you set
# exactly (steps, pulses, a tempo) and sometimes it is an IDENTITY (a MIDI note).
# The arc is the wrong widget for both, and it was drawing all of them.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
]).then(([M, R]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const meta = (p) => M.buildMetaIndex({ chainParams: [p] }).getOrGuess(p.key);
  const widget = (p) => R.widgetKindFor(meta(p));
  const I = (key, min, max, name) => ({ key, name: name || key, type: "int", min, max, step: 1 });

  /* ---- counts and tempos read as numbers, whatever their span ---------- */
  for (const p of [I("bpm", 40, 240), I("tempo", 20, 500), I("lane1_steps", 1, 64),
                   I("lane1_pulses", 0, 64), I("lane1_rotation", 0, 63),
                   I("polyphony", 1, 32), I("max_voices", 1, 64)])
    if (widget(p) !== "bignum") fail(p.key + " should read as a number, got " + widget(p));

  /* A continuous quantity that happens to be an int is still a position. */
  for (const p of [I("cv_cutoff", 0, 127), I("bd_c_decay", 0, 127), I("lo_freq", 20, 20000)])
    if (widget(p) === "bignum") fail(p.key + " is a continuous range and must stay a knob");

  /*
   * THE WIDTH GUARD. Measured: the big number is 27px wide at 3 digits in a
   * 32px cell, 37px at 4. An overflow does NOT clip -- the panel is 128px, so
   * the digits just run over the cell next door and every automated check
   * still passes. A count wider than 3 digits therefore stays a knob.
   */
  if (widget(I("steps", 1, 9999)) === "bignum")
    fail("a 4-digit count would overflow its cell and must stay a knob");
  if (widget(I("drop_seed", 0, 65535)) === "bignum")
    fail("a seed is an identity but does not fit; it must stay a knob");

  /* ---- a MIDI note is named, and becomes a picker ---------------------- */
  {
    const m = meta(I("base_note", 0, 127, "Base Note"));
    if (!Array.isArray(m.options) || m.options.length !== 128)
      fail("a 0..127 note param should be given the 128 note names");
    if (m.options[36] !== "C1" || m.options[60] !== "C3")
      fail("Live octave numbering: 36 is C1 and 60 is C3, got " +
           m.options[36] + " / " + m.options[60]);
    /* The index IS the note number, which is what keeps the wire safe. */
    if (m.options.indexOf("C1") !== 36) fail("option index must equal the note number");

    /* Names that merely CONTAIN note, or key-tracking amounts, are not notes. */
    for (const p of [I("arp_note_length", 0, 127, "Arp Length"),
                     I("vf_track", 0, 127, "KEY"),
                     I("flt_key", 0, 127, "Key Track")])
      if (Array.isArray(meta(p).options))
        fail(p.key + " is not a note number and must not become a note picker");

    /* A narrower range would make the option index an OFFSET and silently
     * transpose everything, so it is refused rather than windowed. */
    if (Array.isArray(meta(I("start_note", 24, 96, "Start Note")).options))
      fail("a note param that is not exactly 0..127 must be left alone");
  }

  console.log("PASS: counts and tempos read as numbers within 3 digits, "
            + "and a 0..127 note names itself");
});
'
