#!/usr/bin/env bash
# THE ASSEMBLY. Tasks 1-11 built every component of the E16 surface and wired
# none of them together: `e16Nav` was null, `createNav` and `createDisplay` were
# never called, and with a device attached Schwung entered remote mode and then
# drew nothing. Every one of those tasks passed its own tests, because the seam
# between two files belongs to neither of them.
#
# So this test is deliberately END TO END. It drives the surface the way the
# device does -- a setting, a clock, raw MIDI bytes in, USB-MIDI packets out --
# and asserts on the BYTES and on the parameter writes. Nothing here asserts
# that a module exports a function; that is what the component tests already do,
# and it is exactly what could not see this gap.
#
# Five rules, each of which fails silently on hardware:
#
#   1. OFF MEANS NOTHING GOES OUT. Not a probe, not a frame. A surface that
#      talks to a port nobody asked it to talk to is somebody else's MIDI gear
#      receiving 1171-byte SysEx messages.
#   2. A COMPONENT CHANGE REPAINTS. The whole point of the feature: focus moves
#      (from the map, or from Move under Follow Focus) and the screen follows.
#   3. A TURN MOVES A PARAMETER, from raw bytes to set_param. That is four
#      modules and a controller in a row, and the first eleven tasks tested
#      every one of them in isolation while the chain did not exist.
#   4. THE SURFACE HOLDS ITS OWN CONTROLLER. createController has ONE current
#      page, and a bottom-half turn moves it before applying the turn -- so a
#      shared controller drags Move's screen to page N+1 on every lower-row
#      knob. Two small in-range integers, nothing logged.
#   5. A REFUSED SEND IS A RETRY, NOT A SEND. move_midi_external_send returns
#      false when the outbound buffer is full; treating that as sent is a
#      framebuffer the device never receives and nothing that would resend it.
set -euo pipefail
cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

# NO APOSTROPHES inside the node script below: it is a single-quoted bash
# string and one apostrophe ends it early, with an error pointing nowhere near
# the real line.
node --input-type=module -e '
const R = process.cwd();
const { createSurface, KEEPALIVE_MS, LOSS_MS } =
  await import(R + "/src/shared/e16_surface.mjs");
const { createController } = await import(R + "/src/shared/param_pages/page_controller.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } else console.log("ok   " + m); };
const eq = (m, g, w) => ok(JSON.stringify(g) === JSON.stringify(w),
                           m + " (got " + JSON.stringify(g) + " want " + JSON.stringify(w) + ")");

/* ---------------------------------------------------------------------------
 * FIXTURE.
 *
 * SIXTEEN keys, not eight. The page planner chunks by eight, so a fixture with
 * one page cannot tell a bottom-half turn from a top-half one -- and the
 * bottom half is where the shared-controller hazard lives. A one-page fixture
 * would let rule 4 pass with the bug applied, because there would be no page
 * N+1 to drag anybody to.
 * ------------------------------------------------------------------------- */
const KEYS = [];
for (let i = 0; i < 16; i++) KEYS.push("p" + i);
const HIER = { levels: { root: { label: "Mod", knobs: KEYS,
                                 params: KEYS.map((k) => ({ key: k })) } } };
const CP = KEYS.map((k) => ({ key: k, name: k.toUpperCase(), type: "float",
                              min: 0, max: 1, step: 0.01 }));

const CHAIN = { slots: [
  { midiFx: ["Arp"], synth: "Braids", fx: ["Freeverb"] },
  { synth: "Surge", fx: ["CloudSeed"] },
  {}, {},
] };

/* The param channel, faked. Records every write so rule 3 can assert on the
 * KEY that moved rather than on "something was written". */
function mkParams() {
  const store = Object.create(null);
  const writes = [];
  const reads = [];
  store["ui_hierarchy"] = JSON.stringify(HIER);
  store["chain_params"] = JSON.stringify(CP);
  for (const k of KEYS) store[k] = "0.5";
  const bare = (k) => String(k).replace(/^[a-z_0-9]+:/, "");
  return {
    writes, reads,
    get: (slot, key) => { const b = bare(key); reads.push(b);
                          return store[b] === undefined ? null : store[b]; },
    set: (slot, key, val) => { const b = bare(key); store[b] = String(val);
                               writes.push({ slot, key: b, val: String(val) }); return true; },
  };
}

/* A sender that records, and can be told to refuse. Returns the same false
 * move_midi_external_send returns when the outbound buffer is full. */
function mkSend() {
  const log = [];
  const fn = (packets) => { if (fn.refuse) return false; log.push(packets); return true; };
  fn.log = log; fn.refuse = false;
  return fn;
}

/* USB-MIDI packets back into message bytes, so assertions can read the wire as
 * the message it is. Mirrors packetize(): CIN 4 carries three bytes, 5/6/7
 * carry one/two/three and end the message. */
function unpack(packets) {
  const out = [];
  for (let i = 0; i < packets.length; i += 4) {
    const cin = packets[i] & 0x0F;
    const n = cin === 0x05 ? 1 : cin === 0x06 ? 2 : 3;
    for (let b = 0; b < n; b++) out.push(packets[i + 1 + b]);
  }
  return out;
}
const msgId = (packets) => unpack(packets).slice(6, 8);
const j = JSON.stringify;
const ENTER = [0x06, 0x55];
const FRAMEBUFFER = [0x06, 0x02];
const LABELS      = [0x06, 0x03];
/* EITHER mode counts as "the device was painted". The parameter view is LABELS
 * and the map is a FRAMEBUFFER -- they override each other on the device, so a
 * test that names one of them is really asserting which view happened to be up
 * rather than that anything was drawn at all. */
const isScreen = (p) => j(msgId(p)) === j(LABELS) || j(msgId(p)) === j(FRAMEBUFFER);
const RING = [0x06, 0x04];
const EXIT = [0x06, 0x00];
const ACK_BYTES = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x06, 0x53, 0xF7];

/* One rig: a surface with its own controller, plus a SEPARATE controller
 * standing in for the one the shadow UI holds for Move. */
function rig(opts) {
  const o = opts || {};
  const params = mkParams();
  const send = mkSend();
  let t = 0;
  const controllers = [];
  let follow = o.follow || (() => null);

  const surface = createSurface({
    now: () => t,
    send,
    chainOf: () => CHAIN,
    followFocusOf: () => follow(),
    makeController: (focus) => {
      const c = createController({
        getParam: (k) => params.get(focus.slot, k),
        setParam: (k, v) => params.set(focus.slot, k, v),
        now: () => t,
      });
      controllers.push(c);
      return c;
    },
  });

  /* Moves own controller, built exactly as the host builds it. */
  const moveCtl = createController({
    getParam: (k) => params.get(0, k),
    setParam: (k, v) => params.set(0, k, v),
    now: () => t,
  });
  moveCtl.load({ slot: 0, component: "synth", prefix: "synth" });
  for (let i = 0; i < 40; i++) moveCtl.tick();

  return {
    params, send, surface, moveCtl, controllers,
    setFollow: (fn) => { follow = fn; },
    now: () => t,
    /* Records the per-tick send count, so rule 6 can assert the ONE-MESSAGE
     * budget over the whole session rather than at one chosen moment. */
    perTick: [],
    ticks(n, ms) { for (let i = 0; i < (n || 1); i++) {
        t += (ms === undefined ? 25 : ms);
        const before = send.log.length;
        surface.tick();
        this.perTick.push(send.log.length - before); } },
    ack: () => surface.feedMidi(ACK_BYTES),
  };
}

/* ===========================================================================
 * RULE 1 -- with the setting off, NOTHING is sent at all.
 * ========================================================================= */
{
  const r = rig();
  r.ticks(200);
  /* And input arriving anyway must not wake it: the port is shared, so CC 1 on
   * channel 1 from somebody elses gear reaches this code whether or not the
   * surface is on. */
  r.surface.feedMidi([0xB0, 0x01, 0x01]);
  r.surface.feedMidi([0x90, 0x10, 0x7F]);
  r.ticks(20);
  eq("off: nothing on the wire", r.send.log.length, 0);
  eq("off: no parameter written", r.params.writes.length, 0);
}

/* ===========================================================================
 * RULE 2 -- on, acked, and a component change puts a FRAMEBUFFER on the wire.
 *
 * Asserted from the SETTING through to the BYTES. The component change is
 * driven the way the device drives it -- a Shift-held press on a map cell --
 * so the map, the nav, the display pacing and the renderer are all in the path.
 * ========================================================================= */
{
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1);
  eq("on: the first message is ENTER", msgId(r.send.log[0]), ENTER);

  /* NOTHING IS PAINTED AT A DEVICE THAT HAS NOT ANSWERED. A framebuffer sent
   * while seeking goes nowhere, and the device that acks a moment later comes
   * up showing the last process-s screen while the surface believes it has
   * painted. */
  /* Baseline taken here: the rig also builds Move-s controller, which reads
   * its own contract at construction. Counting from zero would be counting
   * somebody else-s IPC. */
  const readsBefore = r.params.reads.length;
  r.ticks(6);
  eq("seeking: no screen before the ACK",
     r.send.log.filter(isScreen).length, 0);
  /* Nor any IPC. The controller-s staggered read is ~2.8 ms -- more than a
   * whole page render -- and it exists only to keep the rings and the screen
   * fresh, so with nothing on the port it is spent on nobody. A surface left
   * switched on with the E16 in a bag must cost a comparison, not a frame. */
  eq("seeking: no parameter reads either", r.params.reads.length - readsBefore, 0);

  r.ack();
  r.ticks(2);
  ok(r.send.log.some(isScreen), "an acked device is painted");
  /* The parameter view is LABELS -- 34 packets against 394 for a framebuffer.
   * The framebuffer was tried for a day on hardware and garbled occasionally
   * with every buffer on our side proven clean; the device own ceiling is
   * four characters a cell either way (so Lua would buy nothing here and cost
   * a script install), and the 16-character title is what carries the focused
   * parameter in full. */
  ok(r.send.log.some((p) => j(msgId(p)) === j(LABELS)),
     "...and the parameter view paints LABELS, an eleventh of a framebuffer");
  ok(!r.send.log.some((p) => j(msgId(p)) === j(FRAMEBUFFER)),
     "...and no framebuffer is sent at all -- the 394-packet message is gone");
  ok(r.params.reads.length > readsBefore,
     "...and only then does it read the contract");

  /* Now navigate: Shift down, press a slot cell, press a component cell. */
  const before = r.send.log.length;
  r.surface.feedMidi([0x90, 0x10, 0x7F]);   /* Shift down (note 16) */
  r.ticks(1);
  r.surface.feedMidi([0x90, 0x01, 0x7F]);   /* push encoder 1 -> slot 2 */
  r.ticks(1);
  r.surface.feedMidi([0x90, 0x04, 0x7F]);   /* push encoder 4 -> its first component */
  r.ticks(3);

  const sent = r.send.log.slice(before).map((p) => j(msgId(p)));
  ok(sent.includes(j(LABELS)),
     "a component change produces a screen on the wire");
  eq("the surface followed the jump",
     [r.surface.slot, r.surface.component], [1, "synth"]);

  /* The whole payload, not just the id. LABELS is 80 raw bytes (a 16-char
   * title plus 16 four-char cells) packed 8-to-7 into 92, plus F0, five header
   * bytes, two id bytes and F7 -- 101 against the 1180 a framebuffer costs,
   * which is the entire reason this path replaced that one. */
  const lb = r.send.log.slice(before).find((p) => j(msgId(p)) === j(LABELS));
  eq("the labels message is a whole screen", unpack(lb).length, 1 + 5 + 2 + 92 + 1);
  ok(unpack(lb).some((b, i) => i > 7 && b !== 0), "the screen is not blank");

  /* RATE DISCIPLINE: never more than one framebuffer in flight. Three
   * invalidations inside one tick is ONE repaint. */
  const b2 = r.send.log.length;
  r.surface.feedMidi([0x90, 0x10, 0x7F]);   /* shift down  -> map */
  r.surface.feedMidi([0x80, 0x10, 0x00]);   /* shift up    -> params */
  r.surface.feedMidi([0x90, 0x10, 0x7F]);   /* shift down  -> map again */
  r.ticks(1);
  const fbs = r.send.log.slice(b2).filter((p) => isScreen(p));
  eq("three invalidations in one tick are one repaint", fbs.length, 1);
}

/* ===========================================================================
 * RULE 3 -- a turn arriving as RAW MIDI BYTES moves a parameter.
 *
 * onMidiMessageExternal to set_param, with nothing simulated in between.
 * ========================================================================= */
{
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1); r.ack(); r.ticks(30);       /* let the contract load and settle */

  const before = r.params.writes.length;
  r.surface.feedMidi([0xB0, 0x01, 0x01]);  /* encoder 0, +1 detent */
  const after = r.params.writes.slice(before);
  ok(after.length > 0, "a raw CC turn reaches set_param");
  eq("it wrote the first key of page 0", after[after.length - 1].key, "p0");

  /* And a VALUE CHANGE SENDS A RING, NEVER A FRAMEBUFFER -- the whole rate
   * strategy. A repaint per detent measures fine in isolation and drops
   * packets in use. */
  const b2 = r.send.log.length;
  r.ticks(1);
  const kinds = r.send.log.slice(b2).map((p) => j(msgId(p)));
  ok(kinds.includes(j(RING)), "the moved encoders ring is sent");
  ok(!kinds.includes(j(FRAMEBUFFER)),
     "a value change does NOT repaint the screen");
}

/* ===========================================================================
 * RULE 4 -- the surface holds its OWN controller.
 *
 * The behavioural half: a BOTTOM-HALF turn must move the surfaces page and
 * leave Moves where it was. With a shared controller both move, which on the
 * device is Moves screen jumping to the next page under every lower-row knob.
 * ========================================================================= */
{
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1); r.ack(); r.ticks(30);

  eq("Move starts on page 0", r.moveCtl.pageIndex, 0);
  const surfaceCtl = r.controllers[0];
  ok(surfaceCtl && surfaceCtl !== r.moveCtl,
     "the surface built a controller of its own");
  eq("two authored pages in the fixture", surfaceCtl.pages.length >= 2, true);

  const before = r.params.writes.length;
  r.surface.feedMidi([0xB0, 0x09, 0x01]);  /* encoder 8 -> bottom half, slot 0 */
  const wrote = r.params.writes.slice(before);
  ok(wrote.length > 0, "a bottom-half turn writes a parameter");
  eq("it wrote page 1s first key, not page 0s", wrote[wrote.length - 1].key, "p8");
  eq("the surfaces controller moved to page 1", surfaceCtl.pageIndex, 1);
  eq("MOVES CONTROLLER DID NOT MOVE", r.moveCtl.pageIndex, 0);
}

/* ===========================================================================
 * RULE 5 -- a refused send is a RETRY, not a send.
 * ========================================================================= */
{
  const r = rig();
  r.surface.setEnabled(true);
  r.send.refuse = true;
  r.ticks(5);
  eq("a refusing port sends nothing", r.send.log.length, 0);
  r.send.refuse = false;
  r.ticks(2);
  ok(r.send.log.length > 0, "the refused ENTER is retried once the port frees up");
  eq("and it is still an ENTER", msgId(r.send.log[0]), ENTER);

  /* The same rule on the display: a refused framebuffer stays owed. */
  const r2 = rig();
  r2.surface.setEnabled(true);
  r2.ticks(1); r2.ack(); r2.ticks(2);
  const b = r2.send.log.length;
  r2.surface.feedMidi([0x90, 0x10, 0x7F]);   /* shift -> map: a repaint is owed */
  r2.send.refuse = true;
  r2.ticks(3);
  eq("a refused repaint sends nothing", r2.send.log.length, b);
  r2.send.refuse = false;
  r2.ticks(1);
  const late = r2.send.log.slice(b).map((p) => j(msgId(p)));
  ok(late.includes(j(LABELS)),
     "the owed repaint goes out when the port frees up");

  /* Disabling gives the device back, exactly once, and stops everything. */
  const b3 = r2.send.log.length;
  r2.surface.setEnabled(false);
  r2.ticks(3);
  const off = r2.send.log.slice(b3).map((p) => j(msgId(p)));
  eq("disable sends EXIT once", off.filter((m) => m === j(EXIT)).length, 1);
  eq("and nothing else after it", off.length, 1);
}

/* ===========================================================================
 * RULE 6 -- ONE MESSAGE PER TICK, ACROSS BOTH PRODUCERS, and a replug repaints.
 *
 * createDisplay enforces its own budget but cannot see the lifecycle, which is
 * a second producer on the same port. A keepalive ENTER in the same tick as a
 * 391-packet framebuffer is the "amid other traffic" case that lost 8 packets.
 *
 * And the E16 has no battery, so unplugging it clears its screen and silently
 * drops it out of remote mode. Nothing else invalidates on the way back -- the
 * focus has not changed and no key was turned -- so without a repaint on the
 * presence edge a replugged device stays blank until the user navigates.
 * ========================================================================= */
{
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(4); r.ack(); r.ticks(60);
  r.surface.feedMidi([0xB0, 0x01, 0x01]);
  r.ticks(60);
  eq("no tick ever sent two messages", Math.max.apply(null, r.perTick), 1);

  /* The case that actually collides: a KEEPALIVE falls due on the same tick as
   * an owed repaint. 25 ms ticks never reach it, so it is driven deliberately
   * -- a budget asserted only over a window where the two producers cannot
   * meet is a budget asserted about nothing. */
  r.surface.feedMidi([0x90, 0x10, 0x7F]);   /* shift -> map: a repaint is owed */
  const b = r.send.log.length;
  /* Past the keepalive but INSIDE the loss window, both read from the module.
   * This was a hard-coded 11000, chosen when the keepalive was 10 s; when the
   * constants were retuned so a replug expires presence (2 s / 6 s), that jump
   * silently became an ABSENCE and the test was measuring the seek path
   * instead of the collision it names. Deriving it keeps the two in step. */
  r.ticks(1, KEEPALIVE_MS + 100);
  eq("a keepalive tick sends the keepalive ALONE", r.send.log.length - b, 1);
  eq("and it is the ENTER", msgId(r.send.log[b]), ENTER);
  r.ticks(1);
  const after = r.send.log.slice(b + 1).map((p) => j(msgId(p)));
  ok(after.includes(j(LABELS)),
     "the deferred repaint goes out on the very next tick");

  /* Silence past LOSS_MS: the device is gone. Then it comes back. */
  r.ticks(3, 30000);
  eq("silence drops the device", r.surface.present, false);
  const before = r.send.log.length;
  r.ack();
  r.ticks(3);
  ok(r.send.log.slice(before).some(isScreen),
     "a device that comes back is repainted, with no other gesture");
}

/* ===========================================================================
 * RULE 7 -- A REPLUG RESTORES THE RINGS, NOT ONLY THE SCREEN.
 *
 * Unplug the cable and the device forgets everything: remote mode, the screen,
 * and all sixteen LED rings. The lifecycle re-enters and the presence edge
 * repaints -- that half worked. The rings did not, and could not: a ring is
 * only ever put on the wire when one CHANGES, which is exactly right in use
 * (one 113-byte chunk per detent instead of a 1171-byte repaint) and leaves
 * nothing owed at this edge, because no value moved while the cable was out.
 *
 * The symptom is a surface that looks recovered -- correct page, correct
 * labels -- with every value invisible until a knob is touched. Measured on
 * hardware 2026-09-10: "it did not fully recover, it did go back into remote
 * mode".
 *
 * Driven through presence EXPIRY rather than by calling a seam, so the test
 * exercises the same path the cable does.
 * ========================================================================= */
{
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1);
  r.ack();
  /* Settle: the entry repaint and anything it owes go out. */
  r.ticks(60);
  const settled = r.send.log.length;
  ok(r.surface.present, "replug: present after the ACK");

  /* The cable comes out. Nothing acks, so presence expires after LOSS_MS. */
  r.ticks(40, 1000);
  ok(!r.surface.present, "replug: presence expires with no ACK");

  /* Back in: the device acks the next probe. */
  const before = r.send.log.length;
  r.ack();
  r.ticks(40);
  ok(r.surface.present, "replug: present again after the ACK");

  const after = r.send.log.slice(before).map(msgId).map(j);
  ok(r.send.log.slice(before).some(isScreen), "replug: the screen is repainted");
  ok(after.includes(j(RING)),
     "replug: the rings are restated -- without this the panel recovers its "
     + "screen and shows no values until a knob is turned");
  ok(r.send.log.length > settled, "replug: recovery actually sent something");
}

if (fails) { console.log("FAILED " + fails); process.exit(1); }
console.log("PASS: the surface runs end to end");
'

# ---------------------------------------------------------------------------
# THE HOST SEAM. The behavioural half above proves the surface works when
# something builds it; these pin that shadow_ui.js is that something. They are
# source pins because shadow_ui.js cannot be imported under node -- it opens
# with `import * as os from "os"` and every path in it is an on-device absolute
# -- so this is the one part of the wire that has to be read rather than run.
# ---------------------------------------------------------------------------
fail=0
note() { echo "FAIL: $1"; fail=1; }
UI=src/shadow/shadow_ui.js

grep -q "createSurface" "$UI" || note "shadow_ui.js never imports createSurface"

# CONSTRUCTED, not merely imported. `e16Nav = null` with createNav never called
# is the exact state this whole task exists to end.
grep -qE "^const e16Surface = createE16Surface\(\{" "$UI" \
  || note "the surface is never constructed"
grep -q "e16Nav = null" "$UI" && note "e16Nav is still the null seam"

# Driven from the tick and from the external MIDI path. A constructed surface
# nobody calls is the same gap one layer up.
grep -q "e16Surface.tick()" "$UI" || note "the surface is never ticked"
grep -q "e16Surface.feedMidi(data)" "$UI" || note "external MIDI never reaches the surface"

# ITS OWN CONTROLLER. The io must CONSTRUCT one inside makeController; handing
# over the grid's existing controller is the shared-page bug, and it cannot be
# seen from the surface's side -- both look like "a controller" from there.
#
# Anchored on the makeController line itself rather than searching the file for
# a createController call, because there is more than one controller in this
# tree and a loose grep would be satisfied by somebody else's.
grep -qE "^    makeController: \(focus\) => createPageController\(\{" "$UI" \
  || note "makeController does not construct a controller of its own"

# The follow source and the chain shape are the surfaces two inputs from the
# host. Without either it runs and shows the wrong thing, which is worse than
# not running.
grep -q "followFocusOf: e16FollowFocus" "$UI" || note "the follow source is not wired"
grep -q "chainOf: e16ChainShape" "$UI" || note "the chain shape is not wired"

if [ "$fail" -eq 0 ]; then
  echo "PASS: shadow_ui.js constructs, feeds and ticks the surface"
else
  exit 1
fi
