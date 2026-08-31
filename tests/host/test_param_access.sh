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

  /*
   * ---- turning a trigger FIRES it, ONCE PER GESTURE ----------------------
   *
   * A momentary has no value to walk, so refusing the turn only forced the
   * hand off the knob and onto the jog. What makes it safe is that one flick
   * of an encoder is a dozen detents and a trigger DOES a thing.
   *
   * A LATCH, NOT A RATE LIMIT, and the difference is the whole test. "At most
   * once per 250ms" was the first implementation and it still fired eight
   * times across a two-second spin -- reported from the device as "gesture
   * test fires repeatedly on detent". Every detent extends the gesture; only
   * stillness ends it.
   *
   * Asserted as a SEQUENCE, because each half passes a shorter test on its
   * own: a missing fire and a missing latch both look fine at one detent, and
   * a RATE LIMIT passes any test whose detents are spaced further apart than
   * the window. The long spin below is spaced at 30ms deliberately -- under a
   * rate limit of any plausible size it would fire several times.
   */
  writes.length = 0;
  ctl.onKnobTurn(trigSlot, 1, 5000);
  if (writes.length !== 1)
    fail("turning a trigger wrote " + writes.length + " times, expected 1");
  if (writes[0][1] !== "Rnd!")
    fail("a knob-fired trigger wrote " + JSON.stringify(writes[0][1]) + ", expected \"Rnd!\" — " +
         "the knob must put the same value on the wire as the click");

  /* A LONG spin -- two seconds of detents 30ms apart, far longer than any
   * fixed window. One gesture, so no more fires. */
  writes.length = 0;
  for (let t = 5030; t <= 7000; t += 30) ctl.onKnobTurn(trigSlot, 1, t);
  if (writes.length)
    fail("a 2-second spin fired the trigger " + writes.length + " extra times — " +
         "this is a rate limit, not a gesture latch");

  /*
   * The two sides of the gap, with GENEROUS MARGIN on purpose.
   *
   * These used to be 300ms and 500ms, which pinned the exact constant: tuning
   * the gap from 400 to 270 broke the test for no behavioural reason. What is
   * being asserted is that a gap EXISTS and has two sides, so "clearly inside"
   * is 100ms and "clearly outside" is a full second. Any sane value between
   * ~150ms and ~900ms passes, and a genuinely broken latch fails either way.
   */
  writes.length = 0;
  ctl.onKnobTurn(trigSlot, 1, 7100);          /* 100ms after the spin ended */
  if (writes.length)
    fail("a detent 100ms after the spin started a new gesture — the gap is far too short");

  /* Past the gap, a deliberate second flick is a second event. */
  writes.length = 0;
  ctl.onKnobTurn(trigSlot, 1, 8100);          /* a full second later */
  if (writes.length !== 1) fail("a second flick a second later did not fire");

  /* EITHER direction: a trigger has no up and no down, and a direction-
   * sensitive one would make half of every spin read as a dead knob. */
  writes.length = 0;
  ctl.onKnobTurn(trigSlot, -1, 9200);
  if (writes.length !== 1) fail("turning a trigger the other way did not fire it");

  /*
   * LETTING GO RE-ARMS IT IMMEDIATELY.
   *
   * The gap is a fallback for a cap sensor that never registered; a release is
   * the real gesture boundary and it is unambiguous. Without this you fire,
   * let go, grab the knob again and the next detent is swallowed for up to
   * 400ms -- which reads as the control being broken, not as a safety.
   *
   * Asserted at a time INSIDE the gap, so it can only pass because of the
   * release. 8450 is 50ms after the previous fire at 8400.
   */
  writes.length = 0;
  ctl.onKnobTouch(trigSlot, false);
  ctl.onKnobTurn(trigSlot, 1, 9250);          /* 50ms after the last fire */
  if (writes.length !== 1)
    fail("releasing the knob did not re-arm the trigger -- a detent 50ms later " +
         "was still swallowed by the gesture latch");

  /* ...and the gap still governs when there was NO release, which is what
   * keeps the fallback honest. */
  writes.length = 0;
  ctl.onKnobTurn(trigSlot, 1, 9300);
  if (writes.length)
    fail("without a release, a detent 50ms later fired -- the latch is gone");

  /* A CLICK is never latched: one press is one gesture, and a shared timer
   * between the two paths is exactly how that regresses. */
  writes.length = 0;
  ctl.onClick(trigSlot);
  ctl.onClick(trigSlot);
  if (writes.length !== 2)
    fail("clicks were gated by the knob gesture latch: " + JSON.stringify(writes));

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
      /*
       * A NAME THE HEURISTIC WILL NOT INFER, so the two renders still differ.
       *
       * This used `rnd_preset` with options ["-", "Rnd!"], which the inferred
       * -trigger heuristic now recognises on its own -- the very bug this
       * assertion documents, fixed. Both renders became the action mark and
       * the comparison had nothing left to measure. Keeping the assertion
       * meaningful needs a param the heuristic leaves alone, so the DECLARED
       * side is still the only thing that changes.
       */
      const decl = { key: "wave_shape", name: "Wave Shape", type: "enum",
                     options: ["\u2014", "Alt"] };
      if (access) decl.access = access;
      const ix2 = M.buildMetaIndex({ chainParams: [decl] });
      R.renderPageMovy(ctx, {
        page: { kind: "knobs", name: "P", keys: ["wave_shape"], level: "root" },
        metaIndex: ix2, values: { wave_shape: "\u2014" },
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

  /* ---- the animation must be WIRED, not merely implemented -----------
   *
   * This is the bug it shipped with, reported from the device as "i dont see
   * the animation". The renderer is pure and reads its clock and its fire
   * times off the options object, and the production draw call in
   * page_controller passed NEITHER -- so every button drew its idle phase
   * forever. The renderer tests above did not catch it because they hand
   * renderPageMovy both values directly, proving the renderer and never the
   * wiring.
   *
   * So this drives controller.render(), the real path.
   *
   * Its own controller with an INJECTED clock, for two reasons: the shared
   * one above has already fired this trigger, so it is mid-animation and both
   * snapshots would match (the first version of this test failed exactly
   * there), and settling on the wall clock would mean really sleeping. */
  {
    const fbmod = await import("./tools/param-pages/harness.mjs");
    const RPM = await import("./src/shared/param_pages/render_page_movy.mjs");
    let clock = 100000;
    const anim = C.createController({
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return HIER;
        if (b === "chain_params") return CP;
        if (b === "rnd_preset") return "\u2014";
        return "0";
      },
      setParam: () => {}, announce: () => {}, now: () => clock,
    });
    anim.load({ slot: 0, component: "synth" });
    for (let i = 0; i < 6; i++) anim.tick();
    /* The button only exists in the movy layout; the default is the dial grid,
     * where there would be no button and so no difference to detect. */
    anim.setLayout(RPM.LAYOUT_MOVY);
    const slot = (anim.page.keys || []).indexOf("rnd_preset");
    if (slot < 0) fail("the animation fixture did not put the trigger on the grid");

    /* A SIGNATURE of the buffer, not a pixel COUNT: the button travels, so a
     * count can be identical while the image is completely different. */
    const sig = () => {
      const fb = fbmod.createFramebuffer();
      anim.render(fbmod.drawContext(fb), { title: "T" });
      const px = fb.pixels || fb.px;
      let h = 0;
      for (let i = 0; i < 128 * 64; i++) if (px[i]) h = (h * 31 + i) | 0;
      return h;
    };

    const idle = sig();
    anim.onClick(slot);
    const pressed = sig();
    if (pressed === idle)
      fail("the button drew identically before and after a click -- the press " +
           "animation is not reaching the renderer through controller.render()");

    /* ...and it must come back down on its own, or it is a latch, not a press. */
    clock += 5000;
    if (sig() !== idle)
      fail("the button never returned to idle -- a press that stays down is a switch");
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
  console.log("  ok  the press animation reaches the screen through controller.render()");
  console.log("  ok  default is readwrite; plain enums and floats unaffected");
  console.log("  ok  readout: not turnable, not divable, never written");
  console.log("  ok  trigger: fires on click and on a detent, through the module wire (\"Rnd!\")");
  console.log("  ok  trigger: a whole spin is ONE fire; a second flick is a second; clicks are never gated");
  console.log("PASS: access read/write/readwrite");
});
'

# ---- the footer names the GESTURE, not the consequence ----------------------
#
# The hint vocabulary is a canon (JOG SEL / CLK OPEN / BACK EXIT) and the canon
# names what to DO. A trigger is drawn as a push button, so it asks to be
# PUSHed. It said FIRE first, which names the consequence instead -- a second
# thing to learn about a control the picture already explains.
#
# footerHints() is module-private and reads a module-level controller, so this
# is asserted at the source, the same way the other footer rules in this file
# are. The value is pinned, not merely the absence of MENU: "not MENU" would
# pass for any word at all, including the one being replaced.
pp="src/shadow/shadow_ui_param_pages.mjs"
wo=$(awk '/if \(meta && meta.writeOnly\)/,/^        }/' "$pp")
if [ -z "$wo" ]; then
  echo "FAIL: could not find the held-trigger footer branch in $pp" >&2
  exit 1
fi
# BOTH keys, ONE verb. It said CLK PUSH while the click was the only way to
# fire it -- "name the gesture the picture is asking for". A knob detent fires
# it too now, and you do not PUSH a knob you are turning, so no single
# gesture-name covers both keys and the honest word is the consequence.
if ! grep -q 'click: "FIRE"' <<<"$wo"; then
  echo "FAIL: a held trigger does not advertise CLK FIRE." >&2
  echo "$wo" >&2
  exit 1
fi
if ! grep -q '\["KNB", "FIRE"\]' <<<"$wo"; then
  echo "FAIL: a held trigger does not advertise KNB FIRE -- turning the knob" >&2
  echo "      fires it, and nothing on screen says so." >&2
  echo "$wo" >&2
  exit 1
fi
# The two must not disagree: one action, one verb.
if grep -q 'click: "PUSH"' <<<"$wo"; then
  echo "FAIL: the click still says PUSH while the knob says FIRE -- one action," >&2
  echo "      two verbs" >&2
  exit 1
fi
# The neighbouring vocabulary must not have moved with it.
#
# The branch also covers a cell that is divable only THROUGH the picture it is
# drawn in (granny's spray -- see vizDiveTarget), so it tests two things now.
# Anchored on the orderedHints call rather than on the condition, because the
# condition is the part that grew.
dv=$(awk '/meta.divable\) \|\|$/,/^        }/' "$pp")
if ! grep -q 'click: "OPEN"' <<<"$dv"; then
  echo "FAIL: a held divable no longer advertises CLK OPEN" >&2
  exit 1
fi
echo "  ok  a held trigger advertises CLK FIRE and KNB FIRE; divable still CLK OPEN"
