#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The knob grid follows the focused voice.
#
# IT IS AN EDGE, NOT A PIN. All three focus inputs report a STEADY STATE --
# last_note keeps naming the kick long after you stopped playing it -- so a
# follow that acts on the value rather than on its CHANGE re-navigates on every
# rotation stop. Observed: a jog detent undone two ticks later, the user unable
# to leave the kick page at all, and on a 9W9 shape that means Reverb, Delay and
# Main are unreachable. Every assertion below that spins twice with an unchanged
# answer is testing that edge, and the named navigate-away case is the
# user-visible form of it.
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
#
# THREE VOICES, PARKED ON THE MIDDLE ONE. The tri-state cases used to use a
# two-voice fixture and park on the far voice -- which was ALSO the last voice,
# so "stayed put" and "yanked to the last voice" were the same observation. The
# reviewer replaced the tri-state guard with `vi = voices.length - 1` and the
# whole suite passed. With three voices and the park on index 1, neither 0 nor
# length-1 can impersonate staying put.

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

  /* Three sibling voices, each its own level with its own note, plus a
   * NON-VOICE page (reverb: no note) so the navigate-away case has somewhere
   * to go that the follow can never name. Distinct keys per level
   * deliberately: two levels sharing one key collapse into a single planned
   * page, and then there would be no second page to move TO. */
  const SIBLING = {
    pad_layout: "drums",
    focus_param: "cur_voice",
    levels: {
      root:   { params: [{ level: "kick",  label: "Kick" },
                         { level: "snare", label: "Snare" },
                         { level: "hat",   label: "Hat" },
                         { level: "reverb", label: "Reverb" }] },
      kick:   { name: "Kick",  note: 36, knobs: ["k_tune"], params: [{ key: "k_tune" }] },
      snare:  { name: "Snare", note: 38, knobs: ["s_tune"], params: [{ key: "s_tune" }] },
      hat:    { name: "Hat",   note: 42, knobs: ["h_tune"], params: [{ key: "h_tune" }] },
      reverb: { name: "Reverb",          knobs: ["r_size"], params: [{ key: "r_size" }] },
    },
  };
  const flt = (k) => ({ key: k, name: "P", type: "float", min: 0, max: 1, step: 0.01 });
  const CP = ["k_tune", "s_tune", "h_tune", "r_size"].map(flt);

  /* Every read is recorded WITH the tick it happened on, because "no new
   * rotation stop" is a claim about WHEN a read happens, not how many there
   * are. */
  let ticks = 0;
  let reads = [];
  const mk = (hier, answers, cp, opts) => {
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
    c.load(Object.assign({ slot: 0, component: "synth", prefix: "synth" }, opts || {}));
    c.setLayout("movy");
    return c;
  };
  const spin = (c, n) => { for (let i = 0; i < n; i++) { ticks++; c.tick(); } };
  const level = (c) => (c.page ? c.page.level : null);
  const pageFor = (c, lvl) => c.pages.findIndex((p) => p.level === lvl && p.kind === "knobs");
  const readKeys = () => reads.map((r) => r.key);

  /* The voice list itself, so a later failure can be read as "the follow is
   * wrong" rather than "the fixture is wrong". THREE voices, and the reverb
   * page is not one of them. */
  const voices = V.voicesOf(SIBLING);
  if (voices.length !== 3 || voices[0].level !== "kick"
      || voices[1].level !== "snare" || voices[2].level !== "hat")
    fail("the fixture does not declare three sibling voices: " + JSON.stringify(voices));

  /* --- focus_param moves the grid to the named level ------------------- */
  {
    const c = mk(SIBLING, { cur_voice: "snare" });
    spin(c, 200);
    if (level(c) !== "snare")
      fail("focus_param named snare and the grid sits on " + level(c)
         + " -- the sibling shape does not follow the module");
  }

  /* --- THE EDGE: the user can navigate away and STAY there --------------
   *
   * The whole defect, named. The module keeps answering "snare" -- which is
   * the truth, snare is still the last thing focused -- and the user jogs to
   * the Reverb page. A follow that acts on the VALUE drags them back on the
   * next rotation stop; a follow that acts on the CHANGE leaves them alone.
   *
   * The starting level is captured BEFORE the navigation, so the assertion is
   * about where the user PUT themselves, not about wherever things settled. */
  {
    const c = mk(SIBLING, { cur_voice: "snare" });
    spin(c, 200);
    if (level(c) !== "snare") {
      fail("could not reach the voice page to navigate away from");
    } else {
      const away = pageFor(c, "reverb");
      if (away < 0) fail("the fixture plans no reverb page to navigate to");
      else {
        c.goToPage(away);
        if (level(c) !== "reverb") fail("goToPage did not land on reverb");
        spin(c, 200);
        if (level(c) !== "reverb")
          fail("the user navigated to reverb and the follow dragged them to "
             + level(c) + " -- an unchanged answer must move NOTHING. This is "
             + "the pin: on a 9W9 shape it makes Reverb, Delay and Main "
             + "permanently unreachable while a pad is the last one played");
      }
    }
  }

  /* --- a name that is not a voice moves nothing ------------------------
   *
   * Sampled BEFORE the first tick. It used to sample after 40 ticks -- i.e.
   * after letting the implementation act -- and then assert nothing had
   * changed SINCE, which any deterministic implementation passes by having
   * settled. It printed ok under the yank-to-last mutant. */
  {
    const c = mk(SIBLING, { cur_voice: "reverb" });
    const was = level(c);
    spin(c, 200);
    if (level(c) !== was)
      fail("a level name that is not a voice moved the focus from " + was
         + " to " + level(c));
  }

  /* --- tri-state: a read that did not answer moves NOTHING --------------
   *
   * Parked on the MIDDLE voice. Voice 0 and the last voice are both wrong
   * answers a broken tri-state could produce, and from the middle neither of
   * them looks like staying put. */
  for (const answer of [null, "", "   ", "nonsense"]) {
    const answers = { cur_voice: "snare" };
    const c = mk(SIBLING, answers);
    spin(c, 200);
    if (level(c) !== "snare") { fail("could not reach the middle voice to test the tri-state"); break; }
    answers.cur_voice = answer;
    spin(c, 200);
    if (level(c) !== "snare")
      fail("a " + JSON.stringify(answer) + " focus read moved the focus to "
         + level(c) + " -- a failed read must never re-key the page");
  }

  /* --- a failed read must not RE-ARM the follow -------------------------
   *
   * The latch is what makes this an edge, and a null read must not clear it.
   * If it did, one timeout would resurrect the pin: the next good read would
   * look like a change and yank the user back. So: navigate away, feed one
   * unresolved read, then feed the SAME old answer again. */
  {
    const answers = { cur_voice: "snare" };
    const c = mk(SIBLING, answers);
    spin(c, 200);
    if (level(c) !== "snare") { fail("could not reach the middle voice to test the re-arm"); }
    else {
      c.goToPage(pageFor(c, "reverb"));
      answers.cur_voice = null;
      spin(c, 200);
      answers.cur_voice = "snare";
      spin(c, 200);
      if (level(c) !== "reverb")
        fail("a failed read re-armed the follow: an unchanged answer moved the "
           + "user from reverb to " + level(c) + " after one null read");
    }
  }

  /* --- NO focus param: the module owns the focus, or nothing follows ----
   *
   * There used to be a third input here, `synth:last_note`, and it is gone.
   * A SEQUENCER PLAYS NOTES: with a pattern running every hit is a note, so
   * the grid changed page on every drum in the bar -- unusable exactly while
   * you listen to the thing you are editing. Nor can a press be told from a
   * clip at that point: both reach the synth through the MIDI_OUT echo,
   * tagged the same.
   *
   * These assertions are the inverse of the ones they replace. The second is
   * the load-bearing one: last_note must never be READ, because a read is
   * what a later refactor quietly starts navigating on again. */
  const NOFOCUS = JSON.parse(JSON.stringify(SIBLING));
  delete NOFOCUS.focus_param;
  {
    const c = mk(NOFOCUS, { last_note: "38" });
    const before = level(c);
    spin(c, 300);
    if (level(c) !== before)
      fail("a module declaring no focus param moved from " + before + " to "
         + level(c) + " -- nothing may infer focus from what is PLAYED");
  }

  {
    reads.length = 0;
    const c = mk(NOFOCUS, { last_note: "38" });
    spin(c, 300);
    if (reads.some((r) => r.key.endsWith(":last_note")))
      fail("last_note was READ for a module with no focus param");
  }

  /* --- the read rides the EXISTING stop, it does not add one -----------
   *
   * An IPC read is ~2.8 ms against a 1.68 ms whole-page render, so a read on
   * its own tick is a stop the rotation did not have. The follow read must
   * land on the same tick as the preset-name read that syncChildIndexFromModule
   * already rides. */
  {
    const c = mk(SIBLING, { cur_voice: "snare" });
    spin(c, 200);
    const presetTicks = new Set(reads.filter((r) => r.key.endsWith(":preset_name")).map((r) => r.tick));
    const focusReads = reads.filter((r) => r.key.endsWith(":cur_voice"));
    if (!focusReads.length) fail("the follow never read the focus param at all");
    for (const r of focusReads) {
      if (!presetTicks.has(r.tick)) {
        fail("cur_voice was read on tick " + r.tick + ", which carries no "
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

  /* --- only the SYNTH serves last_note ---------------------------------
   *
   * chain_host stores it at the slot synth on_midi call site and nowhere
   * else, so an FX component declaring voices with no focus_param would burn
   * one ~2.8 ms read per rotation stop, forever, on a key that cannot
   * answer. */
  {
    const c = mk(NOFOCUS, { last_note: "38" }, null, { component: "fx1", prefix: "fx1" });
    spin(c, 200);
    if (readKeys().some((k) => k.endsWith(":last_note")))
      fail("an fx1 component was polled for last_note, which only the synth "
         + "serves -- that read can never answer");
  }

  /* --- the template shape owns its focus, and is NOT asked for a note ---
   *
   * This is the hard acceptance criterion. child_index_param IS the template
   * shape focus input, so a module declaring it must never also be asked for
   * last_note -- enforced by not issuing the read, because ignoring an answer
   * still leaves two live sources one edit away. */
  const mkPads = (extra) => {
    const h = {
      pad_layout: "drums",
      levels: {
        root: { params: [{ level: "pads", label: "Pads" }] },
        pads: {
          label: "Pads", child_count: 4, child_label: "Pad",
          child_key_template: "p{index}_{key}",
          child_index_base: 1, child_index_digits: 2,
          child_note_base: 36,
          knobs: ["vol"], params: [{ key: "vol" }],
        },
      },
    };
    Object.assign(h.levels.pads, extra || {});
    return h;
  };
  const padsCp = (withCur) => {
    const cp = withCur ? [{ key: "ui_cur", name: "Cur", type: "int", min: 1, max: 4 }] : [];
    for (let i = 1; i <= 4; i++) {
      const n = String(i).padStart(2, "0");
      cp.push({ key: "p" + n + "_vol", name: "Vol", type: "float", min: 0, max: 1, step: 0.01 });
    }
    return cp;
  };
  {
    const PADS = mkPads({ child_index_param: "ui_cur" });
    /* The fixture has to be a real rack, or "never read" is vacuous. */
    if (V.voicesOf(PADS).length !== 4)
      fail("the template fixture declares no voices, so the priority assertion "
         + "would pass for any implementation");

    const c = mk(PADS, { ui_cur: "3" }, padsCp(true));
    spin(c, 200);
    if (readKeys().some((k) => k.endsWith(":last_note")))
      fail("last_note was read for a module that declares child_index_param -- "
         + "two live sources, which is exactly what the priority forbids");
    if (c.childIndexOf("pads") !== 2)
      fail("the existing template follow stopped working: childIndexOf is "
         + c.childIndexOf("pads") + ", expected 2");
  }

  /* --- a rack that names NO focus param is NOT followed -----------------
   *
   * This used to assert the opposite: the rack was followed from last_note,
   * so a `pads` level with child_note_base and no focus param of any kind
   * tracked the played note. That went with the fallback, and it should have:
   * a sequencer playing the kit would have walked the instance selector
   * through the pattern.
   *
   * A rack opts in by declaring `child_index_param`, and then
   * syncChildIndexFromModule owns it. Saying nothing gets nothing. */
  {
    const RACK = mkPads(null);
    if (V.voicesOf(RACK).length !== 4) fail("the rack fixture declares no voices");
    const c = mk(RACK, { last_note: "38" }, padsCp(false));
    const before = c.childIndexOf("pads");
    spin(c, 300);
    if (c.childIndexOf("pads") !== before)
      fail("a rack naming no focus param moved from instance " + before
         + " to " + c.childIndexOf("pads")
         + " -- nothing may infer focus from a played note");
  }

  /* --- one unrelated child level does not disable the whole module ------
   *
   * The child_index_param scan used to cover EVERY level in the file, so a
   * module with sibling drum voices plus, say, an 8-instance LFO bank that
   * happens to declare child_index_param got no follow at all. The priority
   * rule is about the level that owns the VOICES. */
  {
    const MIXED = JSON.parse(JSON.stringify(SIBLING));
    MIXED.levels.root.params.push({ level: "lfos", label: "LFOs" });
    MIXED.levels.lfos = {
      label: "LFOs", child_count: 8, child_label: "LFO",
      child_key_template: "lfo{index}_{key}",
      child_index_param: "ui_cur_lfo",
      knobs: ["rate"], params: [{ key: "rate" }],
    };
    const cp = CP.slice();
    cp.push({ key: "ui_cur_lfo", name: "Cur", type: "int", min: 0, max: 7 });
    for (let i = 0; i < 8; i++) cp.push(flt("lfo" + i + "_rate"));
    /* The LFO bank must contribute no voices, or this fixture proves nothing. */
    if (V.voicesOf(MIXED).length !== 3)
      fail("the mixed fixture is wrong: the LFO bank contributes voices");
    const c = mk(MIXED, { cur_voice: "snare", ui_cur_lfo: "0" }, cp);
    spin(c, 200);
    if (level(c) !== "snare")
      fail("an unrelated child level declaring child_index_param disabled the "
         + "follow for the whole module: the grid sits on " + level(c));
  }

  /* --- the page header shows the DECLARED instance name ----------------
   *
   * childLabel already gives the picker "Rim"; the header built its own
   * string from child_label and printed "Pad 3" for the same instance, so a
   * module that named its pads got both names for one thing, one of them the
   * name it asked not to be called. */
  {
    const NAMED = mkPads({ child_names: ["Kick", "Snare", "Rim", "Hat"] });
    const c = mk(NAMED, {}, padsCp(false));
    spin(c, 200);
    const target = pageFor(c, "pads");
    if (target < 0) fail("the named-rack fixture plans no pads page");
    else {
      const lbl = c.pageLabel(c.pages[target]);
      if (!lbl || lbl.indexOf("Kick") !== 0)
        fail("the page header for instance 0 of a rack declaring "
           + "child_names[0] = Kick reads " + JSON.stringify(lbl));
    }
  }

  if (bad === 0) {
    console.log("  ok  focus_param moves the grid to the named level");
    console.log("  ok  the user can navigate away from a voice page and STAY there");
    console.log("  ok  a name that is not a voice moves nothing");
    console.log("  ok  a null, empty or nonsense focus read never re-keys the page");
    console.log("  ok  a failed read does not re-arm the follow");
    console.log("  ok  with no focus param, NOTHING follows -- a sequencer plays notes");
    console.log("  ok  last_note is never READ for a module with no focus param");
    console.log("  ok  the follow rides the existing preset-name stop");
    console.log("  ok  a module declaring no voices is never polled");
    console.log("  ok  a component declaring no focus param is never polled");
    console.log("  ok  a module declaring child_index_param is never asked for last_note");
    console.log("  ok  a rack naming no focus param is not followed either");
    console.log("  ok  one unrelated child level does not disable the follow");
    console.log("  ok  the page header shows the declared instance name");
    console.log("PASS: voice follow");
  }
  process.exit(bad ? 1 : 0);
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
