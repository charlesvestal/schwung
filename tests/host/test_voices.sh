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
Promise.all([
  import("./src/shared/param_pages/voices.mjs"),
  import("./src/shared/param_pages/child_key.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
]).then(([V, CK, PLAN]) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  /* ---- layout is declared, never inferred ---------------------------- */

  if (V.padLayoutOf({ pad_layout: "drums", levels: {} }) !== "drums")
    fail("declared drums not reported");
  if (V.padLayoutOf({ pad_layout: "chromatic", levels: {} }) !== "chromatic")
    fail("declared chromatic not reported");
  /* Absent is a THIRD state. Answering "chromatic" here puts words in the
   * mouth of all 100 fleet modules, and makes "declared melodic"
   * indistinguishable from "never asked". */
  if (V.padLayoutOf({ levels: {} }) !== null)
    fail("absent layout did not report null");
  if (V.padLayoutOf(null) !== null)
    fail("null hierarchy did not report null");
  /* An unrecognised value is unspecified, not a default. */
  if (V.padLayoutOf({ pad_layout: "isomorphic", levels: {} }) !== null)
    fail("unrecognised layout was coerced instead of reported unspecified");
  /* Notes present, layout absent -> still unspecified. This is the melodic
   * per-zone module, and inferring drums here is the bug this asserts. */
  if (V.padLayoutOf({ levels: { zone_a: { note: 60 }, zone_b: { note: 62 } } }) !== null)
    fail("layout was inferred from the presence of notes");

  /* ---- the sibling shape (9W9) --------------------------------------- */

  const SIBLING = {
    pad_layout: "drums",
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
    pad_layout: "drums",
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
  /* NAME A REAL LEVEL HERE. This line first read voiceIndexFromChild(sv,
   * "kick", 0) -- but "kick" is a ROLE, not a level name, and SIBLING declares
   * bass_drum / snare / reverb / ride. So the lookup could not match under any
   * implementation: relaxing the function to let a sibling voice through the
   * child lookup, the exact bug this line names, left the suite green. */
  if (V.voiceIndexFromChild(sv, "bass_drum", 0) !== null)
    fail("a sibling voice resolved through the child lookup");

  /* The wire tri-state, which is the one that costs a user their edit:
   * a failed read must not move the focus to voice 0. */
  for (const raw of [null, undefined, "", "  ", "abc", "-1", "99"]) {
    if (V.voiceIndexFromWire(tv, raw) !== null)
      fail("wire value " + JSON.stringify(raw) + " resolved to a voice");
  }
  if (V.voiceIndexFromWire(tv, "2") !== 2)
    fail("a valid wire index did not resolve");

  /* ---- a root SELF-LINK must not expand root twice --------------------- */

  /* root is expanded first, because mrdrums declares its rack AT root. A "Home"
   * nav entry pointing back at root is an ordinary thing for such a module to
   * declare -- and root was not marked seen before the nav-link loop, so it was
   * expanded a SECOND time: the whole rack duplicated, same levels, same notes,
   * at two sets of indices. The voice index is the identity here, so a doubled
   * list means two consumers disagree about which pad is which. */
  {
    const SELF = { pad_layout: "drums", levels: {
      root: { name: "Home", note: 36, params: [{ level: "root", label: "Home" }] },
    } };
    const s = V.voicesOf(SELF);
    if (s.length !== 1)
      fail("a root self-link duplicated the root voices: got " + s.length + ", want 1");

    /* And the same for a rack at root, which is the real mrdrums shape. */
    const SELFRACK = { pad_layout: "drums", levels: {
      root: {
        child_count: 4, child_label: "Pad", child_key_template: "p{index}_{key}",
        child_note_base: 36, params: [{ level: "root", label: "Home" }],
      },
    } };
    const sr = V.voicesOf(SELFRACK);
    if (sr.length !== 4)
      fail("a root self-link duplicated the root rack: got " + sr.length + ", want 4");
  }

  /* ---- ONE name resolver, not two -------------------------------------- */

  /* A level names itself three ways and page_plan.mjs already owns the priority:
   * lvl.name, then the label on the NAV ENTRY that points at it, then lvl.label.
   * voices.mjs re-spelled two of the three as `level.name || levelKey`, and the
   * two measurably disagreed -- the page header said "Bass Drum" and the voice
   * list said "bd", for the same thing. Same class as the childVoiceName
   * duplication already collapsed on this branch. */
  {
    const H = { pad_layout: "drums", levels: {
      root: { params: [
        { level: "bd", label: "Bass Drum" },
        { level: "sd", label: "Nav Snare" },
      ] },
      bd: { note: 36, params: [{ key: "tune" }] },
      /* lvl.name outranks the nav label, and the nav label outranks lvl.label. */
      sd: { note: 38, name: "Own Snare", label: "Bottom Label", params: [{ key: "tune" }] },
      /* Linked from nowhere and named only by its own `label`: the third source. */
      ht: { note: 50, label: "High Tom", params: [{ key: "tune" }] },
    } };
    const v = V.voicesOf(H);
    const names = v.map((x) => x.name).join(",");
    if (names !== "Bass Drum,Own Snare,High Tom")
      fail("voice names did not come through the shared resolver: " + names);

    /* And the planner must agree, because agreeing is the whole point. */
    const pp = PLAN.planPages({
      hierarchy: H,
      chainParams: [{ key: "tune", type: "float", min: 0, max: 1 }],
    });
    const pnames = (Array.isArray(pp) ? pp : (pp && pp.pages) || []).map((p) => p.name);
    if (!pnames.includes("Bass Drum"))
      fail("the planner and the voice list disagree about the nav-labelled level: "
           + JSON.stringify(pnames));

    /* Undeclared by all three: the raw key, NOT the page title prettify(). A
     * voice name is an identity a sequencer matches on, not chrome. */
    const bare = V.voicesOf({ levels: { root: { params: [{ level: "osc1" }] },
                                        osc1: { note: 60 } } });
    if (bare[0].name !== "osc1")
      fail("an undeclared voice name was not the raw level key: " + bare[0].name);
  }

  /* ---- the tail of the walk is Object.keys order, integer keys FIRST ---- */

  /* The docblock used to promise "levels declaration order". JavaScript does not
   * give that: integer-like keys enumerate first, ascending, before the rest in
   * insertion order. A module naming its parts "1".."16" would get an order it
   * did not write, in the one file that exists to own the order. Pinned to what
   * the code actually does so the comment cannot drift back to the false claim;
   * a module wanting its own order LINKS the levels from root, where an array
   * keeps declared order verbatim. */
  {
    const NUM = { pad_layout: "drums", levels: {
      root: {},
      "10": { note: 46 }, "2": { note: 38 }, "1": { note: 36 }, zz: { note: 60 },
    } };
    const order = V.voicesOf(NUM).map((v) => v.level).join(",");
    if (order !== "1,2,10,zz")
      fail("numeric level names did not walk in Object.keys order: " + order);

    /* Linked from root, declared order is honoured verbatim -- the escape hatch
     * the comment points at, so it has to actually work. */
    const LINKED = JSON.parse(JSON.stringify(NUM));
    LINKED.levels.root = { params: [{ level: "10" }, { level: "2" }, { level: "1" }] };
    const lorder = V.voicesOf(LINKED).map((v) => v.level).join(",");
    if (lorder !== "10,2,1,zz")
      fail("root nav links did not keep declared order: " + lorder);
  }

  /* ---- picker labels ------------------------------------------------- */

  {
    const L = TEMPLATE.levels.pads;
    if (CK.childLabel(L, 0) !== "Kick")
      fail("childLabel ignored a declared child_names entry");
    if (CK.childLabel(L, 3) !== "Clap")
      fail("childLabel ignored the last child_names entry");

    /* Partial arrays fall back PER ITEM. A module that names its first four
     * pads and leaves the rest must not lose "Pad 5" for the others. */
    const partial = { ...L, child_names: ["Kick", "Snare"] };
    if (CK.childLabel(partial, 0) !== "Kick")
      fail("a partial child_names lost its declared entry");
    if (CK.childLabel(partial, 2) !== "Pad 3")
      fail("a partial child_names did not fall back per item");

    /* No names at all: unchanged behaviour, which every fleet module relies on. */
    const unnamed = { ...L };
    delete unnamed.child_names;
    if (CK.childLabel(unnamed, 2) !== "Pad 3")
      fail("an unnamed child level lost its generated label");
  }

  /* ---- index is a LIST POSITION, not the child instance ---------------- */

  /* These coincide in TEMPLATE, whose one voice level starts at position 0 --
   * so `index: out.length` and `index: i` are indistinguishable there, and
   * mutating one into the other left the suite green. A sibling voice linked
   * BEFORE the rack is what separates them, and it is also the real shape:
   * 9W9 has eleven sibling voices, and a module may well have both. */
  const MIXED = {
    pad_layout: "drums",
    levels: {
      root: { params: [{ level: "kick" }, { level: "pads" }] },
      kick: { name: "Kick", note: 36, knobs: ["tune"] },
      pads: {
        child_count: 3, child_label: "Pad",
        child_key_template: "p{index}_{key}", child_index_base: 1,
        child_note_base: 60, knobs: ["vol"],
      },
    },
  };
  const mv = V.voicesOf(MIXED);
  if (mv.length !== 4)
    fail("mixed shape: expected 4 voices, got " + mv.length);
  if (mv[1].index !== 1 || mv[1].childIndex !== 0)
    fail("voice.index is the child instance rather than the list position");
  if (mv[3].index !== 3 || mv[3].childIndex !== 2)
    fail("the last child voice has the wrong list position");
  if (V.voiceIndexFromChild(mv, "pads", 0) !== 1)
    fail("child lookup answered the instance rather than the voice index");

  /* ---- the picker labels, THROUGH THE PLANNER -------------------------- */

  /* The assertions above call childLabel directly, and that is exactly how
   * this shipped broken: childLabel honoured child_names while the grid page
   * that lists the pads built its own labels inline in page_plan.mjs and never
   * called it. So a module that named its pads saw "Kick" in every list except
   * the one the user actually picks from. A test that constructs its subject by
   * hand cannot see that; this one comes through planPages.
   *
   * Only the NAME is shared. The trailing number stays 1-based here while
   * childLabel counts from child_index_base -- minijv part_selector declares no
   * base, so unifying that too would renumber its picker from Part 1-8 to
   * Part 0-7. That is a real, older disagreement and it is deliberately left
   * alone; the second case below pins it so it cannot drift by accident. */
  {
    const pl = PLAN.planPages({
      hierarchy: {
        pad_layout: "drums",
        levels: {
          root: { params: [{ level: "pads", label: "Pads" }] },
          pads: {
            name: "Pads", child_count: 4, child_label: "Pad",
            child_key_template: "p{index}_{key}", child_index_base: 1,
            child_names: ["Kick", "Snare", "Rim", "Clap"],
            knobs: ["vol"], params: [{ key: "vol" }],
          },
        },
      },
      chainParams: [{ key: "p01_vol", type: "float", min: 0, max: 1 }],
    });
    const pages = Array.isArray(pl) ? pl : (pl && pl.pages) || [];
    const picker = pages.find((p) => Array.isArray(p.derivedLabels));
    if (!picker)
      fail("the planner produced no instance picker -- this check would pass against nothing");
    else if (picker.derivedLabels.join(",") !== "Kick,Snare,Rim,Clap")
      fail("the picker ignored child_names: " + JSON.stringify(picker.derivedLabels));

    /* Undeclared: unchanged, 1-based, which every fleet module relies on. */
    const pl2 = PLAN.planPages({
      hierarchy: {
        levels: {
          root: { params: [{ level: "pads" }] },
          pads: {
            name: "Pads", child_count: 4, child_label: "Pad",
            child_key_template: "p{index}_{key}",
            knobs: ["vol"], params: [{ key: "vol" }],
          },
        },
      },
      chainParams: [{ key: "p1_vol", type: "float", min: 0, max: 1 }],
    });
    const pages2 = Array.isArray(pl2) ? pl2 : (pl2 && pl2.pages) || [];
    const picker2 = pages2.find((p) => Array.isArray(p.derivedLabels));
    if (!picker2)
      fail("the planner produced no picker for the undeclared level");
    else if (picker2.derivedLabels.join(",") !== "Pad 1,Pad 2,Pad 3,Pad 4")
      fail("an undeclared level lost its 1-based picker labels: "
           + JSON.stringify(picker2.derivedLabels));
  }

  /* ---- a REAL module, not a fixture I wrote ---------------------------- */

  /* Every fixture above declares `root: { params: [{level: "pads"}] }` and puts
   * the rack in a sibling level. mrdrums does not: its ROOT IS the 16-pad child
   * level. The walk used to start at root nav links and skip root itself, so
   * the real mrdrums hierarchy produced ZERO voices -- before and after adding
   * the declarations a fleet PR would add -- while every test here passed,
   * because every fixture agreed with the code instead of with the fleet.
   *
   * So this case is taken from the captured contract rather than written here.
   * A fixture cannot catch a mistake it shares. */
  {
    const capfs = require("node:fs");
    const cap = JSON.parse(capfs.readFileSync("./tests/fixtures/module-contracts.json", "utf8"));
    const md = cap.modules.find((m) => m.id === "mrdrums");
    if (!md || !md.ui_hierarchy || !md.ui_hierarchy.levels.root)
      fail("the captured mrdrums contract is missing -- this check would pass against nothing");

    /* Undeclared, it must stay silent: this is the inertness rule per-module. */
    if (V.voicesOf(md.ui_hierarchy).length !== 0)
      fail("undeclared mrdrums reported voices");
    if (V.padLayoutOf(md.ui_hierarchy) !== null)
      fail("undeclared mrdrums reported a layout");

    /* Declared exactly as a fleet PR would: a layout and a note base. */
    const h2 = JSON.parse(JSON.stringify(md.ui_hierarchy));
    h2.layout = "drums";
    h2.levels.root.child_note_base = 36;
    const mv2 = V.voicesOf(h2);
    if (mv2.length !== 16)
      fail("real mrdrums declared a rack and produced " + mv2.length + " voices, not 16");
    if (mv2[0].note !== 36 || mv2[15].note !== 51)
      fail("real mrdrums note map is wrong");
    if (mv2[0].level !== "root" || mv2[15].childIndex !== 15)
      fail("real mrdrums voices are not addressed through root");
  }

  /* ---- purity --------------------------------------------------------- */

  /* READ THE FILE, not three stringified exports.
   *
   * This block used to stringify voicesOf/padLayoutOf/voiceIndexFromNote and
   * grep for getParam|setParam|host_. Two holes, both proven by injection:
   * the private helpers were not covered at all, and this codebase binding is
   * `shadow_get_param`, which none of those three patterns match. A real param
   * read inserted into voicesOf passed green. */
  const fs = require("node:fs");
  const src = fs.readFileSync("./src/shared/param_pages/voices.mjs", "utf8")
      .replace(/\/\*[\s\S]*?\*\//g, "")   /* strip block comments -- an */
      .replace(/^\s*\/\/.*$/gm, "");      /* assertion must not trip on prose */
  if (/shadow_get_param|shadow_set_param|getParam|setParam|host_[a-z_]+\s*\(/.test(src))
    fail("voices.mjs reads or writes params -- it must stay pure");

  if (bad) process.exit(1);
  console.log("PASS: voices.mjs");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
