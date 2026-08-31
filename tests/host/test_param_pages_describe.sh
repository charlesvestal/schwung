#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# describePage() — the page as DATA, for a consumer that draws it itself
# (src/shared/param_pages/page_controller.mjs).
#
# This is the seam that lets a tool module with its own look — a sequencer
# showing these params next to its own transport — share everything above the
# pixels: the level walk, the metadata, the viz resolution, the read cursor, the
# throttles, the contract tri-state. See
# docs/plans/2026-08-28-param-pages-embeddable.md.
#
# What is pinned here, and why each one is the thing that would actually break:
#
#   - THE VIEW MODEL AND THE RENDERER AGREE ON THE WIDGET. Not "both call a
#     function named widgetKindFor" — the renderer's own cascade is re-derived
#     here from meta and compared, across the whole fleet, so a future edit to
#     either side that changes the verdict fails. Two definitions that agree
#     today is the shape this repo has already paid for (see isDoor).
#   - NO DEVICE READ. A consumer calling this per frame must not pay IPC; a
#     param read is ~2.8 ms against a 1.68 ms whole-page render.
#   - THE TRI-STATE SURVIVES. A failed ui_hierarchy read must not reach a
#     consumer as "this module declares no params".

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the describePage tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
  import("node:fs"),
]).then(([C, M, RM, P, D, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));

  const setup = (id, initial) => {
    const dev = D.createFakeDevice({ id, initial });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    return { dev, ctl };
  };

  /* ---- 1. the widget verdict matches the renderer cascade, fleetwide ---- */
  /*
   * The renderer decides in this order: opaque, then write-only, then enum,
   * then knob. Restated here ON PURPOSE — this is the one place a copy is
   * correct, because comparing describePage against a copy of the rule is what
   * detects the rule changing under either consumer.
   */
  const cascade = (meta) => {
    if (!meta) return "knob";
    if (meta.kind === M.KIND_OPAQUE) return "opaque";
    if (meta.writeOnly) return "button";
    if (meta.kind === M.KIND_ENUM) return "enum";
    /* BIGNUM came from main while this branch was extracting the same rule
     * elsewhere. Restated here from the renderer'"'"'s own predicate, not
     * reimplemented — the point of the copy is to notice the ORDER changing,
     * not to re-derive what counts as a small integer. */
    if (RM.shouldDrawBigNumber(meta)) return "bignum";
    return "knob";
  };

  /* ---- 1a. the CASCADE ORDER, which the fleet cannot pin --------------- */
  /*
   * No fleet module declares a param that is both opaque and write-only, so
   * swapping the first two tests of widgetKindFor is invisible to every check
   * that runs on fleet data — including the 1434-render snapshot suite. That
   * was verified by mutation: the swap passed everything.
   *
   * The order is a real decision (an opaque write-only param stays a DOOR
   * rather than becoming a bang with nothing to open), so it is pinned here
   * directly, on synthetic metadata, where the conflict actually exists.
   */
  {
    const both = { kind: M.KIND_OPAQUE, writeOnly: true, key: "x" };
    if (RM.widgetKindFor(both) !== RM.WIDGET_OPAQUE) {
      fail("an opaque write-only param must stay a door, got " + RM.widgetKindFor(both));
    }
    const enumWriteOnly = { kind: M.KIND_ENUM, writeOnly: true, key: "x" };
    if (RM.widgetKindFor(enumWriteOnly) !== RM.WIDGET_BUTTON) {
      fail("a write-only enum is a trigger, not an enum square");
    }
    if (RM.widgetKindFor(null) !== RM.WIDGET_KNOB) fail("absent metadata must fall back to a knob");
    if (RM.widgetKindFor({ kind: M.KIND_ENUM, key: "x" }) !== RM.WIDGET_ENUM) fail("enum misclassified");
    if (RM.widgetKindFor({ kind: M.KIND_NUMBER, key: "x" }) !== RM.WIDGET_KNOB) fail("number misclassified");
  }

  let cells = 0, modules = 0;
  const seen = Object.create(null);
  for (const mod of fx.modules) {
    if (!mod.ui_hierarchy && !mod.chain_params) continue;
    let ctl;
    try { ctl = setup(mod.id).ctl; } catch (e) { continue; }
    if (!ctl.pages.length) continue;
    modules++;
    for (let i = 0; i < ctl.pages.length; i++) {
      ctl.goToPage(i);
      const vm = ctl.describePage({ title: "T1 > " + mod.id });
      if (!vm || !vm.header) fail(mod.id + " page " + i + ": no view model");
      const page = ctl.page;
      if (page.kind !== P.PAGE_KNOBS) {
        if (vm.cells.length) fail(mod.id + ": a non-knobs page produced cells");
        continue;
      }
      if (vm.cells.length !== page.keys.length) {
        fail(mod.id + " page " + i + ": " + vm.cells.length + " cells for "
             + page.keys.length + " keys");
      }
      for (const c of vm.cells) {
        if (!c) continue;
        cells++;
        seen[c.widget] = (seen[c.widget] || 0) + 1;
        const meta = ctl.metaIndex.getOrGuess(c.key);
        const want = cascade(meta);
        if (c.widget !== want) {
          fail(mod.id + " " + c.key + ": view model says widget=" + c.widget
               + ", the renderer cascade says " + want);
        }
        if (typeof c.label !== "string" || !c.label.length) {
          fail(mod.id + " " + c.key + ": empty cell label");
        }
        if (typeof c.value !== "string") {
          fail(mod.id + " " + c.key + ": value is not a string");
        }
        if (c.normalized !== null && (c.normalized < 0 || c.normalized > 1)) {
          fail(mod.id + " " + c.key + ": normalized out of range: " + c.normalized);
        }
      }
    }
  }
  if (modules < 50) fail("only exercised " + modules + " modules; the fleet fixture should carry far more");
  if (cells < 500) fail("only " + cells + " cells described; expected the fleet");
  /* All four widgets must actually occur, or the agreement check above is
   * passing on a subset and would not notice three of the four branches. */
  for (const w of ["knob", "enum", "opaque", "button", "bignum"]) {
    if (!seen[w]) fail("no " + w + " cell in the whole fleet — the widget check is not covering it");
  }

  /* ---- 2. describing costs no device reads ----------------------------- */
  {
    const { dev, ctl } = setup("obxd");
    for (let i = 0; i < 40; i++) ctl.tick();     /* let the cursor fill values */
    dev.resetCounters();
    for (let i = 0; i < 20; i++) ctl.describePage({ title: "T1 > OB-XD" });
    if (dev.reads.length) {
      fail("describePage issued " + dev.reads.length + " device reads; it must read none");
    }
    if (dev.writes.length) fail("describePage wrote to the device");
  }

  /* ---- 3. a failed contract read does not read as an empty module ------- */
  {
    const { dev, ctl } = setup("obxd");
    const vm = ctl.describePage({ title: "x" });
    if (vm.unresolved) fail("a healthy module reported unresolved");

    const dev2 = D.createFakeDevice({ id: "obxd" });
    dev2.failParam("ui_hierarchy", 99);
    const ctl2 = C.createController(dev2);
    ctl2.load({ slot: 0, component: "synth" });
    const vm2 = ctl2.describePage({ title: "x" });
    if (!vm2.unresolved) {
      fail("a failed ui_hierarchy read produced unresolved=false — the tri-state was flattened");
    }
  }

  /* ---- 4. p-lock decorations reach the view model ---------------------- */
  {
    const { ctl } = setup("obxd");
    ctl.setDecorations([{ value: 0.5, locked: true }, null, null, null, null, null, null, null]);
    const vm = ctl.describePage({ title: "x" });
    const c0 = vm.cells[0];
    if (!c0) fail("slot 0 has no cell to decorate");
    if (!c0.locked) fail("a locked decoration did not reach the view model");
    if (vm.cells[1] && vm.cells[1].locked) fail("an undecorated slot reported locked");
  }

  /* ---- 5. the header is the renderers header, not a second one --------- */
  {
    const { ctl } = setup("obxd");
    const plain = ctl.describePage({ title: "T1 > OB-XD" });
    if (plain.header.inverted) fail("an untouched page reported an inverted header");
    if (plain.header.left !== "T1 > OB-XD") fail("header left is not the title: " + plain.header.left);

    ctl.onKnobTouch(0, true);
    const held = ctl.describePage({ title: "T1 > OB-XD" });
    if (!held.header.inverted) fail("a held knob did not invert the header");
    if (held.header.left === "T1 > OB-XD") {
      fail("a held knob did not take the header over");
    }
  }

  console.log("PASS: describePage — " + cells + " cells across " + modules
      + " modules, widget verdict matches the renderer, no device reads, tri-state intact");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
