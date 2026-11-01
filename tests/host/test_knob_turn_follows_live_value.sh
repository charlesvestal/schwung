#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

#
# A knob turn steps FROM the value the editor front-end reports, not from the
# value the controller last stepped to.
#
# The unified knob model keeps a per-key knob state (accumulator + current
# value) so a continuous flick walks smoothly instead of re-reading on every
# detent. But an editor-attached parameter can change ON ITS OWN while the
# grid stands on the page: an internal transition or modulation reset, a
# quantizer snapping back, a routine guard that rewrites its own target. The
# read cursor re-reads that new value into s.values, the display follows --
# and the knob state still holds what the hand last stepped to. The next
# detent then steps FROM THE STALE STATE and writes the old number straight
# back over the new value the front-end now reports.
#
# revalue() exists for exactly this class ("a change made from OUTSIDE this
# controller"), but nobody calls it here: the change was CAUSED by a write
# this controller itself made (e.g. writing the parameter that triggers the
# reset), so there is no recall or preset load to hang on. Those handlers
# drop the whole maps because every value changed at once; here we do not
# know WHICH values changed, so instead of a page-wide read storm the fix
# reconciles ONE knob -- the one being turned -- against the value the read
# cursor already carries, and only where the knob state still disagrees.
#
# Guards, each load-bearing:
#   - settleUntil: a value inside its own settle window is the write this
#     gesture just made, still in flight -- adopting the last pre-write read
#     would undo the user's own detent.
#   - pendingWrite: same, before it even reaches the wire.
#   - the read is the routed one (s.modCache keys read ":base"), so the
#     knob's own base is reconciled, never the modulated effective value
#     drawn on top of it.
#   - a realized difference ALWAYS reseeds to the reported value, for enums
#     (resolved through the wire format, unknown names ignored) and
#     floats/ints alike: the reported value is the truth, and knobInit's
#     homing state makes the first detent continue exactly from where the
#     sound is -- the same property the seeding branch above promises.
#
# This pins:
#   1. A float knob walked up, reset by the system to 0, then turned ONCE
#      steps from 0 (not from the stale value).
#   2. A mid-gesture value never reseeds INSIDE a settle window: the second
#      of two quick detents continues the first one (re-adopting the last
#      pre-write reading would undo the detent just made).
#   3. An enum walked to option C, snapped back by the system to A, walks to
#      B past the four-detents-per-option gate (not C).

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node is required" >&2; exit 1; fi

node -e '
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => { console.error("ok: " + m); };

Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
]).then(([C]) => {
  const HIER = JSON.stringify({ modes: null, levels: { root: { label: "S",
      knobs: ["curve", "amount"],
      params: [{ key: "curve" }, { key: "amount" }] } } });
  const CP = JSON.stringify([
    { key: "curve",  name: "Curve", type: "enum", options: ["A", "B", "C"] },
    { key: "amount", name: "Amount", type: "float", min: 0, max: 1 },
  ]);

  const store = { curve: "A", amount: "0" };
  const writes = [];
  let ticks = 0;
  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      return b in store ? String(store[b]) : "0";
    },
    setParam: (k, v) => {
      store[String(k).replace(/^[^:]+:/, "")] = v;
      writes.push([String(k).replace(/^[^:]+:/, ""), v]);
    },
    announce: () => {},
    now: () => ticks * 16,
  });
  ctl.load({ slot: 0, component: "synth" });
  /* One tick of the controller AND of the test clock: SETPARAM_THROTTLE_MS
   * and every settle window in here are wall-clock and tick-count based. */
  const T = () => { ticks++; ctl.tick(); };
  const slotOf = (key) => (ctl.page.keys || []).indexOf(key);
  const amtSlot = slotOf("amount"), curveSlot = slotOf("curve");
  if (amtSlot < 0 || curveSlot < 0) fail("fixture did not put the knobs on the grid");
  for (let i = 0; i < 16; i++) T();

  /* ---- 1. float: the system resets the value, the next detent follows --- */
  for (let d = 0; d < 20; d++) {
    T();
    ctl.onKnobTurn(amtSlot, 1);
    T();
  }
  { /* fly past the settle window and the announce throttle */
    for (let i = 0; i < 24; i++) T();
    const raised = Number(store.amount);
    if (!(raised > 0.05)) fail("the walk up did not raise amount at all: " + store.amount);
  }
  /* THE SYSTEM RESETS THE VALUE BY ITSELF (the internal transition re-began). */
  store.amount = "0";
  /* Enough ticks to expire the settle window and land a rotation stop on the
   * key (stops = 2 knobs + the preset stop). */
  for (let i = 0; i < 40; i++) T();

  writes.length = 0;
  ctl.onKnobTurn(amtSlot, 1);
  T(); T();
  const after = writes.filter(([k]) => k === "amount").map(([, v]) => Number(v));
  if (!after.length) fail("the detent wrote nothing");
  else if (after[0] > 0.05)
    fail("the first detent after the system reset stepped from the stale value " +
         after[0] + " -- a knob must follow the reported value");
  else ok("float: the first detent after a system-side reset stepped from the reported value " + after[0]);

  /* ---- 3. INSIDE a settle window, a mid-gesture value never reseeds ----- */
  /* Reset through the system again, then two QUICK detents while the settle
   * window is live: the second must step from the write the first one just
   * made, not re-adopt the last pre-write reading. */
  store.amount = "0";
  for (let i = 0; i < 40; i++) T();
  writes.length = 0;
  ctl.onKnobTurn(amtSlot, 1);
  T(); /* let its write land (throttle) */
  const first = store.amount;
  ctl.onKnobTurn(amtSlot, 1); /* settle window is still live */
  /* The write throttle holds the second detent pending; assert on the PENDING
   * value, which is what the hand is asking the device to become. */
  const second = ctl.state.pendingWrite["amount"];
  const n1 = Number(first), n2 = Number(second);
  if (!(n2 > n1) || !(n1 > 0))
    fail("two detents inside a settle window produced " + first + " then " + second +
         " -- the second detent must continue the first one");
  else ok("two detents mid-gesture walked " + first + " then " + second);

  /* ---- 2. enum: the walk to C must not relaunch from C after a reset ---- */
  store.curve = "A";
  for (let i = 0; i < 40; i++) T();
  writes.length = 0;
  for (let d = 0; d < 12; d++) { T(); ctl.onKnobTurn(curveSlot, 1); }
  T(); T();
  if (store.curve !== "C") fail("the enum walk did not reach C: " + store.curve);

  store.curve = "A"; /* the system reset it */
  for (let i = 0; i < 40; i++) T();

  writes.length = 0;
  for (let d = 0; d < 5; d++) {
    ctl.onKnobTurn(curveSlot, 1);
    T(); T();
  }
  T(); T();
  const afterEnum = writes.filter(([k]) => k === "curve").map(([, v]) => v).pop();
  if (afterEnum === undefined) fail("the enum detents wrote nothing");
  else if (afterEnum === "C")
    fail("after the system snapped curve back to A, the walk landed on C (the " +
         "stale knob state) instead of stepping from A");
  else if (afterEnum !== "B")
    fail("after the reset the walk resolved " + afterEnum + ", expected B");
  else ok("enum: stepping past the four-detent gate after a reset resolved " + afterEnum);

  if (failures) process.exitCode = 1;
  else console.error("ALL PASS");
}).catch((e) => { console.error("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
