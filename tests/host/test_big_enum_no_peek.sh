#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A DECLARED BIG ENUM DOES NOT PEEK.
#
# `display: "big"` draws the option in the big face, and only when every option
# fits -- so the cell already reads, which is why a list layout and a switch
# decline the peek. io.allowEnumPeek let a HOST decline it, but a module
# declaring `display: "big"` has no hook, and got the panel over its own
# readout on every turn.
#
# The three that must STILL peek are the point: an undeclared enum, a
# declaration that does not fit (it falls back to the enum square), and the
# dial layout (which does not draw declared big cells).
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the big enum peek test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY } from "./src/shared/param_pages/page_controller.mjs";
import { LAYOUT_DIAL } from "./src/shared/param_pages/render_page.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const WORDS = ["Major", "Minor", "Dorian", "Lydian"];

function peekAfterTurn(param, layout) {
  let clock = 1000;
  const CHAIN = [Object.assign({ key: "root", name: "Root", type: "enum" }, param)];
  const HIER = { modes: null, levels: { root: { label: "T", knobs: ["root"],
    params: [{ key: "root" }] } } };
  const store = { root: "0" };
  const ctl = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CHAIN);
      return b in store ? store[b] : "";
    },
    setParam: (k, v) => { store[String(k).replace(/^[^:]+:/, "")] = String(v); },
    announce: () => {},
    now: () => clock,
  });
  ctl.setLayout(layout);
  ctl.load({ prefix: "synth" });
  for (let i = 0; i < 12; i++) ctl.tick();
  for (let i = 0; i < 6; i++) { clock += 20; ctl.onKnobTurn(0, 1, clock); }
  return ctl.enumPeek();
}

if (!peekAfterTurn({ options: NOTES }, LAYOUT_MOVY))
  bad("control: an undeclared enum no longer peeks, so the next check measures nothing");
if (peekAfterTurn({ options: NOTES, display: "big" }, LAYOUT_MOVY))
  bad("a declared big enum raised the peek over its own readout");
if (!peekAfterTurn({ options: WORDS, display: "big" }, LAYOUT_MOVY))
  bad("a declaration that does not fit draws the enum square, and must still peek");
if (!peekAfterTurn({ options: NOTES, display: "big" }, LAYOUT_DIAL))
  bad("the dial layout draws no big cell, so a declared enum there must still peek");

if (fail) process.exit(1);
console.log("PASS: a declared big enum declines the peek; undeclared, unfitting and dial still peek");
'
