#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The footer on a KNOB PAGE DRAWN AS A LIST.
#
# With Param View on List — or with the screen reader on, which forces the
# layout — a knob page becomes five rows driven entirely by the JOG. It is a
# door: click enters, the jog is the row cursor, click opens the row, the jog is
# then the value, Back steps out one layer at a time.
#
# footerHints() had NO branch for any of that and fell through to the grid's
# answer, `JOG PAGE / CLK MENU`, which is wrong in all three states. Reported
# from the device against Global Settings, where the jog is the only control
# being used: "when you're clicked in it actually still says jog page".
#
# The ORDERING assertion at the end is the one that is easy to lose. In this
# layout onClick takes its param from the ROW CURSOR and overrides whatever
# knob is under your hand, so the held-knob branch must not get there first —
# it would describe a cell the click will not act on. That branch's own
# comments record the same promise-versus-behaviour bug twice, reached from the
# other side, which is why it is pinned rather than left to reading order.
#
# footerHints is not exported and the file cannot be imported (its imports are
# absolute /data/UserData paths), so the function is LIFTED with new Function
# and a fixed dependency list, the same way test_chain_edit_read_budget.sh
# lifts drawChainEdit.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
const fs = require("fs");
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
]).then(async ([C, M, P]) => {
  let failures = 0;
  const fail = (m) => { console.error("FAIL: " + m); failures++; };

  /* ---- lift footerHints and orderedHints out of the shadow UI ---------- */
  const SRC = "src/shadow/shadow_ui_param_pages.mjs";
  const text = fs.readFileSync(SRC, "utf8");
  const lift = (name) => {
    const head = text.indexOf("function " + name + "(");
    if (head < 0) throw new Error(SRC + " has no function " + name);
    /* Top-level functions in this file close on a brace in column 0. */
    const end = text.indexOf("\n}\n", head);
    if (end < 0) throw new Error("could not find the end of " + name);
    return text.slice(head, end + 3);
  };

  const make = (controller) => new Function(
    "controller", "shiftIsHeld", "sectionsAreDistinct",
    "PAGE_KNOBS", "PAGE_MENU", "PAGE_PRESET", "PAGE_ITEMS",
    "flipsOnClick", "isTurnable",
    lift("orderedHints") + "\n" + lift("footerHints") + "\nreturn footerHints;"
  )(controller, () => false, () => false,
    P.PAGE_KNOBS, P.PAGE_MENU, P.PAGE_PRESET, P.PAGE_ITEMS,
    M.flipsOnClick, M.isTurnable);

  /* ---- a real controller, in LIST layout ------------------------------- */
  let clock = 10000;   /* driven explicitly: the two-way latch is measured in ms */
  const store = { cutoff: "0.5", flip2: "Off", pick3: "A", trig2: "—", ro2: "No" };
  const HIER = JSON.stringify({ modes: null, levels: { root: { label: "S",
      knobs: ["cutoff", "flip2", "pick3", "trig2", "ro2"],
      params: [{ key: "cutoff" }, { key: "flip2" }, { key: "pick3" },
               { key: "trig2" }, { key: "ro2" }] } } });
  const CP = JSON.stringify([
    { key: "cutoff", name: "Cutoff",  type: "float", min: 0, max: 1, step: 0.01 },
    { key: "flip2",  name: "Mirror",  type: "enum", options: ["Off", "On"] },
    { key: "pick3",  name: "Overlay", type: "enum", options: ["A", "B", "C"] },
    { key: "trig2",  name: "Rnd",     type: "enum", options: ["—", "Rnd!"], access: "write" },
    { key: "ro2",    name: "Key",     type: "enum", options: ["No", "Yes"],      access: "read"  },
  ]);
  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      return b in store ? store[b] : "0";
    },
    setParam: () => {},
    announce: () => {},
    now: () => clock,
  });
  ctl.setLayout(C.LAYOUT_LIST);
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 8; i++) ctl.tick();
  ctl.dismissHint();

  const hints = make(ctl);
  const flat = () => (hints() || []).map((h) => h.join(" ")).join(" / ");
  const pairFor = (key) => {
    const h = hints() || [];
    const found = h.find((p) => p[0] === key);
    return found ? found[1] : null;
  };

  if (ctl.page.kind !== P.PAGE_KNOBS) fail("expected a knob page, got " + ctl.page.kind);
  if (!ctl.isDoor()) fail("a knob page in LIST layout should be a door");

  /* ---- 1. not entered: the jog still pages, and the click goes IN ------- */
  if (pairFor("JOG") !== "PAGE") fail("outside the list the jog should still page, got " + flat());
  if (pairFor("CLK") !== "ENTER")
    fail("outside the list the click enters it; the footer said " + flat() +
         " — CLK MENU is the GRID answer and there is no grid here");

  /* ---- 2. entered: the jog is the ROW CURSOR ---------------------------- */
  ctl.enterMenu();
  if (!ctl.menuEntered()) fail("enterMenu did not enter a knobs-as-list page");
  if (pairFor("JOG") !== "SEL")
    fail("inside the list the jog moves the cursor, but the footer still says " +
         JSON.stringify(pairFor("JOG")) + " — this is the reported bug verbatim");
  if (pairFor("BACK") !== "OUT") fail("inside the list Back steps out one layer, got " + flat());

  /* ---- 3. the click verb is the ROW'"'"'S, not one word for all rows ------- */
  /* The row cursor CLAMPS at both ends rather than wrapping, so a seek has to
   * rewind first and every loop has to be bounded — an unbounded "jog until
   * you get there" spins forever the moment the target is behind you. */
  const seekRow = (key) => {
    const rows = ctl.knobRows();
    const at = rows.findIndex((r) => r && r.key === key);
    if (at < 0) { fail("row " + key + " is not on the page"); return false; }
    for (let i = 0; i < rows.length + 2 && ctl.knobRowIndex() > 0; i++) ctl.onJog(-1);
    for (let i = 0; i < rows.length + 2 && ctl.knobRowIndex() < at; i++) ctl.onJog(1);
    if (ctl.knobRowIndex() !== at) { fail("could not reach row " + key); return false; }
    return true;
  };
  const verbAtRow = (key) => (seekRow(key) ? pairFor("CLK") : null);
  if (verbAtRow("cutoff") !== "EDIT")
    fail("a plain float hands the jog to the value; the footer said " + verbAtRow("cutoff"));
  /* A two-option enum FOCUSES like every other turnable row — it does not flip
   * here and it does not open. That is the request verbatim: "the same gesture
   * for each row". The grid flips the same param; the two surfaces read the
   * widened gate from one predicate, so they cannot drift about WHICH params. */
  if (verbAtRow("flip2") !== "EDIT")
    fail("a two-option enum should focus like any other row; the footer said " +
         verbAtRow("flip2") + " — FLIP is the answer the GRID gives, and there is no knob under " +
         "your hand here");
  if (verbAtRow("pick3") !== "OPEN")
    fail("a three-option enum opens its list; the footer said " + verbAtRow("pick3"));
  if (verbAtRow("trig2") !== "FIRE")
    fail("a trigger fires; the footer said " + verbAtRow("trig2"));
  /* A readout does nothing at all, so it gets no CLK pair. A verb here would
   * be a promise; an absence is the truth. */
  if (verbAtRow("ro2") !== null)
    fail("a readout advertised " + JSON.stringify(verbAtRow("ro2")) + " over a click that does nothing");

  /* ---- 4. editing a row: the jog IS the value --------------------------- */
  seekRow("cutoff");
  ctl.onClick(0);
  if (!ctl.knobEditing) fail("clicking a float row should start editing it");
  if (pairFor("JOG") !== "ADJ") fail("while editing, the jog adjusts the value; got " + flat());
  if (pairFor("CLK") !== "DONE") fail("while editing, the click is done; got " + flat());

  /* ...and a TWO-OPTION ENUM does the same, and the jog then STEPS it. This is
   * the half a footer assertion cannot reach: EDIT could be advertised over a
   * row the jog does nothing to. */
  ctl.exitMenu();                 /* leave edit mode, stay on the row cursor */
  seekRow("flip2");
  ctl.onClick(0);
  if (!ctl.knobEditing)
    fail("clicking a two-option enum row should FOCUS it, not flip or open it");
  const beforeFlip = ctl.state.values["flip2"];
  ctl.onJog(1);
  const afterUp = ctl.state.values["flip2"];
  if (afterUp === beforeFlip)
    fail("the jog did not move a focused two-option enum — focus without a working jog is " +
         "worse than the flip it replaced");

  /* flip2 is Off/On, which is what DRAWS AS A SWITCH, and a switch turns
   * direction-absolute: up is on and down is off, from wherever it already was.
   * The either-way toggle belongs to the BOXED two-way (Mix/Reverb) and is
   * pinned in tests/host/test_two_way_knob_toggle.sh along with the rule that
   * the two partitions must stay equal. What is checked HERE is only that a
   * focused list row reaches the same engine the grid does. */
  if (afterUp !== "On")
    fail("an UP detent on a focused Off/On row gave " + afterUp + " — clockwise is ON");
  ctl.onJog(-1);
  if (ctl.state.values["flip2"] !== "Off")
    fail("a DOWN detent on a focused Off/On row gave " + ctl.state.values["flip2"] +
         " — anticlockwise is OFF");

  /* And it is IDEMPOTENT: a whole spin one way says the same thing every
   * detent, so an already-on switch stays on. */
  clock += 2000;
  for (let i = 0; i < 20; i++) { clock += 30; ctl.onJog(1); }
  if (ctl.state.values["flip2"] !== "On")
    fail("a 20-detent clockwise spin left a switch at " + ctl.state.values["flip2"] +
         " — a switch write is idempotent, so it must land on ON however many detents it took");

  /* ---- 5. A HELD KNOB MUST NOT CLAIM THIS FOOTER ------------------------ */
  ctl.exitMenu();                 /* back to the row cursor */
  seekRow("cutoff");
  const pick3Slot = (ctl.page.keys || []).indexOf("pick3");
  if (pick3Slot < 0) fail("pick3 is not on the page");
  ctl.onKnobTouch(pick3Slot, true, 1000);
  if (pairFor("JOG") !== "SEL" || pairFor("CLK") !== "EDIT")
    fail("holding a knob rewrote the list footer to " + flat() + " — in this layout the " +
         "click acts on the ROW CURSOR, so the held-knob branch is describing a cell the " +
         "click will not touch");
  ctl.onKnobTouch(pick3Slot, false, 1010);

  /* ---- 6. every word is in the canon, and the row fits ------------------ */
  const R = await import("./src/shared/param_pages/render_page_movy.mjs");
  const KEYS = new Set(R.FOOTER_CANON.keys);
  for (const state of [() => ctl.exitMenu(), () => ctl.enterMenu()]) {
    state();
    const h = hints() || [];
    for (const p of h) if (!KEYS.has(p[0])) fail("footer key " + p[0] + " is not in FOOTER_CANON");
    const w = h.reduce((a, p) => a + R.hintPairWidth(p[0], p[1]), 0);
    if (w > 128 && h.length <= 2)
      fail("a two-pair footer overflowed at " + w + "px: " + flat());
  }

  if (failures) process.exit(1);
  console.log("PASS: a knobs-as-list page reports its own three states, the click verb is the " +
              "row’s, and a held knob does not claim the footer");
}).catch((e) => { console.error("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
