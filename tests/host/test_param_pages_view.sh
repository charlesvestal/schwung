#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The shadow-UI view module (src/shadow/shadow_ui_param_pages.mjs).
#
# Shadow view modules import shared code by its DEPLOYED path
# (/data/UserData/schwung/shared/...), which is correct on device and
# unloadable in node. So this test makes a temp copy with those imports pointed
# at the repo, supplies a fake `ctx` and fake display globals, and actually
# RUNS it — rather than pinning its source with a regex and hoping.
#
# What is being checked is only what this file adds: routing, the two hand-offs
# it deliberately does not perform itself (opaque params and non-grid pages go
# to screens that already exist), and the screen-reader override. Everything
# with a decision in it is tested in test_param_pages_controller.sh and
# test_param_pages_input.sh.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the view tests" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
REPO="$PWD"

# Point the deployed import paths at the repo, and the ctx import at the real one.
sed "s#/data/UserData/schwung/shared/#${REPO}/src/shared/#g; s#'./shadow_ui_ctx.mjs'#'${REPO}/src/shadow/shadow_ui_ctx.mjs'#" \
    src/shadow/shadow_ui_param_pages.mjs > "$TMP/view.mjs"

REPO="$REPO" VIEW="$TMP/view.mjs" node -e '
const REPO = process.env.REPO;

/* ---- fake device globals the view module draws through ------------------ */
let cleared = 0;
const drawCalls = [];
globalThis.clear_screen = () => { cleared++; };
globalThis.fill_rect = (...a) => { drawCalls.push(a); };
globalThis.print = (...a) => { drawCalls.push(a); };
globalThis.text_width = (t) => String(t == null ? "" : t).length * 6;
/* The knob indicator ring LEDs (CC 71-78) go out through this. */
globalThis.move_midi_internal_send = () => 0;
/* draw_line / fill_circle: native shapes the Movy-style renderer prefers
 * over a JS-side Bresenham/circle walk when the host provides them — see
 * src/host/js_display.c and viz_draw.mjs / render_page_movy.mjs. */
globalThis.draw_line = (...a) => { drawCalls.push(a); };
globalThis.fill_circle = (...a) => { drawCalls.push(a); };
globalThis.host_send_screenreader = (t) => spoken.push(t);
globalThis.shadow_get_display_mode = () => 1;
let ttsOn = false;
globalThis.tts_get_enabled = () => ttsOn;
let paramView = 1;
globalThis.param_view_get_mode = () => paramView;
let shiftHeld = 0;
globalThis.shadow_get_shift_held = () => shiftHeld;
const spoken = [];

Promise.all([
  import(process.env.VIEW),
  import(REPO + "/src/shadow/shadow_ui_ctx.mjs"),
  import(REPO + "/tools/param-pages/fake_device.mjs"),
]).then(([V, C, D]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  /* ---- a fake shadow_ui: only what this view module touches ------------- */
  const dev = D.createFakeDevice({ id: "obxd", initial: { cutoff: 50 } });
  const views = [];
  const opened = [];
  Object.assign(C.ctx, {
    VIEWS: { PARAM_PAGES: "parampages", CHAIN_EDIT: "chainedit", MASTER_FX: "masterfx" },
    setView: (v) => views.push(v),
    getSlotParam: (slot, key) => dev.getParam(key),
    setSlotParam: (slot, key, value) => dev.setParam(key, value),
    getModuleAbbrev: () => "OB-XD",
    openParamEditor: (slot, key, meta) => opened.push([slot, key, meta && meta.kind]),
    evaluateVisibilityCondition: undefined,
  });

  /* ---- 1. the setting gates it, and the screen reader overrides --------- */
  {
    paramView = 0; ttsOn = false;
    if (V.paramPagesEnabled()) fail("Param View = List must not enable the grid");
    paramView = 1;
    if (!V.paramPagesEnabled()) fail("Param View = Knobs should enable the grid");
    /* A grid has eight cells and nothing selected; until the announce calls are
     * proven on hardware, the list stays the accessible surface. */
    ttsOn = true;
    if (V.paramPagesEnabled()) fail("the screen reader must force the list regardless of the setting");
    ttsOn = false;
  }

  /* ---- 2. entering points at a component and switches view -------------- */
  {
    if (V.paramPagesActive()) fail("should not be active before entering");
    V.enterParamPages(0, "synth", "synth");
    if (!V.paramPagesActive()) fail("entering did not activate");
    if (views[views.length - 1] !== "parampages") fail("entering did not switch view");
    const page = V.currentParamPage();
    if (!page) fail("no current page after entering");
    if (!spoken.length) fail("entering announced nothing");
  }

  /* ---- 3. tick reads exactly one param per frame ------------------------ */
  {
    dev.resetCounters();
    V.tickParamPages();
    /* One is_loading poll plus one staggered value read. */
    const valueReads = dev.reads.filter((k) => !/is_loading|ui_hierarchy|chain_params/.test(k));
    if (valueReads.length !== 1) fail("a frame issued " + valueReads.length + " value reads, must be 1");
  }

  /* ---- 4. it draws, and only draws grids -------------------------------- */
  {
    for (let i = 0; i < 12; i++) V.tickParamPages();
    cleared = 0; drawCalls.length = 0;
    if (!V.drawParamPages()) fail("drawParamPages should report that it drew");
    if (!cleared) fail("the view did not clear the screen");
    if (drawCalls.length < 20) fail("the view barely drew anything: " + drawCalls.length + " calls");
  }

  /* ---- 5. MIDI routes through, and back hands the view back ------------- */
  {
    const before = dev.writes.length;
    for (let i = 0; i < 20; i++) V.handleParamPagesMidi([0xb0, 71, 1]);
    if (dev.writes.length <= before) fail("turning knob 1 wrote nothing");

    /* CC 50 (Menu) is not forwarded to the shadow UI and is not ours. */
    if (V.handleParamPagesMidi([0xb0, 50, 127])) fail("an unrelated CC should not be consumed");

    V.handleParamPagesMidi([0xb0, 51, 127]);
    if (V.paramPagesActive()) fail("back should leave the view");
    if (views[views.length - 1] !== "chainedit") fail("back should hand the view back to the chain editor");
  }

  /* ---- 5b. shift comes from shared memory, never from CC 49 ------------- */
  {
    /* The shim forwards CC 3, 14, 51, 40-43, 71-78 and 88 to the shadow UI and
     * NOT CC 49 — it tracks shift itself and publishes it in SHM. A view that
     * waits for a shift CC silently loses every shift gesture. */
    const src = fs.readFileSync(REPO + "/src/shadow/shadow_ui_param_pages.mjs", "utf8");
    if (!/shadow_get_shift_held/.test(src)) {
      fail("the view must read shift from shadow_get_shift_held — the shim never forwards CC 49");
    }
    if (/intent\.type === .shift./.test(src)) {
      fail("the view is tracking shift from a CC the shim does not deliver");
    }

    V.enterParamPages(0, "synth", "synth");
    for (let i = 0; i < 12; i++) V.tickParamPages();

    /* Holding shift must reveal values without any MIDI arriving. */
    shiftHeld = 1;
    V.tickParamPages();
    if (!V.paramPagesRevealing()) fail("holding shift did not reveal values via the SHM path");
    shiftHeld = 0;
    V.tickParamPages();
    if (V.paramPagesRevealing()) fail("releasing shift did not clear reveal");

    /* And a shift-modified gesture must actually reach the input decoder.
     * Proved with FINE ADJUST rather than shift+jog: from a single-page level,
     * or from the last page of a multi-page one, a section step and a page step
     * legitimately land on the same place, so that comparison proves nothing.
     * Encoder resolution has no such ambiguity. */
    V.exitParamPages();
    const devF = D.createFakeDevice({ id: "branchage" });
    C.ctx.getSlotParam = (slot, key) => devF.getParam(key);
    C.ctx.setSlotParam = (slot, key, value) => devF.setParam(key, value);
    V.enterParamPages(0, "synth", "synth");
    for (let i = 0; i < 24; i++) V.tickParamPages();

    const floatSlot = 0;   /* branchage page 1 knob 1 is a float */
    const readLast = () => {
      const w = devF.writes[devF.writes.length - 1];
      return w ? Number(w[1]) : NaN;
    };
    /* setParam is throttled per key (SETPARAM_THROTTLE_MS in
     * page_controller.mjs -- a fast physical spin decodes to 250-320 CC
     * messages per second on device, and writing every single one was what
     * dropped the grids own redraw rate under that load) -- a burst sent
     * with no real time between messages, as this test does, lands inside
     * one throttle window and only the first write reaches the device
     * immediately. Releasing the knob (note-off) flushes whatever settled
     * value is still pending, same as it would on real hardware the instant
     * release happens -- so that is how this test observes the final
     * position. */
    const release = () => V.handleParamPagesMidi([0x90, floatSlot, 0]);
    shiftHeld = 0;
    for (let i = 0; i < 10; i++) V.handleParamPagesMidi([0xb0, 71 + floatSlot, 1]);
    release();
    const afterCoarse = readLast();
    shiftHeld = 1;
    for (let i = 0; i < 10; i++) V.handleParamPagesMidi([0xb0, 71 + floatSlot, 1]);
    release();
    const afterFine = readLast();
    shiftHeld = 0;

    const coarse = afterCoarse - Number(devF.writes[0][1]);
    const fine = afterFine - afterCoarse;
    if (!(coarse > 0 && fine > 0)) fail("the knob did not move in both modes: " + coarse + " / " + fine);
    if (!(coarse / fine > 4)) {
      fail("shift did not reach the input decoder — fine adjust was only " +
           (coarse / fine).toFixed(1) + "x finer than coarse");
    }
    V.exitParamPages();
  }

  /* ---- 6. a DIVABLE param is handed to the existing editor -------------- */
  {
    const dev2 = D.createFakeDevice({ id: "mrdrums" });
    C.ctx.getSlotParam = (slot, key) => dev2.getParam(key);
    C.ctx.setSlotParam = (slot, key, value) => dev2.setParam(key, value);
    V.enterParamPages(0, "synth", "synth");
    for (let i = 0; i < 12; i++) V.tickParamPages();

    const page = V.currentParamPage();
    if (!page) fail("no page for mrdrums");
    /* Hold the knob whose param opens an editor, then click it. Note this is
     * no longer the same as "a knob cannot turn it": mrdrums pad_start is a
     * ranged wav_position, so it is turnable AND divable. */
    let slot = -1;
    for (let i = 0; i < 8; i++) {
      const before = opened.length;
      V.handleParamPagesMidi([0x90, i, 100]);
      V.handleParamPagesMidi([0xb0, 3, 127]);
      if (opened.length > before) { slot = i; break; }
      V.handleParamPagesMidi([0x80, i, 0]);
    }
    if (slot < 0) fail("clicking a held divable param never reached the existing editor");
    const [, key, kind] = opened[opened.length - 1];
    const handed = V.paramPagesController
      ? V.paramPagesController().metaAt(slot) : null;
    if (handed && !handed.divable) fail("handed a non-divable param to the editor: " + kind);
    if (!/^synth:/.test(key)) fail("the editor was handed an unprefixed key: " + key);
    V.exitParamPages();
  }

  /* ---- 7. non-grid pages are not this view to draw ---------------------- */
  {
    const dev3 = D.createFakeDevice({ id: "minijv" });
    C.ctx.getSlotParam = (slot, key) => dev3.getParam(key);
    C.ctx.setSlotParam = (slot, key, value) => dev3.setParam(key, value);
    V.enterParamPages(0, "synth", "synth");
    /*
     * The grid now owns EVERY page minijv plans, so jogging never ejects.
     *
     * This used to assert the opposite: that jogging back reached a kind the
     * grid did not own (the mode select), and that drawParamPages refused it.
     * That was true while PAGE_MODES was planned and rendered by nobody --
     * which is exactly what made the performance mode of minijv unreachable from
     * the grid. The mode selector is an items page now, and the child selector
     * with it, so there is nothing left to hand off.
     *
     * minijv is the module that had all three unowned features, so if any kind
     * can still escape the grid it will escape here.
     */
    if (V.currentParamPage().kind !== "knobs") fail("entering should land on a grid page");
    const ownKinds = ["knobs", "menu", "preset", "items"];
    for (let i = 0; i < 12; i++) {
      V.handleParamPagesMidi([0xb0, 14, 127]);   /* jog anticlockwise */
      const page = V.currentParamPage();
      if (page && ownKinds.indexOf(page.kind) < 0)
        fail("jogging reached a " + page.kind + " page the grid does not own — it will eject to the list");
      if (page && !V.drawParamPages())
        fail("the grid refused to draw its own " + page.kind + " page");
    }
    V.exitParamPages();
  }

  /* ---- 8. the jump index is offered for big modules --------------------- */
  {
    const dev4 = D.createFakeDevice({ id: "surge" });
    C.ctx.getSlotParam = (slot, key) => dev4.getParam(key);
    C.ctx.setSlotParam = (slot, key, value) => dev4.setParam(key, value);
    V.enterParamPages(0, "synth", "synth");
    const idx = V.paramPagesJumpIndex();
    if (!idx.length) fail("no jump index for a 51-page module");
    if (idx.length >= 51) fail("the jump index should be shorter than the page list, got " + idx.length);
    V.paramPagesGoTo(idx[idx.length - 1].index);
    if (!V.currentParamPage()) fail("jumping left no current page");
    V.exitParamPages();
  }

  /* ---- 9. the same grid, opened from MASTER FX ------------------------- *
   *
   * Master FX is a chain editor too, and the Param View setting was silently
   * slot-chain-only: the same module opened the labelled knob grid in a slot
   * and the scrolling hierarchy list on the master bus. Reported from the
   * device the day after the knob card shipped, and the same drift section 1b
   * of the Master FX variable-length design exists to end.
   *
   * The grid serves either chain given three pieces of CHROME, and all three
   * fail QUIETLY if they are wrong, which is why each is asserted separately:
   *
   *   label      "MFX", never "S1" -- the master bus is ADDRESSED at IPC slot
   *              0 by convention and is not instrument slot 1.
   *   moduleKey  "master_fx:fx1:module". The slot chain spelling "<prefix>_module"
   *              spelling is simply unserved here, and an unserved key reads
   *              back as "" rather than erroring, so the header would lose its
   *              module name with nothing in the logs.
   *   returnView Back goes to the Master FX editor. Hardcoded, it dropped the
   *              user into the slot chain editor instead.
   */
  {
    const devM = D.createFakeDevice({ id: "obxd", prefix: "master_fx:fx1" });
    let abbrevArg = null;
    C.ctx.getSlotParam = (slot, key) =>
      (key === "master_fx:fx1:module" ? "cloudseed" : devM.getParam(key));
    C.ctx.setSlotParam = (slot, key, value) => devM.setParam(key, value);
    C.ctx.getModuleAbbrev = (id) => { abbrevArg = id; return "CS"; };

    V.enterParamPages(0, "master_fx:fx1", "master_fx:fx1", null, null,
                      { label: "MFX", moduleKey: "master_fx:fx1:module",
                        returnView: C.ctx.VIEWS.MASTER_FX });
    for (let i = 0; i < 12; i++) V.tickParamPages();

    drawCalls.length = 0;
    if (!V.drawParamPages()) fail("the grid did not draw for a Master FX component");

    /* Read through headerTitle(), which is what drawParamPages passes to the
     * renderer. It cannot be read back off the framebuffer: the movy renderer
     * sets the header in its own font and draws every glyph as fillRect
     * pixels, so a recording print() sees nothing -- the same reason
     * paramPagesFooterHints() is exported. */
    const title = V.headerTitle();
    if (!/^MFX > /.test(title)) fail("the header does not say MFX: " + JSON.stringify(title));
    if (/^S1 > /.test(title))
      fail("the header called the master bus a slot: " + JSON.stringify(title));

    if (abbrevArg !== "cloudseed")
      fail("the module behind the view was resolved through the wrong key -- " +
           "getModuleAbbrev saw " + JSON.stringify(abbrevArg));

    V.handleParamPagesMidi([0xb0, 51, 127]);
    if (V.paramPagesActive()) fail("back should leave the view");
    if (views[views.length - 1] !== "masterfx")
      fail("back from a Master FX component landed on " + views[views.length - 1] +
           " -- it must return to the editor it was opened from");

    /* And the defaults must be untouched: an entry with NO chrome is still the
     * slot chain, and carrying the last one over would leave an MFX header on
     * it. */
    const devS = D.createFakeDevice({ id: "obxd" });
    C.ctx.getSlotParam = (slot, key) => devS.getParam(key);
    C.ctx.setSlotParam = (slot, key, value) => devS.setParam(key, value);
    V.enterParamPages(1, "synth", "synth");
    for (let i = 0; i < 12; i++) V.tickParamPages();
    V.drawParamPages();
    const t2 = V.headerTitle();
    if (!/^S2 > /.test(t2))
      fail("a chrome-less entry did not fall back to the slot header: " + JSON.stringify(t2));
    V.handleParamPagesMidi([0xb0, 51, 127]);
    if (views[views.length - 1] !== "chainedit")
      fail("a chrome-less entry did not return to the chain editor");
  }

  console.log("PASS: shadow view module — setting and screen-reader gating, one read per frame, " +
              "MIDI routed, opaque params and non-grid pages handed to existing screens, " +
              "and the same grid serves Master FX with its own header, module key and return view");
});
'
