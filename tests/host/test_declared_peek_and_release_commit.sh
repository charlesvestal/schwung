#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# TWO DECLARATIONS A CONTRACT CAN MAKE ABOUT ONE ENUM, both inert when absent.
#
# `peek: false` -- this enum never raises the peek. The square already says
# everything for a short two-way ("UNI"/"BI", "FRE"/"SYN"), and the panel on
# every flip covers the page to show the one other word. allowEnumPeek is the
# HOST asking the same question; this one travels with the contract.
#
# `commit: "release"` -- the cell and the peek follow the knob, the WRITE waits
# for the hand to let go. An LFO target is the case: written per detent, a
# scroll from one routing to another drives every parameter in between. The
# release flushes it; with no release seen (a turn the cap sensor missed) it
# lands after RELEASE_COMMIT_IDLE_MS of stillness, never on the first detent.
# While it waits, the read rotation must not put the old device value back.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, RELEASE_COMMIT_IDLE_MS } from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const OPTS = ["None", "A", "B", "C", "D", "E", "F", "G"];
function rig(extra) {
  const CHAIN = [
    Object.assign({ key: "tgt", name: "Targ", type: "enum", options: OPTS }, extra.tgt || {}),
    Object.assign({ key: "mode", name: "Mode", type: "enum", options: ["Unipolar", "Bipolar"] },
                  extra.mode || {}),
  ];
  const HIER = { modes: null, levels: { root: { label: "T", knobs: ["tgt", "mode"],
    params: CHAIN.map((p) => ({ key: p.key })) } } };
  const state = { clock: 1000, store: { tgt: "0", mode: "0" }, writes: [] };
  const io = {
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CHAIN);
      return b in state.store ? state.store[b] : "";
    },
    setParam: (k, v) => {
      const b = String(k).replace(/^[^:]+:/, "");
      state.writes.push(b + "=" + v);
      state.store[b] = String(v);
    },
    announce: () => {},
    now: () => state.clock,
  };
  const ctl = createController(io);
  ctl.load({ prefix: "synth" });
  for (let i = 0; i < 12; i++) ctl.tick();
  state.slot = (k) => (ctl.page.keys || []).indexOf(k);
  return { ctl, state };
}
const turn = (r, key, n) => {
  for (let i = 0; i < n; i++) { r.state.clock += 40; r.ctl.onKnobTurn(r.state.slot(key), 1, r.state.clock); }
};
const tgtWrites = (r) => r.state.writes.filter((w) => w.startsWith("tgt="));

/* ---- peek: false --------------------------------------------------------- */
{
  /* Mode is a two-way the square can show; the control is the same enum
   * undeclared. Four options on the target so the two-way flip is not what
   * declines it. */
  const plain = rig({ mode: { options: ["Uni", "Bi", "Mid", "Neg"] } });
  turn(plain, "mode", 6);
  if (!plain.ctl.enumPeek()) bad("an undeclared enum stopped raising the peek");
  const quiet = rig({ mode: { options: ["Uni", "Bi", "Mid", "Neg"], peek: false } });
  turn(quiet, "mode", 6);
  if (quiet.ctl.enumPeek()) bad("peek:false still raised the peek");
}

/* ---- commit: release ----------------------------------------------------- */
{
  /* Control: undeclared, the turn writes as it goes. */
  const plain = rig({});
  plain.ctl.onKnobTouch(plain.state.slot("tgt"), true);
  turn(plain, "tgt", 16);
  plain.ctl.tick();
  if (tgtWrites(plain).length < 2) bad("an undeclared enum stopped writing per detent: " + tgtWrites(plain));

  const r = rig({ tgt: { commit: "release" } });
  const slot = r.state.slot("tgt");
  r.ctl.onKnobTouch(slot, true);
  turn(r, "tgt", 16);
  const shown = r.ctl.enumPeek();
  if (!shown || shown.index < 1) bad("the peek did not follow the turn while the write waited");
  for (let i = 0; i < 20; i++) { r.state.clock += 100; r.ctl.tick(); }
  if (tgtWrites(r).length) bad("a release-committed key wrote while held: " + tgtWrites(r));
  /* The rotation must not undo the turn from the device, which still says 0. */
  if (!r.ctl.state || !r.ctl.state.values) bad("the test cannot see the cell value");
  else if (r.ctl.state.values.tgt === "0") bad("the read rotation put the old value back mid-turn");
  r.ctl.onKnobTouch(slot, false);
  const w = tgtWrites(r);
  if (w.length !== 1) bad("the release should write exactly once, got " + JSON.stringify(w));
  else if (w[0] === "tgt=0") bad("the release wrote the old value");

  /* No release ever seen: the idle backstop lands it, and not before. */
  const n = rig({ tgt: { commit: "release" } });
  turn(n, "tgt", 8);
  n.state.clock += RELEASE_COMMIT_IDLE_MS - 200; n.ctl.tick();
  if (tgtWrites(n).length) bad("the backstop fired before RELEASE_COMMIT_IDLE_MS");
  n.state.clock += 400; n.ctl.tick();
  if (tgtWrites(n).length !== 1) bad("with no release the value never landed: " + tgtWrites(n));
}

if (fail) process.exit(1);
console.log("PASS: peek:false declines the peek; commit:release writes once, where the hand stops");
'
