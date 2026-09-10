#!/usr/bin/env bash
# The E16 lifecycle: seek, hold, give back.
#
# Every property here fails SILENTLY on hardware, which is why they are pinned
# against an injected clock instead of a device:
#
#   - a probe that ignores its interval floods the external MIDI buffer, which
#     is shared with everything else Schwung sends out of cable 2. Nothing
#     errors; other traffic just starts getting refused.
#   - a probe that never repeats after the ACK cannot recover a device that was
#     unplugged -- the E16 has no battery, so a replug is a power cycle out of
#     remote mode and the device says nothing about it.
#   - a `send` that returned false has NOT gone out (docs/SYSEX.md, "OUT: a
#     false return means retry"). Advancing the probe clock past a refused
#     send is a message the device never sees and nothing ever re-sends.
#   - an EXIT that is skipped leaves the device blank with the feature off.
#
# The assembler half is the four rules from docs/SYSEX.md, two of which look
# optional and are not: a realtime byte inside a SysEx run is legal, and a
# message interrupted by another status byte must be ABANDONED rather than
# spliced -- splicing produces a message that parses and is fiction.
set -euo pipefail
cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { createLifecycle, createSysexAssembler, KEEPALIVE_MS, LOSS_MS }
    from "./src/shared/e16_surface.mjs";
import { enterMsg, exitMsg, packetize, ACK_BODY } from "./src/shared/e16_protocol.mjs";
import { GLOBAL_SECTIONS, GLOBAL_ROUTING, buildGlobalSettingsContract }
    from "./src/shadow/shadow_ui_global_grid.mjs";
import { planPages } from "./src/shared/param_pages/page_plan.mjs";

let fails = 0;
const eq = (n, g, w) => {
  const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; }
  else console.log("ok   " + n);
};

const ENTER = JSON.stringify(packetize(enterMsg()));
const EXIT  = JSON.stringify(packetize(exitMsg()));

/* A sender that records, and can be told to refuse. */
function recorder() {
  const sent = [];
  const fn = (p) => { if (fn.refuse) return false; sent.push(JSON.stringify(p)); return true; };
  fn.sent = sent;
  fn.refuse = false;
  fn.count = (what) => sent.filter((s) => s === what).length;
  return fn;
}

/* ---- 1. seeking: one ENTER per period and no more ---------------------- */
{
  const lc = createLifecycle({ probeMs: 2000 });
  const s = recorder();
  lc.setEnabled(true, 0, s);
  /* 60 ticks over 6 seconds -- the shadow UI ticks at 60 Hz, so the interval
   * is doing all the work here. */
  for (let t = 0; t <= 6000; t += 100) lc.tick(t, s);
  /* t=0, 2000, 4000, 6000 -- the first probe is immediate, because switching
   * the setting on is the user asking for the device now. */
  eq("four ENTERs across six seconds", s.count(ENTER), 4);
  eq("nothing but ENTER while seeking", s.sent.every((x) => x === ENTER), true);
  eq("not present without an ACK", lc.present, false);
}

/* ---- 2. the ACK stops the heartbeat ------------------------------------ */
{
  const lc = createLifecycle({ probeMs: 2000, keepaliveMs: 10000, lossMs: 25000 });
  const s = recorder();
  lc.setEnabled(true, 0, s);
  lc.tick(0, s);
  eq("one probe out", s.count(ENTER), 1);
  lc.onSysex(ACK_BODY, 10);
  eq("acked means present", lc.present, true);
  const before = s.sent.length;
  for (let t = 100; t <= 8000; t += 100) lc.tick(t, s);
  eq("zero sends after an ACK", s.sent.length - before, 0);
}

/* ---- 3. losing the device returns to seeking --------------------------- */
{
  const lc = createLifecycle({ probeMs: 2000, keepaliveMs: 10000, lossMs: 25000 });
  const s = recorder();
  lc.setEnabled(true, 0, s);
  lc.tick(0, s);
  lc.onSysex(ACK_BODY, 0);
  /* Keepalives go out and are never answered -- which is exactly what an
   * unplugged E16 looks like, because it cannot report its own absence. */
  for (let t = 100; t <= 24000; t += 100) lc.tick(t, s);
  eq("still held inside the loss window", lc.present, true);
  eq("keepalives, not a flood", s.count(ENTER), 1 + 2);
  for (let t = 24100; t <= 30000; t += 100) lc.tick(t, s);
  eq("silence past lossMs is a loss", lc.present, false);
  const seeking = s.count(ENTER);
  for (let t = 30100; t <= 36000; t += 100) lc.tick(t, s);
  eq("and seeking resumes at the fast cadence", s.count(ENTER) - seeking, 3);
  /* A late ACK re-adopts the device with no other gesture. */
  lc.onSysex(ACK_BODY, 36000);
  eq("a replug is re-adopted", lc.present, true);
}

/* ---- 4. disable sends EXIT exactly once -------------------------------- */
{
  const lc = createLifecycle();
  const s = recorder();
  lc.setEnabled(true, 0, s);
  lc.tick(0, s);
  lc.onSysex(ACK_BODY, 0);
  lc.setEnabled(false, 100, s);
  eq("one EXIT", s.count(EXIT), 1);
  eq("disabled is not present", lc.present, false);
  lc.setEnabled(false, 200, s);
  for (let t = 200; t <= 20000; t += 100) lc.tick(t, s);
  eq("still exactly one EXIT, and no probes after it", s.count(EXIT), 1);
  eq("nothing sent while disabled", s.sent[s.sent.length - 1], EXIT);
}

/* ---- 5. a refused send is a RETRY, not a send -------------------------- */
{
  const lc = createLifecycle({ probeMs: 2000 });
  const s = recorder();
  s.refuse = true;
  lc.setEnabled(true, 0, s);
  for (let t = 0; t <= 6000; t += 100) lc.tick(t, s);
  eq("a refused probe sends nothing", s.sent.length, 0);
  s.refuse = false;
  lc.tick(6100, s);
  eq("and retries on the very next tick, not a period later", s.count(ENTER), 1);

  /* The same for EXIT: an owed one is drained by tick, so the device is not
   * left blank because the buffer happened to be full at that instant. */
  lc.onSysex(ACK_BODY, 6100);
  s.refuse = true;
  lc.setEnabled(false, 6200, s);
  eq("refused EXIT is not sent", s.count(EXIT), 0);
  s.refuse = false;
  lc.tick(6300, s);
  eq("owed EXIT goes out on the next tick", s.count(EXIT), 1);
  lc.tick(6400, s);
  eq("and only once", s.count(EXIT), 1);
}

/* ---- 6. inbound reassembly -------------------------------------------- */
{
  const got = [];
  const asm = createSysexAssembler({ onMessage: (m) => got.push(m.join(" ")) });
  const feed3 = (bytes) => { for (let i = 0; i < bytes.length; i += 3) asm.feed(bytes.slice(i, i + 3)); };

  feed3([0xF0].concat(ACK_BODY, [0xF7]));
  eq("an ACK arrives whole across 3-byte packets", got.length, 1);
  eq("body is what lies between F0 and F7", got[0], ACK_BODY.join(" "));

  /* Realtime interleaved mid-message is LEGAL and must not break the run. */
  got.length = 0;
  asm.feed([0xF0, ACK_BODY[0], 0xF8]);
  asm.feed(ACK_BODY.slice(1, 4));
  asm.feed([0xFE].concat(ACK_BODY.slice(4, 6)));
  asm.feed([ACK_BODY[6], 0xF7]);
  eq("realtime bytes are skipped, not fatal", got.join("|"), ACK_BODY.join(" "));

  /* A message interrupted by a channel status byte is ABANDONED. Splicing the
   * halves together makes a message that parses and never existed. */
  got.length = 0;
  asm.feed([0xF0, 0x00, 0x21]);
  asm.feed([0x90, 0x40, 0x7F]);        /* a note-on cuts in */
  asm.feed([0x5B, 0x02, 0xF7]);        /* the rest of the "message" */
  eq("an interrupted message is dropped, never spliced", got.length, 0);

  /* A dropped F7 must not leak. */
  const capped = createSysexAssembler({ max: 8, onMessage: () => {} });
  capped.feed([0xF0]);
  for (let i = 0; i < 400; i++) capped.feed([1, 2, 3]);
  eq("the buffer is capped", capped.pending <= 8, true);
}

/* ---- 7. the setting, on the System page ------------------------------- */
{
  const system = GLOBAL_SECTIONS.find((s) => s.id === "system");
  const p = (system ? system.params : []).find((x) => x.key === "external_surface");
  if (!p) { console.log("FAIL external_surface is not on the System section"); fails++; }
  else {
    eq("Off / E16", p.options, ["Off", "E16"]);
    eq("default off", p.default, 0);
    eq("routed", !!GLOBAL_ROUTING.external_surface, true);
  }
  /* SIX PAGES STILL. A section carrying a `menu` plans a second page, and the
   * one-section-one-page property is what makes sections-as-levels work. */
  const plan = planPages(Object.assign({ paginate: false }, buildGlobalSettingsContract()));
  eq("the page list is unchanged",
     plan.pages.map((x) => x.name),
     ["Display", "Audio", "Screen Reader", "Set Pages", "Shortcuts", "System"]);
}

/* ---- 8. the host actually calls it ------------------------------------ */
/* A pure module nothing invokes is a feature that passes its tests and does
 * not exist. Three seams, each of which has been the whole bug before. */
{
  const FS = await import("node:fs");
  const src = FS.readFileSync("./src/shadow/shadow_ui.js", "utf8");
  eq("shadow_ui imports the surface", /e16_surface\.mjs/.test(src), true);
  eq("the lifecycle is ticked", /externalSurfaceTick\(/.test(src), true);
  eq("external SysEx is fed to the assembler",
     /onMidiMessageExternal[\s\S]{0,400}externalSurfaceMidi\(/.test(src), true);
}


/* ---------------------------------------------------------------------------
 * A REPLUG MUST PRODUCE AN EDGE.
 *
 * The presence edge is the ONLY thing that repaints a device which has come
 * back, so an expiry slower than a realistic replug is not a conservative
 * choice -- it is the bug. With KEEPALIVE 10s / LOSS 25s, a cable out and back
 * inside 25 s never crossed the threshold: present stayed true, no edge fired,
 * and the surface believed it had painted a panel that had been wiped by losing
 * power. The next keepalive ENTER put it back into remote mode with a blank
 * screen, which is why it was reported as "it did go back into remote mode"
 * and "replug didnt recover".
 *
 * Pinned as a RELATIONSHIP, not as two numbers: whatever the constants become,
 * a plausible replug has to expire presence, and one dropped ACK must not.
 * ------------------------------------------------------------------------- */
{
  eq("loss tolerates a dropped ACK (" + LOSS_MS + " vs keepalive " + KEEPALIVE_MS + ")",
     LOSS_MS >= KEEPALIVE_MS * 2, true);
  eq("an absence of a few seconds EXPIRES -- a replug must produce the edge "
     + "that repaints (LOSS_MS=" + LOSS_MS + ")", LOSS_MS <= 8000, true);

  /* Drive the real machine across a replug shorter than the old LOSS_MS. */
  const sent = [];
  const send = (p) => { sent.push(p); return true; };
  let t = 0;
  const lc = createLifecycle();
  lc.setEnabled(true, t, send);
  lc.tick(t, send);
  lc.onSysex(ACK_BODY, t);
  eq("replug: present after the ACK", lc.present, true);

  /* Cable out for 8 s -- well inside the OLD 25 s window. */
  for (let i = 0; i < 40; i++) { t += 200; lc.tick(t, send); }
  eq("replug: an 8 s absence expires presence -- with the old constants this "
     + "stayed true and no repaint ever happened", lc.present, false);

  lc.onSysex(ACK_BODY, t);
  eq("replug: present again on the ACK, which is the repaint edge", lc.present, true);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
