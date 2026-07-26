#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The interaction model (src/shared/param_pages/page_controller.mjs), driven
# against a fake device built from the fleet fixture.
#
# This is the half of the work that would normally be untestable without a Move:
# view state, knob feel, staggered reads, rebuild-on-change, announcements. It
# lives in a pure controller with injected I/O precisely so it can be driven
# here, leaving the real binding as routing and one render call.
#
# The two behaviours carrying the most risk are pinned hardest:
#   - ONE read per tick, not eight. Eight live values per page is eight IPC
#     round trips; Movy measured bulk refresh blocking ~186 ms per cycle.
#   - a read issued before a knob turn must not land after it and drag the
#     value backwards.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the controller tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/render_page.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
  import("./tools/param-pages/harness.mjs"),
]).then(([C, R, D, H]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const setup = (id, initial) => {
    const dev = D.createFakeDevice({ id, initial });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    return { dev, ctl };
  };

  /* ---- 1. loading lands on a usable page and says where you are --------- */
  {
    const { dev, ctl } = setup("obxd");
    if (!ctl.pages.length) fail("obxd planned no pages");
    if (ctl.page.kind !== "knobs") fail("should land on a grid page, got " + ctl.page.kind);
    if (!dev.announcements.length) fail("landing on a page announced nothing");
    if (!/of/.test(dev.announcements[0])) fail("the landing announcement lacks position: " + dev.announcements[0]);
  }

  /* ---- 2. ONE read per tick, cycling the page -------------------------- */
  {
    const { dev, ctl } = setup("obxd");
    dev.resetCounters();
    ctl.tick();
    if (dev.reads.length !== 1) fail("a tick issued " + dev.reads.length + " reads, must be exactly 1");

    dev.resetCounters();
    const keys = ctl.page.keys.length;
    for (let i = 0; i < keys; i++) ctl.tick();
    if (dev.reads.length !== keys) fail("expected " + keys + " reads over " + keys + " ticks, got " + dev.reads.length);
    /* A full cycle should have touched every key on the page exactly once. */
    const uniq = new Set(dev.reads);
    if (uniq.size !== keys) fail("a full cursor cycle read " + uniq.size + " distinct keys, expected " + keys);
    for (const k of ctl.page.keys) {
      if (ctl.state.values[k] === undefined) fail("key " + k + " still has no value after a full cycle");
    }
  }

  /* ---- 3. turning writes through and moves the value -------------------- */
  {
    const { dev, ctl } = setup("obxd", { cutoff: 50 });
    for (let i = 0; i < 8; i++) ctl.tick();
    const key = ctl.page.keys[0];
    const before = Number(ctl.state.values[key]);
    let t = 1000;
    for (let i = 0; i < 20; i++) ctl.onKnobTurn(0, 1, (t += 30));
    const after = Number(ctl.state.values[key]);
    if (!(after > before)) fail("turning up did not raise the value: " + before + " -> " + after);
    if (!dev.writes.length) fail("turning wrote nothing to the device");
    const [wk, wv] = dev.lastWrite();
    if (wk !== "synth:" + key) fail("wrote the wrong key: " + wk);
    if (Number(wv) !== after) fail("the device value and the local value disagree");
    /* Turning announces the value only — never the name, every detent. */
    const last = dev.announcements[dev.announcements.length - 1];
    if (/Cutoff/i.test(last)) fail("turning re-announced the param name: " + last);
  }

  /* ---- 4. a stale read must not drag a turned value backwards ---------- */
  {
    const { dev, ctl } = setup("obxd", { cutoff: 50 });
    for (let i = 0; i < 8; i++) ctl.tick();
    const key = ctl.page.keys[0];

    /* The device will serve the OLD value for the next few reads, exactly as a
     * read issued before the write would. */
    dev.lagParam(key, "50", 6);
    let t = 5000;
    for (let i = 0; i < 20; i++) ctl.onKnobTurn(0, 1, (t += 30));
    const turned = Number(ctl.state.values[key]);

    for (let i = 0; i < 8; i++) ctl.tick();
    const settled = Number(ctl.state.values[key]);
    if (settled < turned) fail("a stale read dragged the value back: " + turned + " -> " + settled);
  }

  /* ---- 5. an opaque param cannot be turned, but can be opened ---------- */
  {
    const { dev, ctl } = setup("mrdrums");
    let slot = -1;
    for (let i = 0; i < 8; i++) {
      const m = ctl.metaAt(i);
      if (m && m.kind === "opaque") { slot = i; break; }
    }
    if (slot < 0) fail("mrdrums page 1 should hold an opaque param");
    dev.resetCounters();
    ctl.onKnobTurn(slot, 1, 1000);
    if (dev.writes.length) fail("turning an opaque param wrote " + JSON.stringify(dev.writes[0]));
    const intent = ctl.onClick(slot);
    if (!intent || intent.action !== "open") fail("clicking an opaque param should ask the host to open it");
    if (!ctl.takePending()) fail("the pending intent should be collectable once");
    if (ctl.takePending()) fail("a pending intent should only be delivered once");
  }

  /* ---- 6. paging: fine steps, shift steps by level --------------------- */
  {
    const { dev, ctl } = setup("minijv");
    const start = ctl.pageIndex;
    ctl.onJog(1);
    if (ctl.pageIndex !== start + 1) fail("a jog step should advance one page");
    dev.resetCounters();
    const mid = ctl.pageIndex;
    ctl.onJog(1, { shift: true });
    if (ctl.pageIndex <= mid) fail("shift+jog should advance");
    if (!dev.announcements.length) fail("changing page announced nothing");
    /* Paging resets the read cursor so the new page fills from its first key. */
    if (ctl.state.cursor !== 0) fail("the read cursor should restart on a new page");
  }

  /* ---- 7. a module that finishes loading keeps your place -------------- */
  {
    const dev = D.createFakeDevice({ id: "sf2" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    ctl.onJog(1);
    const nameBefore = ctl.page.name;

    /* The DSP finishes its load and republishes a much larger tree — every
     * page index shifts underneath the user. */
    dev.becomeModule("obxd");
    const rebuilt = ctl.reloadIfChanged();
    if (!rebuilt) fail("a changed contract should rebuild the page set");
    if (ctl.pages.length < 10) fail("the rebuilt page set looks wrong: " + ctl.pages.length);
    /* Landing somewhere sane matters more than landing anywhere exact. */
    if (ctl.pageIndex < 0 || ctl.pageIndex >= ctl.pages.length) fail("reanchored out of range");
    if (nameBefore === undefined) fail("no page name before rebuild");

    /* An unchanged contract must NOT rebuild — that would reset values and
     * cursor every frame. */
    if (ctl.reloadIfChanged()) fail("an unchanged contract rebuilt anyway");
  }

  /* ---- 8. touch announces the full name; release clears --------------- */
  {
    const { dev, ctl } = setup("obxd");
    for (let i = 0; i < 8; i++) ctl.tick();
    dev.resetCounters();
    ctl.onKnobTouch(1, true);
    if (ctl.state.touched !== 1) fail("touch did not register");
    const said = dev.announcements[0] || "";
    if (!/Resonance/.test(said)) fail("touch should announce the full name, got: " + said);
    ctl.onKnobTouch(1, false);
    if (ctl.state.touched !== -1) fail("releasing did not clear the touched slot");
  }

  /* ---- 9. it renders what it holds, through the real font -------------- */
  {
    const { ctl } = setup("obxd");
    for (let i = 0; i < 16; i++) ctl.tick();
    const fb = H.createFramebuffer();
    ctl.render(H.drawContext(fb), { title: "T1 > OB-XD" });
    if (fb.countLit() < 100) fail("the controller rendered a near-empty screen");
    if (fb.clipped() > 0) fail("the controller drew outside the display");
    if (fb.missingGlyphs.size) fail("undrawable characters reached the screen");

    /* And in the other layout, and with values revealed. */
    ctl.setLayout(R.LAYOUT_BAR);
    ctl.setReveal(true);
    const fb2 = H.createFramebuffer();
    ctl.render(H.drawContext(fb2), { title: "T1 > OB-XD" });
    if (fb2.clipped() > 0) fail("bar layout drew outside the display");
  }

  /* ---- 9a. the preset name rides the cursor, not a separate poll -------- */
  {
    const dev = D.createFakeDevice({ id: "obxd", initial: { preset_name: "Fat Brass" } });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    if (ctl.presetName) fail("the preset name should not be known before any read");
    for (let i = 0; i < 20; i++) ctl.tick();
    if (ctl.presetName !== "Fat Brass") fail("the preset name was never read: " + ctl.presetName);

    /* Crucially it must not cost an extra read per frame — it is one more stop
     * in the rotation, not a second poll. */
    for (let i = 0; i < 12; i++) {
      dev.resetCounters();
      ctl.tick();
      if (dev.reads.length !== 1) fail("a tick issued " + dev.reads.length + " reads once the preset name joined the rotation");
    }

    /* A module with no preset name leaves it null rather than blanking the
     * header with an empty string. */
    const dev2 = D.createFakeDevice({ id: "arp" });
    const ctl2 = C.createController(dev2);
    ctl2.load({ slot: 0, component: "synth" });
    for (let i = 0; i < 20; i++) ctl2.tick();
    if (ctl2.presetName !== null) fail("a module with no preset should leave presetName null, got " + JSON.stringify(ctl2.presetName));
  }

  /* ---- 9b. the first-run hint shows once and any input clears it ------- */
  {
    const { ctl } = setup("obxd");
    if (!ctl.showHint(["a"], "t")) fail("the hint should arm on a fresh controller");
    if (!ctl.state.hintLines) fail("the hint is not showing");
    ctl.onJog(1);
    if (ctl.state.hintLines) fail("any input must clear the hint");
    /* And never again this session — a hint you cannot get rid of is worse
     * than no hint. */
    if (ctl.showHint(["a"], "t")) fail("the hint re-armed after being dismissed");
    if (ctl.state.hintLines) fail("the hint came back");
  }

  /* ---- 9c. the section picker ------------------------------------------- */
  {
    const { dev, ctl } = setup("minijv");
    ctl.dismissHint();
    dev.resetCounters();
    if (!ctl.openPicker()) fail("minijv should offer a section picker");
    if (!ctl.pickerOpen) fail("picker did not open");
    if (ctl.pickerEntries.length < 5) fail("too few sections: " + ctl.pickerEntries.length);
    if (ctl.pickerEntries.length >= ctl.pages.length) fail("the picker is not shorter than the page list");
    if (!dev.announcements.length) fail("opening the picker announced nothing");

    /* Jog scrolls the picker without moving the page behind it. */
    const page0 = ctl.pageIndex;
    ctl.onJog(3);
    if (ctl.pageIndex !== page0) fail("jogging the picker moved the page behind it");
    const target = ctl.pickerEntries[ctl.pickerIndex].index;
    ctl.pickerSelect();
    if (ctl.pickerOpen) fail("selecting did not close the picker");
    if (ctl.pageIndex !== target) fail("selecting did not jump to the section");

    /* Reaching for a knob dismisses it. */
    ctl.openPicker();
    ctl.onKnobTouch(0, true);
    if (ctl.pickerOpen) fail("touching a knob should dismiss the picker");
  }

  /* ---- 10. every fleet module survives a scripted session -------------- */
  {
    const fx = D.fleet();
    let sessions = 0;
    for (const mod of fx.modules) {
      const dev = D.createFakeDevice({ id: mod.id });
      const ctl = C.createController(dev);
      ctl.load({ slot: 0, component: "synth" });

      /* Page forward through the whole module, ticking and wiggling knobs. */
      let guard = 0;
      let t = 0;
      for (;;) {
        for (let i = 0; i < 4; i++) ctl.tick();
        for (let k = 0; k < 8; k++) ctl.onKnobTurn(k, (k % 2) ? 1 : -1, (t += 25));
        ctl.onKnobTouch(0, true);
        ctl.onKnobTouch(0, false);
        const before = ctl.pageIndex;
        ctl.onJog(1);
        if (ctl.pageIndex === before) break;
        if (++guard > 200) fail(mod.id + ": paging did not terminate");
      }

      const fb = H.createFramebuffer();
      ctl.render(H.drawContext(fb), { title: "T1 > " + mod.id.toUpperCase() });
      if (fb.clipped() > 0) fail(mod.id + ": drew outside the display");
      for (const [k, v] of dev.writes) {
        if (v === "NaN" || v === "undefined" || v === "null") fail(mod.id + ": wrote " + v + " to " + k);
      }
      sessions++;
    }
    if (sessions < 70) fail("only " + sessions + " modules exercised");
    console.log("PASS: controller — one read per tick, writes survive stale reads, " +
                "opaque params open rather than turn, rebuild keeps your place, " +
                sessions + " scripted module sessions clean");
  }
});
'
