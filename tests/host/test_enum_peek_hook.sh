#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE HOST MAY DECLINE THE PEEK, PER KEY.
#
# The controller already declines it on a list layout, because a row prints the
# option in full and the panel would cover a legible answer with the same
# answer. A grid cell can be in that same position -- a three-character option
# in a box that fits it -- but only the host knows how its own cells are drawn,
# so the judgement is injected rather than guessed here.
#
# Absent, or null for a key, means raised exactly as before: that is the
# assertion that matters. And true cannot force a peek past the gates that are
# facts about the screen (a list layout, a wide graphic, a switch).
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the enum peek hook test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_LIST } from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const CHAIN = [
  { key: "shape", name: "Shape", type: "enum", options: ["Sine", "Tri", "Saw", "Square", "Noise"] },
  { key: "onoff", name: "Gate",  type: "enum", options: ["Off", "On"] },
];
const HIER = { modes: null, levels: { root: { label: "T", knobs: ["shape", "onoff"],
  params: CHAIN.map((p) => ({ key: p.key })) } } };

function peekAfterTurn(hook, key, layout) {
  let clock = 1000;
  const store = { shape: "0", onoff: "0" };
  const asked = [];
  const io = {
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CHAIN);
      return b in store ? store[b] : "";
    },
    setParam: (k, v) => { store[String(k).replace(/^[^:]+:/, "")] = String(v); },
    announce: () => {},
    now: () => clock,
  };
  if (hook !== undefined) io.allowEnumPeek = (k, m) => { asked.push(k + "|" + (m && m.key)); return hook(k, m); };
  const ctl = createController(io);
  if (layout) ctl.setLayout(layout);
  ctl.load({ prefix: "synth" });
  for (let i = 0; i < 12; i++) ctl.tick();
  const slot = (ctl.page.keys || []).indexOf(key || "shape");
  for (let i = 0; i < 6; i++) { clock += 20; ctl.onKnobTurn(slot, 1, clock); }
  return { peek: ctl.enumPeek(), asked };
}

if (!peekAfterTurn(undefined).peek) bad("the default stopped raising the peek");
if (!peekAfterTurn(() => null).peek) bad("a null answer did not fall through to the default");
const declined = peekAfterTurn(() => false);
if (declined.peek) bad("the host declined and the peek was raised anyway");
if (!declined.asked.length || declined.asked[0] !== "synth:shape|shape")
  bad("the hook was not handed the full key and its meta, got " + JSON.stringify(declined.asked));
if (!peekAfterTurn(() => true).peek) bad("the host allowed it and it was not raised");
/* Per key: decline one, keep the other. */
const perKey = (k) => (k.endsWith(":shape") ? false : null);
if (peekAfterTurn(perKey, "shape").peek) bad("a per-key decline did not hold for its key");
/* true cannot force past a fact about the screen. */
if (peekAfterTurn(() => true, "shape", LAYOUT_LIST).peek) bad("true forced a peek onto a list layout");
if (peekAfterTurn(() => true, "onoff").peek) bad("true forced a peek onto a switch");

if (fail) process.exit(1);
console.log("PASS: allowEnumPeek is consulted per key, absent means raised, and it can only decline");
'
