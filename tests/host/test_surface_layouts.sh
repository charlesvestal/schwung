#!/usr/bin/env bash
# Every surface runs either navigation layout (layout_common.mjs). The two
# pairings the devices were BORN with are tested in test_e16_*.sh and
# test_ec4_surface.sh; this file drives the two CROSSED pairings and a switch:
#
#   EC4 + MAP    sixteen parameters across two pages as names; a Shift HOLD
#                shows the slot map as names, and a Shift + push (a SysEx
#                report on the EC4) jumps; Shift + turn pages by the EC4s
#                selector angle, not per pulse; a Shift TAP is the Mixer
#   E16 + KNOBS  the page on the top eight, labelled navigation knobs below,
#                drawn in pixels and lit on the rings; the slot knob moves the
#                focus, VOL writes the slot volume
#   a switch     changing the setting at runtime lands on the new layout,
#                off the old one s Mixer
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { createEc4Surface, DEFAULT_SETUP, SELECTOR_PULSES } from "./src/shared/ec4_surface.mjs";
import { createSurface, drawScreen } from "./src/shared/e16_surface.mjs";
import { createCanvas } from "./src/shared/e16_canvas.mjs";
import { screenLabels } from "./src/shared/layout_common.mjs";
import { MAP_SHOW_DELAY_MS } from "./src/shared/layout_map.mjs";
import { PAGE_KNOBS } from "./src/shared/param_pages/page_plan.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };
const ok = (n, c) => eq(n, !!c, true);

const page = (name, keys) => ({ kind: PAGE_KNOBS, name, level: name, keys });
function fakeController(writes) {
  return {
    pages: [page("Main", ["cutoff", "reso"]), page("Env", ["attack", "decay"]), page("Mod", ["rate"])],
    pageIndex: 0,
    state: { values: { cutoff: "0.5", reso: "0.1", attack: "0.2", decay: "0.3", rate: "0.4" } },
    load(f) { this.loaded = f; }, tick() {},
    goToPage(i) { this.pageIndex = i; },
    onKnobTurn(slot, dir) { writes.push([this.pages[this.pageIndex].name, slot, dir]); },
  };
}
function fakeMixerIo(slotWrites) {
  return {
    getSlot: (s, k) => ({ "slot:volume": "1", "slot:muted": "0", "slot:soloed": "0", "slot:pan": "0",
                          "buses:main_send1": "0", "buses:main_send2": "0" })[k],
    setSlot: (s, k, v) => { slotWrites.push([s, k]); return true; },
    getGlobal: () => "0", setGlobal: () => true, skipback: () => true,
    nameOf: (s) => "Track " + (s + 1),
  };
}
const chain = { slots: [{ synth: "obxd", fx: ["freeverb"] }, { synth: "dx7" }, {}, {}] };

/* ================= EC4 + MAP ================= */
{
  let t = 1000, nav = "map";
  const writes = [], slotWrites = [];
  const s = createEc4Surface({ now: () => t, send: () => true, chainOf: () => chain,
    makeController: () => fakeController(writes), mixer: fakeMixerIo(slotWrites),
    pulsesPerDetentOf: () => 1, navigationOf: () => nav });
  const HDR = [0xF0, 0x00, 0x00, 0x00, 0x4E, 0x2C, 0x1B];
  const report = (setup) => HDR.concat([0x4E, 0x28, 0x10 | setup, 0x4E, 0x24, 0x10, 0xF7]);
  const key = (k, down) => HDR.concat([0x4E, 0x26, 0x10 | k, 0x4E, 0x2E, down ? 0x11 : 0x10, 0xF7]);
  const shiftedPush = (n) => HDR.concat([0x4E, 0x2A, 0x10 | n, 0x4E, 0x2E, 0x11, 0xF7]);
  const run = (ms) => { for (let i = 0; i < ms / 16; i++) { t += 16; if (i % 30 === 0) s.feedMidi(report(DEFAULT_SETUP)); s.tick(); } };
  const names = () => { const n = s.screen().names; return [0, 1, 2, 3].map((r) => n.slice(r * 16, r * 16 + 16)); };
  const pulse = (enc, n) => { for (let i = 0; i < Math.abs(n); i++) s.feedMidi([0xB0, enc + 1, n > 0 ? 1 : 127]); };

  s.setEnabled(true);
  run(300);
  eq("EC4+map: the map layout is live", s.layout.name, "map");
  eq("EC4+map: sixteen parameters -- page Main on top, Env below",
     [names()[0], names()[2]], ["CUTORESO        ", "ATTADECA        "]);

  pulse(0, 2);
  eq("EC4+map: a top-half turn edits page Main", writes.slice(-1)[0], ["Main", 0, 1]);
  ok("EC4+map: ...and leaves a reading on the overlay", s.screen().overlay);
  pulse(8, 1);
  eq("EC4+map: a bottom-half turn edits page Env", writes.slice(-1)[0], ["Env", 0, 1]);

  run(2000);
  s.feedMidi(key(1, true));
  run(MAP_SHOW_DELAY_MS + 50);
  eq("EC4+map: a Shift HOLD shows the slot map as names",
     names()[0].replace(/ /g, ""), ">1234");
  ok("EC4+map: ...with the slot modules below", names()[1].toLowerCase().includes("obxd"));
  s.feedMidi(shiftedPush(5));
  eq("EC4+map: Shift + push (a SysEx report) jumps to the module", s.component, "fx1");
  s.feedMidi(key(1, false));
  run(100);

  s.feedMidi(key(1, true));
  pulse(0, SELECTOR_PULSES - 1);
  eq("EC4+map: Shift + turn below one selector angle does not page", s.pageIndex, 0);
  pulse(0, 1);
  eq("EC4+map: ...a whole angle pages (by two: a pair of pages)", s.pageIndex, 2);
  s.feedMidi(key(1, false));
  eq("EC4+map: ...and that release was not a tap", s.mixerOn, false);

  run(500);
  s.feedMidi(key(1, true)); t += 60; s.feedMidi(key(1, false));
  run(100);
  eq("EC4+map: a Shift TAP is the Mixer", [s.mixerOn, names()[0].slice(0, 4)], [true, "VOL "]);

  nav = "knobs";
  run(100);
  eq("EC4: switching the setting lands on the knobs layout, off the Mixer",
     [s.layout.name, s.mixerOn, names()[2].slice(0, 4)], ["knobs", false, "<PG "]);
}

/* ================= E16 + KNOBS ================= */
{
  let t = 1000;
  const writes = [], slotWrites = [];
  const s = createSurface({ now: () => t, send: () => true, chainOf: () => chain,
    makeController: () => fakeController(writes), mixer: fakeMixerIo(slotWrites),
    navigationOf: () => "knobs" });
  s.setEnabled(true);
  /* The E16 is present once it ACKs ENTER; the controller loads and the
   * Mixer is read on the ticks after. */
  const ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7];
  const run = (ms) => { for (let i = 0; i < ms / 16; i++) { t += 16; if (i % 60 === 0) s.feedMidi(ACK); s.tick(); } };
  const turn = (enc, n) => s.feedMidi([0xB0, enc + 1, n > 0 ? 0x01 : 0x7F]);
  const shift = (down) => s.feedMidi(down ? [0x90, 16, 0x7F] : [0x80, 16, 0]);
  run(200);

  turn(0, 1);
  eq("E16+knobs: the knobs layout is live", s.layout.name, "knobs");
  eq("E16+knobs: a top-half turn edits the one page shown", writes.slice(-1)[0], ["Main", 0, 1]);

  const labels = () => screenLabels(s.layout.screen(t)).labels;
  eq("E16+knobs: the bottom row is navigation", labels().slice(8, 16),
     ["<PG", "MAIN", "1/3", "PG>", "SL 1", "OBXD", "VOL", "PAN"]);

  turn(9, 1);
  eq("E16+knobs: the page knob pages by one", s.focus.pageIndex, 1);
  eq("E16+knobs: ...and the header counts the module pages, not the one shown",
     [s.layout.screen(t).view.headers[0].index, s.layout.screen(t).view.headers[0].count], [1, 3]);
  turn(12, 1);
  run(50);
  eq("E16+knobs: the slot knob enters the next slot at its synth", [s.slot, s.component], [1, "synth"]);
  t += 1000;
  for (let i = 0; i < 4; i++) { t += 10; turn(14, 1); }
  ok("E16+knobs: VOL writes the focused slot volume",
     slotWrites.some((w) => w[0] === 1 && w[1] === "slot:volume"));

  const rings = s.layout.rings(t);
  eq("E16+knobs: sixteen rings", rings.length, 16);
  eq("E16+knobs: the slot ring says where you are", rings[12].amount, Math.round(16383 / 3));

  const cv = createCanvas();
  drawScreen(cv, s.layout.screen(t));
  const buf = Array.from(cv.toBuffer());
  ok("E16+knobs: the navigation knobs are DRAWN (bottom half inked)", buf.slice(512).some((b) => b));
  ok("E16+knobs: ...and the page on top", buf.slice(0, 512).some((b) => b));

  shift(true); t += 60; shift(false);
  eq("E16+knobs: a Shift TAP is the Mixer here too", s.layout.mixerOn, true);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'

# ---------------------------------------------------------------------------
# THE SETTING REACHES THE SURFACES. A layout nobody selects is the gap one
# layer up: each construction must read ITS device's entry, and the entry must
# be persisted and restored. shadow_ui.js cannot be imported under node, so
# this is a source pin.
# ---------------------------------------------------------------------------
UI=src/shadow/shadow_ui.js
bad=0
note() { echo "FAIL: $1"; bad=1; }
perl -0ne 'exit(!/const e16Surface = createE16Surface\(\{\s*now: \(\) => Date\.now\(\),\s*navigationOf: \(\) => externalSurfaceNav\[1\],/)' "$UI" \
  || note "the E16 is not handed its own Surface Nav"
perl -0ne 'exit(!/const ec4Surface = createEc4Surface\(\{\s*now: \(\) => Date\.now\(\),\s*navigationOf: \(\) => externalSurfaceNav\[2\],/)' "$UI" \
  || note "the EC4 is not handed its own Surface Nav"
grep -q "config.external_surface_nav = " "$UI" || note "Surface Nav is not saved"
grep -q "const nav = config.external_surface_nav;" "$UI" || note "Surface Nav is not restored"
grep -q 'case "surface_nav":' "$UI" || note "the Surface Nav row is not read or written"
# FOLLOW FOCUS follows a module that draws its own screen (COMPONENT_EDIT --
# Teng), and never a synthesised settings grid (Global Settings went blank).
perl -0ne 'exit(!/function currentEditFocus\(\) \{.*?if \(view === VIEWS\.COMPONENT_EDIT && editingComponentKey\) \{\s*return \{ slot: selectedSlot, component: chainComponentId\(editingComponentKey\) \};/s)' "$UI" \
  || note "Follow Focus cannot see a module that draws its own screen"
perl -0ne 'exit(!/function e16FollowFocus\(\) \{.*?\^\(synth\|fx\\d\+\|midi_fx\\d\+\)\$.*?\n\}/s)' "$UI" \
  || note "Follow Focus follows synthesised settings components"
[ "$bad" = 0 ] && echo "PASS: shadow_ui.js hands each surface its Surface Nav, and Follow follows modules" || exit 1

