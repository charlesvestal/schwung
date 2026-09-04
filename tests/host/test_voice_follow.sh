#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The knob grid follows the focused voice.
#
# THE PRIORITY IS THE POINT. Exactly one input is live for any given module:
# a module that owns its focus is authoritative, and the note fallback is not
# consulted for it AT ALL. Two live sources would disagree the moment a module
# moved its focus without a note -- a preset load, an auto-select -- and the
# disagreement would latch, which is the failure this ordering exists to make
# unconstructible. So the child_index_param case is asserted on the READS, not
# on the outcome: ignoring the answer is not the same as never asking.
#
# And the tri-state, the recurring expensive bug in this repo: a read that did
# not answer is null and means "no information". Resolving it to voice 0 would
# yank the user off the pad they were editing, re-keying every page on screen
# and dropping its cached values, because a read timed out.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/voices.mjs"),
]).then(([PC, V]) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  /* Two sibling voices, each its own level with its own note. Distinct keys
   * per level deliberately: two levels sharing one key collapse into a single
   * planned page, and then there would be no second page to move TO. */
  const SIBLING = {
    layout: "drums",
    focus_param: "cur_voice",
    levels: {
      root:  { params: [{ level: "kick", label: "Kick" },
                        { level: "snare", label: "Snare" }] },
      kick:  { name: "Kick",  note: 36, knobs: ["k_tune"], params: [{ key: "k_tune" }] },
      snare: { name: "Snare", note: 38, knobs: ["s_tune"], params: [{ key: "s_tune" }] },
    },
  };
  const CP = [
    { key: "k_tune", name: "Tune", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "s_tune", name: "Tune", type: "float", min: 0, max: 1, step: 0.01 },
  ];

  /* Every read is recorded WITH the tick it happened on, because "no new
   * rotation stop" is a claim about WHEN a read happens, not how many there
   * are. */
  let ticks = 0;
  let reads = [];
  const mk = (hier, answers, cp) => {
    reads = [];
    ticks = 0;
    const c = PC.createController({
      getParam: (k) => {
        reads.push({ tick: ticks, key: k });
        if (k.endsWith(":ui_hierarchy")) return JSON.stringify(hier);
        if (k.endsWith(":chain_params")) return JSON.stringify(cp || CP);
        if (k.endsWith(":preset_name")) return "";
        if (k.endsWith(":is_loading")) return "0";
        if (k.endsWith(":module")) return "poc";
        const bare = k.slice(k.indexOf(":") + 1);
        if (bare in answers) return answers[bare];
        return "0.5";
      },
      setParam: () => {}, announce: () => {}, now: () => 0,
    });
    c.load({ slot: 0, component: "synth", prefix: "synth" });
    c.setLayout("movy");
    return c;
  };
  const spin = (c, n) => { for (let i = 0; i < n; i++) { ticks++; c.tick(); } };
  const level = (c) => (c.page ? c.page.level : null);
  const pageFor = (c, lvl) => c.pages.findIndex((p) => p.level === lvl && p.kind === "knobs");
  const readKeys = () => reads.map((r) => r.key);

  /* The voice list itself, so a later failure can be read as "the follow is
   * wrong" rather than "the fixture is wrong". */
  const voices = V.voicesOf(SIBLING);
  if (voices.length !== 2 || voices[0].level !== "kick" || voices[1].level !== "snare")
    fail("the fixture does not declare two sibling voices: " + JSON.stringify(voices));

  /* --- focus_param moves the grid to the named level ------------------- */
  {
    const c = mk(SIBLING, { cur_voice: "snare" });
    spin(c, 200);
    if (level(c) !== "snare")
      fail("focus_param named snare and the grid sits on " + level(c)
         + " -- the sibling shape does not follow the module");
  }

  /* --- a name that is not a voice moves nothing ------------------------ */
  {
    const c = mk(SIBLING, { cur_voice: "reverb" });
    spin(c, 40);
    const was = level(c);
    spin(c, 200);
    if (level(c) !== was)
      fail("a level name that is not a voice moved the focus from " + was
         + " to " + level(c));
  }

  /* --- tri-state: a read that did not answer moves NOTHING --------------
   *
   * Asserted from the FAR page, not the near one. Starting on voice 0 would
   * pass for an implementation that resolved every failed read to voice 0 --
   * the exact bug -- because staying and being yanked to 0 look identical
   * from there. */
  for (const answer of [null, "", "   ", "nonsense"]) {
    const answers = { cur_voice: "snare" };
    const c = mk(SIBLING, answers);
    spin(c, 200);
    if (level(c) !== "snare") { fail("could not reach the far voice to test the tri-state"); break; }
    answers.cur_voice = answer;
    spin(c, 200);
    if (level(c) !== "snare")
      fail("a " + JSON.stringify(answer) + " focus read moved the focus to "
         + level(c) + " -- a failed read must never re-key the page");
  }

  /* --- no focus param, but voices: last_note is the fallback ----------- */
  const NOFOCUS = JSON.parse(JSON.stringify(SIBLING));
  delete NOFOCUS.focus_param;
  {
    const c = mk(NOFOCUS, { last_note: "38" });
    spin(c, 200);
    if (level(c) !== "snare")
      fail("last_note 38 is the snare and the grid sits on " + level(c)
         + " -- the note fallback does not follow the played pad");
  }

  /* A note no voice plays, and a failed note read, both move nothing. */
  for (const answer of ["99", null, "", "   "]) {
    const answers = { last_note: "38" };
    const c = mk(NOFOCUS, answers);
    spin(c, 200);
    if (level(c) !== "snare") { fail("could not reach the far voice via last_note"); break; }
    answers.last_note = answer;
    spin(c, 200);
    if (level(c) !== "snare")
      fail("last_note " + JSON.stringify(answer) + " moved the focus to " + level(c));
  }

  /* --- the read rides the EXISTING stop, it does not add one -----------
   *
   * An IPC read is ~2.8 ms against a 1.68 ms whole-page render, so a read on
   * its own tick is a stop the rotation did not have. The follow read must
   * land on the same tick as the preset-name read that syncChildIndexFromModule
   * already rides. */
  {
    const c = mk(NOFOCUS, { last_note: "38" });
    spin(c, 200);
    const presetTicks = new Set(reads.filter((r) => r.key.endsWith(":preset_name")).map((r) => r.tick));
    const noteReads = reads.filter((r) => r.key.endsWith(":last_note"));
    if (!noteReads.length) fail("the fallback never read last_note at all");
    for (const r of noteReads) {
      if (!presetTicks.has(r.tick)) {
        fail("last_note was read on tick " + r.tick + ", which carries no "
           + "preset_name read -- that is a NEW rotation stop");
        break;
      }
    }
  }

  /* --- a module declaring neither focus nor voices is not polled ------- */
  {
    const PLAIN = { levels: { root: { knobs: ["k_tune"], params: [{ key: "k_tune" }] } } };
    const c = mk(PLAIN, {});
    spin(c, 200);
    if (readKeys().some((k) => k.endsWith(":last_note")))
      fail("a module declaring no voices was polled for last_note -- the "
         + "rotation must cost nothing for the modules that opt out");
  }

  /* --- the template shape owns its focus, and is NOT asked for a note ---
   *
   * This is the hard acceptance criterion. child_index_param IS the template
   * shape focus input, so a module declaring it must never also be asked for
   * last_note -- enforced by not issuing the read, because ignoring an answer
   * still leaves two live sources one edit away. */
  {
    const PADS = {
      layout: "drums",
      levels: {
        root: { params: [{ level: "pads", label: "Pads" }] },
        pads: {
          label: "Pads", child_count: 4, child_label: "Pad",
          child_key_template: "p{index}_{key}",
          child_index_base: 1, child_index_digits: 2,
          child_index_param: "ui_cur",
          child_note_base: 36,
          knobs: ["vol"], params: [{ key: "vol" }],
        },
      },
    };
    const CPP = [{ key: "ui_cur", name: "Cur", type: "int", min: 1, max: 4 }];
    for (let i = 1; i <= 4; i++) {
      const n = String(i).padStart(2, "0");
      CPP.push({ key: "p" + n + "_vol", name: "Vol", type: "float", min: 0, max: 1, step: 0.01 });
    }
    /* The fixture has to be a real rack, or "never read" is vacuous. */
    if (V.voicesOf(PADS).length !== 4)
      fail("the template fixture declares no voices, so the priority assertion "
         + "would pass for any implementation");

    const c = mk(PADS, { ui_cur: "3" });
    spin(c, 200);
    if (readKeys().some((k) => k.endsWith(":last_note")))
      fail("last_note was read for a module that declares child_index_param -- "
         + "two live sources, which is exactly what the priority forbids");
    if (c.childIndexOf("pads") !== 2)
      fail("the existing template follow stopped working: childIndexOf is "
         + c.childIndexOf("pads") + ", expected 2");
  }

  if (bad === 0) {
    console.log("  ok  focus_param moves the grid to the named level");
    console.log("  ok  a name that is not a voice moves nothing");
    console.log("  ok  a null, empty or nonsense focus read never re-keys the page");
    console.log("  ok  with no focus param, the grid follows last_note");
    console.log("  ok  an unplayed note and a failed note read move nothing");
    console.log("  ok  the follow rides the existing preset-name stop");
    console.log("  ok  a module declaring no voices is never polled");
    console.log("  ok  a module declaring child_index_param is never asked for last_note");
    console.log("PASS: voice follow");
  }
  process.exit(bad ? 1 : 0);
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
