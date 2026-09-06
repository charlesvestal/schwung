#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Hold Copy (or Delete), then pick an instance: the knob grid's instance
# copy / clear gesture, with a one-level Undo. Driven through the real
# controller against a fake device whose params are a plain map, so every
# write the gesture makes is visible and every read it depends on is honest.
#
# Pinned:
#   1. Copy down snapshots the FOCUSED instance; a focus change while held
#      pastes the declared child_copy_keys, in declared order, into the new
#      instance -- and nothing else is written
#   2. Undo restores exactly what the paste overwrote
#   3. Delete + pick writes declared defaults ("" for a filepath), nothing for
#      a key with no default
#   4. no gesture on a page with no instance, and no paste onto the source

fail() { echo "FAIL: $*" >&2; exit 1; }
command -v node >/dev/null 2>&1 || fail "node is required"

node -e '
import("./src/shared/param_pages/page_controller.mjs").then((PC) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  const PADS = {
    label: "Pads", child_count: 4, child_label: "Pad", child_prefix: "pad",
    child_index_param: "ui_current_pad",
    child_copy_keys: ["sample", "vol", "gain"],
    knobs: ["vol", "tune"],
    params: [
      { key: "sample", name: "Sample", type: "filepath" },
      { key: "vol", name: "Vol", type: "float", min: 0, max: 1, default: 0.5 },
      { key: "tune", name: "Tune", type: "float", min: -12, max: 12 },
    ],
  };
  const HIER = { pad_layout: "drums", levels: {
    root: { params: [{ level: "pads", label: "Pads" }, { level: "fx", label: "FX" }] },
    pads: PADS,
    fx: { name: "FX", knobs: ["verb"], params: [{ key: "verb", name: "Verb", type: "float", min: 0, max: 1 }] },
  } };
  const CP = [];
  for (let i = 0; i < 4; i++) {
    CP.push({ key: `pad${i}_sample`, name: "Sample", type: "filepath" });
    CP.push({ key: `pad${i}_vol`, name: "Vol", type: "float", min: 0, max: 1, default: 0.5 });
    CP.push({ key: `pad${i}_tune`, name: "Tune", type: "float", min: -12, max: 12 });
    CP.push({ key: `pad${i}_gain`, name: "Gain", type: "float", min: 0, max: 2 });
  }
  CP.push({ key: "verb", name: "Verb", type: "float", min: 0, max: 1 });

  const dev = {};
  for (let i = 0; i < 4; i++) { dev[`pad${i}_sample`] = `/s/${i}.wav`; dev[`pad${i}_vol`] = String((i + 1) / 10); dev[`pad${i}_tune`] = String(i); dev[`pad${i}_gain`] = String(1 + i); }
  dev.verb = "0.3"; dev.ui_current_pad = "0";
  const writes = [];
  let failKey = null;                       /* a read that does not complete */
  const io = {
    getParam: (k) => {
      const key = k.replace(/^synth:/, "");
      if (failKey && key === failKey) return null;
      if (key === "ui_hierarchy") return JSON.stringify(HIER);
      if (key === "chain_params") return JSON.stringify(CP);
      if (key === "preset_name") return "";
      if (key === "is_loading") return "0";
      if (key === "module") return "dr32";
      return key in dev ? dev[key] : null;
    },
    setParam: (k, v) => { const key = k.replace(/^synth:/, ""); dev[key] = String(v); writes.push([key, String(v)]); },
    announce: () => {}, now: () => Date.now(),
  };
  const c = PC.createController(io);
  c.load({ slot: 0, component: "synth", prefix: "synth" });
  c.setLayout("movy");
  const spin = (n) => { for (let i = 0; i < n; i++) c.tick(); };
  spin(40);
  const pageOf = (lvl) => c.pages.findIndex((p) => p.level === lvl && Array.isArray(p.keys) && p.keys.some(Boolean));
  if (pageOf("pads") < 0 || pageOf("fx") < 0) fail("expected a knob page for pads and fx, got " + JSON.stringify(c.pages.map((p) => p.level)));
  const onPads = () => { c.goToPage(pageOf("pads")); spin(5); };
  onPads();
  const focus = (i) => { dev.ui_current_pad = String(i); spin(30); };

  /* ---- 1. copy pad 0 into pad 2 --------------------------------------- */
  writes.length = 0;
  if (!c.onEditCc(60, true)) fail("Copy down was not taken on a pad page");
  if (!c.editGesture || c.editGesture.kind !== "copy" || c.editGesture.from !== 0) fail("no copy gesture armed from pad 0");
  focus(2);
  const pasted = writes.filter(([k]) => k.startsWith("pad2_"));
  const want = [["pad2_sample", "/s/0.wav"], ["pad2_vol", "0.1"], ["pad2_gain", "1"]];
  if (JSON.stringify(pasted) !== JSON.stringify(want))
    fail("paste wrote " + JSON.stringify(pasted) + ", want " + JSON.stringify(want) + " (declared keys, declared order)");
  if (writes.some(([k]) => k === "pad2_tune")) fail("a key outside child_copy_keys was written");
  if (writes.some(([k]) => k.startsWith("pad0_") || k.startsWith("pad1_") || k.startsWith("pad3_"))) fail("a paste touched an instance that was not picked: " + JSON.stringify(writes));
  c.onEditCc(60, false);
  if (c.editGesture) fail("Copy release did not end the gesture");

  /* ---- 2. undo puts pad 2 back ----------------------------------------- */
  writes.length = 0;
  c.onEditCc(56, true);
  const undone = writes.filter(([k]) => k.startsWith("pad2_"));
  const wantUndo = [["pad2_sample", "/s/2.wav"], ["pad2_vol", "0.3"], ["pad2_gain", "3"]];
  if (JSON.stringify(undone) !== JSON.stringify(wantUndo)) fail("undo wrote " + JSON.stringify(undone) + ", want " + JSON.stringify(wantUndo));
  writes.length = 0;
  c.onEditCc(56, true);
  if (writes.length) fail("a second Undo wrote again: " + JSON.stringify(writes));

  /* ---- 3. clear pad 1 ---------------------------------------------------- */
  writes.length = 0;
  c.onEditCc(119, true);
  focus(1);
  const cleared = writes.filter(([k]) => k.startsWith("pad1_"));
  const wantClear = [["pad1_sample", ""], ["pad1_vol", "0.5"]];
  if (JSON.stringify(cleared) !== JSON.stringify(wantClear)) fail("clear wrote " + JSON.stringify(cleared) + ", want " + JSON.stringify(wantClear) + " (filepath -> \"\", declared default, no-default key untouched)");
  c.onEditCc(119, false);

  /* ---- 4. no instance, no gesture; no paste onto the source ------------- */
  c.goToPage(pageOf("fx")); spin(5);
  if (c.onEditCc(60, true)) fail("Copy was taken on a page with no instance");
  if (c.editGesture) fail("a gesture was armed on a page with no instance");
  onPads();
  writes.length = 0;
  c.onEditCc(60, true);
  focus(3); focus(1);
  if (writes.some(([k]) => k.startsWith("pad1_"))) fail("returning to the source pasted onto it");
  c.onEditCc(60, false);

  /* ---- 5. taps faster than the read rotation ---------------------------- */
  focus(0); writes.length = 0;
  c.onEditCc(60, true);
  dev.ui_current_pad = "1"; c.tick();
  dev.ui_current_pad = "2"; c.tick();
  dev.ui_current_pad = "3"; spin(5);
  const hit = [...new Set(writes.map(([k]) => k.slice(0, 4)))].sort();
  if (JSON.stringify(hit) !== JSON.stringify(["pad1", "pad2", "pad3"]))
    fail("quick picks were dropped: pasted into " + JSON.stringify(hit) + ", want pad1, pad2, pad3");
  c.onEditCc(60, false);

  /* ---- 6. the source is the instance focused NOW ------------------------ */
  focus(0);
  dev.pad3_sample = "/s/three.wav";            /* case 5 pasted over it */
  dev.ui_current_pad = "3";                    /* hit a pad and hold Copy at once: */
                                               /* no tick, so the cache is a pad behind */
  writes.length = 0;
  c.onEditCc(60, true);
  if (!c.editGesture || c.editGesture.from !== 3) fail("Copy snapshotted instance " + (c.editGesture && c.editGesture.from) + ", want the one focused now (3)");
  dev.ui_current_pad = "1"; spin(5);
  const src = writes.filter(([k]) => k === "pad1_sample").map(([, v]) => v);
  if (JSON.stringify(src) !== JSON.stringify(["/s/three.wav"])) fail("pasted from the wrong source: " + JSON.stringify(src));
  c.onEditCc(60, false);

  /* ---- 7. a read that did not complete -------------------------------- */
  focus(0);
  failKey = "pad0_sample";
  writes.length = 0;
  c.onEditCc(60, true);
  if (c.editGesture) fail("a gesture was armed from a source that could not be read");
  dev.ui_current_pad = "2"; spin(5);
  if (writes.length) fail("a failed source read still wrote: " + JSON.stringify(writes));
  c.onEditCc(60, false);
  failKey = null;

  /* ---- 8. the notice outlives the release, inside the page frame -------- */
  focus(0);
  c.onEditCc(60, true);
  dev.ui_current_pad = "2"; spin(5);
  /* Deliberately a frame whose centre is NOT the panel centre, so a notice
   * that ignored the rect lands outside it and the assertion can fail. */
  const RECT = { x: 4, y: 2, w: 70, h: 16 };
  const prints = [];
  const gctx = { fillRect: () => {}, print: (x, y, t) => { prints.push([x, y, String(t)]); }, textWidth: (t) => t.length * 6 };
  c.onEditCc(60, false);
  c.render(gctx, { title: "t", rect: RECT });
  prints.length = 0;
  c.renderOverlays(gctx, { clearScreen: () => {} });
  const said = prints.filter(([, , t]) => t.indexOf("PASTED") === 0);
  if (!said.length) fail("the result notice did not survive the release");
  const [nx, ny] = said[0];
  if (nx < RECT.x || nx > RECT.x + RECT.w || ny < RECT.y || ny > RECT.y + RECT.h)
    fail("the notice was drawn at " + nx + "," + ny + ", outside the frame it was given " + JSON.stringify(RECT));

  if (bad) process.exit(1);
  console.log("  ok  copy pastes the declared keys in order into each picked instance; undo restores; clear writes defaults; quick picks are all taken; a failed read writes nothing; the notice outlives the release");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
' || fail "controller half"

pp="src/shadow/shadow_ui_param_pages.mjs"
command grep -q 'controller.onEditCc(data\[1\], data\[2\] > 0)' "$pp" || fail "the host does not route CC 56/60/119 to the gesture"
command grep -q '^#### Copying and clearing an instance' docs/MODULES.md || fail "the gesture is undocumented"
echo "PASS: test_child_copy_gesture"
