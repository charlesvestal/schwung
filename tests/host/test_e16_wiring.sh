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
const { createSurface, KEEPALIVE_MS, LOSS_MS, TICK_PACKET_BUDGET, RING_RESTATE_MS, PARTIAL_HEARTBEAT_MS } =
  await import(R + "/src/shared/e16_surface.mjs");
const { createController } = await import(R + "/src/shared/param_pages/page_controller.mjs");
/* LABELS is no longer the default view, so its payload shape is pinned by
 * building one directly rather than fishing it out of the wire log. */
const { packetize, labelsMsg, pack7, unpack7 } = await import(R + "/src/shared/e16_protocol.mjs");

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
/* The drawn view goes out as RECTANGLE regions (0x08, one id byte) -- a full
 * repaint is eight 128x8 bands -- never as a whole FRAMEBUFFER any more. */
const isRegion = (p) => unpack(p)[6] === 0x08 || unpack(p)[6] === 0x05;
const isScreen = (p) => j(msgId(p)) === j(LABELS) || isRegion(p);
const RING = [0x06, 0x04];
const EXIT = [0x06, 0x00];
const ACK_BYTES = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7]  /* the ENTER ack, as captured 2026-09-24 */;

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
    /* PACKETS per tick: the budget is in packets now, since small region
     * messages share a tick up to TICK_PACKET_BUDGET. */
    perTickPackets: [],
    /* THE FAKE DEVICE ACKS every region it receives, echoing the address as
     * the real E16 does (captured 2026-09-24). Without it every region would
     * time out as lost and be re-sent forever -- which is exactly what the
     * surface SHOULD do with a device that never answers. */
    autoAck: true,
    ticks(n, ms) { for (let i = 0; i < (n || 1); i++) {
        t += (ms === undefined ? 25 : ms);
        const before = send.log.length;
        surface.tick();
        if (this.autoAck) for (const p of send.log.slice(before)) {
          const u = unpack(p), id = u[6];
          if (id !== 0x08 && id !== 0x05 && id !== 0x07) continue;
          let addr = [0x7F, 0x7F, 0x7F, 0x7F];
          if (id === 0x08) addr = unpack7(u.slice(7, u.length - 1), 4);
          if (id === 0x05) addr = [unpack7(u.slice(7, u.length - 1), 1)[0], 0xFF, 0xFF, 0xFF];
          if (id === 0x07) addr = [0xFF, 0xFF, 0xFF, 0xFF];
          /* PACKETISED AS THE E16 DOES (captured 2026-09-24): three-byte
           * continuations, then a TWO-byte CIN 6 continuation carrying the
           * last two payload bytes WITHOUT F7, then F7 alone -- each handed to
           * JS as its real bytes only. Feeding one whole array hid that the
           * shim dropped that CIN 6 packet, so no reply ever parsed. */
          const msg = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53]
            .concat(pack7([id, 0].concat(addr)));          /* 15 bytes, no F7 */
          for (let q = 0; q + 3 <= 12; q += 3) surface.feedMidi(msg.slice(q, q + 3));
          surface.feedMidi(msg.slice(12, 14));             /* CIN 6: 2 data bytes */
          surface.feedMidi([0xF7]);                        /* CIN 5: F7 alone */
        }
        this.perTick.push(send.log.length - before);
        this.perTickPackets.push(send.log.slice(before)
          .reduce((a, p) => a + p.length / 4, 0)); } },
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
  /*
   * THE PARAMETER VIEW IS THE FRAMEBUFFER AGAIN -- and this pin was the
   * opposite a day ago, so the reversal is worth stating rather than quietly
   * editing.
   *
   * LABELS was adopted as a reliability stopgap: 34 packets against 394, so it
   * garbles far less often. It buys that by giving up the drawn panel and
   * capping every cell at FOUR characters, which is the device own limit and
   * not something we can spend our way out of.
   *
   * The trade turned out not to be ours to make. Neither form is CORRECT --
   * the corruption is Move splicing its own notes into our SysEx on the one
   * cable its XMOS will carry SysEx over (2026-09-11), and quiet-start, retry
   * and restate-suppression were each built and measured and none of them fix
   * it. So the choice is a good screen that sometimes breaks against a poor
   * one that breaks less often, and the user picked the screen.
   *
   * `screenModeOf` still returns "labels" on request, so the fallback is one
   * echo away and this asserts the DEFAULT, not the only possibility.
   */
  ok(r.send.log.some(isRegion),
     "...and the parameter view paints the drawn view (RECTANGLE regions) by default");
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

  ok(r.send.log.slice(before).some(isRegion),
     "a component change produces a screen on the wire");
  eq("the surface followed the jump",
     [r.surface.slot, r.surface.component], [1, "synth"]);

  /* The whole payload, not just the id. Built directly rather than captured
   * from the wire: the default view is the framebuffer now, so a LABELS
   * message no longer appears in the log -- but the encoding is still a live
   * path behind `screenModeOf`, and its shape is worth pinning either way.
   *
   * LABELS is 80 raw bytes (a 16-char title plus 16 four-char cells) packed
   * 8-to-7 into 92, plus F0, five header bytes, two id bytes and F7 -- 101
   * against the 1180 a framebuffer costs. */
  const lb = packetize(labelsMsg("0123456789abcdef", Array(16).fill("wxyz")));
  eq("the labels message is a whole screen", unpack(lb).length, 1 + 5 + 2 + 92 + 1);
  ok(unpack(lb).some((b, i) => i > 7 && b !== 0), "the screen is not blank");

  /* RATE DISCIPLINE: never more than one framebuffer in flight. Three
   * invalidations inside one tick is ONE repaint. */
  r.ticks(40);                              /* let any repaint in flight finish */
  const p0 = r.surface.display.paintsCompleted;
  r.surface.feedMidi([0x90, 0x10, 0x7F]);   /* shift down  -> map */
  r.surface.feedMidi([0x80, 0x10, 0x00]);   /* shift up    -> params */
  r.surface.feedMidi([0x90, 0x10, 0x7F]);   /* shift down  -> map again */
  r.ticks(30);
  eq("three invalidations in one tick are one repaint",
     r.surface.display.paintsCompleted - p0, 1);
}

/* ===========================================================================
 * REGION FIRMWARE: THE READING FOLLOWS A LONG SPIN, AND A VALUE CHANGED ON
 * MOVE REACHES THE E16. Both reported on hardware 2026-09-24. The settle
 * redrew the digits only once the hand stopped, so a continuous spin showed
 * nothing; and nothing repainted for a change that did not come from the
 * E16 own encoders -- the old 1.5 s whole-screen heartbeat had been hiding
 * that. Neither may break the one-message-per-tick budget.
 * ========================================================================= */
{
  const NEW_ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7];
  const isRect = (p) => unpack(p)[6] === 0x08;

  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1); r.surface.feedMidi(NEW_ACK); r.ticks(60);   /* bands drawn, contract loaded */

  /* A continuous spin: one detent every tick, 40 ticks, the hand never
   * still for SETTLE_MS. */
  const b0 = r.send.log.length;
  for (let i = 0; i < 40; i++) { r.surface.feedMidi([0xB0, 0x01, 0x01]); r.ticks(1, 16); }
  const during = r.send.log.slice(b0);
  ok(during.some(isRect), "a long spin redraws the reading WHILE turning, not only after");
  ok(during.some((p) => j(msgId(p)) === j(RING)), "...and the ring still moves during the spin");

  /* A value changed from Move -- nothing touches the E16. */
  r.ticks(40);                                            /* let the spin settle */
  const b1 = r.send.log.length;
  r.params.set(0, "p0", "0.95");
  r.ticks(80);
  const after = r.send.log.slice(b1);
  ok(after.some(isRect), "a value changed on Move repaints the E16 region");
  ok(after.some((p) => j(msgId(p)) === j(RING)), "...and moves that encoders ring");

  /* An unchanged screen costs nothing on the wire: the look diffs to empty. */
  r.ticks(40);
  const b2 = r.send.log.length;
  r.ticks(80);
  const idle = r.send.log.slice(b2).filter((p) => j(msgId(p)) !== j(ENTER) && j(msgId(p)) !== j(RING));
  eq("an idle, unchanged screen sends no screen traffic", idle.length, 0);

  /* RING KEEPALIVE: rings get no ACK, so a lost ring message -- or LED state
   * the device drops -- stayed wrong until that knob moved (hardware,
   * 2026-09-24: every ring blank, screen fine). An idle surface restates all
   * of them on RING_RESTATE_MS. */
  const b3 = r.send.log.length;
  /* A real device answers every keepalive; without that the surface would
   * rightly decide it was gone after LOSS_MS and stop sending. */
  for (let i = 0; i < Math.ceil((RING_RESTATE_MS * 2 + 200) / 25); i++) {
    if (i % 40 === 0) r.surface.feedMidi(NEW_ACK);
    r.ticks(1);
  }
  const restates = r.send.log.slice(b3).filter((p) => j(msgId(p)) === j(RING));
  ok(restates.length >= 2, "an idle surface restates its rings on the keepalive");
  eq("...every ring message fits one SPI frame (<= 12 packets)",
     restates.every((p) => p.length / 4 <= 12), true);

  /* Within the budget, or a single message -- one message always goes, so
   * the all-rings restate (~44 packets) can be a tick on its own. */
  ok(r.perTickPackets.every((n, i) => n <= TICK_PACKET_BUDGET || r.perTick[i] === 1),
     "never more than the packet budget in a tick");
}

/* ===========================================================================
 * A VALUE WRITTEN ON MOVE REACHES THE E16 AT ONCE, AND THE HEARTBEAT NEVER
 * BLANKS. Hardware 2026-09-24: Move-side knob changes took up to ~0.5 s (only
 * the LOOK_MS pass noticed them), and the heartbeat forgot the screen and
 * repainted it through a CLEAR -- a visible blank every 10 s.
 * ========================================================================= */
{
  const NEW_ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7];
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1); r.surface.feedMidi(NEW_ACK);
  /* Wait for QUIET: values load, the first paint drains. Measuring before that
   * would credit the notice with traffic that was coming anyway. */
  let quiet = 0;
  for (let i = 0; i < 800 && quiet < 12; i++) {
    if (i % 40 === 0) r.surface.feedMidi(NEW_ACK);
    const b = r.send.log.length; r.ticks(1);
    const busy = r.send.log.slice(b).some((p) => j(msgId(p)) !== j(ENTER));
    quiet = busy ? 0 : quiet + 1;
  }
  ok(quiet >= 12, "the surface reaches quiet before the measurement");

  /* Moved on Move: ONLY the write notice -- the fake store is left alone, so
   * the fallback (a LOOK_MS pass plus the staggered re-read) has nothing to
   * find, and whatever reaches the wire came through the notice. */
  const b0 = r.send.log.length;
  r.surface.noteParamWrite(0, "synth:p0", "0.95");
  r.ticks(3);                                   /* 75 ms -- well under LOOK_MS */
  const fast = r.send.log.slice(b0);
  ok(fast.some((p) => j(msgId(p)) === j(RING)), "a Move-side write moves the E16 ring within a few ticks");
  ok(fast.some((p) => unpack(p)[6] === 0x08), "...and redraws its digits within a few ticks");

  /* Heartbeat: long idle, device answering keepalives -- no CLEAR, ever. */
  const b1 = r.send.log.length;
  for (let i = 0; i < Math.ceil((PARTIAL_HEARTBEAT_MS + 2000) / 25); i++) {
    if (i % 40 === 0) r.surface.feedMidi(NEW_ACK);
    r.ticks(1);
  }
  const hb = r.send.log.slice(b1);
  ok(hb.some((p) => unpack(p)[6] === 0x08), "the heartbeat restates the screen");
  ok(!hb.some((p) => unpack(p)[6] === 0x07), "...in place, with NO CLEAR");
}

/* ===========================================================================
 * NOTHING BLANKS THE SCREEN BUT A SCREEN WE REALLY LOST. Hardware 2026-09-24:
 * periodic blanking while a knob turned. A NACK whose command byte was lost
 * forced a full CLEAR repaint; and a corrupted message the E16 read as CLEAR
 * blanked it with nothing on our side noticing.
 * ========================================================================= */
{
  const NEW_ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7];
  const reply = (id, raw) => [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, id].concat(pack7(raw), [0xF7]);
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1); r.surface.feedMidi(NEW_ACK);
  let quiet = 0;
  for (let i = 0; i < 800 && quiet < 12; i++) {
    if (i % 40 === 0) r.surface.feedMidi(NEW_ACK);
    const b = r.send.log.length; r.ticks(1);
    quiet = r.send.log.slice(b).some((p) => j(msgId(p)) !== j(ENTER)) ? 0 : quiet + 1;
  }

  /* A NACK that cannot name its command. */
  const b0 = r.send.log.length;
  r.surface.feedMidi(reply(0x54, [0x7F, 0x06, 0xFF, 0xFF, 0xFF, 0xFF]));
  r.ticks(20);
  ok(!r.send.log.slice(b0).some((p) => unpack(p)[6] === 0x07),
     "a NACK that names no region does NOT blank the screen");

  /* An INTERRUPTED strip: the device never read the address, so it comes
   * back FF -- captured on hardware 2026-09-24 right before each blank. */
  const b2 = r.send.log.length;
  r.surface.feedMidi(reply(0x54, [0x08, 0x06, 0x00, 0xFF, 0xFF, 0xFF]));
  r.ticks(20);
  ok(!r.send.log.slice(b2).some((p) => unpack(p)[6] === 0x07),
     "an interrupted strip whose NACK carries no address does NOT blank the screen");

  /* The device ACKs a CLEAR we never sent: it blanked itself. */
  const b1 = r.send.log.length;
  r.surface.feedMidi(reply(0x53, [0x07, 0x00, 0xFF, 0xFF, 0xFF, 0xFF]));
  r.ticks(40);
  const after = r.send.log.slice(b1);
  ok(after.some((p) => unpack(p)[6] === 0x07) && after.some((p) => unpack(p)[6] === 0x08),
     "an unsolicited CLEAR ack is repainted at once, not at the next heartbeat");
}

/* ===========================================================================
 * EVERY MESSAGE FITS ONE SPI FRAME. The outbound queue places a message of at
 * most 12 packets WHOLE in one frame, after the cable-2 packets of Move, so
 * notes from Move can never be spliced into it -- the splice behind every garble
 * on this cable. A single bigger message would reopen that, silently. So a
 * busy session -- connect, paint, spin, map up and down, slot jumps -- must
 * never send one.
 * ========================================================================= */
{
  const NEW_ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7];
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1); r.surface.feedMidi(NEW_ACK); r.ticks(40);
  for (let i = 0; i < 30; i++) { r.surface.feedMidi([0xB0, 1 + (i % 16), i % 2 ? 0x01 : 0x7F]); r.ticks(1); }
  for (let k = 0; k < 4; k++) {
    r.surface.feedMidi([0x90, 0x10, 0x7F]); r.ticks(6);        /* Shift: map up */
    r.surface.feedMidi([0x90, k % 4, 0x7F]); r.ticks(6);       /* jump slot */
    r.surface.feedMidi([0x80, 0x10, 0x00]); r.ticks(10);       /* map down */
    if (k % 2) r.surface.feedMidi(NEW_ACK);
  }
  r.ticks(80);
  const big = r.send.log.filter((p) => p.length / 4 > 12);
  ok(r.send.log.length > 50, "the session sent real traffic (positive control)");
  eq("no message larger than one SPI frame (12 packets) was ever sent",
     big.map((p) => p.length / 4), []);
}

/* ===========================================================================
 * OLED UPDATE NACK -- built against a draft spec, not yet on hardware. A
 * NACK must invalidate the surfaces belief about whats on screen (so the
 * NEXT repaint is a full one) without touching anything else -- not the
 * lifecycles presence tracking, not a resend, no new timer.
 * ========================================================================= */
{
  const r = rig();
  r.surface.setEnabled(true);
  r.ticks(1);
  r.ack();
  r.ticks(2);
  /* Not asserting on the NACK path in detail here -- Tasks 1 and 4 already
   * unit-test parseOledUpdateReply and display.invalidateBuf/tick in
   * isolation. This just confirms the wiring does not throw when a NACK
   * body is fed through the real feedMidi-shaped path, since that seam
   * (asm.feed -> onMessage -> parseOledUpdateReply -> display.invalidateBuf)
   * is exactly what the "tasks pass their own tests, the SEAM between files
   * is what breaks" lesson (this files own header comment) is about. */
  const { pack7 } = await import(R + "/src/shared/e16_protocol.mjs");
  const nackRaw = [0x06, 0x02, 0xFF, 0xFF, 0xFF, 0xFF];   /* invalid bounds */
  const nackBody = [0x00,0x21,0x5B,0x02,0x01,0x54].concat(pack7(nackRaw));
  const nackBytes = [0xF0].concat(nackBody, [0xF7]);
  let threw = false;
  try {
    for (const b of nackBytes) r.surface.feedMidi([b]);
  } catch (e) { threw = true; console.log("  threw: " + e.message); }
  ok(!threw, "feeding an OLED NACK through the surface does not throw");

  /* And it must not be mistaken for the REMOTE MODE ENTERED ACK -- the
   * device must still be considered present (that ACK path is untouched). */
  ok(r.surface.present !== false, "a NACK does not knock the device out of present");

  /* THE ACTUAL RECOVERY: a NACK with no other change happening must still
   * force a screen resend on the VERY NEXT tick -- not "eventually, once
   * something else invalidates, or the heartbeat gets around to it up to
   * SCREEN_HEARTBEAT_MS later" (and the heartbeat is itself gated off while
   * Move is transmitting, which is exactly when a NACK is most likely).
   * invalidateBuf() alone only clears what the surface BELIEVES is shown;
   * without a paired invalidate() nothing is ever marked OWED, and this is
   * the specific bug the holistic review found: the NACK handler had the
   * first call but not the second. */
  const beforeRecovery = r.send.log.length;
  r.ticks(1);
  const recovered = r.send.log.slice(beforeRecovery).some(isScreen);
  ok(recovered, "a NACK forces a screen resend on the very next tick");

  /* An ACK (reply.ok) is a no-op: feeding one must not throw either, and
   * nothing about presence tracking should react to it beyond the normal
   * ACK handling already covered above. */
  const ackRaw = [0x06, 0x02, 0x00, 0x00, 0x00, 0x00];
  const ackBody = [0x00,0x21,0x5B,0x02,0x01,0x53].concat(pack7(ackRaw));
  const ackBytes = [0xF0].concat(ackBody, [0xF7]);
  let threw2 = false;
  try {
    for (const b of ackBytes) r.surface.feedMidi([b]);
  } catch (e) { threw2 = true; console.log("  threw: " + e.message); }
  ok(!threw2, "feeding an OLED UPDATE ACK through the surface does not throw");
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

  /* And a VALUE CHANGE SENDS A RING, NEVER A WHOLE FRAMEBUFFER -- the rate
   * strategy. The printed number now follows as a small region repaint too
   * (it goes out a tick ahead of the ring, since a screen owed outranks it),
   * so the ring is asserted within a few ticks rather than on the first. */
  const b2 = r.send.log.length;
  r.ticks(4);
  const kinds = r.send.log.slice(b2).map((p) => j(msgId(p)));
  ok(kinds.includes(j(RING)), "the moved encoders ring is sent");
  ok(!kinds.includes(j(FRAMEBUFFER)),
     "a value change never sends a whole FRAMEBUFFER");
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
  ok(r2.send.log.slice(b).some(isRegion),
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
 * RULE 6 -- A PACKET BUDGET PER TICK, ONE PRODUCER PER TICK, and a replug
 * repaints. (It was "one message per tick" while a repaint was one 391-packet
 * framebuffer; regions are small, so several share a tick up to
 * TICK_PACKET_BUDGET -- but the keepalive still never shares one.)
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
  /* Within the budget, or a single message -- one message always goes, so
   * the all-rings restate (~44 packets) can be a tick on its own. */
  ok(r.perTickPackets.every((n, i) => n <= TICK_PACKET_BUDGET || r.perTick[i] === 1),
     "no tick ever exceeded the packet budget");

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
  ok(r.send.log.slice(b + 1).some(isRegion),
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
