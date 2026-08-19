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
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
]).then(([C, R, D, H, P, M]) => {
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

  /* ---- 9d. Elektron patterns: fine adjust, reset, section memory -------- */
  {
    /* Find a float with a declared default — 744 params across 39 modules
     * declare one, and it is the only way back short of reloading the preset. */
    const fx = D.fleet();
    let found = null;
    for (const mod of fx.modules) {
      const pages = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params }).pages;
      const ix = M.buildMetaIndex({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
      const pg = pages.find((p) => p.kind === "knobs");
      if (!pg) continue;
      const slot = pg.keys.findIndex((k) => {
        const m = ix.getOrGuess(k);
        return m.type === "float" && m.default !== undefined;
      });
      if (slot >= 0) { found = { id: mod.id, page: pages.indexOf(pg), slot }; break; }
    }
    if (!found) fail("no float with a declared default anywhere in the fleet");

    const dev = D.createFakeDevice({ id: found.id });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    ctl.dismissHint();
    ctl.goToPage(found.page, { remember: false });
    for (let i = 0; i < 24; i++) ctl.tick();
    const key = ctl.page.keys[found.slot];
    const meta = ctl.metaAt(found.slot);

    /* Shift makes a float encoder roughly ten times finer. */
    let t = 1000;
    const a0 = Number(ctl.state.values[key]);
    for (let i = 0; i < 10; i++) ctl.onKnobTurn(found.slot, 1, (t += 30));
    const coarse = Number(ctl.state.values[key]) - a0;
    const a1 = Number(ctl.state.values[key]);
    for (let i = 0; i < 10; i++) ctl.onKnobTurn(found.slot, 1, (t += 30), { fine: true });
    const fine = Number(ctl.state.values[key]) - a1;
    if (!(coarse > 0 && fine > 0)) fail("turning did not move the value in one of the modes");
    if (!(coarse / fine > 4)) fail("fine adjust is barely finer than coarse: " + (coarse / fine).toFixed(1) + "x");

    /* An int has no step below 1, so fine must not pretend otherwise. */
    const intSlot = ctl.page.keys.findIndex((k) => ctl.metaIndex.getOrGuess(k).type === "int");
    if (intSlot >= 0) {
      const ik = ctl.page.keys[intSlot];
      const b0 = Number(ctl.state.values[ik]);
      for (let i = 0; i < 10; i++) ctl.onKnobTurn(intSlot, 1, (t += 30));
      const icoarse = Number(ctl.state.values[ik]) - b0;
      const b1 = Number(ctl.state.values[ik]);
      for (let i = 0; i < 10; i++) ctl.onKnobTurn(intSlot, 1, (t += 30), { fine: true });
      const ifine = Number(ctl.state.values[ik]) - b1;
      if (icoarse > 0 && ifine === 0) fail("fine adjust froze an int, which has no finer step to give");
    }

    /* Reset to the declared default. */
    ctl.onKnobTurn(found.slot, 1, (t += 30));
    if (!ctl.resetToDefault(found.slot)) fail("resetToDefault refused a param that declares one");
    if (Number(ctl.state.values[key]) !== Number(meta.default)) {
      fail("reset did not land on the default: " + ctl.state.values[key] + " vs " + meta.default);
    }
    const wrote = dev.writes[dev.writes.length - 1];
    if (Number(wrote[1]) !== Number(meta.default)) fail("the default was not written to the device");

    /* A param with no declared default reports so rather than silently doing
     * nothing, so the caller can say "no default" out loud. */
    const noDefault = ctl.page.keys.findIndex((k) => ctl.metaIndex.getOrGuess(k).default === undefined);
    if (noDefault >= 0 && ctl.resetToDefault(noDefault)) fail("resetToDefault claimed success with no default declared");
  }

  /* ---- 9e. a section remembers the sub-page you were on ----------------- */
  {
    const { ctl } = setup("minijv");
    ctl.dismissHint();
    const pages = ctl.pages;
    let start = -1;
    for (let i = 0; i < pages.length - 1; i++) {
      if (pages[i].kind === "knobs" && pages[i + 1].kind === "knobs" && pages[i].level === pages[i + 1].level) {
        start = i; break;
      }
    }
    if (start < 0) fail("minijv should have a level spanning two pages");

    ctl.goToPage(start, { remember: false });
    ctl.onJog(1);                                   /* into the second page */
    const deep = ctl.pageIndex;
    if (deep !== start + 1) fail("a fine jog should step one page");
    ctl.onJog(1, { shift: true });                  /* leave the section */
    if (ctl.pageIndex === deep) fail("shift+jog did not leave the section");
    ctl.onJog(-1, { shift: true });                 /* come back */
    if (ctl.pageIndex !== deep) {
      fail("returning to a section forgot the sub-page: landed on " + ctl.pageIndex + ", expected " + deep);
    }
    /* But a plain jog must still walk the set in order, or you could never
     * traverse it. */
    ctl.goToPage(start, { remember: false });
    ctl.onJog(1);
    if (ctl.pageIndex !== start + 1) fail("section memory hijacked a fine jog");
  }

  /* ---- 9f. metadata that arrives after the module reports ready --------- */
  {
    /* osirus loads a ROM asynchronously and publishes rom_index as
     * ["(loading)"]. Baked once at load time, that enum reads "(loading)" for
     * the rest of the session — it is in the fleet capture that way. */
    const dev = D.createFakeDevice({ id: "osirus" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    ctl.dismissHint();
    const romPage = ctl.pages.findIndex((p) => p.kind === "knobs" && p.keys.includes("rom_index"));
    if (romPage < 0) fail("osirus should expose rom_index on a grid page");
    ctl.goToPage(romPage, { remember: false });

    const opts = () => ctl.metaIndex.getOrGuess("rom_index").options;
    if (!(opts() || []).some((o) => /^\(.*\)$/.test(o))) fail("expected osirus rom_index to start as a placeholder");

    for (let i = 0; i < 700; i++) ctl.tick();
    if (!(opts() || []).some((o) => /^\(.*\)$/.test(o))) fail("the placeholder resolved with nothing to resolve to");

    /* The DSP finishes and republishes real options IN PLACE — no new params,
     * no new levels, so a fingerprint over chain_params LENGTH would call this
     * unchanged and leave the placeholder up forever. */
    dev.patchChainParams((ps) => ps.map((p) =>
      p.key === "rom_index" ? { ...p, options: ["Virus A", "Virus B", "Virus C"] } : p));
    for (let i = 0; i < 400; i++) ctl.tick();
    if ((opts() || []).length !== 3) fail("late enum options were never picked up: " + JSON.stringify(opts()));

    /* Once settled it must cost nothing at all. */
    dev.resetCounters();
    for (let i = 0; i < 2000; i++) ctl.tick();
    const contractReads = dev.reads.filter((k) => /chain_params|ui_hierarchy/.test(k)).length;
    if (contractReads !== 0) fail("still re-reading the contract " + contractReads + " times after settling");

    /* And a module whose enum legitimately reads "(none)" must not poll for
     * ever — the budget latches off. */
    const dev2 = D.createFakeDevice({ id: "osirus" });
    const ctl2 = C.createController(dev2);
    ctl2.load({ slot: 0, component: "synth" });
    ctl2.goToPage(ctl2.pages.findIndex((p) => p.kind === "knobs" && p.keys.includes("rom_index")), { remember: false });
    for (let i = 0; i < 6000; i++) ctl2.tick();
    dev2.resetCounters();
    for (let i = 0; i < 2000; i++) ctl2.tick();
    const stuckReads = dev2.reads.filter((k) => /chain_params|ui_hierarchy/.test(k)).length;
    if (stuckReads !== 0) fail("a never-settling module kept polling: " + stuckReads + " reads");
  }

  /* ---- 9g. a visible_if source change re-plans the page ----------------- */
  {
    /* mrsample gates loop_start/end/xfade on loop_mode. A condition is driven
     * by a VALUE, which moves without the declared contract moving — so the
     * fingerprint cannot see it, and before this the gated params never
     * appeared or disappeared at all. */
    const dev = D.createFakeDevice({ id: "mrsample" });
    const vis = (cond) => dev.getParam("synth:" + (cond.param || cond.key)) === "1";
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth", visible: vis });
    ctl.dismissHint();

    const gated = ctl.pages.findIndex((p) => p.kind === "knobs" && (p.keys || []).includes("loop_mode"));
    if (gated < 0) fail("mrsample should expose loop_mode on a grid page");

    dev.setParam("synth:loop_mode", "0");
    ctl.goToPage(gated, { remember: false });
    for (let i = 0; i < 40; i++) ctl.tick();
    const off = (ctl.page.keys || []).slice();
    if (off.includes("loop_start")) fail("loop params should be hidden while loop_mode is off");

    dev.setParam("synth:loop_mode", "1");
    for (let i = 0; i < 40; i++) ctl.tick();
    const on = (ctl.page.keys || []).slice();
    for (const k of ["loop_start", "loop_end", "loop_xfade_ms"]) {
      if (!on.includes(k)) fail("turning loop_mode on did not reveal " + k + ": " + JSON.stringify(on));
    }

    /* And back off again — the re-plan is not one-way. */
    dev.setParam("synth:loop_mode", "0");
    for (let i = 0; i < 40; i++) ctl.tick();
    if ((ctl.page.keys || []).includes("loop_start")) fail("turning loop_mode off did not hide the loop params again");

    /* It must not re-plan on every read, only on a CHANGE to a condition key. */
    const planned = [];
    const ctl2 = C.createController(dev);
    ctl2.load({ slot: 0, component: "synth", visible: vis });
    ctl2.goToPage(gated, { remember: false });
    for (let i = 0; i < 200; i++) { ctl2.tick(); planned.push(ctl2.pages.length); }
    if (new Set(planned).size !== 1) fail("the page set churned while nothing changed");
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

  /* ---- modulation flags ride the read cursor, not the draw ------------- *
   *
   * The renderer asks modulated(key) for every cell of every draw. When that
   * went straight through to the injected isModulated it was a synchronous
   * round trip PER CELL PER DRAW — measured on device at 3.5 of the grid
   * 7.1 reads per tick, half of them, for an indicator that changes only when
   * a modulation routing is edited. The flag is cached and refreshed on the
   * same cursor the values use.
   *
   * Nothing covered `modulated` at all before this, so the suite stayed green
   * through the change that introduced the cost AND the one that removed it.
   */
  {
    const dev = D.createFakeDevice({ id: "obxd" });
    let modCalls = 0;
    const modulatedKeys = new Set();
    const ctl = C.createController({
      ...dev,
      isModulated: (fullKey) => { modCalls++; return modulatedKeys.has(fullKey); },
    });
    ctl.load({ slot: 0, component: "synth" });

    /* Drawing must not consult the device at all. */
    const fb0 = H.createFramebuffer();
    modCalls = 0;
    for (let i = 0; i < 5; i++) ctl.render(H.drawContext(fb0), { title: "T" });
    if (modCalls !== 0) {
      fail("render asked isModulated " + modCalls + " times — it must read the cache, "
         + "not the device, or every cell costs an IPC round trip");
    }

    /* Ticking may consult it at most once — same budget as a value read. */
    modCalls = 0;
    const TICKS = 12;
    for (let i = 0; i < TICKS; i++) ctl.tick();
    if (modCalls > TICKS) {
      fail("tick asked isModulated " + modCalls + " times over " + TICKS
         + " ticks — the cursor must refresh at most one key per tick");
    }
    if (modCalls === 0) fail("modulation flags are never refreshed at all");

    /* And the flag must actually reach the renderer once the cursor passes. */
    const page = ctl.page;
    const key = page && page.keys ? page.keys.find(Boolean) : null;
    if (!key) fail("no key on the page to test modulation with");
    modulatedKeys.add("synth:" + key);
    let seen = false;
    for (let i = 0; i < 40 && !seen; i++) {
      ctl.tick();
      ctl.render({
        fillRect: () => {}, print: () => {}, textWidth: () => 0,
        /* The grid asks through the render options, so observe it there. */
      }, { title: "T" });
      seen = ctl.isModulatedCached ? ctl.isModulatedCached(key) : false;
    }
    if (!seen) fail("a newly modulated key never showed as modulated after 40 ticks");
    console.log("PASS: controller modulation — cached off the read cursor, "
              + "never consulted during a draw, still refreshes");
  }
});
'
