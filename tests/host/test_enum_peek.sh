#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE ENUM PEEK RAISES THE OPTION LIST ON A TURN.
#
# Holding an enum knob and clicking opens the PICKER: nothing is written on the
# way in, so Back is a genuine cancel. Turning the same knob is the opposite --
# the detent has already written -- so the peek is an OVERLAY over the grid, not
# a view, and it decays on its own rather than needing a way out. That is why
# the two share enum_list.mjs (one screen) but not a view (opposite semantics).
#
# THE PART THAT SILENTLY BREAKS IS THE READ.
#
# A parameter round-trip is ~2.8ms against a 1.68ms whole-page render, so a peek
# that read to find its own index would cost more than the frame it draws on. It
# does not have to: onKnobTurn has just computed the new value in the knob
# engine, and the options come from cached chain_params metadata. This asserts
# getParam is never called on the turn path once the page is settled -- not
# visible in code review, because such a read would sit inside knobStep`s seed
# fallback and look like initialisation.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string, and one apostrophe ends it early with an error pointing nowhere
# near the real line.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the enum peek tests" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { createController, ENUM_PEEK_MS }
  from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const CHAIN_PARAMS = [
  { key: "shape", name: "Shape", type: "enum",
    options: ["Sine", "Tri", "Saw", "Square", "Noise"] },
  { key: "onoff", name: "Gate",  type: "enum", options: ["Off", "On"] },
  { key: "solo",  name: "Solo",  type: "enum", options: ["Only"] },  /* one option */
  { key: "bare",  name: "Bare",  type: "enum" },                  /* no options */
  { key: "gain",  name: "Gain",  type: "float", min: 0, max: 1, step: 0.01 },
];
const HIER = { modes: null, levels: { root: { label: "T",
  knobs: ["shape", "onoff", "solo", "bare", "gain"],
  params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };

let clock = 1000;
function mk() {
  clock = 1000;
  const store = { shape: "0", onoff: "0", solo: "0", bare: "0", gain: "0.5" };
  const reads = [];
  const ctl = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
      reads.push(b);
      return b in store ? store[b] : "";
    },
    setParam: (k, v) => { store[String(k).replace(/^[^:]+:/, "")] = String(v); },
    announce: () => {},
    now: () => clock,
  });
  ctl.load({ prefix: "synth" });
  /* Settle the value cursor the way a live page does, so the reads counted
     below are the turn path only and not first-touch seeding. */
  for (let i = 0; i < 12; i++) ctl.tick();
  const slotOf = (key) => (ctl.page.keys || []).indexOf(key);
  return { ctl, store, reads, slotOf };
}

/* Spin a knob far enough to cross at least one enum detent gate (an enum is
   gated at 4 raw detents per option). */
const spin = (ctl, slot, n) => {
  for (let i = 0; i < n; i++) { clock += 20; ctl.onKnobTurn(slot, 1, clock); }
};

/* ===================================================================== 1 ==
 * A divable enum with a real list peeks.
 */
{
  const { ctl, slotOf } = mk();
  const s = slotOf("shape");
  ok(s >= 0, "shape reached the page");
  spin(ctl, s, 6);
  const p = ctl.enumPeek();
  ok(!!p, "turning a 5-option enum raises a peek");
  ok(p && p.key === "shape", "the peek names the turned key");
  ok(p && p.options.length === 5, "the peek carries all 5 options");
  ok(p && p.title === "Shape", "the peek title is the param name");
  ok(p && p.index >= 0 && p.index < 5, "the peek index is inside the list, got " + (p && p.index));
}

/* ===================================================================== 2 ==
 * It decays on its own, and a further detent re-arms it. There is no release
 * event coming for a knob no finger registered on, so a deadline is the only
 * way out.
 */
{
  const { ctl, slotOf } = mk();
  const s = slotOf("shape");
  spin(ctl, s, 6);
  const armedAt = clock;
  clock = armedAt + ENUM_PEEK_MS - 1;
  ok(ctl.enumPeek() !== null, "the peek is alive just inside the window");
  clock = armedAt + ENUM_PEEK_MS + 1;
  ok(ctl.enumPeek() === null, "the peek is gone just outside the window");
  spin(ctl, s, 2);
  ok(ctl.enumPeek() !== null, "a further detent re-arms it");
}

/* ===================================================================== 3 ==
 * Not every knob is a door.
 */
{
  {
    const { ctl, slotOf } = mk();
    spin(ctl, slotOf("gain"), 6);
    ok(ctl.enumPeek() === null, "a float does not peek");
  }
  {
    const { ctl, slotOf } = mk();
    spin(ctl, slotOf("bare"), 6);
    ok(ctl.enumPeek() === null,
       "an enum declaring NO options has no list, so it must not peek");
  }
  {
    const { ctl, slotOf } = mk();
    spin(ctl, slotOf("solo"), 6);
    ok(ctl.enumPeek() === null,
       "a ONE-option enum is not a list either -- there is nothing to scroll");
  }
  {
    const { ctl, slotOf } = mk();
    spin(ctl, slotOf("onoff"), 6);
    ok(ctl.enumPeek() !== null, "a two-option enum DOES peek -- Off/On is worth seeing");
  }
}

/* ===================================================================== 4 ==
 * THE READ BUDGET. An IPC read costs more than rendering the whole screen, so
 * the peek must be free: the index is the knob engine`s and the options are
 * cached metadata.
 */
{
  const { ctl, slotOf, reads } = mk();
  const s = slotOf("shape");
  spin(ctl, s, 4);            /* may seed knob state from the device once */
  const afterSeed = reads.length;
  spin(ctl, s, 12);
  for (let i = 0; i < 10; i++) ctl.enumPeek();
  ok(reads.length === afterSeed,
     "further detents and every enumPeek() read NOTHING (got "
     + (reads.length - afterSeed) + " reads)");
}

/* ===================================================================== 5 ==
 * Anything that moves the target takes the list down. The list describes ONE
 * parameter; leaving it up over another is a wrong reading, not a stale one.
 */
{
  {
    const { ctl, slotOf } = mk();
    spin(ctl, slotOf("shape"), 6);
    ctl.onKnobTouch(slotOf("shape"), true);
    ok(ctl.enumPeek() === null, "a touch clears the peek");
  }
  {
    const { ctl, slotOf } = mk();
    spin(ctl, slotOf("shape"), 6);
    ctl.onJog(1);
    ok(ctl.enumPeek() === null, "a jog clears the peek");
  }
  {
    const { ctl, slotOf } = mk();
    spin(ctl, slotOf("shape"), 6);
    spin(ctl, slotOf("gain"), 2);
    ok(ctl.enumPeek() === null,
       "turning a NEIGHBOUR clears it -- the list was describing a knob your "
       + "hand has left");
  }
}

/* ===================================================================== 6 ==
 * The turn still WRITES. The peek is a display over a committed value; if it
 * ever swallowed the detent it would look identical on screen and do nothing.
 */
{
  const { ctl, store, slotOf } = mk();
  const before = store.shape;
  spin(ctl, slotOf("shape"), 24);
  ctl.tick();
  ok(store.shape !== before,
     "turning a peeking enum still steps the value, got " + JSON.stringify(store.shape));
}

/* ===================================================================== 7 ==
 * THE WIRING, at the source. The pixels of this screen are already pinned by
 * test_enum_picker_chrome.sh, because the peek and the picker share
 * enum_list.mjs -- what is NOT covered there is that the grid reaches it with
 * the right arguments and, crucially, WITHOUT changing view.
 */
{
  const src = readFileSync("src/shadow/shadow_ui_param_pages.mjs", "utf8");
  const at = src.indexOf("export function drawParamPages(");
  const body = src.slice(at, src.indexOf("\nexport ", at + 10));

  ok(/controller\.enumPeek\(\)/.test(body),
     "drawParamPages asks the controller for the peek");
  ok(/drawEnumList\(/.test(body),
     "it draws through the SHARED enum screen, not a second list");
  ok(/headerRight:\s*"TURNING"/.test(body),
     "the header says TURNING, not SELECT -- nothing is being selected here");
  ok(!/setView\(/.test(body),
     "the peek must not change view: the detent already wrote, so a Back that "
     + "cancelled it would be a lie");

  /* Cursor and live value are the same thing on this screen. A markIndex that
     drifted from index would draw the `*` on a row that is not the value. */
  const mk = body.match(/markIndex:\s*([^,\n]+)/);
  const ix = body.match(/\n\s*index:\s*([^,\n]+)/);
  ok(!!mk && !!ix && mk[1].trim() === ix[1].trim(),
     "markIndex tracks index -- on a peek the cursor IS the live value (got "
     + (mk && mk[1].trim()) + " vs " + (ix && ix[1].trim()) + ")");
}

process.exit(fail ? 1 : 0);
'
