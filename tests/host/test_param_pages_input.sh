#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# MIDI → intent mapping for the knob page
# (src/shared/param_pages/page_input.mjs).
#
# This is where the boring bugs live: the wrong CC, a relative encoder decoded
# as absolute, a modifier that latches, a touch that never clears. None of that
# needs a device to get wrong, so none of it needs a device to catch.
#
# The CC map is pinned against CLAUDE.md, "Move Hardware MIDI" — if the hardware
# map ever changes, these fail loudly rather than the UI going subtly deaf.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the input tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_input.mjs"),
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
]).then(([I, C, D, M]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const cc = (n, v) => [0xb0, n, v];
  const noteOn = (n, v) => [0x90, n, v];
  const noteOff = (n) => [0x80, n, 0];

  /* ---- 1. the CC map matches the documented hardware ------------------- */
  {
    if (I.KNOB_CC_FIRST !== 71 || I.KNOB_CC_LAST !== 78) fail("knob CCs must be 71-78");
    if (I.JOG_TURN_CC !== 14) fail("jog turn must be CC 14");
    if (I.JOG_CLICK_CC !== 3) fail("jog click must be CC 3");
    if (I.SHIFT_CC !== 49) fail("shift must be CC 49");
    if (I.BACK_CC !== 51) fail("back must be CC 51");
  }

  /* ---- 2. knobs are RELATIVE encoders ---------------------------------- */
  {
    const up = I.decodeInput(cc(71, 1));
    if (!up || up.type !== "knob" || up.slot !== 0 || up.direction <= 0) fail("CC71 value 1 should be knob 1 clockwise");
    /* 65..127 is anticlockwise. Decoding this as absolute is the classic bug:
     * it would read as a large positive jump. */
    const down = I.decodeInput(cc(71, 127));
    if (!down || down.direction >= 0) fail("CC71 value 127 must decode anticlockwise, got " + JSON.stringify(down));
    const down2 = I.decodeInput(cc(78, 65));
    if (!down2 || down2.slot !== 7 || down2.direction >= 0) fail("CC78 value 65 should be knob 8 anticlockwise");
    if (I.decodeInput(cc(71, 0)) !== null) fail("a zero delta should produce no intent");
    if (I.decodeInput(cc(70, 1)) !== null) fail("CC70 is not a knob");
    if (I.decodeInput(cc(79, 1)) !== null) fail("CC79 is the master knob, not a page knob");
  }

  /* ---- 3. jog pages; shift makes it coarse ----------------------------- */
  {
    const fine = I.decodeInput(cc(14, 1));
    if (!fine || fine.type !== "page" || fine.delta !== 1 || fine.byLevel) fail("jog should page finely: " + JSON.stringify(fine));
    const back = I.decodeInput(cc(14, 127));
    if (!back || back.delta !== -1) fail("jog anticlockwise should page back");
    const coarse = I.decodeInput(cc(14, 1), { shift: true });
    if (!coarse || !coarse.byLevel) fail("shift+jog should step by level");
    /* A multi-detent jog is still one page — pages are discrete. */
    const fast = I.decodeInput(cc(14, 5));
    if (!fast || fast.delta !== 1) fail("a fast jog should still advance one page, got " + fast.delta);
  }

  /* ---- 4. buttons fire on press, not release --------------------------- */
  {
    if (!I.decodeInput(cc(3, 127))) fail("jog click press should produce an intent");
    if (I.decodeInput(cc(3, 0)) !== null) fail("jog click RELEASE must not fire again");
    if (!I.decodeInput(cc(51, 127))) fail("back press should produce an intent");
    if (I.decodeInput(cc(51, 0)) !== null) fail("back release must not fire again");
    const sd = I.decodeInput(cc(49, 127)), su = I.decodeInput(cc(49, 0));
    if (!sd || sd.down !== true) fail("shift press should report down");
    if (!su || su.down !== false) fail("shift release must report up, or the modifier latches");
  }

  /* ---- 5. touch clears on BOTH spellings of release --------------------- */
  {
    const on = I.decodeInput(noteOn(2, 100));
    if (!on || on.type !== "touch" || on.slot !== 2 || !on.down) fail("note 2 should touch knob 3");
    /* Move sends note-on velocity 0 as a release; treating it as a touch leaves
     * a knob stuck as held forever. */
    const zeroVel = I.decodeInput(noteOn(2, 0));
    if (!zeroVel || zeroVel.down !== false) fail("note-on velocity 0 must clear the touch");
    const off = I.decodeInput(noteOff(2));
    if (!off || off.down !== false) fail("note-off must clear the touch");
    if (I.decodeInput(noteOn(8, 100)) !== null) fail("note 8 is not a knob touch");
  }

  /* ---- 6. unrelated traffic is ignored ---------------------------------- */
  {
    for (const msg of [[0x90, 68, 100], [0xf8], [0xe0, 0, 64], null, [0xb0], [0xb0, 50, 127]]) {
      if (I.decodeInput(msg) !== null) fail("should ignore " + JSON.stringify(msg));
    }
    /* CC 88 (Mute) is NOT ignored — it is a modifier other gestures use,
     * and Schwung already uses it for destructive actions in this view. */
    const mute = I.decodeInput(cc(88, 127));
    if (!mute || mute.type !== "mute" || mute.down !== true) fail("CC 88 should report mute held");
    const muteUp = I.decodeInput(cc(88, 0));
    if (!muteUp || muteUp.down !== false) fail("CC 88 release must report mute up, or the modifier latches");
  }

  /* ---- 7. end to end: MIDI in, param out -------------------------------- */
  {
    const dev = D.createFakeDevice({ id: "obxd", initial: { cutoff: 50 } });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    for (let i = 0; i < 8; i++) ctl.tick();
    dev.resetCounters();

    let t = 1000;
    let mods = { shift: false };
    const feed = (msg) => {
      const intent = I.decodeInput(msg, mods);
      if (intent && intent.type === "shift") mods.shift = intent.down;
      return I.applyInput(ctl, intent, { nowMs: (t += 30) });
    };

    const key = ctl.page.keys[0];
    const before = Number(ctl.state.values[key]);
    for (let i = 0; i < 20; i++) feed(cc(71, 1));
    if (!(Number(ctl.state.values[key]) > before)) fail("turning knob 1 over MIDI did not raise the value");
    if (!dev.writes.length) fail("turning over MIDI wrote nothing to the device");

    /* Shift held: reveal values, and the jog goes coarse. */
    feed(cc(49, 127));
    if (!ctl.state.revealValues) fail("holding shift should reveal values");
    const p0 = ctl.pageIndex;
    feed(cc(14, 1));
    feed(cc(49, 0));
    if (ctl.state.revealValues) fail("releasing shift should hide values again");
    if (ctl.pageIndex === p0) fail("shift+jog did not move");

    /* Back asks the host to leave — the controller does not own the view. */
    const out = feed(cc(51, 127));
    if (!out || out.action !== "exit") fail("back should ask the host to exit");
  }

  /* ---- 7b. no modifier resets a value; precision editing is safe -------- */
  {
    const dev = D.createFakeDevice({ id: "branchage" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    ctl.dismissHint();
    for (let i = 0; i < 24; i++) ctl.tick();
    const key = ctl.page.keys[0];
    const meta = ctl.metaAt(0);
    if (meta.default === undefined) fail("expected branchage knob 1 to declare a default");

    let t = 1000;
    const feed = (msg, mods) => I.applyInput(ctl, I.decodeInput(msg, mods || {}), { nowMs: (t += 30) });

    /* Fine-adjust away from the default. */
    for (let i = 0; i < 10; i++) feed(cc(71, 1), { shift: true });
    const tuned = Number(ctl.state.values[key]);
    if (tuned === Number(meta.default)) fail("fine adjust did not move the value off its default");

    /* THE HAZARD: during precision editing you are already holding shift with a
     * knob under your finger, and the jog is live for section stepping. A stray
     * jog press must not wipe the value you are carefully setting. */
    feed(noteOn(0, 100), { shift: true });
    feed(cc(3, 127), { shift: true });
    if (Number(ctl.state.values[key]) !== tuned) {
      fail("shift + jog click destroyed a value mid-precision-edit: " + tuned + " -> " + ctl.state.values[key]);
    }
    feed(noteOff(0), {});

    /*
     * No modifier + touch resets anything.
     *
     * It used to, and the gesture could never be advertised: CC 88 is
     * forwarded to Move unconditionally, so holding Mute to reach it also
     * mutes the selected track. There is no reset-to-default gesture on this
     * page at all now, and Mute stays a pure modifier here.
     */
    for (let i = 0; i < 10; i++) feed(cc(71, 1), {});
    const beforeMute = Number(ctl.state.values[key]);
    if (beforeMute === Number(meta.default)) fail("setup: value should be off its default");
    feed(noteOn(0, 100), { mute: true });
    if (Number(ctl.state.values[key]) !== beforeMute) {
      fail("mute + touch reset the value — that gesture is gone, because holding " +
           "Mute also mutes the selected track");
    }

    /* A plain touch still just announces — it must not reset either. */
    feed(noteOff(0), {});
    for (let i = 0; i < 10; i++) feed(cc(71, 1), {});
    const moved = Number(ctl.state.values[key]);
    feed(noteOn(0, 100), {});
    if (Number(ctl.state.values[key]) !== moved) fail("an unmodified touch reset the value");
  }

  /* ---- 8. click acts on the held knob, and only when DIVABLE ------------ */
  {
    const dev = D.createFakeDevice({ id: "mrdrums" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    for (let i = 0; i < 8; i++) ctl.tick();

    const feed = (msg) => I.applyInput(ctl, I.decodeInput(msg), { nowMs: 1000 });

    /* Nothing held: the click has no param to act on, so it opens the section
     * picker — the only spare gesture, and what a 76-page module needs. */
    if (feed(cc(3, 127)) !== null) fail("opening the picker should not ask the host for anything");
    if (!ctl.pickerOpen) fail("clicking with no knob held should open the section picker");
    /* Jog scrolls the picker rather than paging behind it. */
    const pageBefore = ctl.pageIndex;
    feed(cc(14, 1));
    if (ctl.pageIndex !== pageBefore) fail("jogging in the picker paged the grid behind it");
    /* Back closes the picker, and does NOT leave the view. */
    if (feed(cc(51, 127)) !== null) fail("back should close the picker, not exit the view");
    if (ctl.pickerOpen) fail("back did not close the picker");
    /* Reaching for a knob dismisses it too. */
    feed(cc(3, 127));
    if (!ctl.pickerOpen) fail("picker should reopen");
    feed(noteOn(0, 100));
    if (ctl.pickerOpen) fail("touching a knob should dismiss the picker");
    feed(noteOff(0));

    /* A divable that actually OPENS something. A two-option enum is divable
     * and does not: its click writes the other value in place (flipsOnClick),
     * so picking the first divable cell on the page would test the flip and
     * call it an open. */
    let opaque = -1;
    for (let i = 0; i < 8; i++) {
        const m = ctl.metaAt(i);
        if (m && m.divable && !M.flipsOnClick(m)) { opaque = i; break; }
    }
    if (opaque < 0) fail("expected a divable param on the mrdrums page");
    feed(noteOn(opaque, 100));
    const opened = feed(cc(3, 127));
    if (!opened || opened.action !== "open") fail("clicking a held divable knob should ask the host to open it");

    /* A turnable param has nothing to open. */
    let turnable = -1;
    for (let i = 0; i < 8; i++) { const m = ctl.metaAt(i); if (m && !m.divable) { turnable = i; break; } }
    feed(noteOff(opaque));
    feed(noteOn(turnable, 100));
    if (feed(cc(3, 127)) !== null) fail("clicking a turnable knob should not open anything");
  }

  /* ---- a preset page is a door, and never a trap ------------------------
   *
   * Plain click goes in. Shift+click must still reach the section list from
   * INSIDE it, and Back must step out one layer rather than leaving the view —
   * the same ladder a menu and the picker have. A door you cannot get out of
   * except by exiting the whole view is how the first cut of the menu page
   * went wrong.
   */
  {
    const names = ["A", "B", "C"];
    let index = 0;
    const HIER = { modes: null, levels: { root: {
      label: "S", list_param: "preset", count_param: "preset_count",
      name_param: "preset_name", knobs: ["cutoff"], params: [{ key: "cutoff" }] } } };
    const CP = [{ key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 }];
    const ctl = C.createController({
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return JSON.stringify(HIER);
        if (b === "chain_params") return JSON.stringify(CP);
        if (b === "preset_count") return String(names.length);
        if (b === "preset") return String(index);
        if (b === "preset_name") return names[index];
        return "0.5";
      },
      setParam: (k, v) => { if (String(k).endsWith(":preset")) index = parseInt(v, 10); },
      announce: () => {},
    });
    ctl.load({ slot: 0, component: "synth" });
    const at = ctl.pages.findIndex((p) => p.kind === "preset");
    if (at < 0) fail("no preset page planned");
    ctl.goToPage(at, { remember: false });
    for (let i = 0; i < 6; i++) ctl.tick();

    const press = (mods) => I.applyInput(ctl, I.decodeInput(cc(3, 127), mods || {}), { nowMs: 1000 });
    const back = () => I.applyInput(ctl, I.decodeInput(cc(51, 127)), { nowMs: 1000 });

    press();
    if (!ctl.menuEntered()) fail("plain click should enter the preset page");

    /* From INSIDE: shift+click still reaches the sections. */
    press({ shift: true });
    if (!ctl.pickerOpen) fail("shift+click must open the section list from inside a preset page");
    back();
    if (ctl.pickerOpen) fail("Back should close the picker first");

    /* Then Back steps out of the page, and only THEN leaves the view. */
    if (!ctl.menuEntered()) fail("closing the picker should leave the page still entered");
    if (back() !== null) fail("Back should step out of the entered page, not leave the view");
    if (ctl.menuEntered()) fail("Back did not step out");
    const out = back();
    if (!out || out.action !== "exit") fail("Back on an inert page should leave the view");
  }

  console.log("PASS: input mapping — CC map pinned, relative encoders decoded, " +
              "modifiers do not latch, touch clears on both releases, preset pages are doors, " +
              "MIDI reaches the device");
});
'
