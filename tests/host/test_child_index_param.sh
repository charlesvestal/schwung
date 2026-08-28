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
]).then(([PC, CK]) => {
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
    console.log("  ok  a level that declares none reads none");
    console.log("PASS: a module can own which child instance is focused");
  }
  process.exit(bad ? 1 : 0);
});
'
