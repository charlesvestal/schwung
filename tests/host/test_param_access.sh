#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# `access` — which direction a parameter actually means something in.
#
#   "readwrite"  (default) an ordinary control
#   "read"       a READOUT: the value means something, writing means nothing
#   "write"      a TRIGGER: writing does something, the value means nothing
#
# Two ends of one axis, and both were unexpressible before 1.0.
#
# The read end is a design gap we walked into: keydetect's `detected_key` is 25
# key names with no set_param branch at all, deliberately, and documented as
# such back when an enum could only be nudged one detent. Enums became divable
# in 1.0, so the picker opened on it and silently discarded the choice.
#
# The write end is the dangerous one. euclidrum's `rnd_preset` declares
# ["—","Rnd!"] and fires on anything that is not the em-dash — so an INDEX
# write of "0", which MEANS the em-dash, "do nothing", randomises all eight
# lanes and destroys the kit. A trigger must be fired through the module's own
# enum wire, never as a bare number.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/page_controller.mjs"),
]).then(async ([M, C]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* ---- the axis itself --------------------------------------------- */
  const ix = M.buildMetaIndex({ chainParams: [
    { key: "cutoff",       type: "float", min: 0, max: 1 },
    { key: "mode",         type: "enum",  options: ["LP", "HP"] },
    { key: "detected_key", type: "enum",  options: ["C", "C#", "D"], access: "read" },
    { key: "rnd_preset",   type: "enum",  options: ["—", "Rnd!"], access: "write" },
  ]});
  const meta = (k) => ix.getOrGuess(k);

  /* An ordinary enum is unaffected — the default must stay readwrite. */
  if (!M.isTurnable(meta("mode"))) fail("a plain enum stopped being turnable");
  if (!meta("mode").divable)       fail("a plain enum stopped being divable");
  if (M.isReadOnly(meta("mode")) || M.isTrigger(meta("mode")))
    fail("a param with no access declared is not readwrite by default");
  if (!M.isTurnable(meta("cutoff"))) fail("a plain float stopped being turnable");

  /* A readout: nothing to set, nothing to open. */
  if (M.isTurnable(meta("detected_key"))) fail("a read-only param is still turnable");
  if (meta("detected_key").divable)       fail("a read-only param still opens a picker");
  if (!M.isReadOnly(meta("detected_key"))) fail("isReadOnly did not recognise access:read");

  /* A trigger: fired, not scrubbed, not opened. */
  if (M.isTurnable(meta("rnd_preset")))
    fail("a trigger is still turnable — turning it walks THROUGH the fire value");
  if (meta("rnd_preset").divable) fail("a trigger still opens a picker");
  if (!M.isTrigger(meta("rnd_preset"))) fail("isTrigger did not recognise access:write");

  /* ---- clicking a trigger FIRES it, through the module wire ---- */
  const writes = [];
  const HIER = JSON.stringify({ modes: null, levels: { root: { label: "S",
      knobs: ["rnd_preset", "detected_key"],
      params: [{ key: "rnd_preset" }, { key: "detected_key" }] } } });
  const CP = JSON.stringify([
    { key: "rnd_preset",   name: "Randomise", type: "enum", options: ["—", "Rnd!"], access: "write" },
    { key: "detected_key", name: "Key",       type: "enum", options: ["C", "C#"],        access: "read"  },
  ]);
  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      if (b === "rnd_preset") return "—";      /* a trigger reports its idle spelling */
      if (b === "detected_key") return "C#";
      return "0";
    },
    setParam: (k, v) => writes.push([k, v]),
    announce: () => {},
  });
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 6; i++) ctl.tick();

  const slotOf = (key) => (ctl.page.keys || []).indexOf(key);
  const trigSlot = slotOf("rnd_preset");
  const roSlot   = slotOf("detected_key");
  if (trigSlot < 0 || roSlot < 0) fail("fixture did not put both params on the grid");

  /* Clicking the trigger must WRITE, and must not hand back an open intent. */
  writes.length = 0;
  const intent = ctl.onClick(trigSlot);
  if (intent) fail("clicking a trigger returned an open intent instead of firing");
  if (writes.length !== 1) fail("clicking a trigger wrote " + writes.length + " times, expected 1");
  /* THE bug: the module fires on anything that is not the em-dash, so a bare
   * "0" would mean "do nothing" and a bare "1" is not its spelling either.
   * The module reports names, so the wire must be the NAME of option 1. */
  if (writes[0][1] !== "Rnd!")
    fail("a trigger fired with " + JSON.stringify(writes[0][1]) + ", expected \"Rnd!\" — " +
         "a bare index is exactly what destroys euclidrum kits");

  /* Turning a trigger must do nothing at all. */
  writes.length = 0;
  ctl.onKnobTurn(trigSlot, 1, 5000);
  if (writes.length) fail("turning a trigger wrote " + JSON.stringify(writes));

  /* A readout: click opens nothing, turn writes nothing. */
  writes.length = 0;
  if (ctl.onClick(roSlot)) fail("clicking a readout returned an intent");
  ctl.onKnobTurn(roSlot, 1, 6000);
  if (writes.length) fail("a readout was written to: " + JSON.stringify(writes));

  /* ---- a trigger must LOOK like a button ----------------------------
   *
   * It works and looks broken otherwise: the module reports a constant idle
   * spelling, and euclidrum reports an em-dash the 5x7 atlas cannot draw at
   * all, so the cell rendered as a BLANK square with no footer hint. Reported
   * from the device as "works, but we need some other way to do it than a
   * blank square and no footer".
   *
   * The square is drawn with a bitmap font through fillRect, not ctx.print, so
   * "blank" is a pixel question and is asserted as one: render the same cell
   * as a trigger and as a plain enum showing that same unrenderable value, and
   * the trigger must put glyph pixels inside the box where the plain enum puts
   * none. */
  {
    const R = await import("./src/shared/param_pages/render_page_movy.mjs");
    const render = (access) => {
      let pixels = 0;
      const ctx = {
        /* Count only small rects: the box frame is 1px lines the full width,
         * glyphs are little blocks. Both are fillRect, so size discriminates. */
        fillRect: (x, y, w, h) => { if (w <= 4 && h <= 6) pixels += w * h; },
        print: () => {}, textWidth: (t) => String(t).length * 4,
      };
      const decl = { key: "rnd_preset", name: "Rnd Preset", type: "enum",
                     options: ["\u2014", "Rnd!"] };
      if (access) decl.access = access;
      const ix2 = M.buildMetaIndex({ chainParams: [decl] });
      R.renderPageMovy(ctx, {
        page: { kind: "knobs", name: "P", keys: ["rnd_preset"], level: "root" },
        metaIndex: ix2, values: { rnd_preset: "\u2014" },
        pageIndex: 0, pageCount: 1, header: "T",
      });
      return pixels;
    };
    const plain = render(null);        /* today: an em-dash the font cannot draw */
    const trigger = render("write");   /* the action mark */
    if (trigger <= plain)
      fail("a trigger cell drew " + trigger + " glyph pixels vs " + plain +
           " for the unrenderable idle value — it is still blank");
  }

  /* ---- the button must stay INSIDE its row band ---------------------
   *
   * This is the bug it shipped with: a span from a to b is b - a + 1 rows,
   * so a button budgeted at 2*RY + DEPTH was one row taller than that and
   * overflowed into the label beneath it. The harness clipped() counter
   * cannot catch it, because those pixels are still on the screen.
   *
   * Rendered with a BLANK label, so any ink at or below the label row is the
   * widget overflowing and nothing else. */
  {
    const R = await import("./src/shared/param_pages/render_page_movy.mjs");
    const fbmod = await import("./tools/param-pages/harness.mjs");
    const probe = (access) => {
      const fb = fbmod.createFramebuffer();
      const ix3 = M.buildMetaIndex({ chainParams: [
        { key: "t", name: " ", type: "enum", options: ["a", "b"],
          ...(access ? { access } : {}) } ] });
      R.renderPageMovy(fbmod.drawContext(fb), {
        page: { kind: "knobs", name: "P", keys: ["t"], level: "root" },
        metaIndex: ix3, values: { t: "a" }, pageIndex: 0, pageCount: 1, header: " ",
        triggerFiredAt: { t: 1 }, nowMs: 40,      /* pressed: the tallest state */
      });
      const px = fb.pixels || fb.px;
      let lowest = -1;
      for (let y = 0; y < 64; y++) for (let x = 0; x < 128; x++)
        if (px[y * 128 + x]) { lowest = Math.max(lowest, y); }
      return lowest;
    };
    const band_end = R.ROW0_Y + 15 - 1;          /* BOX_H */
    const lowest = probe("write");
    if (lowest > band_end)
      fail("the trigger widget drew down to row " + lowest + ", past the band end at " +
           band_end + " — it is overflowing into the label");
  }

  console.log("  ok  trigger draws a button, never the unrenderable idle value");
  console.log("  ok  the button stays inside its 15-row band");
  console.log("  ok  default is readwrite; plain enums and floats unaffected");
  console.log("  ok  readout: not turnable, not divable, never written");
  console.log("  ok  trigger: fires once on click, through the module wire (\"Rnd!\"), not turnable");
  console.log("PASS: access read/write/readwrite");
});
'
