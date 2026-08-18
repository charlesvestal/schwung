#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Screen-reader strings for the knob grid
# (src/shared/param_pages/announce_page.mjs).
#
# A grid is WORSE than a list for a screen-reader user unless this is handled
# deliberately: a list announces the selected row as you scroll, so the reading
# order is the navigation order, while a grid has eight cells that nothing
# selects. These tests pin the behaviour that makes it usable — position is
# spoken, the full name replaces the five-character label on touch, turning
# announces the value only, and every page of every fleet module produces
# something a person could act on.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the announcement tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/announce_page.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/param_meta.mjs"),
  import("node:fs"),
]).then(([A, P, M, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const forModule = (id) => {
    const m = fx.modules.find((x) => x.id === id);
    if (!m) fail("fixture has no module " + id);
    return {
      pages: P.planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params }).pages,
      metaIndex: M.buildMetaIndex({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params }),
    };
  };

  /* ---- 1. position is spoken, because the rule is not audible ----------- */
  {
    const { pages } = forModule("obxd");
    const said = A.announcePage(pages[1], 1, pages.length);
    if (!/2 of 17/.test(said)) fail("page position must be spoken: " + said);
    if (!/Main/.test(said)) fail("the page name must be spoken: " + said);
    if (!/control/.test(said)) fail("the number of controls should be spoken: " + said);
  }

  /* ---- 2. punctuation is not read aloud -------------------------------- */
  {
    const said = A.announcePage({ kind: "knobs", name: "Filter - 2", keys: ["a"] }, 4, 9);
    if (/- 2/.test(said)) fail("\"Filter - 2\" should not be spoken as punctuation: " + said);
    if (!/page 2 of Filter/.test(said)) fail("a continuation should read as page N of the level: " + said);
    if (!/1 control\b/.test(said)) fail("a single control should be singular: " + said);
  }

  /* ---- 3. non-grid page kinds identify themselves ----------------------- */
  {
    for (const [kind, want] of [["preset", /preset browser/], ["items", /list/],
                                ["modes", /mode select/], ["child", /selector/]]) {
      const said = A.announcePage({ kind, name: "X" }, 0, 3);
      if (!want.test(said)) fail(kind + " page announced as: " + said);
    }
  }

  /* ---- 4. touch gives the FULL name, turning gives value only ----------- */
  {
    const { metaIndex } = forModule("obxd");
    const meta = metaIndex.getOrGuess("resonance");
    const touched = A.announceTouch(meta, "42", 1, null);
    if (!/Resonance/.test(touched)) fail("touch must speak the unabbreviated name: " + touched);
    if (!/42/.test(touched)) fail("touch must speak the value: " + touched);

    const turned = A.announceTurn(meta, "43");
    if (/Resonance/.test(turned)) fail("turning must not repeat the name every detent: " + turned);
    if (!/43/.test(turned)) fail("turning must speak the value: " + turned);
  }

  /* ---- 5. enums speak the option, never the index ----------------------- */
  {
    const { metaIndex } = forModule("arp");
    const mode = metaIndex.getOrGuess("mode");
    const said = A.announceTurn(mode, "3");
    if (!/up_down/.test(said)) fail("an enum must speak its option name, got: " + said);
    if (said.trim() === "3") fail("an enum spoke a bare index");
  }

  /* ---- 6. unread and opaque values say something useful ----------------- */
  {
    const { metaIndex } = forModule("obxd");
    const meta = metaIndex.getOrGuess("cutoff");
    if (!/not read yet/.test(A.spokenValue(meta, null))) fail("an unread value should say so");
    if (/--/.test(A.spokenValue(meta, null))) fail("the on-screen placeholder must not be spoken");

    const { metaIndex: mi } = forModule("mrdrums");
    const fileMeta = mi.getOrGuess("pad_sample_path");
    const said = A.announceTouch(fileMeta, "/data/UserData/Samples/kick_01.wav", 0, null);
    if (!/kick_01\.wav/.test(said)) fail("a filepath should speak its filename: " + said);
    if (/UserData/.test(said)) fail("a filepath should not read the whole path aloud: " + said);
    if (!/click to open/.test(said)) fail("an opaque cell should say it opens: " + said);
  }

  /* ---- 7. a locked cell says so ---------------------------------------- */
  {
    const { metaIndex } = forModule("obxd");
    const meta = metaIndex.getOrGuess("cutoff");
    const said = A.announceTouch(meta, "60", 0, { value: "12", locked: true });
    if (!/locked/.test(said)) fail("a parameter lock must be announced: " + said);
    if (!/12/.test(said)) fail("a locked cell must speak the LOCKED value, not the live one: " + said);
    if (/60/.test(said)) fail("a locked cell must not speak the live value: " + said);
  }

  /* ---- 8. unassigned knobs do not go silent ----------------------------- */
  {
    const said = A.announceTouch(null, null, 5, null);
    if (!/knob 6/.test(said)) fail("an empty slot should still identify itself: " + said);
  }

  /* ---- 9. every fleet page produces usable speech ----------------------- */
  {
    let checked = 0;
    for (const mod of fx.modules) {
      const { pages, metaIndex } = forModule(mod.id);
      for (let i = 0; i < pages.length; i++) {
        const said = A.announcePage(pages[i], i, pages.length);
        if (!said || said.length < 4) fail(mod.id + " page " + i + " announced as " + JSON.stringify(said));
        if (/undefined|null|NaN/.test(said)) fail(mod.id + " page " + i + " leaked a placeholder: " + said);
        checked++;
      }
      const grid = pages.find((p) => p.kind === P.PAGE_KNOBS);
      if (grid) {
        const values = {};
        for (const k of grid.keys) values[k] = "0.5";
        const all = A.announcePageContents(grid, metaIndex, values, null);
        if (/undefined|NaN/.test(all)) fail(mod.id + " page contents leaked a placeholder: " + all);
        if (!all.length) fail(mod.id + " page contents were empty");
      }
    }
    if (checked < 500) fail("only " + checked + " pages checked");
    console.log("PASS: screen-reader announcements — " + checked +
                " fleet pages, position spoken, no punctuation read aloud, locks and enums correct");
  }
});
'
