#!/usr/bin/env bash
# The E16 parameters view: two AUTHORED pages under sixteen encoders.
#
# Four rules, none of which fail loudly on hardware if they are broken:
#
#   1. ENCODER -> CELL is the same fact as the drawn layout. A surface where
#      knob 5 edits the cell knob 6 is drawn in is two small in-range integers
#      disagreeing, with nothing logged. Pinned by comparing the mapping
#      against cellRect() geometry rather than by restating a table.
#   2. A VALUE CHANGE SENDS A RING, NOT A FRAMEBUFFER. This is the whole rate
#      strategy (docs/plans/2026-09-10-e16-control-surface-design.md): 31
#      packets alone arrived byte-perfect, 34 amid other traffic lost 8. A
#      repaint per detent measures fine in isolation and drops packets in use.
#   3. NEVER MORE THAN ONE FRAMEBUFFER IN FLIGHT, and a refused send stays owed
#      rather than queueing a second copy.
#   4. FEWER THAN NINE CELLS LEAVES THE BOTTOM HALF DARK. Filling it from the
#      next page would break the authored-grouping rule the whole design rests
#      on -- and it is asserted in PIXELS, because cell bookkeeping can agree
#      with itself while the renderer draws anyway.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { buildView, renderView, ringsFor, ringFor, applyTurn, cellRect,
         encHalf, encSlot, ENCODERS, HALF_H, RING_MAX }
    from "./src/shared/e16_view.mjs";
import { createDisplay, SCREEN_HEARTBEAT_MS, STRIP_H, TICK_PACKET_BUDGET, ACK_TIMEOUT_MS,
         WINDOW_PX_START, WINDOW_PX_MIN, REORDER_LOSS, rectPackets, ATOMIC_MAX_PACKETS } from "./src/shared/e16_surface.mjs";
import { unpack7 } from "./src/shared/e16_protocol.mjs";
const STRIPS = 64 / STRIP_H;   /* strips in a fully inked screen */
/* Messages in a FULL repaint of buf: one CLEAR, then one strip per STRIP_H
 * rows holding any ink (blank strips are not sent). */
const fullMsgs = (buf) => { let n = 1;
  for (let y = 0; y < 64; y += STRIP_H) { let ink = false;
    for (let yy = y; yy < y + STRIP_H && !ink; yy++)
      for (let x = 0; x < 128 && !ink; x++) if (buf[(yy >> 3) * 128 + x] & (1 << (yy & 7))) ink = true;
    if (ink) n++; }
  return n; };
import { createCanvas } from "./src/shared/e16_canvas.mjs";
import { KNOBS_PER_PAGE, PAGE_KNOBS } from "./src/shared/param_pages/page_plan.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };

/* ---- fixtures: what planPages() emits, trimmed to what this view reads ---- */
const page = (name, keys) => ({ kind: PAGE_KNOBS, name, level: name, keys });
const twoPages = [
  page("Osc", ["osc_wave", "osc_tune", "osc_pw", "osc_sub",
               "osc_mix", "osc_noise", "osc_drift", "osc_sync"]),
  page("Filter", ["flt_cut", "flt_res", "flt_env", "flt_key",
                  "flt_mode", "flt_drive"]),
];
const sixKeyFirst = [
  page("Amp", ["a", "b", "c", "d", "e", "f"]),
  page("Filter", ["flt_cut", "flt_res", "f3", "f4", "f5", "f6", "f7", "f8"]),
];
const onePage = [ page("Only", ["x", "y", "z"]) ];

const META = {
  osc_tune: { label: "Tune", min: -24, max: 24 },
  flt_cut:  { label: "Cutoff", min: 0, max: 127 },
  flt_res:  { label: "Res", min: 0, max: 127, readOnly: true },
};
const metaOf = (k) => META[k] || { label: k, min: 0, max: 1 };
const VALUES = { osc_tune: 0, flt_cut: 127, flt_res: 64 };
const valueOf = (k) => VALUES[k];

/* ---- 1. encoder -> cell mapping, and it MATCHES THE DRAWN LAYOUT ---- */
const v = buildView(twoPages, 0, { metaOf, valueOf });

eq("sixteen cells always", v.cells.length, ENCODERS);
eq("top half is page N",
   v.cells.slice(0, 8).map(c => c && c.key), twoPages[0].keys);
eq("bottom half is page N+1",
   v.cells.slice(8, 14).map(c => c && c.key), twoPages[1].keys);
eq("headers name both pages", v.headers.map(h => h && h.name), ["Osc", "Filter"]);
eq("bottom header carries its own index", v.headers[1].index, 1);

/* Halves and slots derive from ONE definition; assert the rects agree with it,
 * so a change to either side breaks here rather than on the device. */
let geomBad = [];
for (let e = 0; e < ENCODERS; e++) {
  const r = cellRect(e);
  const halfFromRect = r.y >= HALF_H ? 1 : 0;
  if (halfFromRect !== encHalf(e)) geomBad.push("half:" + e);
  /* row-major within a half: enc 0-3 above enc 4-7 */
  if (encSlot(e) < 4 && r.y >= cellRect(e - encSlot(e) + 4).y) geomBad.push("row:" + e);
}
eq("rects agree with the half/slot mapping", geomBad, []);
eq("enc 8 is the bottom page slot 0", [encHalf(8), encSlot(8)], [1, 0]);
eq("enc 15 is the bottom page slot 7", [encHalf(15), encSlot(15)], [1, 7]);

/* buildView places a key by its own arithmetic; assert it lands where the
 * EXPORTED mapping says it should, or the two can drift apart silently and the
 * only symptom is a knob editing its neighbour. */
let mapBad = [];
for (let e = 0; e < ENCODERS; e++) {
  const src = twoPages[encHalf(e)];
  const want = (src && src.keys[encSlot(e)]) || null;
  const got = (v.cells[e] && v.cells[e].key) || null;
  if (got !== want) mapBad.push(e + ":" + got + "!=" + want);
}
eq("cells land where encHalf/encSlot say", mapBad, []);

/* ---- 2. fewer than nine cells leaves the bottom half DARK ---- */
const small = buildView(onePage, 0, { metaOf, valueOf });
eq("single-page: bottom cells all null",
   small.cells.slice(8).filter(Boolean).length, 0);
eq("single-page: no bottom header", small.headers[1], null);

/* And a SIX-key top page must not pull the next page forward into 6 and 7. */
const six = buildView(sixKeyFirst, 0, { metaOf, valueOf });
eq("six-key page leaves 6 and 7 empty",
   [six.cells[6], six.cells[7]], [null, null]);
eq("next page still starts at encoder 8", six.cells[8].key, "flt_cut");

/* The pixel form of the same rule: the lower 512 bytes of an SSD1306 buffer
 * are y 32..63, so a dark bottom half is literally 512 zero bytes. */
const cv = createCanvas();
renderView(cv, small);
const buf = cv.toBuffer();
let topInk = 0, bottomInk = 0;
for (let i = 0; i < 512; i++) if (buf[i]) topInk++;
for (let i = 512; i < 1024; i++) if (buf[i]) bottomInk++;
eq("top half is drawn", topInk > 0, true);
eq("bottom half is DARK", bottomInk, 0);

/* A pair draws into both halves -- otherwise the assertion above passes for a
 * renderer that draws nothing at all. */
const cv2 = createCanvas();
renderView(cv2, v);
let bottomInk2 = 0;
for (let i = 512; i < 1024; i++) if (cv2.toBuffer()[i]) bottomInk2++;
eq("a page pair inks the bottom half", bottomInk2 > 0, true);

/* ---- 3. rings ---- */
eq("ring count is occupied cells", ringsFor(v).length, 14);
eq("bipolar read from a negative min", ringFor(v, 1).bipolar, true);
eq("unipolar otherwise", ringFor(v, 8).bipolar, false);
eq("centre of a bipolar range", ringFor(v, 1).amount, Math.round(RING_MAX / 2));
eq("top of a unipolar range", ringFor(v, 8).amount, RING_MAX);
eq("read-only ring is dimmer",
   ringFor(v, 9).g < ringFor(v, 8).g, true);
eq("empty cell has no ring", ringFor(v, 15), null);

/* ---- 4. a turn goes through the GRID s knob-turn path ---- */
const calls = [];
const fakeCtl = {
  pageIndex: 0,
  goToPage(i, o) { calls.push(["goToPage", i, o && o.remember]); this.pageIndex = i; },
  onKnobTurn(slot, dir, now, o) { calls.push(["onKnobTurn", slot, dir, !!(o && o.fine)]); },
};

calls.length = 0;
eq("top-half turn returns what moved",
   applyTurn(v, fakeCtl, 2, 1, 1000), { key: "osc_pw", enc: 2, ticks: 1 });
eq("top-half turn needs no page move",
   calls, [["onKnobTurn", 2, 1, false]]);

calls.length = 0;
applyTurn(v, fakeCtl, 9, -3, 1000);
eq("bottom-half turn moves the controller to page N+1 FIRST",
   calls[0], ["goToPage", 1, false]);
eq("...then turns SLOT 1 of that page, three detents",
   calls.slice(1),
   [["onKnobTurn", 1, -1, false], ["onKnobTurn", 1, -1, false],
    ["onKnobTurn", 1, -1, false]]);

calls.length = 0;
eq("an empty encoder does nothing", applyTurn(v, fakeCtl, 15, 1, 1000), null);
eq("...and calls nothing", calls, []);

/* A read-only cell is REPORTED, and still routed: the refusal is the
 * controller s (isTurnable), and duplicating it here would be the second
 * value-application path this whole module exists not to have. */
eq("read-only is reported for drawing", v.cells[9].readOnly, true);
calls.length = 0;
applyTurn(v, fakeCtl, 9, 1, 1000);
eq("read-only still goes through onKnobTurn",
   calls.filter(c => c[0] === "onKnobTurn").length, 1);

/* ---- 5. send discipline ---- */
const unpack = (p) => { const out = [];
  for (let i = 0; i < p.length; i += 4) {
    const cin = p[i];
    const n = cin === 0x04 ? 3 : (cin - 0x04);
    for (let k = 0; k < n; k++) out.push(p[i + 1 + k]);
  }
  return out; };
/* THE DEVICE ANSWERS: ACK (or NACK) every OLED update in send.log not yet
 * answered, as the E16 does. The display holds the next region back while
 * too many pixels are unanswered (WINDOW_PX_START), so a harness that never
 * answers stalls it after the first few rows -- which is the point. */
const answer = (d, send, ok = true) => {
  const from = send.answered || 0;
  for (let i = from; i < send.log.length; i++) {
    const b = unpack(send.log[i]);
    if (b[6] === 0x07) d.acked({ ok, cmd: 0x07, addr: { x: null, y: null, w: null, h: null } });
    else if (b[6] === 0x08) {
      const [x, y, w, h] = unpack7(b.slice(7, b.length - 1), 4);
      d.acked({ ok, cmd: 0x08, addr: { x, y, w, h } });
    }
  }
  send.answered = send.log.length; };
const kindOf = (packets) => { const b = unpack(packets);
  if (b[0] !== 0xF0) return "?";
  if (b[6] === 0x08) return "rect";
  if (b[6] === 0x07) return "clear";
  if (b[6] === 0x05) return "scanline";
  const id = (b[6] << 8) | b[7];
  return id === 0x0602 ? "framebuffer" : id === 0x0604 ? "ring"
       : id === 0x0603 ? "labels" : "other"; };
/* Tick until the repaint in flight has fully drained; returns what each tick
 * sent. A full repaint is eight 128x8 bands, a small one a single region. */
const drain = (d, send, screen, t) => { const out = [];
  for (let i = 0; i < 40; i++) { out.push(d.tick(send, frame, screen, t));
    if (!d.repaintPending) break; }
  return out; };

const mkSend = (accept) => { const log = [];
  const fn = (p) => { log.push(p); return accept === undefined ? true : accept(); };
  fn.log = log; return fn; };

/* A mutable reference, not a bare `() => cv2.toBuffer()`, ONLY so the
 * "navigation" block below can make `frame()` return genuinely different
 * pixels between the priming send and the next one (a real navigation
 * always redraws different content; this fixture otherwise never does).
 * Every other call site below still sees the SAME bytes cv2 always held --
 * this starts out pointing at exactly what `cv2.toBuffer()` returns and is
 * never reassigned except by that one block. */
let frameBuf = cv2.toBuffer();
const frame = () => frameBuf;

/*
 * THE DISPLAY IS A MODE MACHINE NOW, so every one of these starts by putting
 * the device INTO the mode under test.
 *
 * FRAMEBUFFER and LABELS override each other on the device, so the display
 * sends a screen whenever what the caller wants differs from what the device
 * was last told -- including the very first tick, when it has been told
 * nothing. A test that skipped that priming was really measuring the initial
 * paint and calling it a value change.
 */
const PICTURE = { kind: "framebuffer" };
const TEXT = { kind: "labels", title: "T", labels: new Array(16).fill("AB") };
/* A repaint is up to EIGHT region messages now (a full one goes out as
 * 128x8 bands), so priming drains the whole repaint, not one tick. */
const prime = (d, send, screen) => {
  for (let i = 0; i < 40 && (i === 0 || d.repaintPending); i++) d.tick(send, frame, screen);
  send.log.length = 0; };

/* A value change is ONE ring message and NO screen. */
let d = createDisplay();
let send = mkSend();
prime(d, send, PICTURE);
d.ringChanged(ringFor(v, 8));
eq("value change sends rings", d.tick(send, frame, PICTURE), "rings");
eq("...exactly one message", send.log.length, 1);
eq("...and it is a ring, not a framebuffer", kindOf(send.log[0]), "ring");
eq("...carrying one chunk", unpack(send.log[0]).length, 1 /*F0*/ + 5 /*hdr*/
   + 2 /*id*/ + 1 /*pack byte*/ + 7 /*chunk*/ + 1 /*F7*/);
eq("nothing owed afterwards", [d.framebufferOwed, d.ringsPending], [false, 0]);

/* Many detents on one encoder COALESCE. */
d = createDisplay(); send = mkSend();
prime(d, send, PICTURE);
for (let i = 0; i < 20; i++) d.ringChanged(ringFor(v, 8));
d.ringChanged(ringFor(v, 9));
d.tick(send, frame, PICTURE);
eq("twenty detents on one encoder are one chunk each",
   unpack(send.log[0]).length, 1 + 5 + 2 + 2 /*pack bytes for 14 payload*/ + 14 + 1);

/* Navigation is ONE framebuffer however many times it is asked for. */
d = createDisplay(); send = mkSend();
prime(d, send, PICTURE);
/* A real navigation redraws different pixels; invert the whole screen so
 * the diff engine sees a real, screen-spanning change (well past
 * FULL_REPAINT_THRESHOLD) rather than "unchanged content" -- otherwise the
 * new diff-aware tick() correctly sends nothing, which is not what this
 * test is about. */
frameBuf = frameBuf.map((b) => b ^ 0xFF);
d.invalidate(); d.invalidate(); d.invalidate();
/* An inverted screen is a FULL repaint: eight bands, once. Three
 * invalidations must not make it 24. */
const p0 = d.paintsCompleted;
drain(d, send, PICTURE);
eq("three invalidations are ONE repaint", d.paintsCompleted - p0, 1);
/* Every row of the picture changed: a view switch. One repaint, not three --
 * a CLEAR and the new ink when that is fewer packets, never a FRAMEBUFFER. */
const oneRepaint = send.log.length;
eq("...of one set of messages, not three", oneRepaint <= STRIPS + 1, true);
eq("...RECTANGLEs after at most one leading CLEAR, never a framebuffer",
   send.log.every((p, i) => kindOf(p) === "rect" || (i === 0 && kindOf(p) === "clear")), true);
/* Full-width rows included: SCANLINE was the only opcode the device NACKed
 * after frame-atomic placement (2 of 52 vs 0 of 194 rects, 2026-09-24). */
eq("...and never a SCANLINE, even for a full-width row",
   send.log.some(p => kindOf(p) === "scanline"), false);
eq("nothing more owed", d.tick(send, frame, PICTURE), null);
eq("still one repaint on the wire", send.log.length, oneRepaint);

/* A repaint never shares a tick with rings, and finishes before them: a
 * half-drawn screen is worse than a ring that arrives a few ticks late. */
d = createDisplay(); send = mkSend();
d.invalidate(); d.ringChanged(ringFor(v, 8));
eq("repaint goes out first, CLEAR leading", d.tick(send, frame), "clear");
eq("...within the packet budget this tick",
   send.log.reduce((a, p) => a + p.length / 4, 0) <= TICK_PACKET_BUDGET, true);
drain(d, send);
eq("...and drains completely before the rings",
   send.log.every(p => ["rect", "scanline", "clear"].includes(kindOf(p))), true);
eq("rings follow once the repaint is complete", d.tick(send, frame), "rings");

/* A REFUSED send stays owed; it must not queue a second copy. */
let accept = false;
d = createDisplay(); send = mkSend(() => accept);
d.invalidate();
eq("refused repaint reports nothing sent", d.tick(send, frame), null);
eq("...and stays owed", d.repaintPending, true);
accept = true;
eq("...going out when accepted, CLEAR leading", d.tick(send, frame), "clear");
eq("...the refused CLEAR is retried, not queued twice: two attempts",
   send.log.filter(p => kindOf(p) === "clear").length, 2);
drain(d, send);
eq("...every message once, after the one retry", send.log.length, 1 + fullMsgs(frame()));
eq("...and nothing owed after", d.repaintPending, false);

/* A refused ring message keeps its positions owed for the same reason. */
accept = false;
d = createDisplay(); send = mkSend(() => accept);
d.ringChanged(ringFor(v, 8));
eq("refused rings report nothing sent", d.tick(send, frame), null);
eq("...and stay pending", d.ringsPending, 1);


/* The authored short label lives on the PAGE (page_plan collects
 * { key: short_name } into page.shortNames, which render_page_movy reads), NOT
 * on the param meta -- getOrGuess never carries it. Reading only meta.short_name
 * meant every cell fell through to the raw parameter id, and on hardware four
 * columns of truncated ids over four columns of digits read as a corrupted
 * screen rather than as text that does not fit. */
{
    const pg = { kind: PAGE_KNOBS, name: "P", level: "P",
                 keys: ["env_attack", "filter_cutoff"],
                 shortNames: { env_attack: "Atk", filter_cutoff: "Cut" } };
    const v = buildView([pg], 0, { metaOf: (k) => ({ min: 0, max: 127, label: k }),
                                   valueOf: () => 64 });
    eq("page shortNames win over the key",
       v.cells.filter(Boolean).map(c => c.label).join(","), "Atk,Cut");

    /* And the fallback still holds for a module that declares none. */
    const bare = { kind: PAGE_KNOBS, name: "P", level: "P", keys: ["some_param"] };
    const v2 = buildView([bare], 0, { metaOf: (k) => ({ min: 0, max: 1, label: "lbl" }),
                                      valueOf: () => 0 });
    eq("falls back when no short name is declared",
       v2.cells[0].label, "lbl");
}


/* ---- 6. THE TWO MODES OVERRIDE EACH OTHER ON THE DEVICE ----------------
 *
 * FRAMEBUFFER and LABELS are not layers: sending one replaces the other
 * (docs/E16_REMOTE.md). So "nothing changed" is NOT "nothing to send" across a
 * mode transition -- dismissing the Shift map leaves the map picture on the
 * panel, and every later value change goes out as a ring nobody can read a
 * name for. The display therefore tracks what the DEVICE was last told, not
 * what the surface last decided.
 * --------------------------------------------------------------------- */
d = createDisplay(); send = mkSend();
eq("the first tick paints, having told the device nothing yet",
   d.tick(send, frame, TEXT), "labels");
eq("...and a second identical tick sends nothing",
   d.tick(send, frame, TEXT), null);
eq("raising the map repaints the drawn view with no invalidate at all",
   drain(d, send, PICTURE)[0], "clear");
eq("dismissing it owes LABELS back, though not one label changed",
   d.tick(send, frame, TEXT), "labels");
eq("and then it settles again", d.tick(send, frame, TEXT), null);

/* A value reading moved: cheap text, and LOWER priority than the ring the hand
 * is watching. */
d = createDisplay(); send = mkSend();
prime(d, send, TEXT);
d.ringChanged(ringFor(v, 8));
d.invalidateLabels();
eq("a ring outranks a title refresh", d.tick(send, frame, TEXT), "rings");
eq("...and the title follows on the next tick", d.tick(send, frame, TEXT), "labels");
eq("...then nothing is owed", d.tick(send, frame, TEXT), null);

/* A replug wipes the panel, so what the device was told is no longer true. */
d = createDisplay(); send = mkSend();
prime(d, send, TEXT);
eq("settled", d.tick(send, frame, TEXT), null);
d.forgetShown();
eq("after forgetShown the screen is resent", d.tick(send, frame, TEXT), "labels");


/* ---- 8. THE SCREEN SELF-HEALS, because the link loses packets -----------
 *
 * Not because our messages are too big: a 101-byte LABELS message still
 * corrupts occasionally on hardware with every buffer on our side proven
 * clean and the message verified well-formed -- legal framing, correct CINs,
 * byte-exact through device-side unpacking. The 394-packet framebuffer
 * garbled more often for the obvious reason, not a different one.
 *
 * So the screen is restated on a heartbeat: 34 packets to repair a corruption
 * we can neither prevent nor detect. It is a RESTATE, not a retry -- there is
 * no acknowledgement to wait for -- and idempotent, since the same labels set
 * the same labels.
 * --------------------------------------------------------------------- */
let clock = 0;
d = createDisplay(); send = mkSend();
/* A STAMPED paint: screenAge measures from a completed send, so the first
 * tick has to carry a clock. prime() deliberately does not, which is why the
 * age is null until a real send happens here. */
d.tick(send, frame, TEXT, clock);
send.log.length = 0;
eq("a completed send starts the clock", d.screenAge(clock), 0);
eq("settled: nothing owed", d.tick(send, frame, TEXT, clock += 10), null);
/* Just short of the heartbeat: still quiet. */
eq("no restate before the interval",
   d.tick(send, frame, TEXT, clock += (SCREEN_HEARTBEAT_MS - 200)), null);
eq("screenAge tracks the wait",
   d.screenAge(clock) >= SCREEN_HEARTBEAT_MS - 200, true);

/* The surface is what asks for it, so drive the check the way it does. */
{
  let t2 = 0;
  const dd = createDisplay(); const ss = mkSend();
  dd.tick(ss, frame, TEXT, t2);            /* first paint */
  const before = ss.log.length;
  t2 += SCREEN_HEARTBEAT_MS + 50;
  if (dd.screenAge(t2) >= SCREEN_HEARTBEAT_MS) dd.invalidate();
  dd.tick(ss, frame, TEXT, t2);
  eq("a stale screen is restated", ss.log.length - before, 1);
}

/* A restate must never interrupt a turn. The SCREEN outranks rings in this
 * design (the settle timer is what keeps it out of a gesture), so the guard
 * cannot live in the display -- the surface withholds the heartbeat entirely
 * while a gesture is in flight or rings are pending. Asserted at that seam:
 * a display with rings queued reports them, and the surface is what must
 * decline to add a repaint on top. */
d = createDisplay(); send = mkSend();
prime(d, send, TEXT);
d.ringChanged(ringFor(v, 8));
eq("rings pending is visible to the caller that gates the heartbeat",
   d.ringsPending, 1);

/* --------------------------------------------------------------------------
 * PARTIAL OLED UPDATES. Built against a draft spec, not yet on hardware --
 * see docs/superpowers/specs/2026-09-22-e16-partial-oled-updates-design.md.
 * These are unit-level: a fresh createDisplay(), fake send/frameBytes, no
 * device.
 * ---------------------------------------------------------------------- */
{
  const WIDTH = 128, HEIGHT = 64;
  const setPx = (buf, x, y) => { buf[(y >> 3) * WIDTH + x] |= (1 << (y & 7)); };
  const mkSend = () => { const log = []; const fn = (p) => { if (fn.refuse) return false; log.push(p); return true; }; fn.log = log; fn.refuse = false; return fn; };
  const unpackMsgId = (packets) => {
    const out = [];
    for (let i = 0; i < packets.length; i += 4) {
      const cin = packets[i] & 0x0F; const n = cin === 0x05 ? 1 : cin === 0x06 ? 2 : 3;
      for (let b = 0; b < n; b++) out.push(packets[i + 1 + b]);
    }
    return out.slice(6, 7);   /* the single id byte, per OLED_SUBCOMMAND_HAS_CATEGORY_PREFIX=false */
  };

  /* A display, primed: a full repaint is EIGHT 128x8 bands, one per tick. */
  const mk = () => createDisplay();
  /* Ticks until the repaint has fully drained; returns the per-tick kinds. */
  const paintAll = (d, send, fb, t) => { const k = [];
    for (let i = 0; i < 64; i++) { k.push(d.tick(send, fb, { kind: "framebuffer" }, t + i));
      answer(d, send);
      if (!d.repaintPending) break; } return k; };
  const allRect = (k) => k.length > 0 && k.every((x) => x === "rect");

  const d1 = mk();
  const send1 = mkSend();
  let buf1 = new Uint8Array(1024);
  const frameBytes1 = () => buf1;
  d1.invalidate();
  paintAll(d1, send1, frameBytes1, 0);
  eq("first paint of a BLANK screen is a single CLEAR -- no strips at all",
     send1.log.map((p) => unpack(p)[6]), [0x07]);
  eq("...and nothing is left owed", d1.tick(send1, frameBytes1, { kind: "framebuffer" }, 9), null);

  buf1 = new Uint8Array(1024);
  setPx(buf1, 10, 10);
  d1.invalidate();
  const second = d1.tick(send1, frameBytes1, { kind: "framebuffer" }, 100);
  eq("single-pixel diff sends rect, not framebuffer", second, "rect");
  eq("rect message id byte (0x08, measured on hardware)", unpackMsgId(send1.log[send1.log.length - 1]), [0x08]);

  /* Two-region diff drains across two ticks. */
  const d2 = mk();
  const send2 = mkSend();
  let buf2 = new Uint8Array(1024);
  const frameBytes2 = () => buf2;
  d2.invalidate();
  paintAll(d2, send2, frameBytes2, 0);   /* establish baseline */
  buf2 = new Uint8Array(1024);
  setPx(buf2, 0, 0);
  setPx(buf2, WIDTH - 1, HEIGHT - 1);
  d2.invalidate();
  const b2 = send2.log.length;
  const r1 = d2.tick(send2, frameBytes2, { kind: "framebuffer" }, 100);
  /* Two tiny regions fit the PACKET budget of one tick, so both go out together
   * -- top one first -- and nothing is left for the next tick. */
  eq("two-region diff: both small regions go out in ONE tick", r1, "rect");
  eq("...exactly two messages", send2.log.length - b2, 2);
  eq("...top region first", unpack(send2.log[b2])[9] < unpack(send2.log[b2 + 1])[9], true);
  const r3 = d2.tick(send2, frameBytes2, { kind: "framebuffer" }, 101);
  eq("two-region diff: next tick has nothing left", r3, null);

  /* A refused send changes nothing. */
  const d3 = mk();
  const send3 = mkSend();
  let buf3 = new Uint8Array(1024);
  const frameBytes3 = () => buf3;
  d3.invalidate();
  paintAll(d3, send3, frameBytes3, 0);
  buf3 = new Uint8Array(1024);
  setPx(buf3, 5, 5);
  d3.invalidate();
  send3.refuse = true;
  const refused = d3.tick(send3, frameBytes3, { kind: "framebuffer" }, 100);
  eq("refused send returns null", refused, null);
  send3.refuse = false;
  const retried = d3.tick(send3, frameBytes3, { kind: "framebuffer" }, 101);
  eq("retry after refusal still sends the same diff", retried, "rect");

  /* Switching to labels drops any queued regions and forces a full repaint
   * on the way back to framebuffer mode -- as bands. */
  const d4 = mk();
  const send4 = mkSend();
  let buf4 = new Uint8Array(1024);
  const frameBytes4 = () => buf4;
  d4.invalidate();
  paintAll(d4, send4, frameBytes4, 0);
  buf4 = new Uint8Array(1024);
  setPx(buf4, 0, 0);
  setPx(buf4, WIDTH - 1, HEIGHT - 1);
  d4.invalidate();
  d4.tick(send4, frameBytes4, { kind: "framebuffer" }, 100);   /* first of two regions queued */
  const toLabels = d4.tick(send4, frameBytes4, { kind: "labels", title: "T", labels: [] }, 101);
  eq("switch to labels sends labels", toLabels, "labels");
  d4.invalidate();
  const b4 = send4.log.length;
  paintAll(d4, send4, frameBytes4, 102);
  eq("switch back to the drawn view is a FULL repaint, not a stale region",
     send4.log.length - b4, fullMsgs(buf4));

  /* invalidateBuf() IS THE HEARTBEAT FIX, DIRECTLY TESTED. Its only real
   * caller is the self-heal heartbeat in createSurface, whose job is to
   * resend BYTE-IDENTICAL content -- a plain invalidate() on unchanged
   * content is exactly what the diff correctly answers "none" for. */
  const d5 = mk();
  const send5 = mkSend();
  const buf5 = new Uint8Array(1024);
  setPx(buf5, 10, 10);
  const frameBytes5 = () => buf5;   /* deliberately IDENTICAL every call */
  d5.invalidate();
  eq("priming paint", paintAll(d5, send5, frameBytes5, 0)[0], "clear");
  d5.invalidate();
  const unchanged = d5.tick(send5, frameBytes5, { kind: "framebuffer" }, 100);
  eq("plain invalidate() on unchanged content sends nothing", unchanged, null);
  d5.invalidateBuf();
  d5.invalidate();
  const b5 = send5.log.length;
  paintAll(d5, send5, frameBytes5, 200);
  eq("invalidateBuf() forces a full resend of the SAME content",
     send5.log.length - b5, fullMsgs(buf5));

  /* A NACK NAMES ITS REGION, and only that region is re-sent. */
  const d6 = mk();
  const send6 = mkSend();
  const buf6 = new Uint8Array(1024);
  setPx(buf6, 40, 50);
  const frameBytes6 = () => buf6;   /* unchanged content throughout */
  d6.invalidate();
  paintAll(d6, send6, frameBytes6, 0);
  const before6 = send6.log.length;
  d6.invalidateRegion(40, 47, 46, 5);   /* the exact rect a device NACKed */
  d6.invalidate();
  paintAll(d6, send6, frameBytes6, 100);
  const { unpack7 } = await import("./src/shared/e16_protocol.mjs");
  const resent = send6.log.slice(before6).map((p) => { const u = unpack(p);
    return unpack7(u.slice(7, u.length - 1), 4); });
  /* Rows 47..51 of x 40..85, cut into blocks as tall as fit one message --
   * and nothing else, and never a CLEAR for a repair. */
  let hMax6 = 1; while (rectPackets(46, hMax6 + 1) <= ATOMIC_MAX_PACKETS) hMax6++;
  const want6 = [];
  for (let y = 47; y < 52; y += hMax6) want6.push([40, y, 46, Math.min(hMax6, 52 - y)]);
  eq("a NACKed region re-sends exactly that region, as message-sized blocks", resent, want6);
  eq("...and nothing else goes out after it", d6.tick(send6, frameBytes6, { kind: "framebuffer" }, 200), null);
}

/* A FULL REPAINT IS ONE CLEAR, THEN ONLY THE INK, each strip trimmed to its
 * inked width -- and a region the device never answers is re-sent. */
{
  const { unpack7 } = await import("./src/shared/e16_protocol.mjs");
  const px = (b, x, y) => { b[(y >> 3) * 128 + x] |= (1 << (y & 7)); };
  const addr = (p) => { const u = unpack(p); return unpack7(u.slice(7, u.length - 1), 4); };

  const d = createDisplay(); const snd = mkSend();
  const b = new Uint8Array(1024); px(b, 40, 50); px(b, 45, 51);
  d.invalidate();
  /* The CLEAR goes alone (it is the whole screen to the in-flight window);
   * the device ACKs it, and the rows follow. It never answers the rows. */
  d.tick(snd, () => b, { kind: "framebuffer" }, 0);
  eq("a CLEAR goes out alone, ahead of any row", snd.log.map((p) => unpack(p)[6]), [0x07]);
  d.acked({ ok: true, cmd: 0x07, addr: {} });
  for (let i = 1; i < 8 && d.repaintPending; i++) d.tick(snd, () => b, { kind: "framebuffer" }, i);
  eq("full repaint = CLEAR + the ink, adjacent inked rows in ONE block",
     snd.log.map((p) => unpack(p)[6]), [0x07, 0x08]);
  eq("...trimmed to its own ink", addr(snd.log[1]), [40, 50, 6, 2]);

  eq("the unanswered block is outstanding", d.outstandingCount, 1);
  const n0 = snd.log.length;
  d.tick(snd, () => b, { kind: "framebuffer" }, ACK_TIMEOUT_MS - 50);
  eq("...not re-sent before ACK_TIMEOUT_MS", snd.log.length, n0);
  d.tick(snd, () => b, { kind: "framebuffer" }, ACK_TIMEOUT_MS + 10);
  const resent = snd.log.slice(n0).map(addr);
  eq("...re-sent once it times out, the same block", resent, [[40, 50, 6, 2]]);
  eq("...and counted", d.ackTimeouts, 1);

  /* An answered region is never re-sent. */
  for (const [x, y, w, h] of resent) d.acked({ cmd: 0x08, addr: { x, y, w, h } });
  const n1 = snd.log.length;
  d.tick(snd, () => b, { kind: "framebuffer" }, ACK_TIMEOUT_MS * 4);
  eq("an ACKed region is not re-sent", snd.log.length, n1);
}

/* NO ESCALATION LOOP. On hardware a handful of NACKed strips on different
 * rows crossed the diff region cap, the diff called it "full", and the full
 * repaint drew more NACKs -- four whole-screen repaints a second. Repairs of a
 * KNOWN screen must stay repairs: exactly the damaged strips, never a CLEAR. */
{
  const d = createDisplay(); const snd = mkSend();
  const b = new Uint8Array(1024);
  for (let x = 0; x < 128; x++) for (let y = 0; y < 64; y++) if ((x + y) % 5 === 0) b[(y >> 3) * 128 + x] |= (1 << (y & 7));
  d.invalidate();
  for (let i = 0; i < 64 && (i === 0 || d.repaintPending); i++) d.tick(snd, () => b, { kind: "framebuffer" });
  const n0 = snd.log.length;
  for (const y of [0, 10, 20, 30, 40, 50, 60]) d.invalidateRegion(0, y, 128, 2);   /* seven NACKs, seven rows */
  d.invalidate();
  for (let i = 0; i < 64 && (i === 0 || d.repaintPending); i++) d.tick(snd, () => b, { kind: "framebuffer" });
  const ids = snd.log.slice(n0).map((p) => unpack(p)[6]);
  eq("seven NACKed 2-row regions re-send exactly those rows", ids.length, 7 * 2 / STRIP_H);
  eq("...and never a CLEAR", ids.includes(0x07), false);
}

/* A REPAIR MARK SURVIVES THE REST OF ITS DRAIN. Belief used to be set to the
 * whole target picture on every strip sent, so a NACK on an EARLIER strip of
 * the same drain was overwritten by the next send and never repaired --
 * missing lines, and incomplete view switches (a long drain), on hardware. */
{
  const { unpack7 } = await import("./src/shared/e16_protocol.mjs");
  const d = createDisplay(); const snd = mkSend();
  /* Every row inked, but only x 0..99, so each row is a RECTANGLE (a
   * full-width row would be a SCANLINE). */
  const b = new Uint8Array(1024);
  for (let x = 0; x < 100; x++) for (let p8 = 0; p8 < 8; p8++) b[p8 * 128 + x] = 0xFF;
  d.invalidate();
  d.tick(snd, () => b, { kind: "framebuffer" });      /* CLEAR + first strips */
  const firstStrip = snd.log.map((p) => unpack(p)).find((u) => u[6] === 0x08);
  const a = unpack7(firstStrip.slice(7, firstStrip.length - 1), 4);
  eq("the drain is still running after one tick", d.repaintPending, true);
  d.invalidateRegion(a[0], a[1], a[2], a[3]);          /* the device NACKed it */
  d.invalidate();
  for (let i = 0; i < 64 && d.repaintPending; i++) d.tick(snd, () => b, { kind: "framebuffer" });
  const sends = snd.log.map((p) => unpack(p)).filter((u) => u[6] === 0x08)
    .map((u) => JSON.stringify(unpack7(u.slice(7, u.length - 1), 4)))
    .filter((k) => k === JSON.stringify(a)).length;
  eq("the NACKed strip goes out TWICE: original, then its repair -- not forgotten", sends, 2);
}

/* THE BUDGET FOLLOWS THE PACE: a display handed a bigger budget puts more
 * strips out per tick, a smaller one fewer -- never more than one message
 * past the budget. */
{
  const perTick = (budget) => { const d = createDisplay({ budgetOf: () => budget });
    const snd = mkSend(); const b = new Uint8Array(1024).fill(0xFF);
    d.invalidate(); d.tick(snd, () => b, { kind: "framebuffer" });
    return snd.log.reduce((a, p) => a + p.length / 4, 0); };
  eq("a pace-12 budget (60) sends more per tick than pace 8 (40)", perTick(60) > perTick(40), true);
  eq("...and each stays within its budget", perTick(60) <= 60 && perTick(40) <= 40, true);
}

/* A NEWER PICTURE REPLACES THE REST OF THE QUEUE. A quick Shift tap used to
 * draw the whole map before the knob view even started. Start drawing A,
 * switch to B after one tick: A remaining strips are abandoned, and what
 * reaches the device is exactly B. */
{
  const { unpack7 } = await import("./src/shared/e16_protocol.mjs");
  const d = createDisplay(); const snd = mkSend();
  const blank = new Uint8Array(1024);
  let cur = blank;
  d.invalidate();
  for (let i = 0; i < 8 && (i === 0 || d.repaintPending); i++) d.tick(snd, () => cur, { kind: "framebuffer" });
  const A = new Uint8Array(1024).fill(0xFF);             /* the map: every row */
  const B = new Uint8Array(1024);                        /* the knob view: one mark */
  B[(40 >> 3) * 128 + 10] |= (1 << (40 & 7));
  cur = A; d.invalidate();
  d.tick(snd, () => cur, { kind: "framebuffer" });        /* one tick of A goes out */
  const n0 = snd.log.length;
  cur = B; d.invalidate();                                /* Shift released */
  for (let i = 0; i < 64 && d.repaintPending; i++) d.tick(snd, () => cur, { kind: "framebuffer" });
  const afterSwitch = snd.log.length - n0;
  eq("switching mid-drain does NOT finish the old picture first",
     afterSwitch < STRIPS - 4, true);
  /* Apply everything that went out, in order, to a model screen: it must be B. */
  const screen = new Uint8Array(1024);
  for (const p of snd.log) { const u = unpack(p);
    if (u[6] === 0x07) screen.fill(0);
    if (u[6] !== 0x08) continue;
    const payload = u.slice(7, u.length - 1);
    const hdr = unpack7(payload, 4); const [x, y, w, h] = hdr;
    const bits = unpack7(payload, 4 + Math.ceil(w / 8) * h).slice(4);
    for (let ry = 0; ry < h; ry++) for (let rx = 0; rx < w; rx++) {
      const on = (bits[ry * Math.ceil(w / 8) + (rx >> 3)] >> (7 - (rx & 7))) & 1;
      const i = ((y + ry) >> 3) * 128 + x + rx, bit = 1 << ((y + ry) & 7);
      screen[i] = on ? (screen[i] | bit) : (screen[i] & ~bit); } }
  eq("...and the device ends up showing exactly the NEW picture",
     Array.from(screen).join(), Array.from(B).join());
}

/* ONE VALUE, ONE PICTURE. The drawn view printed String(cell.value), so the
 * same reading drew as the module own string before a turn (hank ratio
 * "11.000") and as the controller number after one ("11"). It goes through
 * displayValue now, the formatter the knob grid uses. */
{
  const pg = [page("Comp", ["ratio"])];
  const meta = { ratio: { type: "float", min: 1, max: 20, step: 1, label: "Ratio" } };
  const draw = (val) => { const c = createCanvas(); c.clear();
    renderView(c, buildView(pg, 0, { metaOf: (k) => meta[k], valueOf: () => val }));
    return Array.from(c.toBuffer()).join(","); };
  eq("a module string and the same number draw identically", draw("11.000"), draw(11));
  eq("...and an unread value still draws nothing where the value goes",
     draw(undefined) === draw(null), true);
}

/* FLOW CONTROL: the next row waits for the pixels in flight. Hardware,
 * 2026-09-24: wide rows sent one a frame were NACKed "interrupted" at ~15%
 * (315 of 2276) while narrow ones never were -- the device queue overflows by
 * PIXELS. So: never more than the window unanswered, a NACK halves it, it can
 * never stall, and an answer is what lets the next row go. */
{
  const full = new Uint8Array(1024).fill(0xFF);
  const d = createDisplay(); const snd = mkSend();
  const blank = new Uint8Array(1024); blank[0] = 1;
  d.invalidate();
  for (let i = 0; i < 64 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => blank, { kind: "framebuffer" }, i); answer(d, snd); }
  const w0 = d.windowPx;
  const n0 = snd.log.length;
  d.invalidate();
  for (let i = 0; i < 20; i++) d.tick(snd, () => full, { kind: "framebuffer" }, 100 + i);
  const rows = Math.max(1, Math.floor(w0 / 128));
  eq("unanswered: no more full-width rows than the window holds", snd.log.length - n0, rows);
  eq("...and the rest stays owed, not dropped", d.repaintPending, true);
  answer(d, snd);
  d.tick(snd, () => full, { kind: "framebuffer" }, 130);
  eq("an answer is what lets the next rows go", snd.log.length - n0 > rows, true);
  const before = d.windowPx;
  answer(d, snd, false);                      /* the device NACKs them */
  eq("a NACK halves the window", d.windowPx < before, true);
  for (let i = 0; i < 40; i++) {              /* NACK everything, repeatedly */
    d.tick(snd, () => full, { kind: "framebuffer" }, 200 + i); answer(d, snd, false); }
  eq("...never below WINDOW_PX_MIN", d.windowPx, WINDOW_PX_MIN);
  const n1 = snd.log.length;
  d.tick(snd, () => full, { kind: "framebuffer" }, 300);
  eq("...and a row still goes when nothing is in flight -- it cannot stall", snd.log.length > n1, true);
  eq("the window starts at WINDOW_PX_START", createDisplay().windowPx, WINDOW_PX_START);
}

/* A STALLED FRAME IS NOT A LOST ANSWER. Replies are read before the UI tick;
 * a tick that stalls (slot switch, blocking reads) reaches this sweep with
 * answers still unread. Timers age against when replies were last READ. */
{
  const d = createDisplay(); const snd = mkSend();
  const b = new Uint8Array(1024); b[5] = 1;
  d.invalidate();
  d.tick(snd, () => b, { kind: "framebuffer" }, 0);
  answer(d, snd);
  for (let i = 1; i < 8 && d.repaintPending; i++) d.tick(snd, () => b, { kind: "framebuffer" }, i);
  eq("a row is outstanding", d.outstandingCount, 1);
  const n0 = snd.log.length;
  d.tick(snd, () => b, { kind: "framebuffer" }, 5000, 20);
  eq("a clock far past ACK_TIMEOUT_MS with replies unread is NOT a timeout", [d.ackTimeouts, snd.log.length], [0, n0]);
  d.tick(snd, () => b, { kind: "framebuffer" }, 5001, ACK_TIMEOUT_MS + 10);
  eq("...replies READ past it with no answer IS one", d.ackTimeouts, 1);
}

/* LOSS BY ORDER. Hardware, 2026-09-24: a fixed 250 ms timeout fired on live
 * rows under load, re-sent them, and fed itself until the whole unchanged
 * screen went out once a second. A region is lost when REORDER_LOSS later
 * ones are answered without it -- never merely because it is slow. */
{
  const ackOne = (d, p, ok = true) => { const b = unpack(p);
    const [x, y, w, h] = unpack7(b.slice(7, b.length - 1), 4);
    d.acked({ ok, cmd: 0x08, addr: { x, y, w, h } }); };
  const rowsOf = (log) => log.map((p) => { const b = unpack(p); return unpack7(b.slice(7, b.length - 1), 4)[1]; });
  /* A fully inked screen, then 40 of its rows changed: full-width rows, one
   * message each, and a DIFF (a CLEAR would re-send all 64). */
  const base = new Uint8Array(1024), pic = new Uint8Array(1024);
  for (let y = 0; y < 64; y++) for (let x = 0; x < 128; x++) {
    if ((x + y) % 3 === 1) base[(y >> 3) * 128 + x] |= 1 << (y & 7);
    if ((x + y) % 3 === ((y >= 10 && y < 50) ? 0 : 1)) pic[(y >> 3) * 128 + x] |= 1 << (y & 7);
  }
  const mk = () => { const d = createDisplay(); const snd = mkSend();
    d.invalidate();
    for (let i = 0; i < 200 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => base, { kind: "framebuffer" }, i); answer(d, snd); }
    return { d, snd }; };
  const run = (skip) => { const { d, snd } = mk();
    const n0 = snd.log.length;
    d.invalidate();
    const answeredIdx = new Set(); let lossAt = null;
    for (let t = 0; t < 400 && (d.repaintPending || d.outstandingCount); t++) {
      d.tick(snd, () => pic, { kind: "framebuffer" }, 100 + t);
      const sent = snd.log.slice(n0);
      for (let i = 0; i < sent.length; i++) if (!answeredIdx.has(i) && !skip(i, sent.length)) { answeredIdx.add(i); ackOne(d, sent[i]);
        if (d.ackTimeouts && lossAt === null) lossAt = answeredIdx.size; }
      if (d.ackTimeouts) break;
    }
    return { d, snd, n0, lossAt }; };

  /* Never answer the FIRST message: it is declared lost only once
   * REORDER_LOSS later ones are answered, and then re-sent. */
  let r = run((i) => i === 0);
  eq("the first row is LOST only after REORDER_LOSS later answers", [r.d.ackTimeouts, r.lossAt], [1, REORDER_LOSS]);
  const n1 = r.snd.log.length;
  r.d.tick(r.snd, () => pic, { kind: "framebuffer" }, 900);
  eq("...and exactly that row is re-sent first", rowsOf(r.snd.log.slice(n1, n1 + 1)), [rowsOf(r.snd.log.slice(r.n0, r.n0 + 1))[0]]);

  /* Answer the first one place LATE (after the second): not a loss. */
  let held = true;
  r = run((i, n) => { if (i === 0 && held) { if (n > 2) { held = false; return false; } return true; } return false; });
  eq("an answer one place out of order is not a loss", [r.d.ackTimeouts, r.d.outstandingCount], [0, 0]);
  eq("the clock backstop is far past the OLD 250 ms that fired on live rows", ACK_TIMEOUT_MS >= 1000, true);
}

/* TWO REPAIRS OF ONE ROW DO NOT CANCEL. The repair used to XOR the belief,
 * so a second NACK or loss on the same row before its re-send flipped it
 * back and the row was silently never repaired. */
{
  const d = createDisplay(); const snd = mkSend();
  const b = new Uint8Array(1024); b[(20 >> 3) * 128 + 7] |= 1 << (20 & 7);
  d.invalidate();
  for (let i = 0; i < 64 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => b, { kind: "framebuffer" }, i); answer(d, snd); }
  const n0 = snd.log.length;
  d.invalidateRegion(0, 20, 128, 1); d.invalidateRegion(0, 20, 128, 1);
  d.invalidate();
  d.tick(snd, () => b, { kind: "framebuffer" }, 200);
  eq("a row repaired twice is still re-sent", snd.log.length - n0, 1);
  answer(d, snd);
  d.invalidate();
  eq("...and once re-sent it is clean", d.tick(snd, () => b, { kind: "framebuffer" }, 201), null);
}

/* BLOCKS, AND A CLEAR ONLY FOR A VIEW CHANGE. A message holds ~16 bytes of
 * pixels whatever its shape (one SPI frame), so narrow content goes as a
 * block many rows tall, not a message per row. A switch to a sparser view is
 * a CLEAR and its ink; a small change never blanks the panel. */
{
  const { rectangleMsg } = await import("./src/shared/e16_protocol.mjs");
  let formulaOk = true;
  for (const [w, h] of [[1, 1], [8, 16], [16, 7], [46, 3], [128, 1], [64, 2], [20, 5]]) {
    const real = Math.ceil(rectangleMsg(0, 0, w, h, new Uint8Array(Math.ceil(w / 8) * h)).length / 3);
    if (real !== rectPackets(w, h)) formulaOk = false;
  }
  eq("rectPackets matches the real message length", formulaOk, true);

  const label = new Uint8Array(1024);   /* a 14 x 7 "text" block at 20,30 */
  for (let y = 30; y < 37; y++) for (let x = 20; x < 34; x += 2) label[(y >> 3) * 128 + x] |= 1 << (y & 7);
  let d = createDisplay(); let snd = mkSend();
  d.invalidate();
  for (let i = 0; i < 20 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => label, { kind: "framebuffer" }, i); answer(d, snd); }
  eq("a 7-row label is ONE block after the CLEAR, not seven rows",
     snd.log.map((p) => unpack(p)[6]), [0x07, 0x08]);

  /* Dense view, then a sparse one: a view switch leads with a CLEAR. */
  const dense = new Uint8Array(1024).fill(0x55);
  d = createDisplay(); snd = mkSend();
  d.invalidate();
  for (let i = 0; i < 200 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => dense, { kind: "framebuffer" }, i); answer(d, snd); }
  let n0 = snd.log.length;
  d.invalidate();
  for (let i = 0; i < 20 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => label, { kind: "framebuffer" }, 300 + i); answer(d, snd); }
  eq("switch to a sparser view: CLEAR, then only its ink",
     snd.log.slice(n0).map((p) => unpack(p)[6]), [0x07, 0x08]);

  /* A few rows changing never CLEARs, however sparse the screen. */
  const label2 = label.slice(); label2[(31 >> 3) * 128 + 21] |= 1 << (31 & 7);
  n0 = snd.log.length;
  d.invalidate();
  for (let i = 0; i < 20 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => label2, { kind: "framebuffer" }, 400 + i); answer(d, snd); }
  eq("a one-pixel change is a region, never a CLEAR",
     snd.log.slice(n0).map((p) => unpack(p)[6]), [0x08]);
  /* ...nor a repair of the whole sparse screen (the heartbeat restate). */
  n0 = snd.log.length;
  d.invalidateRegion(0, 0, 128, 64); d.invalidate();
  for (let i = 0; i < 40 && (i === 0 || d.repaintPending); i++) { d.tick(snd, () => label2, { kind: "framebuffer" }, 500 + i); answer(d, snd); }
  eq("a whole-screen REPAIR never CLEARs", snd.log.slice(n0).some((p) => unpack(p)[6] === 0x07), false);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'

# A SECOND VALUE-APPLICATION PATH IS THE THING THIS DESIGN FORBIDS, and it
# cannot be caught by driving a fake controller: a module that wrote params
# itself would still call onKnobTurn and pass every assertion above.
if grep -nE 'setParam|host_module_set_param|shadow_set_param' src/shared/e16_view.mjs; then
    echo "FAIL: e16_view.mjs writes parameters itself -- turns must route"
    echo "      through the controller's onKnobTurn (enum quantization,"
    echo "      read-only refusal and momentary latching all live there)."
    exit 1
fi
echo "ok   no second value-application path"
