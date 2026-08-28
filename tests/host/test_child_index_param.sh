#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A module can own which child instance is focused.
#
# Without child_index_param the index is UI-only state, written in exactly one
# place: a pick from the instance list. That is right for a synth whose "Part 2"
# is a deliberate navigation choice and wrong for a drum module, where hitting a
# pad is how you choose what you are editing. mrdrums is the case -- it follows
# the pad you play -- so declaring a child level at all would have COST that
# behaviour, and the declaration is what makes 209 per-pad params addressable
# (and their modulation indicators visible). This is what makes it additive.
#
# Both directions are asserted, because either alone is satisfied by a
# one-directional implementation:
#   module -> UI   the rotation adopts the module's index
#   UI -> module   a pick WRITES it, so the two can never disagree
#
# And the tri-state, which is the one that costs a user their edit: a failed or
# nonsense read must NOT move the focus. Adopting 0 there re-keys every page on
# screen and drops its cached values because a read timed out.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/child_key.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/knob_engine.mjs"),
  import("./src/shared/param_pages/page_nav.mjs"),
]).then(([PC, CK, PLAN, KE, NAV]) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  /* ---- the pure half ------------------------------------------------- */
  const LEVEL = {
    label: "Pads", child_count: 16, child_label: "Pad",
    child_key_template: "p{index}_{key}",
    child_index_base: 1, child_index_digits: 2,
    child_index_param: "focused_pad",
    knobs: ["vol", "start"],
    params: [{ key: "vol" }, { key: "start" }],
  };

  if (CK.childIndexParam(LEVEL) !== "focused_pad")
    fail("childIndexParam does not report the declared key");
  /* A level with no children cannot have one, however it is spelled. */
  if (CK.childIndexParam({ child_index_param: "x" }) !== null)
    fail("a level with no children reported a child index param");

  /* The wire speaks the MODULE numbering (pads are 1..16), the engine is
   * zero-based. A round trip that agreed with itself would pass for any
   * consistent-but-wrong base, so the wire value is asserted literally. */
  if (CK.childIndexToWire(LEVEL, 0) !== "1")
    fail("instance 0 must go on the wire as 1 with child_index_base 1, got "
       + CK.childIndexToWire(LEVEL, 0));
  if (CK.childIndexFromWire(LEVEL, "1") !== 0)
    fail("wire 1 must read back as instance 0");
  if (CK.childIndexFromWire(LEVEL, "16") !== 15)
    fail("wire 16 must read back as instance 15");

  /* Tri-state: none of these name an instance, and none may become 0. */
  for (const junk of [null, undefined, "", "  ", "abc", "0", "17", "-1", "NaN"]) {
    if (CK.childIndexFromWire(LEVEL, junk) !== null)
      fail("childIndexFromWire accepted " + JSON.stringify(junk)
         + " as an instance (got " + CK.childIndexFromWire(LEVEL, junk) + ")");
  }

  /* ---- driven through the real controller ----------------------------- */
  const HIER = { modes: null, levels: { root: LEVEL } };
  const CP = [];
  for (let i = 1; i <= 16; i++) {
    const n = String(i).padStart(2, "0");
    CP.push({ key: `p${n}_vol`,   name: "Vol",   type: "float", min: 0, max: 1, step: 0.01 });
    CP.push({ key: `p${n}_start`, name: "Start", type: "float", min: 0, max: 1, step: 0.01 });
  }

  let focused = "1";           /* what the MODULE says */
  let focusedWrites = 0;
  const reads = [];
  const mk = () => {
    const io = {
      getParam: (k) => {
        reads.push(k);
        if (k.endsWith(":ui_hierarchy")) return JSON.stringify(HIER);
        if (k.endsWith(":chain_params")) return JSON.stringify(CP);
        if (k.endsWith(":focused_pad"))  return focused;
        if (k.endsWith(":preset_name"))  return "";
        if (k.endsWith(":is_loading"))   return "0";
        if (k.endsWith(":module"))       return "mrdrums";
        return "0.5";
      },
      setParam: (k, v) => {
        if (k.endsWith(":focused_pad")) { focusedWrites++; focused = String(v); }
      },
      announce: () => {}, now: () => 0,
    };
    const c = PC.createController(io);
    c.load({ slot: 0, component: "synth", prefix: "synth" });
    c.setLayout("movy");
    return c;
  };

  const spin = (c, n) => { for (let i = 0; i < n; i++) c.tick(); };
  /* Which concrete key does knob 0 actually address? The MAPPING is the
   * behaviour; the index cache is an implementation detail. */
  const keyOf = (c) => {
    const r = reads.filter((k) => /^synth:p\d\d_vol$/.test(k));
    return r.length ? r[r.length - 1] : null;
  };

  {
    const c = mk();
    spin(c, 40);
    if (keyOf(c) !== "synth:p01_vol")
      fail("focused_pad=1 should address p01_vol, addressed " + keyOf(c));

    /* MODULE MOVES THE FOCUS -- the pad you just played. */
    focused = "5";
    reads.length = 0;
    spin(c, 40);
    if (keyOf(c) !== "synth:p05_vol")
      fail("the grid did not follow the module to pad 5; it addressed "
         + keyOf(c) + " -- auto-select-pad would be dead");

    /* A FAILED read must not move it back. */
    const before = keyOf(c);
    focused = "";
    reads.length = 0;
    spin(c, 40);
    if (keyOf(c) !== before)
      fail("an empty focused_pad read moved the focus to " + keyOf(c)
         + " -- a timeout must never re-key the page");
    focused = "nonsense";
    reads.length = 0;
    spin(c, 40);
    if (keyOf(c) !== before)
      fail("a nonsense focused_pad read moved the focus to " + keyOf(c));
  }

  /* ---- UI -> MODULE: a pick WRITES the param --------------------------
   *
   * Without this half the two sides can disagree: the user picks Pad 5 from
   * the list, the grid re-keys locally, and the module goes on thinking Pad 1
   * is focused -- so the next thing the module drives (or the next read of the
   * param) yanks the user back. The param being the single source of truth in
   * BOTH directions is what makes that impossible.
   */
  {
    const c = mk();
    spin(c, 20);
    focusedWrites = 0;
    /* The child picker (kind "items", childOf the level): enter it, move,
     * and choose. */
    const picker = c.pages.findIndex((pg) => pg.childOf);
    if (picker < 0) fail("no child picker page was planned");
    else {
      c.goToPage(picker);
      c.onClick();              /* enter the list */
      c.onJog(2);               /* -> instance 2 */
      c.onClick();              /* choose it */
      if (focusedWrites === 0)
        fail("choosing a child wrote nothing -- the module still thinks the "
           + "old instance is focused and will yank the user back");
      else {
        /* Asserted as an INVARIANT between the two spellings, not against a
         * hard-coded instance: how far one jog detent travels belongs to
         * the list widget and changed this assertion once already. What must
         * hold is that the wire value and the key the grid then addresses name
         * the SAME pad -- an off-by-one in child_index_base breaks exactly
         * that, and nothing else here would catch it. */
        if (focused === "1")
          fail("the pick did not move off the starting instance, so this "
             + "assertion proves nothing");
        reads.length = 0;
        spin(c, 40);
        const want = "synth:p" + String(Number(focused)).padStart(2, "0") + "_vol";
        if (keyOf(c) !== want)
          fail("wrote focused_pad=" + focused + " but the grid addresses "
             + keyOf(c) + ", expected " + want
             + " -- the wire numbering and the key template disagree");
      }
    }
  }


  /* ---- a generic child key BORROWS the concrete declaration ------------
   *
   * A child level lists `start`; the module declares p01_start … p16_start and
   * nothing called `start`. Without an alias that falls to getOrGuess and
   * becomes a plain 0..1 float -- a STRUCTURE guess, which is the one thing
   * the meta index is not allowed to make. On mrdrums it cost Sample Start its
   * wav_position type and its filepath_param, so the cell drew as a bare knob
   * instead of the waveform. Reported from the device.
   */
  {
    const CP2 = [{ key: "ui_current_pad", name: "Current Pad", type: "int", min: 1, max: 16 }];
    for (let i = 1; i <= 16; i++) {
      const n = String(i).padStart(2, "0");
      CP2.push({ key: `p${n}_start`, name: "Start", type: "wav_position",
                 filepath_param: `p${n}_sample_path`, min: 0, max: 1, step: 0.01 });
      CP2.push({ key: `p${n}_sample_path`, name: "Sample", type: "filepath" });
      CP2.push({ key: `p${n}_vol`, name: "Vol", type: "float", min: 0, max: 1, step: 0.01 });
    }
    const L3 = {
      label: "Pads", child_count: 16, child_label: "Pad",
      child_key_template: "p{index}_{key}",
      child_index_base: 1, child_index_digits: 2,
      child_index_param: "ui_current_pad",
      knobs: ["vol", "start"], params: [{ key: "vol" }, { key: "start" }],
    };
    let pad = "1";
    const c = PC.createController({
      getParam: (k) => {
        if (k.endsWith(":ui_hierarchy")) return JSON.stringify({ modes: null, levels: { root: L3 } });
        if (k.endsWith(":chain_params")) return JSON.stringify(CP2);
        if (k.endsWith(":ui_current_pad")) return pad;
        if (k.endsWith(":preset_name")) return "";
        if (k.endsWith(":is_loading")) return "0";
        if (k.endsWith(":module")) return "mrdrums";
        return "0.5";
      },
      setParam: () => {}, announce: () => {}, now: () => 0,
    });
    c.load({ slot: 0, component: "synth", prefix: "synth" });
    c.setLayout("movy");
    for (let i = 0; i < 30; i++) c.tick();

    const meta = c.metaIndex.getOrGuess("start");
    if (meta.guessed)
      fail("the generic child key `start` was GUESSED -- it must borrow the "
         + "concrete declaration, or the widget is chosen from an invented type");
    if (meta.type !== "wav_position")
      fail("`start` resolved to type " + meta.type + ", expected wav_position");
    if (meta.filepath_param !== "p01_sample_path")
      fail("`start` borrowed filepath_param " + meta.filepath_param
         + ", expected p01_sample_path");

    /* It must FOLLOW the focused instance: the cross-reference names a pad, so
     * a stale alias keeps drawing the previous pad file. */
    pad = "5";
    for (let i = 0; i < 30; i++) c.tick();
    if (c.metaIndex.getOrGuess("start").filepath_param !== "p05_sample_path")
      fail("after moving to pad 5 the borrowed filepath_param is still "
         + c.metaIndex.getOrGuess("start").filepath_param);

    /* A DECLARED key is never shadowed. */
    if (c.metaIndex.getOrGuess("p01_vol").key !== "p01_vol")
      fail("aliasing disturbed a declared key");
  }


  /* ---- the GRAPHIC follows the focused child, without a page change -----
   *
   * A viz group memoises on fingerprint + page index, and NEITHER changes when
   * the focused pad does. So the cached group went on naming the previous
   * pad`s file as its extra key: the read rotation kept fetching pad 1 while
   * pad 5 was on screen, and only jogging to another page and back busted the
   * cache. Reported from the device as the waveform updating only after a
   * detour.
   *
   * Asserted on the KEY THAT GETS READ, not on the cache: what the user sees
   * is which file the rotation fetched, and a test that inspected the cache
   * would pass on a correctly-rebuilt group whose value never arrived.
   */
  {
    const CP3 = [{ key: "cur_pad", name: "Cur", type: "int", min: 1, max: 4 }];
    for (let i = 1; i <= 4; i++) {
      const n = String(i).padStart(2, "0");
      CP3.push({ key: `p${n}_start`, name: "Start", type: "wav_position",
                 filepath_param: `p${n}_sample_path`, min: 0, max: 1, step: 0.01 });
      CP3.push({ key: `p${n}_sample_path`, name: "Sample", type: "filepath" });
      CP3.push({ key: `p${n}_vol`, name: "Vol", type: "float", min: 0, max: 1, step: 0.01 });
    }
    const L4 = { label: "Pads", child_count: 4, child_label: "Pad",
                 child_key_template: "p{index}_{key}", child_index_base: 1,
                 child_index_digits: 2, child_index_param: "cur_pad",
                 knobs: ["vol", "start"], params: ["vol", "start"] };
    let pad = "1";
    const c = PC.createController({
      getParam: (k) => {
        if (k.endsWith(":ui_hierarchy")) return JSON.stringify({ modes: null, levels: { root: L4 } });
        if (k.endsWith(":chain_params")) return JSON.stringify(CP3);
        if (k.endsWith(":cur_pad")) return pad;
        if (k.endsWith(":preset_name")) return "";
        if (k.endsWith(":is_loading")) return "0";
        if (k.endsWith(":module")) return "m";
        if (/sample_path$/.test(k)) return "/x/f.wav";
        return "0.5";
      }, setParam: () => {}, announce: () => {}, now: () => 0,
    });
    c.load({ slot: 0, component: "synth", prefix: "synth" });
    c.setLayout("movy");
    for (let i = 0; i < 40; i++) c.tick();
    pad = "3";
    for (let i = 0; i < 40; i++) c.tick();   /* NO page change */
    const files = Object.keys(c.state.values).filter((k) => /sample_path$/.test(k));
    if (files.indexOf("p03_sample_path") < 0)
      fail("after the module moved the focus to pad 3, the graphic still asks for "
         + JSON.stringify(files) + " -- the viz cache does not key on the focused child, "
         + "so the waveform shows the previous pad until you jog away and back");
  }


  /* ---- the picker only goes when the index is REALLY reachable ---------
   *
   * The picker is suppressed when the module owns the focus AND offers the
   * control itself. "Offers" has to mean a CELL, not a mention: an overflow
   * key pulled from params[] is dropped when it is `ui_`-prefixed (page_plan
   * filters exactly that, naming ui_current_pad in its comment), so a module
   * can list its index param and still give it no cell.
   *
   * Getting this wrong removed the picker while the control it deferred to did
   * not exist -- with auto-select off there was then no way to reach another
   * instance at all. Reported from the device as "if I turn off autoselect how
   * do I get to another pads settings?".
   */
  {
    const mkLvl = (params, knobs) => ({
      label: "Pads", child_count: 4, child_label: "Pad",
      child_key_template: "p{index}_{key}",
      child_index_base: 1, child_index_digits: 2,
      child_index_param: "ui_cur",
      child_key_overrides: { ui_cur: "ui_cur" },
      params, knobs,
    });
    const CP4 = [{ key: "ui_cur", name: "Cur", type: "int", min: 1, max: 4 }];
    for (let i = 1; i <= 4; i++) {
      const n = String(i).padStart(2, "0");
      CP4.push({ key: `p${n}_vol`, name: "Vol", type: "float", min: 0, max: 1, step: 0.01 });
    }
    const plan = (lvl) => PLAN.planPages({
      hierarchy: { modes: null, levels: { root: lvl } }, chainParams: CP4 });
    const hasPicker = (r) => (r.pages || r).some((p) => p.childOf);
    const hasCell = (r) => (r.pages || r).some((p) => (p.keys || []).indexOf("ui_cur") >= 0);

    /* Listed in params[] ONLY, and ui_-prefixed: no cell, so the picker stays. */
    const onlyParams = plan(mkLvl(["ui_cur", "vol"], ["vol"]));
    if (hasCell(onlyParams))
      fail("a ui_-prefixed key from params[] got a cell — this test no longer "
         + "covers the case it was written for");
    if (!hasPicker(onlyParams))
      fail("the child picker was suppressed while the index param has NO cell — "
         + "with the module`s own focus control off there is no way to change instance");

    /* On knobs[]: the author placed it, so it gets a cell and the picker goes. */
    const onKnobs = plan(mkLvl(["ui_cur", "vol"], ["ui_cur", "vol"]));
    if (!hasCell(onKnobs))
      fail("an authored knob was dropped for being ui_-prefixed — knobs[] is intent");
    if (hasPicker(onKnobs))
      fail("the picker survived even though the index param has its own cell — "
         + "two controls for one fact");
  }


  /* ---- a 1..16 selector steps like an enum, not like a sweep -----------
   *
   * One flick of an encoder is a dozen detents, so at one value per detent a
   * pad index or a MIDI channel crosses its whole range before you can read
   * it. Reported from the device as "these numbers move crazy fast on a single
   * detent".
   *
   * The BOUNDARY is what is pinned, not the constant: 1..16 selectors gated,
   * 0..24 quantities not. Measured over the fleet, 9..16 is entirely discrete
   * identities and 17..24 is entirely things you sweep.
   */
  {
    const gated = (min, max) => KE.detentsPerStep({ type: "int", min, max });
    if (gated(1, 16) <= 1)
      fail("a 1..16 selector steps once per detent — one flick crosses all 16");
    if (gated(1, 16) !== gated(0, 4))
      fail("a narrow int and a narrower one step differently — one gate, one feel");
    /* ...and a sweep must NOT be gated, or it becomes 4x harder to move. */
    if (gated(0, 24) !== 1)
      fail("a 0..24 quantity is gated like a selector — pitch bend range and "
         + "envelope depth are swept, not chosen");
    if (gated(0, 136) !== 1)
      fail("a wide int is gated — crossing it would take 500+ detents");
    /* The gate is the ENUM gate, shared. Two numbers here would be two feels
     * for controls that look alike. */
    if (gated(1, 16) !== KE.ENUM_DELTA_DIV)
      fail("the narrow-int gate is no longer the enum gate — they must stay one number");
  }


  /* ---- the selector is the page BEFORE what it selects ------------------
   *
   * A pad list is not somewhere you should land: with auto-select on you never
   * need it. It sits at index 0 -- ahead of the pages it governs, so you jog
   * BACK to reach it -- and firstGrid lands past it. Reported from the device
   * as "should it be like page -1?".
   *
   * It is also named for WHAT IT SELECTS. Inheriting the level`s name called
   * mrdrums` pad list "Main": a list of pads under the name of the page you
   * were looking for. A page you arrive at by going backwards has to say what
   * it is on its own.
   */
  {
    const L5 = { label: "Pads", child_count: 16, child_label: "Pad",
                 child_key_template: "p{index}_{key}", child_index_base: 1,
                 child_index_digits: 2, child_index_param: "ui_cur",
                 knobs: ["vol"], params: ["vol"] };
    const CP5 = [{ key: "ui_cur", name: "Cur", type: "int", min: 1, max: 16 }];
    for (let i = 1; i <= 16; i++)
      CP5.push({ key: `p${String(i).padStart(2,"0")}_vol`, name: "Vol",
                 type: "float", min: 0, max: 1, step: 0.01 });
    const pages = PLAN.planPages({
      hierarchy: { modes: null, levels: { root: L5 } }, chainParams: CP5 }).pages;

    const at = pages.findIndex((p) => p.childOf);
    if (at !== 0)
      fail("the child selector is at page " + at + "; it must PRECEDE the pages "
         + "it governs so it is reachable by jogging back and never in the way");
    if (pages[at].name !== "Selected Pad")
      fail("the selector is called " + JSON.stringify(pages[at].name)
         + ", expected \"Selected Pad\" — it must name what it selects, not its level");
    const land = NAV.firstGrid(pages);
    if (land === at)
      fail("the module LANDS on the pad list — with auto-select on you never need it");
    if (pages[land].kind !== "knobs")
      fail("landing page is " + pages[land].kind + ", expected a grid");
  }

  /* ---- a level with NO child_index_param is untouched ------------------ */
  {
    const L2 = Object.assign({}, LEVEL);
    delete L2.child_index_param;
    HIER.levels.root = L2;
    focusedWrites = 0;
    reads.length = 0;
    const c = mk();
    spin(c, 40);
    if (reads.some((k) => k.endsWith(":focused_pad")))
      fail("a level without child_index_param still read one -- the rotation "
         + "must cost nothing for the levels that do not declare it");
    HIER.levels.root = LEVEL;
  }

  if (bad === 0) {
    console.log("  ok  the wire speaks the module numbering, the engine is zero-based");
    console.log("  ok  a junk or missing index never moves the focus");
    console.log("  ok  the grid follows the module when it changes the focused child");
    console.log("  ok  choosing a child writes it back, so the two cannot disagree");
    console.log("  ok  a generic child key borrows the concrete declaration, and follows");
    console.log("  ok  the graphic follows the focused child with no page change");
    console.log("  ok  the picker goes only when the index param really has a cell");
    console.log("  ok  a 1..16 selector steps like an enum; a 0..24 sweep does not");
    console.log("  ok  the selector precedes what it selects, and is not where you land");
    console.log("  ok  a level that declares none reads none");
    console.log("PASS: a module can own which child instance is focused");
  }
  process.exit(bad ? 1 : 0);
});
'
