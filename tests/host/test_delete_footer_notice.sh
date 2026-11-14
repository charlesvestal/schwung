#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Delete ("X") + Jog Click, the VISIBLE half: what the footer says while Delete
# is held, and the short confirmation a removal leaves in the footer row.
#
# The gesture itself (which removal path runs, Delete read from the shim's
# delete_held byte, the Delete press dismissing a notice) is pinned by
# test_delete_jogclick_remove.sh. The pixels of the two footers are in
# the chain-editor baseline (chain/len2/delete, chain/len2/removed-notice,
# master/len2/delete, ...), which also checks that a notice changes nothing
# above the footer row. The footer wiring of both editors is evaluated in
# test_chain_gestures.sh. This file covers what those cannot:
#
#   1. the notice's LIFETIME -- it expires on its own (FOOTER_NOTICE_MS), and a
#      jog turn or a button press dismisses it early, but a button RELEASE and
#      a capacitive touch do not;
#   2. the notice is drawn inside the footer row and nowhere else, at any
#      length, and fits the display;
#   3. the Delete footer fits and keeps the resting footer's places;
#   4. shadow_ui.js wires it in the right ORDER: the dismissal runs ahead of
#      the jog-click dispatch (or the click that raises a notice would be the
#      input that clears it), the Delete byte is followed ahead of both, and
#      both removal branches raise the notice for their own editor.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { createFooterNotice, footerNoticeDismissedBy, drawFooterNotice,
         deleteHintsFor, CHAIN_HINTS_AT_REST, FOOTER_NOTICE_MS, DELETE_CLICK_VERB }
  from "./src/shared/chain_editor_chrome.mjs";
import { drawFooter, RULE_Y } from "./src/shared/param_pages/render_page_movy.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => console.log("  ok  " + m);
const flat = (h) => (h || []).map((p) => p.join(" ")).join(" / ");

/* ---- 1. lifetime ------------------------------------------------------- */
{
  let t = 1000;
  const n = createFooterNotice({ now: () => t });
  if (n.active() || n.textFor("slot") !== "") fail("a fresh notice is already showing");
  n.show("slot", "Removed CloudSeed");
  if (n.textFor("slot") !== "Removed CloudSeed") fail("show() did not raise the notice");
  if (n.textFor("master") !== "") fail("a slot notice is drawn under Master FX");
  t += FOOTER_NOTICE_MS - 1;
  if (n.textFor("slot") !== "Removed CloudSeed") fail("the notice expired early");
  t += 1;
  if (n.textFor("slot") !== "") fail("the notice outlived FOOTER_NOTICE_MS");
  if (n.active()) fail("an expired notice still reports active");
  if (FOOTER_NOTICE_MS < 1000 || FOOTER_NOTICE_MS > 2000)
    fail("FOOTER_NOTICE_MS is " + FOOTER_NOTICE_MS + ", not a ~1.5 s confirmation");
  ok("the notice expires on its own after " + FOOTER_NOTICE_MS + " ms and is scoped to its editor");

  /* Early dismissal: the MIDI the handler sees, fed through the same
     predicate + clear() pair shadow_ui.js runs. */
  const feed = (status, d1, d2) => {
    if (footerNoticeDismissedBy(status, d1, d2)) n.clear();
  };
  const raise = () => n.show("master", "Removed Freeverb");
  const cases = [
    ["a jog turn (CC 14)",          [0xB0, 14, 1],   true],
    ["a jog turn back (CC 14 127)", [0xB0, 14, 127], true],
    ["a jog click press (CC 3)",    [0xB0, 3, 127],  true],
    ["a Back press (CC 51)",        [0xB0, 51, 127], true],
    ["a claimed Delete press (CC 119)", [0xB0, 119, 127], true],
    ["a step press (note 16)",      [0x90, 16, 100], true],
    ["a pad press (note 68)",       [0x90, 68, 100], true],
    ["a claimed Delete RELEASE (CC 119 0)", [0xB0, 119, 0], false],
    ["a jog click RELEASE (CC 3 0)", [0xB0, 3, 0],   false],
    ["a jog TOUCH (note 9)",        [0x90, 9, 127],  false],
    ["a knob TOUCH (note 0)",       [0x90, 0, 127],  false],
    ["a note-off (note 16 vel 0)",  [0x90, 16, 0],   false],
  ];
  for (const [what, msg, dismisses] of cases) {
    raise();
    feed(...msg);
    const gone = n.textFor("master") === "";
    if (gone !== dismisses)
      fail(what + (dismisses ? " did not dismiss" : " dismissed") + " the notice");
  }
  ok("a jog turn or any button/step/pad press dismisses it; releases and touches do not");
  n.show("slot", "x");
  if (n.clear() !== true) fail("clear() on a showing notice reports nothing cleared");
  if (n.clear() !== false) fail("clear() with nothing showing reports a change (needless redraw)");
}

/* ---- 2. the notice draws in the footer row only, and fits ------------- */
{
  for (const text of ["Removed CloudSeed", "Removed A Very Long Module Display Name That Cannot Fit",
                      "Removed fr", ""]) {
    const fb = createFramebuffer();
    const d = drawContext(fb);
    drawFooterNotice(d, text);
    if (fb.clipped() !== 0) fail("the notice [" + text + "] drew outside the 128x64 display");
    let above = 0, lit = 0;
    for (let y = 0; y < fb.height; y++)
      for (let x = 0; x < fb.width; x++)
        if (fb.pixels[y * fb.width + x]) { lit++; if (y < RULE_Y) above++; }
    if (above) fail("the notice [" + text + "] lit " + above + " pixels above the footer row");
    if (text && !lit) fail("the notice [" + text + "] drew nothing");
    if (!text && lit) fail("an empty notice drew something");
  }
  ok("the notice stays inside the footer row (rows " + RULE_Y + "..63) at any length");
}

/* ---- 3. the Delete footer ---------------------------------------------- */
{
  const ctx = { fillRect: () => {}, print: () => {}, setPixel: () => {} };
  const full = deleteHintsFor({ kind: "module", module: "cloudseed" });
  if (flat(full) !== "JOG SEL / CLK " + DELETE_CLICK_VERB)
    fail("Delete on a module reads [" + flat(full) + "]");
  if (drawFooter(ctx, full) !== full.length) fail("the Delete footer does not fit: [" + flat(full) + "]");
  if (full[0] !== CHAIN_HINTS_AT_REST[0]) fail("JOG SEL left its slot under Delete");
  if (flat(deleteHintsFor({ kind: "synth", module: "braids" })) !== flat(full))
    fail("Delete on the synth reads differently from Delete on a module");
  for (const kind of ["add", "patch", "settings", "sendbus", "busback"])
    if (deleteHintsFor({ kind }) !== CHAIN_HINTS_AT_REST)
      fail("Delete on a `" + kind + "` cell changes the footer, but the click is a plain click there");
  if (deleteHintsFor(null) !== CHAIN_HINTS_AT_REST) fail("Delete with nothing focused changes the footer");
  ok("Delete names CLK " + DELETE_CLICK_VERB + " on a loaded position, fits, and leaves every other cell at rest");
}

/* ---- 4. the wiring in shadow_ui.js ------------------------------------- */
{
  const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
  /* Delete is not tracked from CC 119 (the shim does not forward it): the
     MIDI handler follows the shim byte, ahead of the dismissal. */
  const midi = src.indexOf("globalThis.onMidiMessageInternal = function(data) {");
  const track = src.indexOf("observeDeleteHeld();", midi);
  const dismiss = src.indexOf("footerNoticeDismissedBy(status, d1, d2) && chainFooterNotice.clear()");
  /* EVERY jog-click dispatch -- the ordinary one and the co-run one -- must
     come after the dismissal, so the earliest is what is compared. */
  /* The FIRST jog-click dispatch in the file: there are two (the ordinary
     one and the co-run one), and both must come after the dismissal. */
  const click = src.indexOf("view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0)");
  if (track < 0) fail("the MIDI handler no longer follows the Delete byte");
  if (dismiss < 0) fail("the MIDI handler never dismisses the footer notice");
  if (click < 0) fail("could not find the jog-click dispatch");
  if (dismiss >= 0 && click >= 0 && dismiss > click)
    fail("the notice is dismissed AFTER the jog-click dispatch -- the click that raises it would clear it");
  if (track >= 0 && dismiss >= 0 && dismiss < track)
    fail("the dismissal runs before the Delete byte is followed");
  const observe = src.match(/^function observeDeleteHeld\(\) \{[^]*?^\}/m);
  if (!observe || !/needsRedraw = true/.test(observe[0]))
    fail("a Delete press/release does not redraw, so the footer would lag the button");
  ok("the dismissal runs after the Delete byte is followed and ahead of the jog-click dispatch");

  const chainBranch = src.slice(src.indexOf("Delete(\"X\")+JogClick: remove a populated module directly"));
  const cb = chainBranch.slice(0, chainBranch.indexOf("break;"));
  if (!/applyChainComponentPick\(selectedSlot, comp\.key, "", null\);[\s\S]*chainFooterNotice\.show\("slot", `Removed \$\{removedName\}`\)/.test(cb))
    fail("the slot-chain removal does not raise its footer notice after removing");
  const mfxBranch = src.slice(src.indexOf("Delete(\"X\")+JogClick: remove a populated MFX slot directly"));
  const mb = mfxBranch.slice(0, mfxBranch.indexOf("break;"));
  if (!/removeMasterFxComponent\(selectedComp\);[\s\S]*chainFooterNotice\.show\("master", `Removed \$\{removedName\}`\)/.test(mb))
    fail("the Master FX removal does not raise its footer notice after removing");
  ok("both removal branches raise the notice for their own editor, after the removal");

  if (!/_ctx\.isDeleteHeld = /.test(src) || !/_ctx\.chainFooterNoticeText = /.test(src))
    fail("Master FX cannot see the Delete flag or the notice through ctx");
  const mfx = readFileSync("src/shadow/shadow_ui_master_fx.mjs", "utf8");
  if (!/notice: chainFooterNoticeText\("master"\)/.test(mfx))
    fail("drawMasterFx does not draw its footer notice");
  if (!/notice: chainFooterNoticeText\("slot"\)/.test(src))
    fail("drawChainEdit does not draw its footer notice");
  ok("both editors draw the notice and the Delete footer through the shared bands");
}

if (failures) process.exit(1);
console.log("PASS: Delete+JogClick footer -- CLK " + DELETE_CLICK_VERB + " while Delete is held, " +
            "and a footer-row confirmation that expires and is dismissed by the next input");
'
