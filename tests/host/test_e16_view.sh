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
import { createDisplay } from "./src/shared/e16_surface.mjs";
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
const kindOf = (packets) => { const b = unpack(packets);
  if (b[0] !== 0xF0) return "?";
  const id = (b[6] << 8) | b[7];
  return id === 0x0602 ? "framebuffer" : id === 0x0604 ? "ring"
       : id === 0x0603 ? "labels" : "other"; };

const mkSend = (accept) => { const log = [];
  const fn = (p) => { log.push(p); return accept === undefined ? true : accept(); };
  fn.log = log; return fn; };

const frame = () => cv2.toBuffer();

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
const prime = (d, send, screen) => { d.tick(send, frame, screen); send.log.length = 0; };

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
d.invalidate(); d.invalidate(); d.invalidate();
eq("navigation sends a framebuffer", d.tick(send, frame, PICTURE), "framebuffer");
eq("three invalidations are one repaint", send.log.length, 1);
eq("...and it is a framebuffer", kindOf(send.log[0]), "framebuffer");
eq("nothing more owed", d.tick(send, frame), null);
eq("still one message on the wire", send.log.length, 1);

/* A framebuffer never shares its tick with rings. */
d = createDisplay(); send = mkSend();
d.invalidate(); d.ringChanged(ringFor(v, 8));
eq("repaint goes out alone", d.tick(send, frame), "framebuffer");
eq("...one message this tick", send.log.length, 1);
eq("rings follow on the NEXT tick", d.tick(send, frame), "rings");
eq("...two messages total", send.log.length, 2);

/* A REFUSED send stays owed; it must not queue a second copy. */
let accept = false;
d = createDisplay(); send = mkSend(() => accept);
d.invalidate();
eq("refused repaint reports nothing sent", d.tick(send, frame), null);
eq("...and stays owed", d.framebufferOwed, true);
accept = true;
eq("...going out once when accepted", d.tick(send, frame), "framebuffer");
eq("...still only one framebuffer ever in flight",
   send.log.filter(p => kindOf(p) === "framebuffer").length, 2 /*1 refused + 1 accepted*/);
eq("...and nothing owed after", d.framebufferOwed, false);

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
eq("raising the map sends a FRAMEBUFFER with no invalidate at all",
   d.tick(send, frame, PICTURE), "framebuffer");
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
