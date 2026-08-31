#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Embedding the knob grid: band selection, a rect, and a sequencer's p-locks
# (src/shared/param_pages/render_page_movy.mjs).
#
# The point of the exercise is that a tool module does not write its own knob
# widgets. It asks for the BODY and gets ours — the same knobs, enum squares,
# opaque doors and graphics the device draws — under its own header and footer.
#
# What is pinned, and why each is the thing that would break:
#
#   - THE DEFAULT LAYOUT IS WHAT THE CONSTANTS SAY. Asserted against BAR_Y,
#     ROW0_Y, LBL0_Y, ROW1_Y, LBL1_Y and FOOTER_Y themselves, because the
#     vertical-rhythm COMMENT above them disagrees with them: it describes a
#     6-row header with the bank bar on row 6 and equal 2-row gutters, while
#     HEADER_H and BAR_Y are both 7 and the gutters are 1 and 2. Pinning the
#     prose is how the first cut shifted the bank bar up a pixel.
#   - AN OMITTED BAND CLOSES THE STACK. Asking for the body alone puts row 0 at
#     the top of the rect, not 9 rows down where our header would have been.
#   - NOTHING IS DRAWN OUTSIDE THE RECT. An embedded page that scribbles into
#     the caller's chrome is the whole failure mode.
#   - A BAND THAT DOES NOT FIT IS STOOD DOWN AND SAID SO. Silent truncation
#     reads as "it drew fine".
#   - A P-LOCK CHANGES THE PIXELS. A decoration that reaches the renderer and
#     alters nothing is the defect that looks like success.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the embedding tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
  import("./tools/param-pages/harness.mjs"),
]).then(([R, C, D, H]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  /* A framebuffer that records every pixel touched, so "outside the rect" is a
   * measurement rather than an eyeball. */
  const makeCtx = (W, Hh) => {
    const px = [];
    return {
      px,
      fillRect: (x, y, w, h, c) => {
        for (let i = 0; i < w; i++) for (let j = 0; j < h; j++) px.push([x + i, y + j, c]);
      },
      print: (x, y, t) => { px.push([x, y, 1]); },
      textWidth: (t) => String(t).length * 4,
    };
  };

  /* ---- 1. the default layout IS the vertical rhythm table --------------- */
  {
    const L = R.movyBandLayout();
    if (L.header !== 0) fail("default header band at " + L.header + ", table says 0");
    if (L.bank !== R.BAR_Y) fail("default bank bar at " + L.bank + ", BAR_Y says " + R.BAR_Y);
    if (L.rows.length !== 2) fail("default layout has " + L.rows.length + " knob rows");
    if (L.rows[0].rowY !== R.ROW0_Y) fail("knob row 0 at " + L.rows[0].rowY + ", ROW0_Y says " + R.ROW0_Y);
    if (L.rows[0].lblY !== R.LBL0_Y) fail("label row 0 at " + L.rows[0].lblY + ", LBL0_Y says " + R.LBL0_Y);
    if (L.rows[1].rowY !== R.ROW1_Y) fail("knob row 1 at " + L.rows[1].rowY + ", ROW1_Y says " + R.ROW1_Y);
    if (L.rows[1].lblY !== R.LBL1_Y) fail("label row 1 at " + L.rows[1].lblY + ", LBL1_Y says " + R.LBL1_Y);
    if (L.footer !== R.FOOTER_Y) fail("footer at " + L.footer + ", FOOTER_Y says " + R.FOOTER_Y);
    /* And the whole stack accounts for the screen, so a band cannot be
     * silently unowned. */
    if (L.height !== 64) fail("the default stack is " + L.height + " rows, not 64");
    if (L.cellW !== 32) fail("default cell width " + L.cellW + ", grid says 32");
    if (!L.fits) fail("the default layout does not fit the screen it was cut for");
  }

  /* ---- 2. an omitted band closes the stack ------------------------------ */
  {
    /*
     * `bands` SAYS WHAT TO DRAW; `rect` SAYS WHERE TO LAY OUT.
     *
     * Both halves are pinned because conflating them was a real bug: omitting
     * the header used to move the whole grid up seven rows even with no rect,
     * so a caller keeping its OWN header got the Schwung body drawn on top.
     * Every other page kind draws at absolute coordinates and merely skips the
     * chrome it was not asked for; the knob grid was the one that relaid out.
     */
    const body = R.movyBandLayout({ bands: { header: false, bank: false, footer: false } });
    if (body.header !== null || body.bank !== null || body.footer !== null) {
      fail("bands were requested off but still placed");
    }
    /* NO RECT: the rhythm positions, chrome simply absent. */
    if (body.rows[0].rowY !== R.ROW0_Y) {
      fail("with no rect, dropping the header moved row 0 to " + body.rows[0].rowY
           + " instead of leaving it at ROW0_Y=" + R.ROW0_Y
           + " — bands must not relayout on their own");
    }
    if (body.rows[1].rowY !== R.ROW1_Y) fail("row 1 moved without a rect");

    /* WITH A RECT: the stack packs into it, chrome-less bands closing up. */
    const packed = R.movyBandLayout({
      bands: { header: false, bank: false, footer: false },
      rect: { x: 0, y: 0, w: 128, h: 64 },
    });
    if (packed.rows[0].rowY !== R.BAND_H.gutter0) {
      fail("with a rect, body-only puts row 0 at " + packed.rows[0].rowY
           + "; the stack did not close up (expected the first gutter alone, "
           + R.BAND_H.gutter0 + ")");
    }
    /* THE BODY IS 48 OF THE 64 ROWS — two gutters, two 15-row widget bands and
     * two 7-row label bands, none of which scale. A tool embedding it has 16
     * rows left for its own chrome, which is enough for a 6-row header and a
     * 7-row footer and not much else. Pinned because it is the number that
     * decides whether the layout a given tool wants is possible at all. */
    const wantBody = R.BAND_H.gutter0 + R.BAND_H.gutter1
                   + 2 * (R.BAND_H.widget + R.BAND_H.label);
    if (packed.height !== wantBody) {
      fail("the body block is " + packed.height + " rows, expected " + wantBody);
    }

    const offset = R.movyBandLayout({
      bands: { header: false, bank: false, footer: false },
      rect: { x: 0, y: 16, w: 128, h: wantBody },
    });
    if (!offset.rows.length) fail("a rect of exactly the body height must hold it");
    if (offset.rows[0].rowY !== 16 + R.BAND_H.gutter0) {
      fail("a rect at y=16 put row 0 at " + offset.rows[0].rowY
           + ", expected " + (16 + R.BAND_H.gutter0));
    }
  }

  /* ---- 3. a narrow rect narrows the cells ------------------------------- */
  {
    const L = R.movyBandLayout({ rect: { x: 8, y: 0, w: 64, h: 64 } });
    if (L.x !== 8) fail("rect x was ignored");
    if (L.cellW !== 16) fail("cell width " + L.cellW + " for a 64px rect, expected 16");
  }

  /* ---- 4. what does not fit is stood down, and named -------------------- */
  {
    /* 56 rows: everything but the footer fits (6 + 1 + 48 = 55). The chrome
     * goes and the CONTENT survives, which is the ordering that matters. */
    const tight = R.movyBandLayout({ rect: { x: 0, y: 0, w: 128, h: 56 } });
    if (tight.fits) fail("a 56px rect cannot hold the whole page but reported fits");
    if (!tight.dropped.length) fail("bands were dropped but `dropped` is empty — silent truncation");
    if (!tight.rows.length) fail("chrome should go before the body does");
    if (tight.dropped.join() !== "footer") {
      fail("expected only the footer to go at 56 rows, dropped: " + tight.dropped.join());
    }
    if (tight.footer !== null) fail("a dropped footer was still placed");

    /* Too small for even one widget row: everything goes, and says so. */
    const nothing = R.movyBandLayout({ rect: { x: 0, y: 0, w: 128, h: 8 } });
    if (nothing.rows.length) fail("the body was drawn into 8 rows; it is a fixed 15+7 per row");
    if (nothing.dropped.indexOf("body") < 0) fail("the body went unreported");
  }

  /* ---- 5. an embedded page draws nothing outside its rect --------------- */
  {
    const dev = D.createFakeDevice({ id: "obxd" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    /* THE CONTROLLER DEFAULTS TO LAYOUT_DIAL. Without this the whole file
     * exercises render_page.mjs, which has supported rect and decorations all
     * along — so every assertion below passed while the Movy grid, the layout
     * the device actually draws and the one this change is about, was never
     * called. Caught by mutation: deleting the lock mark changed nothing. */
    ctl.setLayout(C.LAYOUT_MOVY);
    for (let i = 0; i < 60; i++) ctl.tick();

    /*
     * KNOB PAGES ONLY, and that is the library contract rather than a dodge.
     *
     * Only PAGE_KNOBS goes through renderPageMovy. The preset, items and menu
     * kinds are drawn by the controller at absolute coordinates, and README.md
     * has always said so: "Only PAGE_KNOBS is drawn here. The other kinds name
     * a screen the shadow UI already has, and the caller dispatches to it." An
     * embedding tool dispatches those to its OWN screens, exactly as our host
     * does — which is why they are not asked to honour a rect.
     *
     * Written as a filter with a count rather than as a skip, so a build where
     * knob pages stopped reaching this path fails instead of vacuously passing.
     */
    const rect = { x: 0, y: 16, w: 128, h: 48 };
    let worst = null, outside = 0, checked = 0;
    for (let p = 0; p < ctl.pages.length; p++) {
      ctl.goToPage(p);
      if (ctl.page.kind !== "knobs") continue;
      checked++;
      const ctx = makeCtx(128, 64);
      ctl.render(ctx, { title: "T1 > OB-XD", rect,
                        bands: { header: false, bank: false, footer: false } });
      for (const [x, y] of ctx.px) {
        if (y < rect.y || y >= rect.y + rect.h || x < rect.x || x >= rect.x + rect.w) {
          outside++;
          if (!worst) worst = [x, y];
        }
      }
    }
    if (outside) {
      fail(outside + " pixels drawn outside the rect (first at "
           + JSON.stringify(worst) + ") — an embedded page scribbled on its host");
    }
    if (checked < 3) fail("only " + checked + " knob pages reached the embedded path");
  }

  /* ---- 6. a p-lock actually changes the pixels -------------------------- */
  {
    const ctlMetaMax = (key) => {
      const dev = D.createFakeDevice({ id: "obxd" });
      const ctl = C.createController(dev);
      ctl.load({ slot: 0, component: "synth" });
      for (let i = 0; i < 60; i++) ctl.tick();
      const m = ctl.metaIndex.getOrGuess(key);
      if (!m || typeof m.max !== "number") fail("no declared max for " + key);
      return m.max;
    };
    const paint = (decorations) => {
      const dev = D.createFakeDevice({ id: "obxd" });
      const ctl = C.createController(dev);
      ctl.load({ slot: 0, component: "synth" });
      /* MOVY, not the default DIAL — see the note in check 5. The dial renderer
       * has always drawn decorations (it inverts the label strip for a locked
       * cell), so without this line every assertion below passes on the wrong
       * renderer and says nothing about the grid the device draws. */
      ctl.setLayout(C.LAYOUT_MOVY);
      for (let i = 0; i < 60; i++) ctl.tick();
      if (decorations) ctl.setDecorations(decorations);
      const ctx = makeCtx(128, 64);
      ctl.render(ctx, { title: "T1 > OB-XD" });
      return JSON.stringify(ctx.px);
    };

    /*
     * BOTH SIDES CARRY DECORATIONS, and that is the whole point.
     *
     * The first cut of this compared "no decorations" against "one locked
     * slot" and passed with the lock mark DELETED — verified by mutation.
     * Setting any decoration at all makes graphics stand down (a picture
     * spanning four cells cannot say which is locked), and that stand-down
     * changes the pixels by itself. So the comparison was measuring the viz
     * rule and reporting it as the lock.
     *
     * Holding decorations non-null on both sides pins viz-stood-down in both,
     * leaving the mark as the only difference.
     */
    const unlocked = new Array(8).fill(null);
    unlocked[0] = { locked: false };
    const locks = new Array(8).fill(null);
    locks[0] = { locked: true };
    if (paint(unlocked) === paint(locks)) {
      fail("the lock MARK draws nothing: a locked and an unlocked decoration "
           + "on the same slot produce identical pixels");
    }

    /*
     * And the VALUE override, which is the half that is not a mark: on a held
     * step you look at what the step will play, not at the knob. Compared
     * against a decoration carrying no value, for the same reason.
     *
     * THE VALUE HAS TO BE FAR ENOUGH TO MOVE A PIXEL. The first cut used
     * 0.123456 against a live 0, which failed — not because the override was
     * broken but because obxd cutoff is 0..100, so 0.123456 normalises to
     * 0.0012 and the pointer lands on the same pixel. A probe that cannot
     * resolve the difference it is looking for reports the implementation
     * broken; picking the far end of the declared range removes the ambiguity.
     */
    const range = ctlMetaMax("cutoff");
    const valued = new Array(8).fill(null);
    valued[0] = { value: range, locked: false };
    if (paint(valued) === paint(unlocked)) {
      fail("a decoration value did not replace the live value in the Movy layout");
    }

    /* The viz stand-down is a real rule, so it gets its own assertion rather
     * than being an invisible passenger in the two above. */
    if (paint(null) === paint(unlocked)) {
      fail("decorations did not stand the graphics down");
    }

    /*
     * AN ENUM CELL, because a knob cannot pin the live value.
     *
     * A knob legibly shows TWO values — the pointer on the base and a dot on
     * the arc — so decorating `raw` alone moves its pointer and the assertion
     * above passes whether or not the LIVE value was decorated too. Verified by
     * mutation: disabling the live override changed nothing there.
     *
     * An enum square has one line of text and draws the live value only. It is
     * the cell that can tell the two apart, so the p-lock override is pinned on
     * one of those.
     */
    const ENUM_MOD = "forge";   /* obxd has no multi-option enum on a knob page */
    const enumAt = (() => {
      const dev = D.createFakeDevice({ id: ENUM_MOD });
      const ctl = C.createController(dev);
      ctl.load({ slot: 0, component: "synth" });
      ctl.setLayout(C.LAYOUT_MOVY);
      for (let i = 0; i < 60; i++) ctl.tick();
      for (let p = 0; p < ctl.pages.length; p++) {
        ctl.goToPage(p);
        if (ctl.page.kind !== "knobs") continue;
        for (let sl = 0; sl < ctl.page.keys.length; sl++) {
          const k = ctl.page.keys[sl];
          if (!k) continue;
          const m = ctl.metaIndex.getOrGuess(k);
          if (m && m.kind === "enum" && Array.isArray(m.options) && m.options.length > 2) {
            return { page: p, slot: sl, options: m.options.length };
          }
        }
      }
      return null;
    })();
    if (!enumAt) fail("no multi-option enum cell in the fleet fixture to pin the live value with");

    /*
     * THE LIVE VALUE MUST ACTUALLY BE PRESENT, or this proves nothing.
     *
     * Every widget draws `liveRaw ?? raw`, so when the read cursor has not yet
     * reached a key the live value is undefined and the widget falls back to
     * the decorated `raw` WHETHER OR NOT the live value is overridden. Both
     * sides then agree and the check passes vacuously — which is exactly what
     * happened: forge cv_wave is never read within the tick budget, and the
     * mutation that disables the override survived.
     *
     * So the value is SEEDED into the device rather than waited for, and the
     * precondition is asserted below instead of assumed.
     */
    const paintAt = (pageIndex, decorations) => {
      const dev = D.createFakeDevice({ id: ENUM_MOD, initial: { cv_wave: "0" } });
      const ctl = C.createController(dev);
      ctl.load({ slot: 0, component: "synth" });
      ctl.setLayout(C.LAYOUT_MOVY);
      for (let i = 0; i < 60; i++) ctl.tick();
      ctl.goToPage(pageIndex);
      for (let i = 0; i < 60; i++) ctl.tick();
      if (ctl.state.values["cv_wave"] === undefined) {
        fail("the enum live value was never read; this check cannot discriminate");
      }
      if (decorations) ctl.setDecorations(decorations);
      const ctx = makeCtx(128, 64);
      ctl.render(ctx, { title: "T1 > forge" });
      return JSON.stringify(ctx.px);
    };

    const eBase = new Array(8).fill(null);
    eBase[enumAt.slot] = { locked: false };
    const eLast = new Array(8).fill(null);
    eLast[enumAt.slot] = { value: enumAt.options - 1, locked: false };
    if (paintAt(enumAt.page, eBase) === paintAt(enumAt.page, eLast)) {
      fail("a p-lock on an ENUM cell drew the live value instead of the locked one "
           + "(page " + enumAt.page + " slot " + enumAt.slot + ")");
    }
  }

  console.log("PASS: knob-grid embedding — default layout matches the rhythm table, "
      + "bands close up, nothing drawn outside the rect, p-locks reach the pixels");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
